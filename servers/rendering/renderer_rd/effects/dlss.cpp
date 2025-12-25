/**************************************************************************/
/*  dlss.cpp                                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHOR OR AUTHORS BE LIABLE FOR ANY CLAIM,       */
/* DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,           */
/* TORT OR OTHERWISE, ARISING FROM, FROM, OUT OF OR IN CONNECTION WITH     */
/* THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.             */
/**************************************************************************/

#include "dlss.h"

#include "../storage_rd/material_storage.h"
#include "../uniform_set_cache_rd.h"
#include "drivers/vulkan/godot_vulkan.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

#ifndef _MSC_VER
#include <cwchar>
#define wcscpy_s wcscpy
#endif

// For dlopen/dlsym to load Vulkan directly, bypassing volk
#ifndef _WIN32
#include <dlfcn.h>
#endif

// ============================================================================
// NGX Logging Callback
// ============================================================================

namespace {

// Logging callback for NGX SDK
void NVSDK_CONV ngx_logging_callback(const char *message, NVSDK_NGX_Logging_Level logging_level, NVSDK_NGX_Feature source_component) {
	const char *level_str = "INFO";
	switch (logging_level) {
		case NVSDK_NGX_LOGGING_LEVEL_OFF:
			level_str = "OFF";
			break;
		case NVSDK_NGX_LOGGING_LEVEL_ON:
			level_str = "ON";
			break;
		case NVSDK_NGX_LOGGING_LEVEL_VERBOSE:
			level_str = "VERBOSE";
			break;
		default:
			level_str = "UNKNOWN";
			break;
	}
	print_line(vformat("[NGX %s] %s", level_str, String(message)));
}

} // anonymous namespace

// ============================================================================
// TLS (Temporal Stable Super-sampling) Jitter Pattern
// ============================================================================

// Global thread-local storage for Vulkan function pointers
// Defined here to avoid multiple definition errors
// Must be in RendererRD namespace to match the extern declaration
namespace RendererRD {
thread_local DLSSVulkanFunctions *global_dlss_vulkan_functions = nullptr;
}

Vector<Vector2> TLS::get_halton_sequence(int p_count) {
	Vector<Vector2> sequence;
	// Halton sequence (2, 3) for low-discrepancy sampling
	for (int i = 0; i < p_count; i++) {
		float x = 0.0f;
		float y = 0.0f;

		// Base 2 for x
		{
			int index = i + 1;
			float f = 1.0f;
			while (index > 0) {
				f /= 2.0f;
				x += f * (index % 2);
				index /= 2;
			}
		}

		// Base 3 for y
		{
			int index = i + 1;
			float f = 1.0f;
			while (index > 0) {
				f /= 3.0f;
				y += f * (index % 3);
				index /= 3;
			}
		}

		sequence.push_back(Vector2(x - 0.5f, y - 0.5f));
	}
	return sequence;
}

Vector<Vector2> TLSJitterPattern::get_jittered_offset(int p_frame, int p_jitter_phase_count) {
	Vector<Vector2> halton_sequence = TLS::get_halton_sequence(p_jitter_phase_count);
	Vector<Vector2> jittered_sequence;

	for (int i = 0; i < halton_sequence.size(); i++) {
		jittered_sequence.push_back(halton_sequence[i]);
	}

	return jittered_sequence;
}

// ============================================================================
// Vulkan Function Loading (bypassing volk)
// ============================================================================

void DLSSVulkanFunctions::cleanup() {
	GetInstanceProcAddr = nullptr;
	GetDeviceProcAddr = nullptr;
	CreateCommandPool = nullptr;
	DestroyCommandPool = nullptr;
	AllocateCommandBuffers = nullptr;
	FreeCommandBuffers = nullptr;
	BeginCommandBuffer = nullptr;
	EndCommandBuffer = nullptr;
	QueueSubmit = nullptr;
	QueueWaitIdle = nullptr;
}

