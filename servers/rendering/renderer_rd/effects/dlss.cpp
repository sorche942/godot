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

#include "dlss.h"

#include "../uniform_set_cache_rd.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"
#include "core/version.h"
#include <stdio.h>
#ifdef __linux__
#include <dlfcn.h>
#endif

#ifdef DLSS_STREAMLINE_ENABLED
#include "drivers/streamline/streamline_context.h"
#endif

#ifdef DLSS_NGX_ENABLED
#include "drivers/vulkan/godot_vulkan.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssd.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssd_vk.h"
#endif

using namespace RendererRD;

#ifdef DLSS_ENABLED

// Texture layout/state constants (avoid including backend-specific state headers here).
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY = 5;
static constexpr uint64_t DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR = 0x40;
static constexpr float DLSS_OPTIMAL_MODE_MAX_DISTANCE = 1000000.0f;

namespace {

struct DlssOptimalSettings {
	uint32_t optimal_render_width = 0;
	uint32_t optimal_render_height = 0;
	float optimal_sharpness = 0.0f;
	uint32_t render_width_min = 0;
	uint32_t render_height_min = 0;
	uint32_t render_width_max = 0;
	uint32_t render_height_max = 0;
};

static void fill_matrix_array(const Projection &p_matrix, float *r_values) {
	r_values[0] = p_matrix.columns[0].x;
	r_values[1] = p_matrix.columns[1].x;
	r_values[2] = p_matrix.columns[2].x;
	r_values[3] = p_matrix.columns[3].x;
	r_values[4] = p_matrix.columns[0].y;
	r_values[5] = p_matrix.columns[1].y;
	r_values[6] = p_matrix.columns[2].y;
	r_values[7] = p_matrix.columns[3].y;
	r_values[8] = p_matrix.columns[0].z;
	r_values[9] = p_matrix.columns[1].z;
	r_values[10] = p_matrix.columns[2].z;
	r_values[11] = p_matrix.columns[3].z;
	r_values[12] = p_matrix.columns[0].w;
	r_values[13] = p_matrix.columns[1].w;
	r_values[14] = p_matrix.columns[2].w;
	r_values[15] = p_matrix.columns[3].w;
}

#ifdef DLSS_STREAMLINE_ENABLED
static sl::float4x4 sl_make_identity_matrix() {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
	ret.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
	ret.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
	ret.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
	return ret;
}

static sl::float4x4 sl_convert_matrix(const Projection &p_matrix) {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(p_matrix.columns[0].x, p_matrix.columns[1].x, p_matrix.columns[2].x, p_matrix.columns[3].x));
	ret.setRow(1, sl::float4(p_matrix.columns[0].y, p_matrix.columns[1].y, p_matrix.columns[2].y, p_matrix.columns[3].y));
	ret.setRow(2, sl::float4(p_matrix.columns[0].z, p_matrix.columns[1].z, p_matrix.columns[2].z, p_matrix.columns[3].z));
	ret.setRow(3, sl::float4(p_matrix.columns[0].w, p_matrix.columns[1].w, p_matrix.columns[2].w, p_matrix.columns[3].w));
	return ret;
}

static sl::float3 sl_convert_vector(const Vector3 &p_vector) {
	return sl::float3(p_vector.x, p_vector.y, p_vector.z);
}
#endif

#ifdef DLSS_NGX_ENABLED
static const char *const DLSS_NGX_PROJECT_ID = "f5d967b4-cf10-49fe-8f55-7c3f8b3d92d3";
static const char *const DLSS_NGX_ENABLE_ENV = "GODOT_DLSS_NGX_ENABLE";

struct DlssNgxState {
	bool init_attempted = false;
	bool initialized = false;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	NVSDK_NGX_FeatureCommonInfo feature_common_info = {};
	Vector<Vector<uint8_t>> feature_path_buffers;
	Vector<const wchar_t *> feature_path_ptrs;
};

static DlssNgxState g_dlss_ngx_state;

static void ngx_debug_log(const String &p_message) {
	CharString utf8 = p_message.utf8();
	fprintf(stderr, "[GodotNGX] %s\n", utf8.get_data());
	fflush(stderr);
}

static bool ngx_runtime_enabled() {
	String enabled = OS::get_singleton()->get_environment(DLSS_NGX_ENABLE_ENV).strip_edges().to_lower();
	return enabled == "1" || enabled == "true" || enabled == "yes";
}

static String ngx_result_to_string(NVSDK_NGX_Result p_result) {
	return vformat("0x%08x", uint32_t(p_result));
}

static bool ngx_result_failed(NVSDK_NGX_Result p_result) {
	return p_result != NVSDK_NGX_Result_Success;
}

static void ngx_log_required_extensions() {
	unsigned int instance_extension_count = 0;
	unsigned int device_extension_count = 0;
	const char **instance_extensions = nullptr;
	const char **device_extensions = nullptr;
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(&instance_extension_count, &instance_extensions, &device_extension_count, &device_extensions);
	if (ngx_result_failed(result)) {
		ngx_debug_log("NGX required extension query failed: " + ngx_result_to_string(result));
		return;
	}

	String instance_list;
	for (unsigned int i = 0; i < instance_extension_count; i++) {
		if (i > 0) {
			instance_list += ", ";
		}
		instance_list += String(instance_extensions[i]);
	}

	String device_list;
	for (unsigned int i = 0; i < device_extension_count; i++) {
		if (i > 0) {
			device_list += ", ";
		}
		device_list += String(device_extensions[i]);
	}

	ngx_debug_log(vformat("NGX required instance extensions (%d): %s", int(instance_extension_count), instance_list));
	ngx_debug_log(vformat("NGX required device extensions (%d): %s", int(device_extension_count), device_list));
}

static Vector<uint8_t> make_wchar_string_buffer(const String &p_string) {
	Vector<uint8_t> buffer = p_string.to_wchar_buffer();
	buffer.resize(buffer.size() + sizeof(wchar_t));
	uint8_t *w = buffer.ptrw();
	memset(w + buffer.size() - sizeof(wchar_t), 0, sizeof(wchar_t));
	return buffer;
}

static void ngx_add_feature_path(DlssNgxState &r_state, const String &p_path) {
	if (p_path.is_empty() || !DirAccess::dir_exists_absolute(p_path)) {
		return;
	}

	r_state.feature_path_buffers.push_back(make_wchar_string_buffer(p_path));
	const Vector<uint8_t> &path_buffer = r_state.feature_path_buffers[r_state.feature_path_buffers.size() - 1];
	r_state.feature_path_ptrs.push_back((const wchar_t *)path_buffer.ptr());
}

static void NVSDK_CONV ngx_log_callback(const char *message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent) {
	const char *level_str = "?";
	switch (loggingLevel) {
		case NVSDK_NGX_LOGGING_LEVEL_OFF: level_str = "OFF"; break;
		case NVSDK_NGX_LOGGING_LEVEL_ON: level_str = "ON"; break;
		case NVSDK_NGX_LOGGING_LEVEL_VERBOSE: level_str = "VERBOSE"; break;
		default: break;
	}
	print_line(vformat("[NGX feature=%d %s] %s", int(sourceComponent), level_str, String(message)));
}

