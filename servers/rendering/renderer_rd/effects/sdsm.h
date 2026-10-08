/**************************************************************************/
/*  sdsm.h                                                                */
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

#pragma once

#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/shaders/effects/sdsm.glsl.gen.h"
#include "servers/rendering/renderer_sdsm.h"

namespace RendererRD {

class SDSM {
public:
	struct Bounds {
		float minimum[4];
		float maximum[4];
	};
	// Matches SdsmResult in sdsm_data_inc.glsl in both std140 and std430.
	struct Result {
		float splits[4];
		Bounds bounds[4];
		float texel[4];
		float projection[4][16];
		float inv_projection[4][16];
		float inv_view[4][16];
		float view[4][16];
		float inv_view_precision[4][4];
		float shadow_matrix[4][16];
		float shadow_params[4][4];
		float shadow_z_range[4];
		float shadow_range_begin[4];
		float shadow_uv_scale[2][4];
		float fade[4];
		uint32_t meta[4];
	};
	static_assert(sizeof(Result) == 1664);

	struct FitSettings {
		float resolution = 1;
		float blur = 0;
		float normal_bias = 0;
		float angular_size = 0;
		float pancake = 0;
		float fog_length = 0;
		Vector2 fog_near_size;
		Vector2 fog_far_size;
	};

	void begin_frame();
	bool has_light(RID p_light) const;
	void free_light(RID p_light);
	void reduce_depth(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region);
	void fit_light(const RendererSDSM::Light &p_light, const LocalVector<RendererSDSM::Caster> &p_casters, const Vector<AABB> &p_extra_receivers, const FitSettings &p_settings);
	RID get_result_buffer(RID p_light) const;
	RID get_caster_buffer(RID p_light) const;
	RID get_directional_light_buffer() const { return directional_results; }
	void publish_directional_light(RID p_light, uint32_t p_index, const Rect2 *p_atlas_rects, float p_bias, float p_normal_bias, float p_transmittance_bias, float p_soft_shadow_scale, float p_fade_start);
	~SDSM();

private:
	enum Mode {
		SUMMARY,
		SUMMARY_MSAA,
		SCALAR_REDUCE,
		CLASSIFY,
		BUILD_RESCAN_ARGS,
		RESCAN,
		RESCAN_MSAA,
		FULL_BOUNDS,
		FULL_BOUNDS_MSAA,
		BOUNDS_REDUCE,
		SPLITS,
		CASTERS,
		FIT_RECEIVERS,
		FIT_CASTERS,
		PUBLISH_LIGHT,
		MODE_MAX
	};
	static constexpr uint32_t TILE_SIZE = 16;
	static constexpr uint32_t WORKGROUP_SIZE = 64;
	static constexpr uint32_t REDUCTION_FAN_IN = 256;
	struct CameraData {
		float clip_to_light[16];
		float camera_depth_row[4];
	};
	struct LightData {
		float camera_to_light[16];
		float light_to_world[16];
		float world_origin_low[4];
		float corners[8][4];
		float ranges[4];
		float extra_range[4];
		float settings[4];
		float fog[4];
		float fog_sizes[4];
		uint32_t counts[4];
	};
	struct CasterInput {
		float transform[12];
		Bounds local_bounds;
	};
	struct PushConstant {
		int32_t size[2];
		uint32_t groups_x;
		uint32_t output_offset;
		uint32_t input_count;
		uint32_t cascade_count;
		uint32_t sample_count;
		uint32_t pad;
		int32_t region_position[2];
		int32_t region_size[2];
		uint32_t records_per_view;
		uint32_t view_count;
		uint32_t dispatch_limit;
		uint32_t reserved;
	};
	struct PublishData {
		float atlas[4][4];
		float settings[4];
		float fade[4];
	};
	struct LightState {
		RID data;
		RID inputs;
		RID caster_data;
		RID result;
		RID publish;
		uint32_t capacity = 0;
		uint32_t caster_capacity = 0;
		bool active = false;
	};
	SdsmShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX] = {};
	RID summary_buffer;
	RID reduction_buffers[2];
	RID scalar_buffers[2];
	RID depth_range;
	RID rescan_tiles;
	RID rescan_counts;
	RID rescan_args;
	RID directional_results;
	Vector<RID> camera_buffers;
	Vector<RID> depth_views;
	RD::Uniform depth_uniforms[RendererSceneRender::MAX_RENDER_VIEWS];
	Vector<Projection> inverse_projections;
	Vector<Transform3D> view_to_cameras;
	Size2i depth_size;
	Rect2i depth_region;
	HashMap<RID, LightState> lights;
	Vector<Bounds> inputs;
	Vector<CasterInput> caster_inputs;
	uint32_t buffer_capacity = 0;
	uint32_t scalar_capacity = 0;
	bool depth_ready = false;

	void initialize();
	void ensure_buffers(uint32_t p_record_count, uint32_t p_scalar_count);
	RID reduce_scalar(RD::ComputeListID p_list, RID p_source, uint32_t p_count, PushConstant p_params, RID p_destination = RID());
	RID reduce_bounds(RD::ComputeListID p_list, uint32_t p_count, PushConstant p_params);
	void bind_stage(RD::ComputeListID p_list, Mode p_mode, RID p_output, std::initializer_list<RD::Uniform> p_inputs, const PushConstant &p_params);
};

} // namespace RendererRD