bool DLSSContext::load_vulkan_functions(DLSSVulkanFunctions &r_functions) {
	// Check if already loaded
	if (r_functions.is_valid()) {
		return true;
	}

	ERR_PRINT("DLSS: Loading Vulkan functions directly from libvulkan.so...");

#ifdef _WIN32
	// Windows: Load vulkan-1.dll
	HMODULE vulkan_lib = LoadLibraryA("vulkan-1.dll");
	if (!vulkan_lib) {
		ERR_PRINT("DLSS: Failed to load vulkan-1.dll");
		return false;
	}

#define GET_VK_PROC_ADDR(member, name) \
	r_functions.member = reinterpret_cast<PFN_##name>(GetProcAddress(vulkan_lib, #name)); \
	if (!r_functions.member) { \
		ERR_PRINT(vformat("DLSS: Failed to load function: " #name)); \
		FreeLibrary(vulkan_lib); \
		return false; \
	}

#else
	// Linux/Unix: Load libvulkan.so
	void *vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!vulkan_lib) {
		vulkan_lib = dlopen("libvulkan.so", RTLD_NOW);
	}
	if (!vulkan_lib) {
		ERR_PRINT("DLSS: Failed to load libvulkan.so");
		ERR_PRINT(vformat("DLSS: dlopen error: %s", dlerror()));
		return false;
	}

#define GET_VK_PROC_ADDR(member, name) \
	r_functions.member = reinterpret_cast<PFN_##name>(dlsym(vulkan_lib, #name)); \
	if (!r_functions.member) { \
		ERR_PRINT(vformat("DLSS: Failed to load function: " #name)); \
		dlclose(vulkan_lib); \
		return false; \
	}

#endif

	// Load instance-level functions first
	GET_VK_PROC_ADDR(GetInstanceProcAddr, vkGetInstanceProcAddr);

	// Load vkGetDeviceProcAddr - NGX SDK needs this to load device extension functions
	GET_VK_PROC_ADDR(GetDeviceProcAddr, vkGetDeviceProcAddr);

	// Load device-level functions directly (for our own command buffer management)
	GET_VK_PROC_ADDR(CreateCommandPool, vkCreateCommandPool);
	GET_VK_PROC_ADDR(DestroyCommandPool, vkDestroyCommandPool);
	GET_VK_PROC_ADDR(AllocateCommandBuffers, vkAllocateCommandBuffers);
	GET_VK_PROC_ADDR(FreeCommandBuffers, vkFreeCommandBuffers);
	GET_VK_PROC_ADDR(ResetCommandBuffer, vkResetCommandBuffer);
	GET_VK_PROC_ADDR(BeginCommandBuffer, vkBeginCommandBuffer);
	GET_VK_PROC_ADDR(EndCommandBuffer, vkEndCommandBuffer);
	GET_VK_PROC_ADDR(QueueSubmit, vkQueueSubmit);
	GET_VK_PROC_ADDR(QueueWaitIdle, vkQueueWaitIdle);

#undef GET_VK_PROC_ADDR

	ERR_PRINT("DLSS: Successfully loaded Vulkan functions directly");
	return true;
}

void DLSSContext::cleanup_vulkan_functions() {
	if (global_dlss_vulkan_functions) {
		global_dlss_vulkan_functions->cleanup();
		memdelete(global_dlss_vulkan_functions);
		global_dlss_vulkan_functions = nullptr;
	}
}

// ============================================================================
// DLSS Context Implementation
// ============================================================================

Size2i DLSSContext::get_render_size(const Size2i &p_output_size, DLSSQuality p_quality) {
	// Get optimal render size for DLSS based on output size and quality mode
	// NOTE: This is a static helper using approximate scale factors
	// The actual optimal resolution is queried via NGX_DLSS_GET_OPTIMAL_SETTINGS in ensure_context()
	float scale_factor = 0.5f; // Default to Ultra Performance (0.5x)

	switch (p_quality) {
		case DLSSQuality::DLAA:
			scale_factor = 1.0f;
			break;
		case DLSSQuality::ULTRA_QUALITY:
			scale_factor = 0.77f;
			break;
		case DLSSQuality::QUALITY:
			scale_factor = 0.67f;
			break;
		case DLSSQuality::BALANCED:
			scale_factor = 0.59f;
			break;
		case DLSSQuality::PERFORMANCE:
			scale_factor = 0.5f;
			break;
		case DLSSQuality::ULTRA_PERFORMANCE:
			scale_factor = 0.33f;
			break;
	}

	return Size2i(
			static_cast<int>(Math::ceil(p_output_size.width * scale_factor)),
			static_cast<int>(Math::ceil(p_output_size.height * scale_factor)));
}

bool DLSSContext::is_supported() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!rd) {
		return false;
	}

	return true;
}

DLSSContext::DLSSContext() {
}

DLSSContext::~DLSSContext() {
	shutdown();
}

bool DLSSContext::create_command_pool() {
	ERR_FAIL_COND_V_MSG(vk_device == VK_NULL_HANDLE, false, "Vulkan device not set");
	ERR_FAIL_COND_V_MSG(command_pool != VK_NULL_HANDLE, false, "Command pool already created");
	ERR_FAIL_COND_V_MSG(!vk_functions.CreateCommandPool, false, "vkCreateCommandPool function not loaded");

	const String device_hex = String::num_uint64((uint64_t)vk_device, 16);
	ERR_PRINT(vformat("DLSS: Creating command pool (device=0x%s queue_family=%d)",
			device_hex, vk_queue_family_index));

	VkCommandPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = vk_queue_family_index;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	VkResult result = vk_functions.CreateCommandPool(vk_device, &pool_info, nullptr, &command_pool);
	if (result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to create command pool with error code: %d", (int)result));
		return false;
	}

	ERR_PRINT("DLSS: Command pool created");

	return true;
}

bool DLSSContext::allocate_command_buffer() {
	ERR_FAIL_COND_V_MSG(command_pool == VK_NULL_HANDLE, false, "Command pool not created");
	ERR_FAIL_COND_V_MSG(command_buffer != VK_NULL_HANDLE, false, "Command buffer already allocated");
	ERR_FAIL_COND_V_MSG(!vk_functions.AllocateCommandBuffers, false, "vkAllocateCommandBuffers function not loaded");

	ERR_PRINT("DLSS: Allocating command buffer");

	VkCommandBufferAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool = command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;

	VkResult result = vk_functions.AllocateCommandBuffers(vk_device, &alloc_info, &command_buffer);
	if (result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to allocate command buffer with error code: %d", (int)result));
		return false;
	}

	ERR_PRINT("DLSS: Command buffer allocated");

	return true;
}

void DLSSContext::free_command_buffer() {
	if (command_buffer != VK_NULL_HANDLE && vk_device != VK_NULL_HANDLE && vk_functions.FreeCommandBuffers) {
		vk_functions.FreeCommandBuffers(vk_device, command_pool, 1, &command_buffer);
		command_buffer = VK_NULL_HANDLE;
	}
}

bool DLSSContext::initialize(const CreateParams &p_params) {
	if (initialized) {
		return true;
	}

	ERR_PRINT("DLSS: Context initialize begin");

	render_size = p_params.render_size;
	output_size = p_params.output_size;
	quality = p_params.quality;

	// Create command pool and allocate command buffer
	if (command_pool == VK_NULL_HANDLE) {
		if (!create_command_pool()) {
			return false;
		}
	}

	if (command_buffer == VK_NULL_HANDLE) {
		if (!allocate_command_buffer()) {
			return false;
		}
	}

	available = true;
	initialized = true;

	ERR_PRINT("DLSS: Context initialize complete");

	return initialized;
}

void DLSSContext::shutdown() {
	if (!initialized) {
		return;
	}

	// Note: capability_params are owned by NGX SDK and will be cleaned up during NGX shutdown
	// We don't destroy them here

	// Free command buffer
	free_command_buffer();

	// Destroy command pool
	if (command_pool != VK_NULL_HANDLE && vk_device != VK_NULL_HANDLE && vk_functions.DestroyCommandPool) {
		vk_functions.DestroyCommandPool(vk_device, command_pool, nullptr);
		command_pool = VK_NULL_HANDLE;
	}

	if (feature_handle) {
		feature_handle->Id = 0;
	}
	initialized = false;
}

// ============================================================================
// DLSS Upscaler Implementation
// ============================================================================

DLSSUpscaler::DLSSUpscaler() {
	// Set application data path for logs
	String data_path = OS::get_singleton()->get_user_data_dir();
	if (data_path.is_empty()) {
		data_path = OS::get_singleton()->get_temp_path();
	}

	const char *data_path_utf8 = data_path.utf8().get_data();
	mbstowcs(app_data_path, data_path_utf8, sizeof(app_data_path) / sizeof(wchar_t));
}

DLSSUpscaler::~DLSSUpscaler() {
	shutdown_ngx();
	if (context) {
		memdelete(context);
		context = nullptr;
	}
}

bool DLSSUpscaler::initialize_ngx() {
	if (ngx_initialized) {
		return true;
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	ERR_PRINT("DLSS: Initializing NGX");

	// Get Vulkan handles
	VkInstance vk_instance = (VkInstance)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT);
	VkDevice vk_device = (VkDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice vk_physical_device = (VkPhysicalDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE);

	if (vk_instance == VK_NULL_HANDLE || vk_device == VK_NULL_HANDLE || vk_physical_device == VK_NULL_HANDLE) {
		ERR_PRINT("DLSS: Failed to get Vulkan handles");
		return false;
	}

	// Get queue
	uint64_t queue_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE);
	VkQueue vk_queue = (queue_handle != 0) ? (VkQueue)queue_handle : VK_NULL_HANDLE;

	uint64_t queue_family = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY);
	uint32_t queue_family_index = (queue_family != 0) ? (uint32_t)queue_family : 0;

	if (vk_queue == VK_NULL_HANDLE) {
		ERR_PRINT("DLSS: Failed to get Vulkan queue");
		return false;
	}

	// Create or get the global Vulkan functions (loaded via dlopen, bypassing volk)
	if (!global_dlss_vulkan_functions) {
		global_dlss_vulkan_functions = memnew(DLSSVulkanFunctions);
		if (!DLSSContext::load_vulkan_functions(*global_dlss_vulkan_functions)) {
			ERR_PRINT("DLSS: Failed to load Vulkan functions via dlopen");
			memdelete(global_dlss_vulkan_functions);
			global_dlss_vulkan_functions = nullptr;
			return false;
		}
	}

	// Set up path to DLSS runtime libraries
	// The DLSS libraries (libnvidia-ngx-dlss.so) are located in the DLSS/lib/Linux_x86_64/rel folder
	static wchar_t dlss_library_path[256] = L"/home/sorche/Development/godot_src/godot/DLSS/lib/Linux_x86_64/rel/";
	const wchar_t *path_list[] = { dlss_library_path };

	// Set up logging info
	NVSDK_NGX_LoggingInfo logging_info = {};
	logging_info.LoggingCallback = ngx_logging_callback;
	logging_info.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
	logging_info.DisableOtherLoggingSinks = false;

	// Set up feature common info with path list and logging
	NVSDK_NGX_FeatureCommonInfo feature_info = {};
	feature_info.PathListInfo.Path = path_list;
	feature_info.PathListInfo.Length = 1;
	feature_info.InternalData = nullptr;
	feature_info.LoggingInfo = logging_info;

	// Initialize NGX with proper project ID string
	ERR_PRINT("DLSS: Initializing NGX...");
	print_line(vformat("DLSS: Using library path: %s", String(dlss_library_path).utf8().get_data()));

	// Use NVSDK_NGX_VULKAN_Init_with_ProjectID which takes a proper project ID string
	// The Project ID must be GUID-like, e.g., "a0f57b54-1daf-4934-90ae-c4035c19df04"
	// For custom engines, you can generate your own GUID
	const char *project_id = "a0f57b54-1daf-4934-90ae-c4035c19df04";  // GUID format for Godot
	const char *engine_version = "4.6.beta";                           // Godot version
	NVSDK_NGX_EngineType engine_type = NVSDK_NGX_ENGINE_TYPE_CUSTOM;

	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			project_id,
			engine_type,
			engine_version,
			app_data_path,
			vk_instance,
			vk_physical_device,
			vk_device,
			global_dlss_vulkan_functions->GetInstanceProcAddr,
			global_dlss_vulkan_functions->GetDeviceProcAddr,
			&feature_info,
			NVSDK_NGX_Version_API);

	if (result != NVSDK_NGX_Result_Success) {
		ERR_PRINT(vformat("DLSS: NGX initialization failed with error code: %d", (int)result));
		return false;
	}

	ERR_PRINT("DLSS: NGX initialized successfully");

	// Get capability parameters - this is the parameters object we should use for all feature operations
	// Following NVIDIA's pattern: get capability params once, reuse for all operations
	NVSDK_NGX_Parameter *capability_params = nullptr;
	result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capability_params);
	if (result != NVSDK_NGX_Result_Success) {
		ERR_PRINT(vformat("DLSS: Failed to get capability parameters with error code: %d", (int)result));
		NVSDK_NGX_VULKAN_Shutdown1(vk_device);
		return false;
	}

	// Create context if needed
	ERR_PRINT("DLSS: Checking if context exists...");
	if (!context) {
		ERR_PRINT("DLSS: Creating new DLSSContext...");
		context = memnew(DLSSContext());
		ERR_PRINT("DLSS: DLSSContext created");
	}

	// Store Vulkan handles and function pointers
	ERR_PRINT("DLSS: Storing Vulkan handles...");
	context->vk_instance = vk_instance;
	context->vk_physical_device = vk_physical_device;
	context->vk_device = vk_device;
	context->vk_queue = vk_queue;
	context->vk_queue_family_index = queue_family_index;
	// Copy the global function pointers to this context
	context->vk_functions = *global_dlss_vulkan_functions;

	// Store capability parameters - use this for all NGX feature operations
	// Do NOT allocate new parameters with NVSDK_NGX_VULKAN_AllocateParameters
	context->capability_params = capability_params;
	ERR_PRINT("DLSS: Capability parameters stored");

	ERR_PRINT("DLSS: Vulkan handles and functions stored");

	ngx_initialized = true;
	return true;
}