static void ngx_setup_feature_info(DlssNgxState &r_state) {
	if (!r_state.feature_path_ptrs.is_empty()) {
		return;
	}

	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	ngx_add_feature_path(r_state, exe_dir);
	ngx_add_feature_path(r_state, exe_dir.path_join("../references/DLSS/lib/Linux_x86_64/rel").simplify_path());
	ngx_add_feature_path(r_state, exe_dir.path_join("../references/DLSS/lib/Linux_x86_64/dev").simplify_path());

	r_state.feature_common_info = {};
	if (!r_state.feature_path_ptrs.is_empty()) {
		r_state.feature_common_info.PathListInfo.Path = r_state.feature_path_ptrs.ptr();
		r_state.feature_common_info.PathListInfo.Length = r_state.feature_path_ptrs.size();
	}
	r_state.feature_common_info.LoggingInfo.LoggingCallback = ngx_log_callback;
	r_state.feature_common_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
	r_state.feature_common_info.LoggingInfo.DisableOtherLoggingSinks = false;
}

static bool ngx_ensure_initialized() {
	if (g_dlss_ngx_state.init_attempted) {
		return g_dlss_ngx_state.initialized;
	}
	g_dlss_ngx_state.init_attempted = true;

	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	if (rd->get_device_api_name().to_lower() != "vulkan") {
		return false;
	}

	g_dlss_ngx_state.instance = (VkInstance)rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT);
	g_dlss_ngx_state.physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	g_dlss_ngx_state.device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);

	String ngx_data_path = OS::get_singleton()->get_user_data_dir().path_join("ngx");
	if (DirAccess::make_dir_recursive_absolute(ngx_data_path) != OK) {
		WARN_PRINT("Failed to create NGX data directory: " + ngx_data_path);
	}
	Vector<uint8_t> ngx_data_path_wide = make_wchar_string_buffer(ngx_data_path);
	ngx_setup_feature_info(g_dlss_ngx_state);
	ngx_log_required_extensions();
	ngx_debug_log(vformat("NGX init begin. data_path=%s feature_paths=%d", ngx_data_path, g_dlss_ngx_state.feature_path_ptrs.size()));

	// Resolve Vulkan proc addr functions for NGX. DLSS-D (ray reconstruction)
	// needs these to load advanced Vulkan extension functions internally.
	// With Volk, the global function pointers may not be suitable, so load
	// the raw function from the Vulkan loader via dlsym.
	PFN_vkGetInstanceProcAddr gipa = nullptr;
	PFN_vkGetDeviceProcAddr gdpa = nullptr;
#ifdef __linux__
	{
		void *vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
		if (!vk_lib) {
			vk_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
		}
		if (vk_lib) {
			gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk_lib, "vkGetInstanceProcAddr");
			gdpa = (PFN_vkGetDeviceProcAddr)dlsym(vk_lib, "vkGetDeviceProcAddr");
			dlclose(vk_lib);
		}
	}
#endif
	ngx_debug_log(vformat("NGX Vulkan proc addrs: gipa=%s gdpa=%s", gipa ? "loaded" : "null", gdpa ? "loaded" : "null"));

	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			DLSS_NGX_PROJECT_ID,
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			VERSION_FULL_BUILD,
			(const wchar_t *)ngx_data_path_wide.ptr(),
			g_dlss_ngx_state.instance,
			g_dlss_ngx_state.physical_device,
			g_dlss_ngx_state.device,
			gipa,
			gdpa,
			&g_dlss_ngx_state.feature_common_info,
			NVSDK_NGX_Version_API);
	if (ngx_result_failed(result)) {
		WARN_PRINT("NGX Vulkan initialization failed: " + ngx_result_to_string(result));
		return false;
	}

	ngx_debug_log("NGX init succeeded.");
	g_dlss_ngx_state.initialized = true;
	return true;
}

static void ngx_shutdown() {
	if (!g_dlss_ngx_state.initialized) {
		return;
	}

	NVSDK_NGX_VULKAN_Shutdown1(g_dlss_ngx_state.device);
	g_dlss_ngx_state = {};
}

static NVSDK_NGX_PerfQuality_Value ngx_select_perf_quality(uint32_t p_output_width, uint32_t p_output_height, uint32_t p_render_width, uint32_t p_render_height) {
	if (p_output_width == 0 || p_output_height == 0) {
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}

	if (p_render_width >= p_output_width && p_render_height >= p_output_height) {
		return NVSDK_NGX_PerfQuality_Value_DLAA;
	}

	const float scale_x = float(p_render_width) / float(p_output_width);
	const float scale_y = float(p_render_height) / float(p_output_height);
	const float scale = MIN(scale_x, scale_y);

	if (scale >= 0.66f) {
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
	if (scale >= 0.58f) {
		return NVSDK_NGX_PerfQuality_Value_Balanced;
	}
	if (scale >= 0.50f) {
		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	}
	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

static int ngx_make_feature_flags(bool p_reverse_depth, bool p_auto_exposure, bool p_ray_reconstruction = false) {
	int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	if (p_ray_reconstruction) {
		// DLSS-D requires low-res motion vectors (rendered at internal resolution).
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
	}
	if (p_reverse_depth) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	}
	if (p_auto_exposure) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	}
	return flags;
}

// Query DLSS-D optimal settings to find the correct quality mode for the given
// render/output resolution. Uses capability parameters with the
// DLSSDOptimalSettingsCallback from the DLSS-D library. Returns true if a valid
// mode was found. Falls back to heuristic if the callback is unavailable.
static bool ngx_dlssd_query_quality_mode(
		uint32_t p_output_width, uint32_t p_output_height,
		uint32_t p_render_width, uint32_t p_render_height,
		NVSDK_NGX_PerfQuality_Value &r_quality) {
	NVSDK_NGX_Parameter *cap_params = nullptr;
	NVSDK_NGX_Result cap_result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&cap_params);
	if (ngx_result_failed(cap_result) || cap_params == nullptr) {
		r_quality = ngx_select_perf_quality(p_output_width, p_output_height, p_render_width, p_render_height);
		return true;
	}

	const NVSDK_NGX_PerfQuality_Value modes[] = {
		NVSDK_NGX_PerfQuality_Value_DLAA,
		NVSDK_NGX_PerfQuality_Value_UltraQuality,
		NVSDK_NGX_PerfQuality_Value_MaxQuality,
		NVSDK_NGX_PerfQuality_Value_Balanced,
		NVSDK_NGX_PerfQuality_Value_MaxPerf,
		NVSDK_NGX_PerfQuality_Value_UltraPerformance,
	};
	const char *mode_names[] = { "DLAA", "UltraQuality", "MaxQuality", "Balanced", "MaxPerf", "UltraPerf" };
	constexpr int mode_count = sizeof(modes) / sizeof(modes[0]);

	float best_distance = 1e30f;
	NVSDK_NGX_PerfQuality_Value best_mode = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	bool found_valid = false;
	String debug_info;

	for (int i = 0; i < mode_count; i++) {
		unsigned int opt_w = 0, opt_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
		float sharpness = 0.0f;
		NVSDK_NGX_Result opt_result = NGX_DLSSD_GET_OPTIMAL_SETTINGS(
				cap_params, p_output_width, p_output_height, modes[i],
				&opt_w, &opt_h, &max_w, &max_h, &min_w, &min_h, &sharpness);

		if (ngx_result_failed(opt_result) || opt_w == 0 || opt_h == 0) {
			debug_info += vformat(" %s=unsupported", mode_names[i]);
			continue;
		}

		debug_info += vformat(" %s=opt(%dx%d)range(%dx%d-%dx%d)", mode_names[i], opt_w, opt_h, min_w, min_h, max_w, max_h);

		if (p_render_width >= min_w && p_render_width <= max_w &&
				p_render_height >= min_h && p_render_height <= max_h) {
			float dist = Math::abs((float)opt_w - (float)p_render_width) +
					Math::abs((float)opt_h - (float)p_render_height);
			if (!found_valid || dist < best_distance) {
				best_distance = dist;
				best_mode = modes[i];
				found_valid = true;
			}
		}
	}

	print_line(vformat("DLSS-D optimal settings query (render=%dx%d output=%dx%d):%s => %s",
			p_render_width, p_render_height, p_output_width, p_output_height,
			debug_info, found_valid ? vformat("selected mode %d", int(best_mode)) : "no valid mode"));

	if (found_valid) {
		r_quality = best_mode;
	}
	return found_valid;
}

