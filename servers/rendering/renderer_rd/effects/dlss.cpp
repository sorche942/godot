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
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef DLSS_ENABLED

#include "dlss.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/version.h"

#include <vulkan/vulkan.h>

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

using namespace RendererRD;

DLSSEffect *DLSSEffect::singleton = nullptr;

DLSSEffect *DLSSEffect::get_singleton() {
	return singleton;
}

DLSSEffect::DLSSEffect() {
	singleton = this;
}

DLSSEffect::~DLSSEffect() {
	if (capability_parameters != nullptr) {
		NVSDK_NGX_VULKAN_DestroyParameters(capability_parameters);
	}
	if (ngx_initialized) {
		NVSDK_NGX_VULKAN_Shutdown1(VK_NULL_HANDLE);
	}
	singleton = nullptr;
}

static void _to_wchar(const String &p_string, LocalVector<wchar_t> &r_buffer) {
	Char16String utf16 = p_string.utf16();
	r_buffer.resize(utf16.length() + 1);
	for (int i = 0; i < utf16.length(); i++) {
		r_buffer[i] = wchar_t(utf16[i]);
	}
	r_buffer[utf16.length()] = 0;
}

bool DLSSEffect::_ensure_initialized() {
	if (init_attempted) {
		return dlss_available;
	}
	init_attempted = true;

	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || rd->get_device_api_name() != "Vulkan") {
		return false;
	}

	VkInstance vk_instance = (VkInstance)rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT);
	VkPhysicalDevice vk_physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	VkDevice vk_device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	if (vk_instance == VK_NULL_HANDLE || vk_device == VK_NULL_HANDLE) {
		return false;
	}

	// Directories searched for the DLSS runtime (libnvidia-ngx-dlss.so /
	// nvngx_dlss.dll): next to the executable, plus an optional override.
	static LocalVector<wchar_t> exe_dir_w;
	static LocalVector<wchar_t> env_dir_w;
	static LocalVector<wchar_t> data_path_w;
	static const wchar_t *search_paths[2];

	_to_wchar(OS::get_singleton()->get_executable_path().get_base_dir(), exe_dir_w);
	_to_wchar(OS::get_singleton()->get_user_data_dir(), data_path_w);

	uint32_t path_count = 0;
	search_paths[path_count++] = exe_dir_w.ptr();

	String env_dir = OS::get_singleton()->get_environment("GODOT_DLSS_PATH");
	if (!env_dir.is_empty()) {
		_to_wchar(env_dir, env_dir_w);
		search_paths[path_count++] = env_dir_w.ptr();
	}

	// The DLSS SDK's Linux snippets don't carry the ELF signature section that
	// newer NVIDIA drivers validate on load, so signature checking must be
	// relaxed for the snippet to load. Only set when the user hasn't chosen a
	// value themselves; shipping driver-signed snippets makes this unnecessary.
	if (OS::get_singleton()->get_environment("__NV_SIGNED_LOAD_CHECK").is_empty()) {
		OS::get_singleton()->set_environment("__NV_SIGNED_LOAD_CHECK", "none");
		print_verbose("DLSS: relaxed NGX snippet signature validation (__NV_SIGNED_LOAD_CHECK=none).");
	}

	NVSDK_NGX_FeatureCommonInfo common_info = {};
	common_info.PathListInfo.Path = search_paths;
	common_info.PathListInfo.Length = path_count;
	// NGX writes its own logs to the application data path.

	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			"b8e26ee6-0f5b-4f5b-aa69-9e338a07a4f0", // Stable project identifier for NGX.
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			VERSION_FULL_BUILD,
			data_path_w.ptr(),
			vk_instance,
			vk_physical_device,
			vk_device,
			nullptr, // NGX resolves the Vulkan loader itself; the engine may be
			nullptr, // built with volk, whose globals aren't safe to hand out.
			&common_info);
	if (NVSDK_NGX_FAILED(result)) {
		WARN_PRINT(vformat("DLSS: NGX initialization failed (0x%x).", (uint64_t)result));
		return false;
	}
	ngx_initialized = true;

	result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capability_parameters);
	if (NVSDK_NGX_FAILED(result) || capability_parameters == nullptr) {
		WARN_PRINT(vformat("DLSS: failed to query NGX capability parameters (0x%x).", (uint64_t)result));
		return false;
	}

	int dlss_supported = 0;
	NVSDK_NGX_Parameter_GetI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_supported);
	if (dlss_supported == 0) {
		int needs_update = 0;
		NVSDK_NGX_Parameter_GetI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_update);
		int feature_init_result = 0;
		NVSDK_NGX_Parameter_GetI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &feature_init_result);
		WARN_PRINT(vformat("DLSS: SuperSampling not available (driver update needed: %d, feature init result: 0x%x).", needs_update, (uint64_t)(uint32_t)feature_init_result));
		return false;
	}

	dlss_available = true;
	print_verbose("DLSS: Super Resolution available.");
	return true;
}