void DLSSUpscaler::shutdown_ngx() {
	if (!ngx_initialized) {
		return;
	}

	release_dlss_feature();

	if (context && context->vk_device != VK_NULL_HANDLE) {
		NVSDK_NGX_VULKAN_Shutdown1(context->vk_device);
	}

	ngx_initialized = false;
}

bool DLSSUpscaler::is_available() const {
	if (!context) {
		return false;
	}
	return context->is_available() && context->is_initialized();
}

void DLSSUpscaler::set_use_hdr(bool p_hdr) {
	if (use_hdr == p_hdr) {
		return;
	}
	use_hdr = p_hdr;
	needs_reinit = true;
}

void DLSSUpscaler::set_use_auto_exposure(bool p_use_auto_exposure) {
	if (use_auto_exposure == p_use_auto_exposure) {
		return;
	}
	use_auto_exposure = p_use_auto_exposure;
}

void DLSSUpscaler::ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (!p_render_buffers.is_valid()) {
		return;
	}

	if (!ngx_initialized) {
		if (!initialize_ngx()) {
			return;
		}
	}

	Size2i internal_size = p_render_buffers->get_internal_size();
	Size2i target_size = p_render_buffers->get_target_size();

	if (context == nullptr) {
		context = memnew(DLSSContext());
		needs_reinit = true;
	}

	if (needs_reinit || !context->is_initialized()) {
		ERR_PRINT("DLSS: Reinitializing context/feature");
		const String device_hex = String::num_uint64((uint64_t)context->vk_device, 16);
		const String queue_hex = String::num_uint64((uint64_t)context->vk_queue, 16);
		ERR_PRINT(vformat("DLSS: Context Vulkan handles (device=0x%s queue=0x%s qfi=%d)",
				device_hex,
				queue_hex,
				(int)context->vk_queue_family_index));
		release_dlss_feature();

		// Auto-select quality based on resolution ratio
		float ratio = (float)internal_size.width / (float)target_size.width;
		DLSSQuality auto_quality = DLSSQuality::PERFORMANCE; // Default fallback

		// Ratios: DLAA (1.0), UltraQuality (0.77), Quality (0.67), Balanced (0.58), Performance (0.50), UltraPerformance (0.33)
		if (ratio >= 0.99f) auto_quality = DLSSQuality::DLAA;
		else if (ratio >= 0.72f) auto_quality = DLSSQuality::ULTRA_QUALITY;
		else if (ratio >= 0.62f) auto_quality = DLSSQuality::QUALITY;
		else if (ratio >= 0.54f) auto_quality = DLSSQuality::BALANCED;
		else if (ratio >= 0.41f) auto_quality = DLSSQuality::PERFORMANCE;
		else auto_quality = DLSSQuality::ULTRA_PERFORMANCE;

		quality = auto_quality;
		
		// CRITICAL: Query DLSS SDK for optimal render resolution using NGX_DLSS_GET_OPTIMAL_SETTINGS
		// This tells us what DLSS would prefer, but we must use Godot's actual internal size
		// The feature creation size MUST match the actual render resolution
		unsigned int optimal_width = 0;
		unsigned int optimal_height = 0;
		unsigned int max_width = 0;
		unsigned int max_height = 0;
		unsigned int min_width = 0;
		unsigned int min_height = 0;
		float sharpness = 0.0f;

		ERR_FAIL_NULL_MSG(context->capability_params, "DLSS: capability_params not initialized");

		NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
			context->capability_params,
			target_size.width,
			target_size.height,
			(NVSDK_NGX_PerfQuality_Value)quality,
			&optimal_width,
			&optimal_height,
			&max_width,
			&max_height,
			&min_width,
			&min_height,
			&sharpness);

		if (result != NVSDK_NGX_Result_Success) {
			ERR_PRINT(vformat("DLSS: NGX_DLSS_GET_OPTIMAL_SETTINGS failed with error code: %d", (int)result));
		} else {
			recommended_sharpness = CLAMP(sharpness, 0.0f, 1.0f);

			// Log the optimal size for comparison
			// Note: We create the feature with Godot's internal_size (not optimal) to match the actual texture
			WARN_PRINT(vformat("DLSS: Optimal render size for quality %d: %dx%d (output=%dx%d, Godot's internal=%dx%d, sharpness=%.2f) - Using Godot's size to match actual texture",
				(int)quality, optimal_width, optimal_height, target_size.width, target_size.height,
				internal_size.width, internal_size.height, sharpness));

			// Check if Godot's size is within acceptable range (min/max from DLSS)
			if ((unsigned int)internal_size.width < min_width || (unsigned int)internal_size.width > max_width ||
				(unsigned int)internal_size.height < min_height || (unsigned int)internal_size.height > max_height) {
				ERR_PRINT(vformat("DLSS: WARNING - Godot's internal size (%dx%d) is outside DLSS's recommended range (%dx%d to %dx%d). Quality may be reduced.",
					internal_size.width, internal_size.height, min_width, min_height, max_width, max_height));
			}
		}

		DLSSContext::CreateParams params;
		// CRITICAL: Use Godot's actual internal size, not DLSS's optimal size
		// The feature creation size must match the actual texture size
		params.render_size = internal_size;
		params.output_size = target_size;
		params.quality = quality;
		params.hdr = use_hdr;
		params.auto_exposure = use_auto_exposure;

		if (!context->initialize(params)) {
			ERR_PRINT("DLSS: Context initialize failed");
			return;
		}

		if (!create_dlss_feature(internal_size, target_size)) {
			ERR_PRINT("DLSS: Feature creation failed");
			return;
		}

		needs_reinit = false;
	}
}