// Map a preset character ('D', 'E', etc.) to the NGX RayReconstruction hint
// render preset enum value and set it on the parameter object for all quality modes.
static void ngx_set_rr_presets(NVSDK_NGX_Parameter *p_params, char p_preset) {
	int preset_value = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
	if (p_preset >= 'D' && p_preset <= 'O') {
		preset_value = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D + (p_preset - 'D');
	}
	NVSDK_NGX_Parameter_SetI(p_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, preset_value);
	NVSDK_NGX_Parameter_SetI(p_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, preset_value);
	NVSDK_NGX_Parameter_SetI(p_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, preset_value);
	NVSDK_NGX_Parameter_SetI(p_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, preset_value);
	NVSDK_NGX_Parameter_SetI(p_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, preset_value);
	NVSDK_NGX_Parameter_SetI(p_params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, preset_value);
}

static NVSDK_NGX_Resource_VK ngx_texture_to_resource(RID p_texture, bool p_write_access = false) {
	RD *rd = RD::get_singleton();
	VkImageSubresourceRange subresource_range = {};
	subresource_range.aspectMask = (VkImageAspectFlags)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW_ASPECT_MASK, p_texture);
	subresource_range.baseMipLevel = uint32_t(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW_BASE_MIPMAP, p_texture));
	subresource_range.levelCount = uint32_t(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW_MIPMAP_COUNT, p_texture));
	subresource_range.baseArrayLayer = uint32_t(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW_BASE_LAYER, p_texture));
	subresource_range.layerCount = uint32_t(rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW_LAYER_COUNT, p_texture));

	RD::TextureFormat texture_format = rd->texture_get_format(p_texture);
	const bool read_write = p_write_access || (rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture) & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
	return NVSDK_NGX_Create_ImageView_Resource_VK(
			(VkImageView)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture),
			(VkImage)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_texture),
			subresource_range,
			(VkFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture),
			texture_format.width,
			texture_format.height,
			read_write);
}
#endif

} // namespace

#ifdef DLSS_STREAMLINE_ENABLED
namespace RendererRD {
class DLSSContextStreamline : public DLSSContext {
public:
	sl::ViewportHandle viewport;
	sl::Constants constants;
	sl::DLSSOptions current_dlss_options;
	sl::DLSSOptimalSettings current_optimal_settings;
	sl::DLSSDOptions current_dlssd_options;

	DLSSContextStreamline();
	~DLSSContextStreamline() override;

	sl::DLSSMode find_optimal_mode(uint32_t p_output_width, uint32_t p_output_height, uint32_t p_desired_width, uint32_t p_desired_height, sl::DLSSOptimalSettings &r_optimal_settings, bool p_ray_reconstruction = false) {
		if (p_ray_reconstruction) {
			if (StreamlineContext::get().slDLSSDGetOptimalSettings == nullptr) {
				return sl::DLSSMode::eOff;
			}
		} else if (StreamlineContext::get().slDLSSGetOptimalSettings == nullptr) {
			return sl::DLSSMode::eOff;
		}

		sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
		sl::DLSSOptimalSettings settings[sizeof(modes) / sizeof(modes[0])];
		bool valid_settings[sizeof(modes) / sizeof(modes[0])];
		Vector2 distance[sizeof(modes) / sizeof(modes[0])];
		memset(valid_settings, 0, sizeof(valid_settings));

		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			sl::Result result;
			if (p_ray_reconstruction) {
				sl::DLSSDOptions dlssd_options = {};
				dlssd_options.outputWidth = p_output_width;
				dlssd_options.outputHeight = p_output_height;
				dlssd_options.mode = modes[i];
				sl::DLSSDOptimalSettings dlssd_settings;
				result = StreamlineContext::get().slDLSSDGetOptimalSettings(dlssd_options, dlssd_settings);
				if (result == sl::Result::eOk) {
					settings[i].optimalRenderWidth = dlssd_settings.optimalRenderWidth;
					settings[i].optimalRenderHeight = dlssd_settings.optimalRenderHeight;
					settings[i].optimalSharpness = dlssd_settings.optimalSharpness;
					settings[i].renderWidthMin = dlssd_settings.renderWidthMin;
					settings[i].renderHeightMin = dlssd_settings.renderHeightMin;
					settings[i].renderWidthMax = dlssd_settings.renderWidthMax;
					settings[i].renderHeightMax = dlssd_settings.renderHeightMax;
				}
			} else {
				sl::DLSSOptions dlss_options = {};
				dlss_options.outputWidth = p_output_width;
				dlss_options.outputHeight = p_output_height;
				dlss_options.mode = modes[i];
				result = StreamlineContext::get().slDLSSGetOptimalSettings(dlss_options, settings[i]);
			}

			if (result != sl::Result::eOk) {
				continue;
			}

			sl::DLSSOptimalSettings &optimal_settings = settings[i];
			if (p_desired_width >= optimal_settings.renderWidthMin &&
					p_desired_width <= optimal_settings.renderWidthMax &&
					p_desired_height >= optimal_settings.renderHeightMin &&
					p_desired_height <= optimal_settings.renderHeightMax) {
				valid_settings[i] = true;
				distance[i] = Vector2(Math::abs((float)optimal_settings.optimalRenderWidth - (float)p_desired_width), Math::abs((float)optimal_settings.optimalRenderHeight - (float)p_desired_height));
			}
		}

		Vector2 closest_distance(DLSS_OPTIMAL_MODE_MAX_DISTANCE, DLSS_OPTIMAL_MODE_MAX_DISTANCE);
		int closest_distance_match = -1;
		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			if (valid_settings[i] && distance[i].length_squared() < closest_distance.length_squared()) {
				closest_distance_match = int(i);
				closest_distance = distance[i];
			}
		}