bool DLSSEffect::is_available() {
	return _ensure_initialized();
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	ERR_FAIL_COND_V(!is_available(), nullptr);

	DLSSContext *context = memnew(DLSSContext);
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;

	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_AllocateParameters(&context->ngx_parameters);
	if (NVSDK_NGX_FAILED(result)) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "DLSS: failed to allocate NGX parameters.");
	}

	// The feature itself is created lazily inside the first driver callback,
	// since creation requires a live command buffer.
	return context;
}

DLSSContext::~DLSSContext() {
	for (uint32_t v = 0; v < MAX_VIEWS; v++) {
		if (features[v] != nullptr) {
			NVSDK_NGX_VULKAN_ReleaseFeature(features[v]);
		}
	}
	if (ngx_parameters != nullptr) {
		NVSDK_NGX_VULKAN_DestroyParameters(ngx_parameters);
	}
}

void DLSSEffect::_fill_texture_handles(RID p_texture, DLSSContext::EvalPayload::TextureHandles &r_handles) {
	if (p_texture.is_null()) {
		r_handles = {};
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	r_handles.image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_texture);
	r_handles.image_view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
	r_handles.vk_format = uint32_t(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture));
	RD::TextureFormat format = rd->texture_get_format(p_texture);
	r_handles.width = format.width;
	r_handles.height = format.height;
}

static NVSDK_NGX_Resource_VK _make_ngx_resource(const DLSSContext::EvalPayload::TextureHandles &p_handles, bool p_read_write, bool p_depth = false) {
	VkImageSubresourceRange range = {};
	range.aspectMask = p_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	return NVSDK_NGX_Create_ImageView_Resource_VK(
			(VkImageView)p_handles.image_view,
			(VkImage)p_handles.image,
			range,
			(VkFormat)p_handles.vk_format,
			p_handles.width,
			p_handles.height,
			p_read_write);
}