void DLSSUpscaler::process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_source_rd_texture, RID p_destination_texture) {
	ERR_FAIL_NULL(p_render_buffers);
	ERR_FAIL_NULL(context);

	// Debug: Check if process() is being called
	static int frame_count = 0;
	if (frame_count % 60 == 0) { // Print every 60 frames to avoid spam
		print_line(vformat("DLSS: process() called - Frame %d", frame_count));
	}
	frame_count++;

	// Get sizes
	Size2i internal_size = p_render_buffers->get_internal_size();
	Size2i target_size = p_render_buffers->get_target_size();

	// Important: DLSS only makes sense when upscaling (internal < target)
	if (internal_size == target_size) {
		if (frame_count == 60) { // Only warn once
			WARN_PRINT_ONCE("DLSS: Internal size equals target size - nothing to upscale!");
			print_line(vformat("DLSS: internal=%dx%d, target=%dx%d",
					internal_size.width, internal_size.height,
					target_size.width, target_size.height));
		}
		return; // Skip DLSS when not upscaling
	}

	// Phase 1: Hook up velocity and depth textures from Godot's render buffers
	// Ensure velocity buffer exists (it's only created when TAA or other features need it)
	p_render_buffers->ensure_velocity();

	// Get velocity texture
	RID velocity_texture = p_render_buffers->get_velocity_buffer(false);

	// Get depth texture
	RID depth_texture = p_render_buffers->get_depth_texture();

	// Debug: Check if textures are valid
	print_line(vformat("DLSS: Phase 1 - Texture Status"));
	print_line(vformat("  Velocity: %s", velocity_texture.is_valid() ? "VALID" : "INVALID"));
	print_line(vformat("  Depth: %s", depth_texture.is_valid() ? "VALID" : "INVALID"));
	print_line(vformat("  Internal size: %dx%d", internal_size.width, internal_size.height));
	print_line(vformat("  Target size: %dx%d", target_size.width, target_size.height));

	// Build parameters for DLSS evaluation
	Parameters params;
	params.internal_texture = p_source_rd_texture;
	params.output_texture = p_destination_texture;
	params.internal_size = internal_size;
	params.output_size = target_size;
	params.depth_texture = depth_texture;
	params.velocity_texture = velocity_texture;
	params.exposure_texture = RID(); // Optional - not used for now
	params.jitter = Vector2(0.0f, 0.0f); // TODO: Phase 2 - implement jitter
	params.delta_time = 0.0f; // Not used currently
	params.sharpness = 0.0f; // Use default sharpness
	params.z_near = 0.0f; // TODO: Get from camera
	params.z_far = 0.0f; // TODO: Get from camera
	params.fovy = 75.0f; // TODO: Get from camera
	params.reprojection = Projection(); // TODO: Get from camera
	params.reset_accumulation = false; // Reset on scene changes
	params.use_auto_exposure = false;
	params.pre_exposure = 1.0f;
	params.exposure_scale = 1.0f;

	// Call the upscale function which will call evaluate_dlss
	upscale(params);
}