		if (closest_distance_match != -1) {
			r_optimal_settings = settings[closest_distance_match];
			return modes[closest_distance_match];
		}

		return sl::DLSSMode::eOff;
	}
};
} // namespace RendererRD

static Vector<unsigned int> g_dlss_free_viewport_indices;
static unsigned int g_dlss_viewport_index = 1;

DLSSContextStreamline::DLSSContextStreamline() {
	backend_type = BACKEND_STREAMLINE;
	if (g_dlss_free_viewport_indices.is_empty()) {
		g_dlss_free_viewport_indices.push_back(g_dlss_viewport_index++);
	}
	viewport = g_dlss_free_viewport_indices[g_dlss_free_viewport_indices.size() - 1];
	g_dlss_free_viewport_indices.remove_at(g_dlss_free_viewport_indices.size() - 1);
}

DLSSContextStreamline::~DLSSContextStreamline() {
	g_dlss_free_viewport_indices.push_back((unsigned int)viewport);
}
#endif

#ifdef DLSS_NGX_ENABLED
namespace RendererRD {
class DLSSContextNgx : public DLSSContext {
public:
	NVSDK_NGX_PerfQuality_Value perf_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
	NVSDK_NGX_Parameter *runtime_parameters = nullptr;
	NVSDK_NGX_Handle *dlss_handle = nullptr;
	NVSDK_NGX_Handle *dlssd_handle = nullptr;
	bool dlssd_creation_failed = false; // Set to true on first creation failure to avoid per-frame retries.
	RID scratch_buffer;
	size_t scratch_size = 0;
	uint32_t render_width = 0;
	uint32_t render_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	float world_to_view[16] = {};
	float view_to_clip[16] = {};

	DLSSContextNgx() {
		backend_type = BACKEND_NGX;
	}

	~DLSSContextNgx() override {
		if (dlss_handle != nullptr) {
			NVSDK_NGX_VULKAN_ReleaseFeature(dlss_handle);
		}
		if (dlssd_handle != nullptr) {
			NVSDK_NGX_VULKAN_ReleaseFeature(dlssd_handle);
		}
		if (runtime_parameters != nullptr) {
			NVSDK_NGX_VULKAN_DestroyParameters(runtime_parameters);
		}
		if (scratch_buffer.is_valid()) {
			RD::get_singleton()->free_rid(scratch_buffer);
		}
	}
};
} // namespace RendererRD
#endif

DLSSEffect::DLSSEffect() {
	Vector<String> modes;
	modes.push_back("\n");
	shaders.mvec_decode_shader.initialize(modes, "");
	shaders.mvec_decode_version = shaders.mvec_decode_shader.version_create();
	shaders.mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0));

#ifdef DLSS_STREAMLINE_ENABLED
	backend_type = DLSSContext::BACKEND_STREAMLINE;
#endif
}