void DLSSEffect::_eval_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DLSSContext::EvalPayload *payload = static_cast<DLSSContext::EvalPayload *>(p_userdata);
	DLSSContext *context = payload->context;
	DLSSEffect *effect = DLSSEffect::get_singleton();

	VkCommandBuffer vk_command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);
	ERR_FAIL_COND(vk_command_buffer == VK_NULL_HANDLE);

	uint32_t view = MIN(payload->view, uint32_t(DLSSContext::MAX_VIEWS - 1));

	if (context->features[view] == nullptr) {
		if (context->create_failed) {
			return;
		}

		NVSDK_NGX_DLSS_Create_Params create_params = {};
		create_params.Feature.InWidth = context->internal_size.width;
		create_params.Feature.InHeight = context->internal_size.height;
		create_params.Feature.InTargetWidth = context->target_size.width;
		create_params.Feature.InTargetHeight = context->target_size.height;
		create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
		// Godot renders linear HDR before tonemapping, uses reverse-Z depth,
		// and renders motion vectors at the internal resolution.
		create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
				NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
				NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
		if (payload->exposure.image == 0) {
			create_params.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		}

		VkDevice vk_device = (VkDevice)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
		NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(vk_device, vk_command_buffer, 1, 1, &context->features[view], context->ngx_parameters, &create_params);
		if (NVSDK_NGX_FAILED(result)) {
			context->create_failed = true;
			ERR_PRINT(vformat("DLSS: feature creation failed (0x%x).", (uint64_t)result));
			return;
		}
	}

	NVSDK_NGX_Resource_VK color = _make_ngx_resource(payload->color, false);
	NVSDK_NGX_Resource_VK depth = _make_ngx_resource(payload->depth, false, true);
	NVSDK_NGX_Resource_VK velocity = _make_ngx_resource(payload->velocity, false);
	NVSDK_NGX_Resource_VK output = _make_ngx_resource(payload->output, true);
	NVSDK_NGX_Resource_VK exposure;
	if (payload->exposure.image != 0) {
		exposure = _make_ngx_resource(payload->exposure, false);
	}

	NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
	eval_params.Feature.pInColor = &color;
	eval_params.Feature.pInOutput = &output;
	eval_params.pInDepth = &depth;
	eval_params.pInMotionVectors = &velocity;
	if (payload->exposure.image != 0) {
		eval_params.pInExposureTexture = &exposure;
	}
	eval_params.InJitterOffsetX = payload->jitter.x;
	eval_params.InJitterOffsetY = payload->jitter.y;
	eval_params.InRenderSubrectDimensions.Width = payload->render_size.width;
	eval_params.InRenderSubrectDimensions.Height = payload->render_size.height;
	// Godot's velocity buffer is in UV space; DLSS wants pixels.
	eval_params.InMVScaleX = float(payload->render_size.width);
	eval_params.InMVScaleY = float(payload->render_size.height);
	eval_params.InReset = payload->reset ? 1 : 0;

	NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(vk_command_buffer, context->features[view], context->ngx_parameters, &eval_params);
	if (NVSDK_NGX_FAILED(result)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			ERR_PRINT(vformat("DLSS: evaluation failed (0x%x).", (uint64_t)result));
		}
	}

	(void)p_driver;
	(void)effect;
}

void DLSSEffect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	DLSSContext *context = p_params.context;

	DLSSContext::EvalPayload &payload = context->payload_ring[context->payload_cursor];
	context->payload_cursor = (context->payload_cursor + 1) % DLSSContext::PAYLOAD_RING_SIZE;

	payload.context = context;
	_fill_texture_handles(p_params.color, payload.color);
	_fill_texture_handles(p_params.depth, payload.depth);
	_fill_texture_handles(p_params.velocity, payload.velocity);
	_fill_texture_handles(p_params.exposure, payload.exposure);
	_fill_texture_handles(p_params.output, payload.output);
	payload.jitter = p_params.jitter;
	payload.render_size = p_params.internal_size;
	payload.view = p_params.view;
	payload.reset = p_params.reset_accumulation;

	thread_local LocalVector<RD::CallbackResource> resources;
	resources.clear();

	RD::CallbackResource cr;
	cr.type = RD::CALLBACK_RESOURCE_TYPE_TEXTURE;

	cr.rid = p_params.color;
	cr.usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	resources.push_back(cr);
	cr.rid = p_params.depth;
	cr.usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	resources.push_back(cr);
	cr.rid = p_params.velocity;
	cr.usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	resources.push_back(cr);
	if (p_params.exposure.is_valid()) {
		cr.rid = p_params.exposure;
		cr.usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
		resources.push_back(cr);
	}
	cr.rid = p_params.output;
	cr.usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
	resources.push_back(cr);

	RD::get_singleton()->driver_callback_add(&DLSSEffect::_eval_callback, &payload, resources);
}

#endif // DLSS_ENABLED