void DLSSUpscaler::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(context);

	// Check if resolution has changed
	// CRITICAL: DLSS supports dynamic input resolution internally via InRenderSubrectDimensions
	// DO NOT recreate on input resolution changes - this destroys temporal history!
	// Only recreate if the OUTPUT size changes (e.g. window resize, display mode change)
	bool recreate = false;

	if (context->output_size != p_params.output_size) {
		print_line(vformat("DLSS: Output resolution changed from %dx%d to %dx%d, recreating feature",
				context->output_size.width, context->output_size.height,
				p_params.output_size.width, p_params.output_size.height));
		recreate = true;
	}
	// NOTE: We intentionally DO NOT check input resolution changes!
	// DLSS handles dynamic resolution internally - recreating breaks temporal accumulation

	if (recreate) {
		// Release old feature
		if (context->get_feature_id() != 0) {
			release_dlss_feature();
		}

		// CRITICAL: Create new feature with Godot's actual internal size
		// The feature creation size must match the actual texture size
		if (!create_dlss_feature(p_params.internal_size, p_params.output_size)) {
			ERR_PRINT("DLSS: Failed to recreate feature after resolution change");
			return;
		}
	}

	ERR_FAIL_COND_MSG(context->get_feature_id() == 0, "DLSS: Feature not initialized");
	ERR_FAIL_COND_MSG(!p_params.internal_texture.is_valid(), "DLSS: Invalid internal texture");
	ERR_FAIL_COND_MSG(!p_params.output_texture.is_valid(), "DLSS: Invalid output texture");

	evaluate_dlss(p_params);
}

bool DLSSUpscaler::create_dlss_feature(const Size2i &p_render_size, const Size2i &p_output_size) {
	ERR_FAIL_NULL_V(context, false);
	ERR_FAIL_COND_V_MSG(!ngx_initialized, false, "NGX not initialized");

	ERR_PRINT(vformat("DLSS: Creating feature (render=%dx%d output=%dx%d quality=%d)",
			p_render_size.width, p_render_size.height, p_output_size.width, p_output_size.height, (int)quality));

	ERR_FAIL_NULL_V(context->capability_params, false);

	// Create a transient command pool and buffer for feature creation
	// This follows NVIDIA's reference pattern: create, submit, destroy
	VkCommandPool transient_pool = VK_NULL_HANDLE;
	VkCommandBuffer transient_cmd = VK_NULL_HANDLE;

	VkCommandPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = context->vk_queue_family_index;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	VkResult vk_result = context->vk_functions.CreateCommandPool(context->vk_device, &pool_info, nullptr, &transient_pool);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to create transient command pool with error code: %d", (int)vk_result));
		return false;
	}

	VkCommandBufferAllocateInfo buffer_info = {};
	buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	buffer_info.commandPool = transient_pool;
	buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	buffer_info.commandBufferCount = 1;

	vk_result = context->vk_functions.AllocateCommandBuffers(context->vk_device, &buffer_info, &transient_cmd);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to allocate transient command buffer with error code: %d", (int)vk_result));
		context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);
		return false;
	}

	// Begin the command buffer
	VkCommandBufferBeginInfo begin_info = {};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vk_result = context->vk_functions.BeginCommandBuffer(transient_cmd, &begin_info);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to begin transient command buffer with error code: %d", (int)vk_result));
		context->vk_functions.FreeCommandBuffers(context->vk_device, transient_pool, 1, &transient_cmd);
		context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);
		return false;
	}

	// Use capability parameters for feature creation
	// Following NVIDIA's pattern: reuse capability params, don't allocate new ones
	NVSDK_NGX_Parameter *params = context->capability_params;

	// CRITICAL FIX: Width/Height should be OUTPUT size, not render size!
	// NVIDIA guide: "The feature is created with the output (display) resolution"
	// We were incorrectly using render_size here, causing DLSS to expect wrong dimensions
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Width, p_output_size.width);
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Height, p_output_size.height);
	// OutWidth/OutHeight are the same (output size)
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_OutWidth, p_output_size.width);
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_OutHeight, p_output_size.height);
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_PerfQualityValue, (int)quality);

	// Set feature creation flags - critical for correct DLSS operation
	// From NVIDIA guide: these flags must match the actual rendering setup
	int create_flags = NVSDK_NGX_DLSS_Feature_Flags_None;

	// Motion vectors are at render resolution (not display/output resolution)
	create_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

	// MVJittered flag DISABLED - Godot's motion vectors do NOT include jitter
	// The shader explicitly subtracts jitter when calculating motion vectors (scene_forward_clustered.glsl:2981-2987):
	//   vec2 position_clip = (screen_position.xy / screen_position.w) - scene_data.taa_jitter;
	//   vec2 prev_position_clip = (prev_screen_position.xy / prev_screen_position.w) - scene_data_block.prev_data.taa_jitter;
	// Therefore motion vectors represent true object motion without jitter component
	// create_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;  // DISABLED

	// Depth buffer is inverted (1 = near, 0 = far) as is standard in Vulkan
	create_flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

	// Godot's internal color buffer is linear HDR, so set HDR mode unless explicitly disabled.
	if (use_hdr) {
		create_flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	}

	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, create_flags);

	// Enable output subrects - allows DLSS to render to a sub-region of the output texture
	// This is important for dynamic resolution support
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);

	// Create DLSS feature using the transient command buffer
	// The NGX SDK will manage command buffer recording and submission internally
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_CreateFeature1(
			context->vk_device,
			transient_cmd,  // Use transient command buffer, not stored one
			NVSDK_NGX_Feature_SuperSampling,
			params,
			&context->feature_handle);

	if (result != NVSDK_NGX_Result_Success) {
		ERR_PRINT(vformat("DLSS: Failed to create feature with error code: %d (render=%dx%d output=%dx%d quality=%d)",
				(int)result,
				p_render_size.width,
				p_render_size.height,
				p_output_size.width,
				p_output_size.height,
				(int)quality));
		context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);
		return false;
	}

	// End and submit the transient command buffer to finalize feature creation.
	ERR_FAIL_COND_V_MSG(!context->vk_functions.EndCommandBuffer, false, "DLSS: vkEndCommandBuffer function not loaded");
	vk_result = context->vk_functions.EndCommandBuffer(transient_cmd);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to end transient command buffer with error code: %d", (int)vk_result));
		NVSDK_NGX_VULKAN_ReleaseFeature(context->get_feature_handle());
		context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);
		return false;
	}

	VkSubmitInfo submit_info = {};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &transient_cmd;

	ERR_FAIL_COND_V_MSG(!context->vk_functions.QueueSubmit, false, "DLSS: vkQueueSubmit function not loaded");
	vk_result = context->vk_functions.QueueSubmit(context->vk_queue, 1, &submit_info, VK_NULL_HANDLE);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to submit transient command buffer with error code: %d", (int)vk_result));
		NVSDK_NGX_VULKAN_ReleaseFeature(context->get_feature_handle());
		context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);
		return false;
	}

	ERR_FAIL_COND_V_MSG(!context->vk_functions.QueueWaitIdle, false, "DLSS: vkQueueWaitIdle function not loaded");
	vk_result = context->vk_functions.QueueWaitIdle(context->vk_queue);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to wait for transient queue idle with error code: %d", (int)vk_result));
		NVSDK_NGX_VULKAN_ReleaseFeature(context->get_feature_handle());
		context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);
		return false;
	}

	ERR_PRINT(vformat("DLSS: Feature created (id=%d)", (int)context->get_feature_id()));

	// Store the render and output sizes for later comparison
	context->render_size = p_render_size;
	context->output_size = p_output_size;

	// Clean up transient command pool and buffer
	// The SDK has finished using them, so we can safely destroy
	context->vk_functions.DestroyCommandPool(context->vk_device, transient_pool, nullptr);

	// Note: The stored command_buffer is ONLY used for evaluation, not creation
	// Creation uses a transient buffer that is submitted and destroyed

	return true;
}

