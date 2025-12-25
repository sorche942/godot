/**************************************************************************/
/*  dlss.h                                                                */
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
/* IN ANY ACT OR OTHERWISE ARISING FROM, FROM, OUT OF OR IN CONNECTION    */
/* WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.         */
/**************************************************************************/

#pragma once

#include "drivers/vulkan/godot_vulkan.h"
#include "servers/rendering/renderer_rd/effects/spatial_upscaler.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

// NGX SDK includes - must come after Vulkan headers
#include "DLSS/include/nvsdk_ngx_vk.h"
#include "DLSS/include/nvsdk_ngx_params.h"
#include "DLSS/include/nvsdk_ngx_helpers_vk.h"
#include "DLSS/include/nvsdk_ngx_helpers.h"  // For NGX_DLSS_GET_OPTIMAL_SETTINGS

namespace RendererRD {

class DLSSContext;

// Vulkan instance and device function table for DLSS - loaded via dlopen to avoid volk conflicts
struct DLSSVulkanFunctions {
	// Instance functions
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

	// Device functions
	PFN_vkCreateCommandPool CreateCommandPool = nullptr;
	PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
	PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
	PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
	PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
	PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
	PFN_vkQueueSubmit QueueSubmit = nullptr;
	PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;

	bool is_valid() const {
		return GetInstanceProcAddr && GetDeviceProcAddr &&
			   CreateCommandPool && DestroyCommandPool &&
			   AllocateCommandBuffers && FreeCommandBuffers &&
			   ResetCommandBuffer &&
			   BeginCommandBuffer && EndCommandBuffer &&
			   QueueSubmit && QueueWaitIdle;
	}

	void cleanup();
};

class TLS {
public:
	static Vector<Vector2> get_halton_sequence(int p_count);
};

class TLSJitterPattern {
public:
	static Vector<Vector2> get_jittered_offset(int p_frame, int p_jitter_phase_count);
};

// DLSS quality preset (corresponds to PerfQualityValue)
enum class DLSSQuality {
	DLAA = NVSDK_NGX_PerfQuality_Value_DLAA,
	ULTRA_QUALITY = NVSDK_NGX_PerfQuality_Value_UltraQuality,
	QUALITY = NVSDK_NGX_PerfQuality_Value_MaxQuality,
	BALANCED = NVSDK_NGX_PerfQuality_Value_Balanced,
	PERFORMANCE = NVSDK_NGX_PerfQuality_Value_MaxPerf,
	ULTRA_PERFORMANCE = NVSDK_NGX_PerfQuality_Value_UltraPerformance,
};

class DLSSContext {
public:
	struct CreateParams {
		Size2i render_size;
		Size2i output_size;
		DLSSQuality quality = DLSSQuality::QUALITY;
		bool hdr = false;
		bool motion_vectors_jittered = true;
		bool depth_inverted = true;
		bool auto_exposure = false;
	};

	DLSSContext();
	~DLSSContext();

	bool is_available() const { return available; }
	bool is_initialized() const { return initialized; }
	NVSDK_NGX_Handle *get_feature_handle() { return feature_handle; }
	uint64_t get_feature_id() const { return feature_handle ? feature_handle->Id : 0; }

	// Initialize the DLSS context
	bool initialize(const CreateParams &p_params);

	// Shutdown and cleanup
	void shutdown();

	// Get the required render size for a given output size and quality
	static Size2i get_render_size(const Size2i &p_output_size, DLSSQuality p_quality);

	// Get feature info
	static bool is_supported();

	// Command buffer management for DLSS
	bool create_command_pool();
	bool allocate_command_buffer();
	void free_command_buffer();
	VkCommandBuffer get_command_buffer() const { return command_buffer; }

	// Load Vulkan functions directly from libvulkan.so using dlopen/dlsym
	// This bypasses volk to avoid conflicts with the DLSS SDK
	static bool load_vulkan_functions(DLSSVulkanFunctions &r_functions);
	static void cleanup_vulkan_functions();

	bool available = false;
	bool initialized = false;

	NVSDK_NGX_Handle* feature_handle = nullptr;
	Size2i render_size;
	Size2i output_size;
	DLSSQuality quality;

	// Capability parameters - obtained from NGX during init, used for all feature operations
	// Following NVIDIA's pattern: get capability params once, reuse for all operations
	NVSDK_NGX_Parameter *capability_params = nullptr;

	// Scratch buffer for DLSS
	RID scratch_buffer;
	uint32_t scratch_buffer_size = 0;

	// Vulkan objects for NGX
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
	VkDevice vk_device = VK_NULL_HANDLE;
	uint32_t vk_queue_family_index = 0;
	VkQueue vk_queue = VK_NULL_HANDLE;

	// Command buffer for DLSS operations
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;

	// Vulkan function pointers - loaded via dlopen to bypass volk
	DLSSVulkanFunctions vk_functions;
};

// External declaration for global Vulkan functions (defined in dlss.cpp)
extern thread_local DLSSVulkanFunctions *global_dlss_vulkan_functions;

class DLSSUpscaler : public SpatialUpscaler {
public:
	struct Parameters {
		RID internal_texture;  // Source texture (lower resolution)
		RID depth_texture;     // Depth buffer
		RID velocity_texture;  // Motion vectors
		RID exposure_texture;  // Optional exposure texture
		RID output_texture;    // Destination texture (upscaled)
		Size2i internal_size;
		Size2i output_size;
		Vector2 jitter;
		float delta_time;
		float sharpness;
		float z_near;
		float z_far;
		float fovy;
		Projection reprojection;
		bool reset_accumulation;
		bool use_auto_exposure = false;
		float pre_exposure = 1.0f;
		float exposure_scale = 1.0f;
	};

	DLSSUpscaler();
	~DLSSUpscaler() override;

	virtual const Span<char> get_label() const override {
		return Span<char>(const_cast<char *>("DLSS"), 4);
	}

	virtual void ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) override;
	virtual void process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_source_rd_texture, RID p_destination_texture) override;

	// Main upscale function (similar to FSR2)
	void upscale(const Parameters &p_params);

	bool is_available() const;
	void set_quality(DLSSQuality p_quality) { quality = p_quality; needs_reinit = true; }
	DLSSQuality get_quality() const { return quality; }
	float get_recommended_sharpness() const { return recommended_sharpness; }
	void set_use_hdr(bool p_hdr);
	void set_use_auto_exposure(bool p_use_auto_exposure);

private:
	bool initialize_ngx();
	void shutdown_ngx();

	bool create_dlss_feature(const Size2i &p_render_size, const Size2i &p_output_size);
	void release_dlss_feature();

	void evaluate_dlss(const Parameters &p_params);

	DLSSContext *context = nullptr;
	DLSSQuality quality = DLSSQuality::QUALITY;
	bool needs_reinit = true;
	bool ngx_initialized = false;
	bool use_hdr = true;
	bool use_auto_exposure = false;
	float recommended_sharpness = 0.0f;

	// NGX application data path
	wchar_t app_data_path[256];
};

} // namespace RendererRD
