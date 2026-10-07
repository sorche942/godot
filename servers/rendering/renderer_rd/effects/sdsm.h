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
	enum Mode { DEPTH, BOUNDS, REDUCE, DEPTH_MSAA, BOUNDS_MSAA, SPLITS, FIT, PATCH_SCENE, PATCH_LIGHT, MODE_MAX };
	struct Reduction { float minimum[4]; float maximum[4]; };
	struct CameraData {
		float inverse_projection[16];
		float view_to_camera[16];
		float view_to_light[16];
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
	struct InputBounds { float minimum[4]; float maximum[4]; };
	struct Result { float splits[4]; InputBounds bounds[4]; float texel[4]; };
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
	};
	struct PatchData {
		float atlas[4][4];
		float settings[4];
		float fade[4];
	};
	struct LightState {
		RID data;
		RID inputs;
		RID result;
		RID patch;
		uint32_t capacity = 0;
		bool active = false;
	};
	SdsmShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID reduction_buffers[2];
	RID depth_range;
	Vector<RID> camera_buffers;
	Vector<RID> depth_views;
	Vector<Projection> inverse_projections;
	Vector<Transform3D> view_to_cameras;
	Size2i depth_size;
	Rect2i depth_region;
	HashMap<RID, LightState> lights;
	Vector<InputBounds> inputs;
	uint32_t buffer_capacity = 0;
	void initialize();
	RID reduce(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region, const RendererSDSM::Light *p_light);
	void dispatch(Mode p_mode, RID p_output, RID p_input, RID p_data, RID p_result, const PushConstant &p_params);

public:
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
	void patch_scene_data(RID p_light, uint32_t p_cascade, RID p_buffer, uint32_t p_byte_offset, bool p_flip_y, uint32_t p_view_count = 1);
	void patch_directional_light(RID p_light, uint32_t p_index, RID p_buffer, const Rect2 *p_atlas_rects, float p_bias, float p_normal_bias, float p_transmittance_bias, float p_soft_shadow_scale, float p_fade_start);
	~SDSM();
};

} // namespace RendererRD