void DLSSUpscaler::release_dlss_feature() {
	if (context && context->get_feature_id() != 0) {
		NVSDK_NGX_VULKAN_ReleaseFeature(context->get_feature_handle());
		if (context->feature_handle) {
			context->feature_handle->Id = 0;
		}
	}

	// Note: capability_params are owned by NGX SDK, don't destroy them here
	// They will be cleaned up during NGX shutdown
}

void DLSSUpscaler::evaluate_dlss(const Parameters &p_params) {
	ERR_FAIL_NULL(context);
	ERR_FAIL_COND_MSG(context->get_feature_id() == 0, "DLSS: Feature not initialized");
	ERR_FAIL_COND_MSG(!p_params.internal_texture.is_valid(), "DLSS: Invalid internal texture");
	ERR_FAIL_COND_MSG(!p_params.output_texture.is_valid(), "DLSS: Invalid output texture");
	ERR_FAIL_COND_MSG(context->command_buffer == VK_NULL_HANDLE, "DLSS: Command buffer not allocated");

	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(rd);

	// Get VkImage and VkImageView handles for textures
	uint64_t color_image_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.internal_texture);
	uint64_t color_view_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.internal_texture);

	uint64_t output_image_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.output_texture);
	uint64_t output_view_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.output_texture);

	VkImage color_image = (VkImage)color_image_vk;
	VkImageView color_image_view = (VkImageView)color_view_vk;
	VkImage output_image = (VkImage)output_image_vk;
	VkImageView output_image_view = (VkImageView)output_view_vk;

	ERR_FAIL_COND_MSG(color_image == VK_NULL_HANDLE, "DLSS: Invalid color texture image");
	ERR_FAIL_COND_MSG(color_image_view == VK_NULL_HANDLE, "DLSS: Invalid color texture view");
	ERR_FAIL_COND_MSG(output_image == VK_NULL_HANDLE, "DLSS: Invalid output texture image");
	ERR_FAIL_COND_MSG(output_image_view == VK_NULL_HANDLE, "DLSS: Invalid output texture view");

	// Get texture format
	uint64_t format_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.internal_texture);
	VkFormat format = (VkFormat)format_vk;

	// Create resource wrappers for NGX
	VkImageSubresourceRange subresource_range = {};
	subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource_range.baseMipLevel = 0;
	subresource_range.levelCount = 1;
	subresource_range.baseArrayLayer = 0;
	subresource_range.layerCount = 1;

	NVSDK_NGX_Resource_VK color_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
			color_image_view, color_image, subresource_range, format,
			p_params.internal_size.width, p_params.internal_size.height, false);

	NVSDK_NGX_Resource_VK output_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
			output_image_view, output_image, subresource_range, format,
			p_params.output_size.width, p_params.output_size.height, true);

	// Get optional textures
	NVSDK_NGX_Resource_VK depth_resource = {0};
	NVSDK_NGX_Resource_VK motion_vectors_resource = {0};
	NVSDK_NGX_Resource_VK exposure_resource = {0};

	// Debug: Check what textures we're receiving
	static int debug_frame = 0;
	static uint64_t last_feature_id = 0;
	static uint64_t last_real_time = 0;
	uint64_t current_feature_id = context->get_feature_id();
	bool feature_recreated = (last_feature_id != 0 && current_feature_id != last_feature_id);
	last_feature_id = current_feature_id;

	debug_frame++;

	// Check actual time between calls to detect if we're being called every frame or only when scene changes
	uint64_t current_real_time = OS::get_singleton()->get_ticks_msec();
	if (last_real_time != 0) {
		uint64_t time_delta = current_real_time - last_real_time;
		if (debug_frame <= 10 || debug_frame % 60 == 0 || feature_recreated) {
			print_line(vformat("DLSS: evaluate_dlss() - Frame %d (feature_id=%d%s, time_since_last=%dms)",
					debug_frame, current_feature_id, feature_recreated ? " RECREATED!" : "", time_delta));
		}
	}
	last_real_time = current_real_time;

	// Print every frame to diagnose temporal issues
	if (debug_frame <= 10 || debug_frame % 60 == 0 || feature_recreated) { // Print first 10 frames, then every 60, or if feature was recreated
		print_line(vformat("DLSS: evaluate_dlss() - Frame %d (feature_id=%d%s)", debug_frame, current_feature_id, feature_recreated ? " RECREATED!" : ""));
		print_line(vformat("  depth valid: %s, velocity valid: %s",
				p_params.depth_texture.is_valid() ? "YES" : "NO",
				p_params.velocity_texture.is_valid() ? "YES" : "NO"));
		print_line(vformat("  jitter: (%.4f, %.4f)", p_params.jitter.x, p_params.jitter.y));
		print_line(vformat("  render_size: %dx%d, output_size: %dx%d",
				p_params.internal_size.width, p_params.internal_size.height,
				p_params.output_size.width, p_params.output_size.height));
		print_line(vformat("  input_tex=%d, output_tex=%d, depth_tex=%d, velocity_tex=%d",
				p_params.internal_texture.get_id(),
				p_params.output_texture.get_id(),
				p_params.depth_texture.get_id(),
				p_params.velocity_texture.get_id()));

		// CRITICAL: Check if TAA is actually enabled
		// DLSS needs TAA to be active to generate proper jitter
		// If TAA is disabled internally, jitter might not be generated
		static bool taa_checked = false;
		if (!taa_checked && debug_frame >= 1) {
			print_line("DLSS: WARNING - Make sure TAA is enabled in the viewport for DLSS to work properly!");
			print_line("DLSS: When TAA is disabled, Godot may not generate jitter values.");
			taa_checked = true;
		}

		// Also check if evaluation succeeds
		if (debug_frame == 1) {
			print_line(vformat("  internal_size: %dx%d, output_size: %dx%d",
					p_params.internal_size.width, p_params.internal_size.height,
					p_params.output_size.width, p_params.output_size.height));
		}
	}

	if (p_params.depth_texture.is_valid()) {
		uint64_t depth_image_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.depth_texture);
		uint64_t depth_view_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.depth_texture);
		VkImage depth_image = (VkImage)depth_image_vk;
		VkImageView depth_image_view = (VkImageView)depth_view_vk;
		ERR_FAIL_COND_MSG(depth_image == VK_NULL_HANDLE, "DLSS: Invalid depth texture image");
		ERR_FAIL_COND_MSG(depth_image_view == VK_NULL_HANDLE, "DLSS: Invalid depth texture view");

		uint64_t depth_format_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.depth_texture);
		VkFormat depth_format = (VkFormat)depth_format_vk;

		VkImageSubresourceRange depth_range = {};
		depth_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depth_range.baseMipLevel = 0;
		depth_range.levelCount = 1;
		depth_range.baseArrayLayer = 0;
		depth_range.layerCount = 1;

		depth_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
				depth_image_view, depth_image, depth_range, depth_format,
				p_params.internal_size.width, p_params.internal_size.height, false);
	}

	if (p_params.velocity_texture.is_valid()) {
		uint64_t velocity_image_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.velocity_texture);
		uint64_t velocity_view_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.velocity_texture);
		VkImage velocity_image = (VkImage)velocity_image_vk;
		VkImageView velocity_image_view = (VkImageView)velocity_view_vk;
		ERR_FAIL_COND_MSG(velocity_image == VK_NULL_HANDLE, "DLSS: Invalid velocity texture image");
		ERR_FAIL_COND_MSG(velocity_image_view == VK_NULL_HANDLE, "DLSS: Invalid velocity texture view");

		uint64_t velocity_format_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.velocity_texture);
		VkFormat velocity_format = (VkFormat)velocity_format_vk;

		VkImageSubresourceRange velocity_range = {};
		velocity_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		velocity_range.baseMipLevel = 0;
		velocity_range.levelCount = 1;
		velocity_range.baseArrayLayer = 0;
		velocity_range.layerCount = 1;

		motion_vectors_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
				velocity_image_view, velocity_image, velocity_range, velocity_format,
				p_params.internal_size.width, p_params.internal_size.height, false);
	}

	if (p_params.exposure_texture.is_valid()) {
		RD::TextureFormat exposure_tf = rd->texture_get_format(p_params.exposure_texture);
		Size2i exposure_size(exposure_tf.width, exposure_tf.height);

		uint64_t exposure_image_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.exposure_texture);
		uint64_t exposure_view_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.exposure_texture);
		VkImage exposure_image = (VkImage)exposure_image_vk;
		VkImageView exposure_image_view = (VkImageView)exposure_view_vk;
		ERR_FAIL_COND_MSG(exposure_image == VK_NULL_HANDLE, "DLSS: Invalid exposure texture image");
		ERR_FAIL_COND_MSG(exposure_image_view == VK_NULL_HANDLE, "DLSS: Invalid exposure texture view");

		uint64_t exposure_format_vk = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.exposure_texture);
		VkFormat exposure_format = (VkFormat)exposure_format_vk;

		VkImageSubresourceRange exposure_range = {};
		exposure_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		exposure_range.baseMipLevel = 0;
		exposure_range.levelCount = 1;
		exposure_range.baseArrayLayer = 0;
		exposure_range.layerCount = 1;

		exposure_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
				exposure_image_view, exposure_image, exposure_range, exposure_format,
				exposure_size.width, exposure_size.height, false);
	}

	// Begin command buffer recording
	VkCommandBufferBeginInfo begin_info = {};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	ERR_FAIL_COND_MSG(!context->vk_functions.BeginCommandBuffer, "DLSS: vkBeginCommandBuffer function not loaded");
	VkResult vk_result = context->vk_functions.BeginCommandBuffer(context->command_buffer, &begin_info);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to begin command buffer with error code: %d", (int)vk_result));
		return;
	}

	// Set up DLSS evaluation parameters
	NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
	eval_params.Feature.pInColor = &color_resource;
	eval_params.Feature.pInOutput = &output_resource;
	eval_params.Feature.InSharpness = p_params.sharpness;

	// CRITICAL FIX: Type == 0 is valid for NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW!
	// We need to check if the texture was provided, not if Type != 0
	eval_params.pInDepth = p_params.depth_texture.is_valid() ? &depth_resource : nullptr;
	eval_params.pInMotionVectors = p_params.velocity_texture.is_valid() ? &motion_vectors_resource : nullptr;
	eval_params.pInExposureTexture = p_params.exposure_texture.is_valid() ? &exposure_resource : nullptr;

	// Jitter offset - use directly (same as FSR2)
	// Godot's TAA jitter is already in the correct space for DLSS

	// COMPREHENSIVE DEBUG OUTPUT - diagnose what DLSS is receiving
	static int jitter_debug_frame = 0;
	if (jitter_debug_frame < 60) {  // Log first 60 frames (2+ cycles at 30fps)
		print_line(vformat("=== DLSS Frame %d ===", jitter_debug_frame));
		print_line(vformat("  jitter: (%.6f, %.6f)", p_params.jitter.x, p_params.jitter.y));
		print_line(vformat("  internal: %dx%d", p_params.internal_size.width, p_params.internal_size.height));
		print_line(vformat("  output: %dx%d", p_params.output_size.width, p_params.output_size.height));
		print_line(vformat("  upscale_ratio: %.2fx, %.2fy",
			float(p_params.output_size.width) / float(p_params.internal_size.width),
			float(p_params.output_size.height) / float(p_params.internal_size.height)));
		print_line(vformat("  reset_accumulation: %s", p_params.reset_accumulation ? "YES" : "NO"));
		jitter_debug_frame++;
	}

	eval_params.InJitterOffsetX = p_params.jitter.x;
	eval_params.InJitterOffsetY = p_params.jitter.y;

	eval_params.InReset = p_params.reset_accumulation ? 1 : 0;
	eval_params.InPreExposure = p_params.pre_exposure;
	eval_params.InExposureScale = p_params.exposure_scale;
	eval_params.InFrameTimeDeltaInMsec = p_params.delta_time * 1000.0f;

	// CRITICAL: Motion vector scale - convert from Godot's format to pixel space
	// Godot's motion vectors are likely in normalized space and need to be scaled
	// Following FSR2's approach: scale by internal resolution
	eval_params.InMVScaleX = float(p_params.internal_size.width);
	eval_params.InMVScaleY = float(p_params.internal_size.height);

	// CRITICAL: Set the render subrect dimensions
	// This tells DLSS what region of the texture we're rendering to
	// The subrect must match the creation resolution for DLSS to work properly
	eval_params.InRenderSubrectDimensions.Width = p_params.internal_size.width;
	eval_params.InRenderSubrectDimensions.Height = p_params.internal_size.height;

	// Debug: Check if resources are valid
	static int check_frame = 0;
	if (check_frame == 0) {
		print_line(vformat("DLSS: Resource check"));
		print_line(vformat("  depth_resource.Type = %d", depth_resource.Type));
		print_line(vformat("  motion_vectors_resource.Type = %d", motion_vectors_resource.Type));
		print_line(vformat("  pInDepth valid: %s", eval_params.pInDepth ? "YES" : "NO"));
		print_line(vformat("  pInMotionVectors valid: %s", eval_params.pInMotionVectors ? "YES" : "NO"));
		check_frame = 1;
	}

	// DLSS requires both depth and motion vectors to work
	// Skip evaluation if these required inputs are not available
	if (!eval_params.pInDepth || !eval_params.pInMotionVectors) {
		// Silently skip DLSS evaluation - this is expected when renderer doesn't provide required inputs
		static int skip_warn = 0;
		if (skip_warn < 3) {
			ERR_PRINT(vformat("DLSS: Skipping evaluation - missing required inputs (depth valid: %s, motion valid: %s)",
					eval_params.pInDepth ? "YES" : "NO",
					eval_params.pInMotionVectors ? "YES" : "NO"));
			skip_warn++;
		}
		context->vk_functions.EndCommandBuffer(context->command_buffer);
		return;
	}

	// Evaluate DLSS
	// Use capability params (not nullptr!) for the evaluation
	NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(context->command_buffer, context->get_feature_handle(), context->capability_params, &eval_params);

	// Debug: Check evaluation result
	static int eval_frame = 0;
	if (eval_frame < 5) {
		if (result == NVSDK_NGX_Result_Success) {
			print_line(vformat("DLSS: Evaluation SUCCESS - Frame %d", eval_frame));
		} else {
			ERR_PRINT(vformat("DLSS: Evaluation FAILED - Frame %d, error code: %d", eval_frame, (int)result));
		}
	}
	eval_frame++;

	if (result != NVSDK_NGX_Result_Success) {
		ERR_PRINT(vformat("DLSS: Evaluation failed with error code: %d (render=%dx%d output=%dx%d format=%d feature_id=%d)",
				(int)result,
				p_params.internal_size.width,
				p_params.internal_size.height,
				p_params.output_size.width,
				p_params.output_size.height,
				(int)format,
				(int)context->get_feature_id()));
		context->vk_functions.EndCommandBuffer(context->command_buffer);
		return;
	}

	// End command buffer recording
	ERR_FAIL_COND_MSG(!context->vk_functions.EndCommandBuffer, "DLSS: vkEndCommandBuffer function not loaded");
	vk_result = context->vk_functions.EndCommandBuffer(context->command_buffer);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to end command buffer with error code: %d", (int)vk_result));
		return;
	}

	// Submit command buffer
	VkSubmitInfo submit_info = {};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &context->command_buffer;

	ERR_FAIL_COND_MSG(!context->vk_functions.QueueSubmit, "DLSS: vkQueueSubmit function not loaded");
	vk_result = context->vk_functions.QueueSubmit(context->vk_queue, 1, &submit_info, VK_NULL_HANDLE);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to submit command buffer with error code: %d", (int)vk_result));
		return;
	}

	// Wait for completion
	ERR_FAIL_COND_MSG(!context->vk_functions.QueueWaitIdle, "DLSS: vkQueueWaitIdle function not loaded");
	vk_result = context->vk_functions.QueueWaitIdle(context->vk_queue);
	if (vk_result != VK_SUCCESS) {
		ERR_PRINT(vformat("DLSS: Failed to wait for queue idle with error code: %d", (int)vk_result));
	}
}