DLSSEffect::~DLSSEffect() {
	shaders.mvec_decode_shader.version_free(shaders.mvec_decode_version);
#ifdef DLSS_NGX_ENABLED
	if (backend_type == DLSSContext::BACKEND_NGX) {
		ngx_shutdown();
	}
#endif
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
#ifdef DLSS_NGX_ENABLED
	if (backend_type == DLSSContext::BACKEND_NONE && RD::get_singleton()->get_device_api_name().to_lower() == "vulkan" && ngx_runtime_enabled()) {
		backend_type = DLSSContext::BACKEND_NGX;
	}
#endif

#ifdef DLSS_STREAMLINE_ENABLED
	if (backend_type == DLSSContext::BACKEND_STREAMLINE) {
		DLSSContextStreamline *context = memnew(DLSSContextStreamline);
		context->current_dlss_options.mode = context->find_optimal_mode(p_target_size.width, p_target_size.height, p_internal_size.width, p_internal_size.height, context->current_optimal_settings);
		context->current_dlss_options.outputWidth = p_target_size.width;
		context->current_dlss_options.outputHeight = p_target_size.height;
		context->is_d3d12 = (RD::get_singleton()->get_device_api_name().to_lower() == "d3d12");
		return context;
	}
#endif

#ifdef DLSS_NGX_ENABLED
	if (backend_type == DLSSContext::BACKEND_NGX) {
		ngx_debug_log(vformat("Creating NGX DLSS context. render=%dx%d output=%dx%d", p_internal_size.width, p_internal_size.height, p_target_size.width, p_target_size.height));
		if (!ngx_ensure_initialized()) {
			ngx_debug_log("NGX init failed during DLSS context creation.");
			return nullptr;
		}

		DLSSContextNgx *context = memnew(DLSSContextNgx);
		context->render_width = p_internal_size.width;
		context->render_height = p_internal_size.height;
		context->output_width = p_target_size.width;
		context->output_height = p_target_size.height;
		context->perf_quality = ngx_select_perf_quality(p_target_size.width, p_target_size.height, p_internal_size.width, p_internal_size.height);
		ngx_debug_log(vformat("NGX DLSS context perf quality=%d", int(context->perf_quality)));

		NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_AllocateParameters(&context->runtime_parameters);
		if (ngx_result_failed(result)) {
			WARN_PRINT("Failed to allocate NGX runtime parameters: " + ngx_result_to_string(result));
			memdelete(context);
			return nullptr;
		}
		ngx_debug_log("NGX runtime parameters allocated.");

		return context;
	}
#endif

	return nullptr;
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {
	DLSSContext *base_context = p_params.context;
	if (base_context == nullptr) {
		return;
	}

	if (base_context->delay > 0) {
		--base_context->delay;
		return;
	}

#ifdef DLSS_STREAMLINE_ENABLED
	if (base_context->backend_type == DLSSContext::BACKEND_STREAMLINE) {
		if (StreamlineContext::get().slDLSSSetOptions == nullptr) {
			return;
		}
		if (StreamlineContext::get().last_token == nullptr) {
			StreamlineContext::get().get_new_frame_token();
		}
	}
#endif

#ifdef DLSS_NGX_ENABLED
	if (base_context->backend_type == DLSSContext::BACKEND_NGX && !ngx_ensure_initialized()) {
		return;
	}
#endif

	base_context->last_parameters = p_params;
	base_context->last_effect = this;

	{
		RD::get_singleton()->draw_command_begin_label("Decode Invalid Motion Vectors");
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);

		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RID shader = shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shaders.mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);

		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		memcpy(push_constants + 4, &p_params.reprojection.columns[0].x, sizeof(float) * 16);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));

		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	RD::CallbackResource resources[9];
	int resource_count = 0;
	auto add_resource = [&](RID p_rid, RD::CallbackResourceUsage p_usage) {
		if (!p_rid.is_valid()) {
			return;
		}
		resources[resource_count].rid = p_rid;
		resources[resource_count].usage = p_usage;
		resource_count++;
	};

	add_resource(p_params.color, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	add_resource(p_params.output, base_context->backend_type == DLSSContext::BACKEND_NGX ? RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE : RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	add_resource(p_params.depth, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	add_resource(p_params.velocity, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	add_resource(p_params.exposure, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	if (p_params.dlss_rr) {
		add_resource(p_params.dlss_rr_diffuse_albedo, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
		add_resource(p_params.dlss_rr_specular_albedo, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
		add_resource(p_params.dlss_rr_normal_roughness, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
		add_resource(p_params.dlss_rr_specular_hit_dist, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);
	}

	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_upscale_internal_graph_callback, base_context, VectorView<RD::CallbackResource>(resources, resource_count));
}

#ifdef DLSS_STREAMLINE_ENABLED
void DLSSEffect::_upscale_internal_streamline(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {
	DLSSContextStreamline *context = (DLSSContextStreamline *)p_params.context;
	void *native_cmdlist = RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(p_cmdid);

	auto assign_resource = [context](sl::Resource *r_resources, sl::ResourceTag *r_resource_tags, int &r_resource_count, RID p_texture_rid, sl::BufferType p_buffer_type, sl::ResourceLifecycle p_lifecycle) {
		if (!p_texture_rid.is_valid() || p_texture_rid.is_null()) {
			return;
		}

		RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_texture_rid);
		uint64_t texture_image = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_texture_rid);
		uint64_t texture_view = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture_rid);
		uint64_t texture_device_memory = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_texture_rid);
		uint64_t texture_state = context->is_d3d12 ? DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR : DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY;
		uint64_t texture_vk_format = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture_rid);
		uint64_t texture_usage_flags = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture_rid);
		auto &destination_resource = r_resources[r_resource_count];
		if (context->is_d3d12) {
			destination_resource = sl::Resource(sl::ResourceType::eTex2d, (void *)texture_view, texture_state);
		} else {
			destination_resource = sl::Resource(sl::ResourceType::eTex2d, (void *)texture_image, (void *)texture_device_memory, (void *)texture_view, texture_state);
		}
		destination_resource.width = texture_format.width;
		destination_resource.height = texture_format.height;
		destination_resource.nativeFormat = texture_vk_format;
		destination_resource.arrayLayers = texture_format.array_layers;
		destination_resource.flags = 0;
		destination_resource.mipLevels = texture_format.mipmaps;
		destination_resource.usage = texture_usage_flags;

		r_resource_tags[r_resource_count] = sl::ResourceTag(r_resources + r_resource_count, p_buffer_type, p_lifecycle, nullptr);
		r_resource_count++;
	};

	bool use_dlss_rr = p_params.dlss_rr && StreamlineContext::get().slDLSSDSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_rr_available;

	if (use_dlss_rr) {
		context->current_dlssd_options.mode = context->current_dlss_options.mode;
		context->current_dlssd_options.outputWidth = context->current_dlss_options.outputWidth;
		context->current_dlssd_options.outputHeight = context->current_dlss_options.outputHeight;
		context->current_dlssd_options.colorBuffersHDR = sl::Boolean::eTrue;
		context->current_dlssd_options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;

		Transform3D view_matrix = p_params.cam_transform.affine_inverse();
		context->current_dlssd_options.worldToCameraView = sl_convert_matrix(Projection(view_matrix));
		context->current_dlssd_options.cameraViewToWorld = sl_convert_matrix(Projection(view_matrix).inverse());
		char dlss_preset = p_params.preset == '?' ? StreamlineContext::get().dlss_rr_default_preset : p_params.preset;

		if (dlss_preset == '?') {
			context->current_dlssd_options.dlaaPreset = sl::DLSSDPreset::eDefault;
			context->current_dlssd_options.qualityPreset = sl::DLSSDPreset::eDefault;
			context->current_dlssd_options.balancedPreset = sl::DLSSDPreset::eDefault;
			context->current_dlssd_options.performancePreset = sl::DLSSDPreset::eDefault;
			context->current_dlssd_options.ultraPerformancePreset = sl::DLSSDPreset::eDefault;
		} else {
			int preset_no = ((int)dlss_preset - (int)'D');
			sl::DLSSDPreset preset = (sl::DLSSDPreset)((int)sl::DLSSDPreset::ePresetD + preset_no);
			context->current_dlssd_options.dlaaPreset = preset;
			context->current_dlssd_options.qualityPreset = preset;
			context->current_dlssd_options.balancedPreset = preset;
			context->current_dlssd_options.performancePreset = preset;
			context->current_dlssd_options.ultraPerformancePreset = preset;
		}

		sl::Result result = StreamlineContext::get().slDLSSDSetOptions(context->viewport, context->current_dlssd_options);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSDSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
	} else if (StreamlineContext::get().slDLSSSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_available) {
		context->current_dlss_options.useAutoExposure = (p_params.exposure.is_null() || !p_params.exposure.is_valid()) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->current_dlss_options.colorBuffersHDR = sl::Boolean::eTrue;
		char dlss_preset = p_params.preset == '?' ? StreamlineContext::get().dlss_default_preset : p_params.preset;

		if (dlss_preset == '?') {
			context->current_dlss_options.dlaaPreset = sl::DLSSPreset::eDefault;
			context->current_dlss_options.qualityPreset = sl::DLSSPreset::eDefault;
			context->current_dlss_options.balancedPreset = sl::DLSSPreset::eDefault;
			context->current_dlss_options.performancePreset = sl::DLSSPreset::eDefault;
			context->current_dlss_options.ultraPerformancePreset = sl::DLSSPreset::eDefault;
		} else {
			int preset_no = ((int)dlss_preset - (int)'F');
			sl::DLSSPreset preset = (sl::DLSSPreset)((int)sl::DLSSPreset::ePresetF + preset_no);
			context->current_dlss_options.dlaaPreset = preset;
			context->current_dlss_options.qualityPreset = preset;
			context->current_dlss_options.balancedPreset = preset;
			context->current_dlss_options.performancePreset = preset;
			context->current_dlss_options.ultraPerformancePreset = preset;
		}

		sl::Result result = StreamlineContext::get().slDLSSSetOptions(context->viewport, context->current_dlss_options);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	if (StreamlineContext::get().slSetConstants != nullptr) {
		sl::float4x4 mtx_identity = sl_make_identity_matrix();
		context->constants.cameraViewToClip = sl_convert_matrix(p_params.cam_projection);
		context->constants.clipToCameraView = sl_convert_matrix(p_params.cam_projection.inverse());
		context->constants.clipToLensClip = mtx_identity;
		context->constants.clipToPrevClip = sl_convert_matrix(p_params.reprojection);
		context->constants.prevClipToClip = sl_convert_matrix(p_params.reprojection.inverse());

		context->constants.cameraPos = sl_convert_vector(p_params.cam_transform.get_origin());
		context->constants.cameraFwd = sl_convert_vector(-p_params.cam_transform.get_basis().rows[2]);
		context->constants.cameraUp = sl_convert_vector(p_params.cam_transform.get_basis().rows[1]);
		context->constants.cameraRight = sl_convert_vector(p_params.cam_transform.get_basis().rows[0]);

		context->constants.cameraNear = p_params.z_near;
		context->constants.cameraFar = p_params.z_far;
		context->constants.cameraFOV = Math::deg_to_rad(p_params.fovy);
		context->constants.cameraMotionIncluded = sl::Boolean::eTrue;
		context->constants.cameraAspectRatio = static_cast<float>(context->current_dlss_options.outputWidth) / static_cast<float>(context->current_dlss_options.outputHeight);
		context->constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
		context->constants.depthInverted = p_params.reverse_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->constants.motionVectors3D = sl::Boolean::eFalse;
		context->constants.motionVectorsDilated = sl::Boolean::eFalse;
		context->constants.motionVectorsJittered = sl::Boolean::eFalse;
		context->constants.jitterOffset = sl::float2(p_params.jitter.x, p_params.jitter.y);
		context->constants.mvecScale = sl::float2(1.0f, 1.0f);
		context->constants.orthographicProjection = sl::Boolean::eFalse;
		context->constants.reset = p_params.reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		sl::Result result = StreamlineContext::get().slSetConstants(context->constants, *StreamlineContext::get().last_token, context->viewport);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slSetConstants. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	if (StreamlineContext::get().slSetTag != nullptr) {
		sl::Resource resources[10];
		sl::ResourceTag resource_tags[10];
		int resource_count = 0;

		assign_resource(resources, resource_tags, resource_count, p_params.color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assign_resource(resources, resource_tags, resource_count, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assign_resource(resources, resource_tags, resource_count, p_params.depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent);
		assign_resource(resources, resource_tags, resource_count, p_params.velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent);

		if (use_dlss_rr) {
			assign_resource(resources, resource_tags, resource_count, p_params.dlss_rr_diffuse_albedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			assign_resource(resources, resource_tags, resource_count, p_params.dlss_rr_specular_albedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			assign_resource(resources, resource_tags, resource_count, p_params.dlss_rr_normal_roughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent);
			assign_resource(resources, resource_tags, resource_count, p_params.dlss_rr_specular_hit_dist, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent);
		}

		sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resource_tags, resource_count, native_cmdlist);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slSetTag. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	if (StreamlineContext::get().slDLSSGSetOptions != nullptr && StreamlineContext::get().is_game && StreamlineContext::get().streamline_capabilities.dlss_g_available) {
		sl::DLSSGOptions dlssg_options{};
		bool want_activate_dlssg = p_params.dlss_g;
		bool can_activate_dlssg = StreamlineContext::get().dlssg_delay == 0;

		if (StreamlineContext::get().dlssg_viewport != sl::ViewportHandle(-1) && ((!want_activate_dlssg && StreamlineContext::get().dlssg_viewport == context->viewport) || (want_activate_dlssg && StreamlineContext::get().dlssg_viewport != context->viewport))) {
			dlssg_options.mode = sl::DLSSGMode::eOff;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(StreamlineContext::get().dlssg_viewport, dlssg_options);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			StreamlineContext::get().dlssg_viewport = sl::ViewportHandle(-1);
		}

		if (can_activate_dlssg && want_activate_dlssg && StreamlineContext::get().dlssg_viewport != context->viewport) {
			dlssg_options.mode = sl::DLSSGMode::eOn;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(context->viewport, dlssg_options);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
			StreamlineContext::get().dlssg_viewport = context->viewport;
		}
	}

	if (context->current_dlss_options.mode != sl::DLSSMode::eOff) {
		const sl::BaseStructure *inputs[] = { &context->viewport };
		sl::Result result;

		if (use_dlss_rr && StreamlineContext::get().streamline_capabilities.dlss_rr_available) {
			result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS_RR, *StreamlineContext::get().last_token, inputs, 1, native_cmdlist);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slEvaluateFeature for DLSS Ray Reconstruction. Result: " + String(StreamlineContext::result_to_string(result)));
		} else if (StreamlineContext::get().streamline_capabilities.dlss_available) {
			result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS, *StreamlineContext::get().last_token, inputs, 1, native_cmdlist);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slEvaluateFeature for DLSS Super Resolution. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	if (p_params.sharpness > 0.0f && StreamlineContext::get().slNISSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.nis_available) {
		sl::NISOptions options;
		options.hdrMode = sl::NISHDR::eNone;
		options.mode = sl::NISMode::eSharpen;
		options.sharpness = p_params.sharpness;
		StreamlineContext::get().slNISSetOptions(context->viewport, options);

		sl::Resource resources[3];
		sl::ResourceTag resource_tags[3];
		int resource_count = 0;
		assign_resource(resources, resource_tags, resource_count, p_params.output, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow);
		assign_resource(resources, resource_tags, resource_count, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);

		sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resource_tags, resource_count, native_cmdlist);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slSetTag for NIS. Result: " + String(StreamlineContext::result_to_string(result)));

		const sl::BaseStructure *inputs[] = { &context->viewport };
		result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureNIS, *StreamlineContext::get().last_token, inputs, 1, native_cmdlist);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slEvaluateFeature for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
	}
}
#else
void DLSSEffect::_upscale_internal_streamline(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {}
#endif

#ifdef DLSS_NGX_ENABLED
void DLSSEffect::_upscale_internal_ngx(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {
	DLSSContextNgx *context = (DLSSContextNgx *)p_params.context;
	if (!ngx_ensure_initialized()) {
		return;
	}

	VkCommandBuffer command_buffer = (VkCommandBuffer)RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(p_cmdid);
	ERR_FAIL_NULL(command_buffer);

	const bool use_auto_exposure = p_params.exposure.is_null() || !p_params.exposure.is_valid();
	const bool use_dlss_rr = p_params.dlss_rr;

	if (context->runtime_parameters == nullptr) {
		ERR_FAIL_MSG("NGX runtime parameters are not initialized.");
	}

	auto ensure_scratch_buffer = [&](NVSDK_NGX_Feature p_feature, uint32_t p_width, uint32_t p_height) {
		context->runtime_parameters->Reset();
		NVSDK_NGX_Parameter_SetUI(context->runtime_parameters, NVSDK_NGX_Parameter_Width, p_width);
		NVSDK_NGX_Parameter_SetUI(context->runtime_parameters, NVSDK_NGX_Parameter_Height, p_height);
		NVSDK_NGX_Parameter_SetUI(context->runtime_parameters, NVSDK_NGX_Parameter_OutWidth, context->output_width);
		NVSDK_NGX_Parameter_SetUI(context->runtime_parameters, NVSDK_NGX_Parameter_OutHeight, context->output_height);
		size_t scratch_size = 0;
		NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetScratchBufferSize(p_feature, context->runtime_parameters, &scratch_size);
		if (ngx_result_failed(result) || scratch_size == 0) {
			ngx_debug_log(vformat("GetScratchBufferSize failed for feature %d: %s (scratch_size=%d)", int(p_feature), ngx_result_to_string(result), (int)scratch_size));
			return;
		}
		if (context->scratch_size >= scratch_size && context->scratch_buffer.is_valid()) {
			return;
		}
		if (context->scratch_buffer.is_valid()) {
			RD::get_singleton()->free_rid(context->scratch_buffer);
			context->scratch_buffer = RID();
		}
		context->scratch_buffer = RD::get_singleton()->storage_buffer_create(uint32_t(scratch_size));
		context->scratch_size = scratch_size;
	};

	auto create_feature_if_needed = [&](bool p_ray_reconstruction) {
		NVSDK_NGX_Handle *&handle = p_ray_reconstruction ? context->dlssd_handle : context->dlss_handle;
		if (handle != nullptr) {
			return;
		}
		if (p_ray_reconstruction && context->dlssd_creation_failed) {
			return; // Don't retry a permanently failed creation.
		}

		// Check DLSS-D preconditions before attempting creation.
		if (p_ray_reconstruction) {
			NVSDK_NGX_Parameter *cap_params = nullptr;
			NVSDK_NGX_Result cap_result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&cap_params);
			if (!ngx_result_failed(cap_result) && cap_params != nullptr) {
				int available = 0;
				NVSDK_NGX_Parameter_GetI(cap_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
				if (!available) {
					int needs_driver = 0;
					unsigned int min_major = 0, min_minor = 0;
					NVSDK_NGX_Parameter_GetI(cap_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needs_driver);
					NVSDK_NGX_Parameter_GetUI(cap_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &min_major);
					NVSDK_NGX_Parameter_GetUI(cap_params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &min_minor);
					if (needs_driver) {
						ERR_PRINT_ONCE(vformat("DLSS Ray Reconstruction not available: driver update required (minimum %d.%d).", min_major, min_minor));
					} else {
						ERR_PRINT_ONCE("DLSS Ray Reconstruction not available on this GPU/driver. The feature model may not be installed.");
					}
					context->dlssd_creation_failed = true;
					return;
				}
			}
		}

		const NVSDK_NGX_Feature feature_id = p_ray_reconstruction ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling;
		ensure_scratch_buffer(feature_id, context->render_width, context->render_height);

		context->runtime_parameters->Reset();
		if (context->scratch_buffer.is_valid()) {
			NVSDK_NGX_Parameter_SetVoidPointer(context->runtime_parameters, NVSDK_NGX_Parameter_Scratch, (void *)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, context->scratch_buffer));
			NVSDK_NGX_Parameter_SetULL(context->runtime_parameters, NVSDK_NGX_Parameter_Scratch_SizeInBytes, context->scratch_size);
		}

		NVSDK_NGX_Result result;
		if (p_ray_reconstruction) {
			// Query DLSS-D optimal settings to find the correct quality mode.
			// The DLSS-D library strictly validates that InPerfQualityValue matches
			// the render/output resolution ratio and rejects mismatches.
			NVSDK_NGX_PerfQuality_Value dlssd_quality;
			if (!ngx_dlssd_query_quality_mode(context->output_width, context->output_height,
						context->render_width, context->render_height, dlssd_quality)) {
				ERR_PRINT_ONCE(vformat("DLSS Ray Reconstruction: no valid quality mode for render=%dx%d output=%dx%d. "
						"Try a different DLSS scaling mode.",
						context->render_width, context->render_height,
						context->output_width, context->output_height));
				context->dlssd_creation_failed = true;
				return;
			}

			// Set RayReconstruction hint render presets before feature creation.
			ngx_set_rr_presets(context->runtime_parameters, p_params.preset);

			NVSDK_NGX_DLSSD_Create_Params create_params = {};
			create_params.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
			create_params.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
			create_params.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
			create_params.InWidth = context->render_width;
			create_params.InHeight = context->render_height;
			create_params.InTargetWidth = context->output_width;
			create_params.InTargetHeight = context->output_height;
			create_params.InPerfQualityValue = dlssd_quality;
			create_params.InFeatureCreateFlags = ngx_make_feature_flags(p_params.reverse_depth, use_auto_exposure, true);
			create_params.InEnableOutputSubrects = false;

			print_line(vformat("DLSS-D creating feature: render=%dx%d output=%dx%d quality=%d preset='%c' flags=0x%x",
					context->render_width, context->render_height,
					context->output_width, context->output_height,
					int(dlssd_quality), p_params.preset, create_params.InFeatureCreateFlags));

			result = NGX_VULKAN_CREATE_DLSSD_EXT1(g_dlss_ngx_state.device, command_buffer, 1, 1, &handle, context->runtime_parameters, &create_params);
		} else {
			NVSDK_NGX_DLSS_Create_Params create_params = {};
			create_params.Feature.InWidth = context->render_width;
			create_params.Feature.InHeight = context->render_height;
			create_params.Feature.InTargetWidth = context->output_width;
			create_params.Feature.InTargetHeight = context->output_height;
			create_params.Feature.InPerfQualityValue = context->perf_quality;
			create_params.InFeatureCreateFlags = ngx_make_feature_flags(p_params.reverse_depth, use_auto_exposure);
			create_params.InEnableOutputSubrects = false;
			result = NGX_VULKAN_CREATE_DLSS_EXT1(g_dlss_ngx_state.device, command_buffer, 1, 1, &handle, context->runtime_parameters, &create_params);
		}

		if (ngx_result_failed(result)) {
			if (p_ray_reconstruction) {
				ERR_PRINT_ONCE("DLSS Ray Reconstruction: failed to create NGX feature: " + ngx_result_to_string(result) +
						vformat(" (render=%dx%d output=%dx%d)",
								context->render_width, context->render_height,
								context->output_width, context->output_height));
				context->dlssd_creation_failed = true;
			} else {
				ERR_PRINT("DLSS SR: failed to create NGX feature: " + ngx_result_to_string(result));
			}
			return;
		}
	};

	create_feature_if_needed(use_dlss_rr);

	if (use_dlss_rr && context->dlssd_handle == nullptr) {
		ERR_PRINT_ONCE("DLSS RR: feature handle is null after creation attempt; DLSS-RR may not be supported on this GPU/driver.");
		return;
	}
	if (!use_dlss_rr && context->dlss_handle == nullptr) {
		ERR_PRINT_ONCE("DLSS SR: feature handle is null after creation attempt.");
		return;
	}

	context->runtime_parameters->Reset();
	if (context->scratch_buffer.is_valid()) {
		NVSDK_NGX_Parameter_SetVoidPointer(context->runtime_parameters, NVSDK_NGX_Parameter_Scratch, (void *)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, context->scratch_buffer));
		NVSDK_NGX_Parameter_SetULL(context->runtime_parameters, NVSDK_NGX_Parameter_Scratch_SizeInBytes, context->scratch_size);
	}

	fill_matrix_array(Projection(p_params.cam_transform.affine_inverse()), context->world_to_view);
	fill_matrix_array(p_params.cam_projection, context->view_to_clip);

	NVSDK_NGX_Resource_VK color_resource = ngx_texture_to_resource(p_params.color);
	NVSDK_NGX_Resource_VK output_resource = ngx_texture_to_resource(p_params.output, true);
	NVSDK_NGX_Resource_VK depth_resource = ngx_texture_to_resource(p_params.depth);
	NVSDK_NGX_Resource_VK velocity_resource = ngx_texture_to_resource(p_params.velocity);
	NVSDK_NGX_Resource_VK exposure_resource = {};
	NVSDK_NGX_Resource_VK diffuse_resource = {};
	NVSDK_NGX_Resource_VK specular_resource = {};
	NVSDK_NGX_Resource_VK normal_roughness_resource = {};
	NVSDK_NGX_Resource_VK specular_hit_dist_resource = {};

	NVSDK_NGX_Resource_VK *exposure_ptr = nullptr;
	if (p_params.exposure.is_valid()) {
		exposure_resource = ngx_texture_to_resource(p_params.exposure);
		exposure_ptr = &exposure_resource;
	}

	NVSDK_NGX_Result result;
	if (use_dlss_rr) {
		static bool dlss_rr_first_eval = true;
		if (dlss_rr_first_eval) {
			print_line("DLSS-RR: first evaluation, handle=", (void *)context->dlssd_handle,
					" diffuse_valid=", p_params.dlss_rr_diffuse_albedo.is_valid(),
					" specular_valid=", p_params.dlss_rr_specular_albedo.is_valid(),
					" normal_valid=", p_params.dlss_rr_normal_roughness.is_valid(),
					" hitdist_valid=", p_params.dlss_rr_specular_hit_dist.is_valid());
			dlss_rr_first_eval = false;
		}

		diffuse_resource = ngx_texture_to_resource(p_params.dlss_rr_diffuse_albedo);
		specular_resource = ngx_texture_to_resource(p_params.dlss_rr_specular_albedo);
		normal_roughness_resource = ngx_texture_to_resource(p_params.dlss_rr_normal_roughness);
		specular_hit_dist_resource = ngx_texture_to_resource(p_params.dlss_rr_specular_hit_dist);

		NVSDK_NGX_VK_DLSSD_Eval_Params eval_params = {};
		eval_params.pInDiffuseAlbedo = &diffuse_resource;
		eval_params.pInSpecularAlbedo = &specular_resource;
		eval_params.pInNormals = &normal_roughness_resource;
		eval_params.pInRoughness = &normal_roughness_resource;
		eval_params.pInColor = &color_resource;
		eval_params.pInOutput = &output_resource;
		eval_params.pInDepth = &depth_resource;
		eval_params.pInMotionVectors = &velocity_resource;
		eval_params.pInExposureTexture = exposure_ptr;
		eval_params.pInSpecularHitDistance = &specular_hit_dist_resource;
		eval_params.InJitterOffsetX = p_params.jitter.x;
		eval_params.InJitterOffsetY = p_params.jitter.y;
		eval_params.InRenderSubrectDimensions.Width = p_params.internal_size.width;
		eval_params.InRenderSubrectDimensions.Height = p_params.internal_size.height;
		eval_params.InReset = p_params.reset_accumulation ? 1 : 0;
		// Motion vectors are in UV space (0-1); scale to pixel space for DLSS.
		eval_params.InMVScaleX = (float)p_params.internal_size.width;
		eval_params.InMVScaleY = (float)p_params.internal_size.height;
		eval_params.InPreExposure = 1.0f;
		eval_params.InExposureScale = 1.0f;
		eval_params.InFrameTimeDeltaInMsec = p_params.delta_time * 1000.0f;
		eval_params.pInWorldToViewMatrix = context->world_to_view;
		eval_params.pInViewToClipMatrix = context->view_to_clip;

		result = NGX_VULKAN_EVALUATE_DLSSD_EXT(command_buffer, context->dlssd_handle, context->runtime_parameters, &eval_params);
		ERR_FAIL_COND_MSG(ngx_result_failed(result), "Failed to evaluate NGX DLSS Ray Reconstruction: " + ngx_result_to_string(result));
	} else {
		NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
		eval_params.Feature.pInColor = &color_resource;
		eval_params.Feature.pInOutput = &output_resource;
		eval_params.Feature.InSharpness = p_params.sharpness;
		eval_params.pInDepth = &depth_resource;
		eval_params.pInMotionVectors = &velocity_resource;
		eval_params.pInExposureTexture = exposure_ptr;
		eval_params.InJitterOffsetX = p_params.jitter.x;
		eval_params.InJitterOffsetY = p_params.jitter.y;
		eval_params.InRenderSubrectDimensions.Width = p_params.internal_size.width;
		eval_params.InRenderSubrectDimensions.Height = p_params.internal_size.height;
		eval_params.InReset = p_params.reset_accumulation ? 1 : 0;
		// Motion vectors are in UV space (0-1); scale to pixel space for DLSS.
		eval_params.InMVScaleX = (float)p_params.internal_size.width;
		eval_params.InMVScaleY = (float)p_params.internal_size.height;
		eval_params.InPreExposure = 1.0f;
		eval_params.InExposureScale = 1.0f;
		eval_params.InFrameTimeDeltaInMsec = p_params.delta_time * 1000.0f;

		result = NGX_VULKAN_EVALUATE_DLSS_EXT(command_buffer, context->dlss_handle, context->runtime_parameters, &eval_params);
		ERR_FAIL_COND_MSG(ngx_result_failed(result), "Failed to evaluate NGX DLSS Super Resolution: " + ngx_result_to_string(result));
	}
}
#else
void DLSSEffect::_upscale_internal_ngx(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {}
#endif

void DLSSEffect::_upscale_internal(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {
	DLSSContext *context = p_params.context;
	if (context == nullptr) {
		return;
	}

#ifdef DLSS_STREAMLINE_ENABLED
	if (context->backend_type == DLSSContext::BACKEND_STREAMLINE) {
		_upscale_internal_streamline(p_cmdid, p_params);
		return;
	}
#endif

#ifdef DLSS_NGX_ENABLED
	if (context->backend_type == DLSSContext::BACKEND_NGX) {
		_upscale_internal_ngx(p_cmdid, p_params);
		return;
	}
#endif
}

void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DLSSContext *self = (DLSSContext *)p_userdata;
	self->last_effect->_upscale_internal(p_command_buffer, self->last_parameters);
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	if (p_context == nullptr || p_context->delay > 0) {
		return false;
	}

#ifdef DLSS_STREAMLINE_ENABLED
	if (p_context->backend_type == DLSSContext::BACKEND_STREAMLINE) {
		DLSSContextStreamline *context = (DLSSContextStreamline *)p_context;
		return context->current_dlss_options.mode != sl::DLSSMode::eOff;
	}
#endif

#ifdef DLSS_NGX_ENABLED
	if (p_context->backend_type == DLSSContext::BACKEND_NGX) {
		return true;
	}
#endif

	return false;
}

#else

DLSSEffect::DLSSEffect() {}
DLSSEffect::~DLSSEffect() {}
DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	return nullptr;
}
void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {}
bool DLSSEffect::is_ready(DLSSContext *p_context) {
	return false;
}
void DLSSEffect::_upscale_internal(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {}
void DLSSEffect::_upscale_internal_streamline(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {}
void DLSSEffect::_upscale_internal_ngx(RDD::CommandBufferID p_cmdid, const DLSSContext::Parameters &p_params) {}
void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}

#endif
