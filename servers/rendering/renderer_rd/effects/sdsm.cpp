/**************************************************************************/
/*  sdsm.cpp                                                              */
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

#include "sdsm.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include <cfloat>

using namespace RendererRD;

void SDSM::initialize() {
	if (shader_version.is_valid()) {
		return;
	}
	Vector<String> modes;
	modes.push_back("\n#define REDUCE_DEPTH\n");
	modes.push_back("\n");
	modes.push_back("\n#define REDUCE_RECORDS\n");
	modes.push_back("\n#define REDUCE_DEPTH\n#define MULTISAMPLE\n");
	modes.push_back("\n#define MULTISAMPLE\n");
	modes.push_back("\n#define MAKE_SPLITS\n");
	modes.push_back("\n#define FIT_BOUNDS\n");
	String precision;
#ifdef REAL_T_IS_DOUBLE
	precision = "\n#define DOUBLE_WORLD\n";
#endif
	modes.push_back("\n#define PATCH_SCENE\n" + precision);
	modes.push_back("\n#define PATCH_LIGHT\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	depth_range = RD::get_singleton()->storage_buffer_create(sizeof(Reduction) * 4);
}

void SDSM::begin_frame() {
	for (KeyValue<RID, LightState> &entry : lights) {
		entry.value.active = false;
	}
	depth_views.clear();
}

bool SDSM::has_light(RID p_light) const {
	const LightState *state = lights.getptr(p_light);
	return state && state->active;
}

void SDSM::free_light(RID p_light) {
	LightState *state = lights.getptr(p_light);
	if (!state) {
		return;
	}
	RD *rd = RD::get_singleton();
	rd->free_rid(state->data);
	rd->free_rid(state->inputs);
	rd->free_rid(state->result);
	rd->free_rid(state->patch);
	lights.erase(p_light);
}

SDSM::~SDSM() {
	RD *rd = RD::get_singleton();
	for (const KeyValue<RID, LightState> &entry : lights) {
		rd->free_rid(entry.value.data);
		rd->free_rid(entry.value.inputs);
		rd->free_rid(entry.value.result);
		rd->free_rid(entry.value.patch);
	}
	for (RID buffer : camera_buffers) {
		rd->free_rid(buffer);
	}
	for (RID buffer : reduction_buffers) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
	}
	if (depth_range.is_valid()) {
		rd->free_rid(depth_range);
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

RID SDSM::reduce(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region, const RendererSDSM::Light *p_light) {
	ERR_FAIL_COND_V(p_depth.size() != p_inverse_projection.size() || p_depth.size() != p_view_to_camera.size(), RID());
	ERR_FAIL_COND_V(p_depth.is_empty() || p_size.x <= 0 || p_size.y <= 0, RID());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *materials = MaterialStorage::get_singleton();
	initialize();
	const uint32_t groups_x = (p_size.x + 7) / 8;
	const uint32_t groups_y = (p_size.y + 7) / 8;
	const uint32_t records_per_view = groups_x * groups_y;
	const uint32_t record_count = records_per_view * p_depth.size();
	if (record_count > buffer_capacity) {
		for (RID &buffer : reduction_buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
			buffer = rd->storage_buffer_create(record_count * sizeof(Reduction) * 4);
		}
		buffer_capacity = record_count;
	}
	while (camera_buffers.size() < p_depth.size()) {
		camera_buffers.push_back(rd->uniform_buffer_create(sizeof(CameraData)));
	}
	for (int view = 0; view < p_depth.size(); view++) {
		CameraData data = {};
		MaterialStorage::store_camera(p_inverse_projection[view], data.inverse_projection);
		MaterialStorage::store_camera(Projection(p_view_to_camera[view]), data.view_to_camera);
		if (p_light) {
			MaterialStorage::store_camera(Projection(p_light->light_to_world.affine_inverse() * p_light->camera_transform * p_view_to_camera[view]), data.view_to_light);
		}
		rd->buffer_update(camera_buffers[view], 0, sizeof(CameraData), &data);
	}
	RID sampler = materials->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform output(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ reduction_buffers[0] }));
	PushConstant params = {};
	params.size[0] = p_size.x;
	params.size[1] = p_size.y;
	params.groups_x = groups_x;
	params.cascade_count = p_light ? p_light->cascade_count : 0;
	const Rect2i region = p_region.has_area() ? p_region : Rect2i(Point2i(), p_size);
	params.region_position[0] = region.position.x;
	params.region_position[1] = region.position.y;
	params.region_size[0] = region.size.x;
	params.region_size[1] = region.size.y;
	RD::ComputeListID list = rd->compute_list_begin();
	for (int view = 0; view < p_depth.size(); view++) {
		RD::TextureSamples samples = rd->texture_get_format(p_depth[view]).samples;
		Mode mode = p_light ? BOUNDS : DEPTH;
		if (samples != RD::TEXTURE_SAMPLES_1) {
			mode = p_light ? BOUNDS_MSAA : DEPTH_MSAA;
		}
		params.sample_count = 1u << samples;
		params.output_offset = view * records_per_view;
		RID shader_rid = shader.version_get_shader(shader_version, mode);
		RD::Uniform depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_depth[view] }));
		RD::Uniform camera(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, Vector<RID>({ camera_buffers[view] }));
		rd->compute_list_bind_compute_pipeline(list, pipelines[mode]);
		rd->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 0, output), 0);
		if (p_light) {
			LightState &state = lights[p_light->instance];
			RD::Uniform data(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, Vector<RID>({ state.data }));
			RD::Uniform result(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ state.result }));
			rd->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 1, depth, camera, data, result), 1);
		} else {
			rd->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 1, depth, camera), 1);
		}
		rd->compute_list_set_push_constant(list, &params, sizeof(params));
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
	}
	uint32_t count = record_count;
	uint32_t source = 0;
	RID reduction_shader = shader.version_get_shader(shader_version, REDUCE);
	while (count > 1) {
		rd->compute_list_add_barrier(list);
		params.input_count = count;
		uint32_t groups = (count + 63) / 64;
		RD::Uniform input(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ reduction_buffers[source] }));
		RD::Uniform destination(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ reduction_buffers[1 - source] }));
		rd->compute_list_bind_compute_pipeline(list, pipelines[REDUCE]);
		rd->compute_list_bind_uniform_set(list, cache->get_cache(reduction_shader, 0, destination), 0);
		rd->compute_list_bind_uniform_set(list, cache->get_cache(reduction_shader, 1, input), 1);
		params.groups_x = MIN(groups, uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X)));
		rd->compute_list_set_push_constant(list, &params, sizeof(params));
		rd->compute_list_dispatch(list, params.groups_x, (groups + params.groups_x - 1) / params.groups_x, 1);
		count = groups;
		source = 1 - source;
	}
	rd->compute_list_end();
	return reduction_buffers[source];
}

void SDSM::dispatch(Mode p_mode, RID p_output, RID p_input, RID p_data, RID p_result, const PushConstant &p_params) {
	RD *rd = RD::get_singleton();
	RID shader_rid = shader.version_get_shader(shader_version, p_mode);
	RD::Uniform output(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ p_output }));
	RD::Uniform input(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ p_input }));
	RD::Uniform data(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, Vector<RID>({ p_data }));
	RD::Uniform result(p_mode == PATCH_LIGHT ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ p_result }));
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipelines[p_mode]);
	rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, output), 0);
	if (p_mode == SPLITS || p_mode == PATCH_SCENE) {
		rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 1, input, data), 1);
	} else {
		rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 1, input, data, result), 1);
	}
	rd->compute_list_set_push_constant(list, &p_params, sizeof(p_params));
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
}

void SDSM::reduce_depth(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region) {
	begin_frame();
	depth_views = p_depth;
	inverse_projections = p_inverse_projection;
	view_to_cameras = p_view_to_camera;
	depth_size = p_size;
	depth_region = p_region;
	RID reduced = reduce(p_depth, p_inverse_projection, p_view_to_camera, p_size, p_region, nullptr);
	ERR_FAIL_COND(reduced.is_null());
	RD::get_singleton()->buffer_copy(reduced, depth_range, 0, 0, sizeof(Reduction) * 4);
}

void SDSM::fit_light(const RendererSDSM::Light &p_light, const LocalVector<RendererSDSM::Caster> &p_casters, const Vector<AABB> &p_extra_receivers, const FitSettings &p_settings) {
	ERR_FAIL_COND(depth_views.is_empty());
	RD *rd = RD::get_singleton();
	LightState &state = lights[p_light.instance];
	if (state.data.is_null()) {
		state.data = rd->uniform_buffer_create(sizeof(LightData));
		state.result = rd->storage_buffer_create(sizeof(Result));
		state.patch = rd->uniform_buffer_create(sizeof(PatchData));
	}
	const uint32_t input_count = MAX(1u, uint32_t(p_casters.size() + p_extra_receivers.size()));
	if (state.capacity < input_count) {
		if (state.inputs.is_valid()) {
			rd->free_rid(state.inputs);
		}
		state.capacity = MAX(input_count, state.capacity * 2);
		state.inputs = rd->storage_buffer_create(state.capacity * sizeof(InputBounds));
	}
	LightData data = {};
	const Transform3D camera_to_light = p_light.light_to_world.affine_inverse() * p_light.camera_transform;
	MaterialStorage::store_camera(Projection(camera_to_light), data.camera_to_light);
	MaterialStorage::store_camera(Projection(p_light.light_to_world), data.light_to_world);
#ifdef REAL_T_IS_DOUBLE
	for (int axis = 0; axis < 3; axis++) {
		MaterialStorage::split_double(p_light.light_to_world.origin[axis], &data.light_to_world[12 + axis], &data.world_origin_low[axis]);
	}
#endif
	Vector3 corners[8];
	ERR_FAIL_COND(!p_light.camera_projection.get_endpoints(camera_to_light, corners));
	for (int i = 0; i < 8; i++) {
		for (int axis = 0; axis < 3; axis++) {
			data.corners[i][axis] = corners[i][axis];
		}
	}
	data.ranges[0] = p_light.camera_near;
	data.ranges[1] = p_light.camera_far;
	data.ranges[2] = p_light.shadow_far;
	data.ranges[3] = p_light.blend_splits ? 1 : 0;
	data.extra_range[0] = FLT_MAX;
	data.extra_range[1] = -FLT_MAX;
	data.settings[0] = MAX(p_settings.resolution, 16.0f);
	data.settings[1] = MAX(p_settings.blur, 0.0f);
	data.settings[2] = Math::abs(p_settings.normal_bias);
	data.settings[3] = MAX(p_settings.angular_size, 0.0f);
	data.fog[0] = p_settings.fog_length;
	data.fog[1] = MAX(p_settings.pancake, 0.0f);
	data.fog_sizes[0] = p_settings.fog_near_size.x;
	data.fog_sizes[1] = p_settings.fog_near_size.y;
	data.fog_sizes[2] = p_settings.fog_far_size.x;
	data.fog_sizes[3] = p_settings.fog_far_size.y;
	data.counts[0] = p_light.cascade_count;
	data.counts[1] = p_casters.size();
	data.counts[2] = p_extra_receivers.size();
	inputs.resize(input_count);
	const Transform3D world_to_light = p_light.light_to_world.affine_inverse();
	const Transform3D world_to_camera = p_light.camera_transform.affine_inverse();
	for (uint32_t i = 0; i < input_count; i++) {
		InputBounds bounds = {};
		if (i < p_casters.size()) {
			const AABB &aabb = p_casters[i].bounds;
			for (int axis = 0; axis < 3; axis++) {
				bounds.minimum[axis] = aabb.position[axis];
				bounds.maximum[axis] = aabb.get_end()[axis];
			}
		} else if (i < p_casters.size() + p_extra_receivers.size()) {
			const AABB &world = p_extra_receivers[i - p_casters.size()];
			const AABB light = world_to_light.xform(world);
			const AABB camera = world_to_camera.xform(world);
			for (int axis = 0; axis < 3; axis++) {
				bounds.minimum[axis] = light.position[axis];
				bounds.maximum[axis] = light.get_end()[axis];
			}
			bounds.minimum[3] = -camera.get_end().z;
			bounds.maximum[3] = -camera.position.z;
			if (bounds.maximum[3] >= p_light.camera_near && bounds.minimum[3] <= p_light.shadow_far) {
				data.extra_range[0] = MIN(data.extra_range[0], MAX(bounds.minimum[3], p_light.camera_near));
				data.extra_range[1] = MAX(data.extra_range[1], MIN(bounds.maximum[3], p_light.shadow_far));
			}
		}
		inputs.write[i] = bounds;
	}
	if (p_settings.fog_length > p_light.camera_near) {
		data.extra_range[0] = MIN(data.extra_range[0], p_light.camera_near);
		data.extra_range[1] = MAX(data.extra_range[1], MIN(p_settings.fog_length, p_light.shadow_far));
	}
	rd->buffer_update(state.data, 0, sizeof(data), &data);
	rd->buffer_update(state.inputs, 0, input_count * sizeof(InputBounds), inputs.ptr());
	PushConstant params = {};
	dispatch(SPLITS, state.result, depth_range, state.data, state.inputs, params);
	RID bounds = reduce(depth_views, inverse_projections, view_to_cameras, depth_size, depth_region, &p_light);
	ERR_FAIL_COND(bounds.is_null());
	dispatch(FIT, state.result, bounds, state.data, state.inputs, params);
	state.active = true;
}

void SDSM::patch_scene_data(RID p_light, uint32_t p_cascade, RID p_buffer, uint32_t p_byte_offset, bool p_flip_y, uint32_t p_view_count) {
	if (!has_light(p_light)) {
		return;
	}
	LightState &state = lights[p_light];
	PushConstant params = {};
	params.output_offset = p_byte_offset / sizeof(float);
	params.cascade_count = p_cascade;
	params.sample_count = p_view_count;
	params.pad = p_flip_y;
	dispatch(PATCH_SCENE, p_buffer, state.result, state.data, state.inputs, params);
}

void SDSM::patch_directional_light(RID p_light, uint32_t p_index, RID p_buffer, const Rect2 *p_atlas_rects, float p_bias, float p_normal_bias, float p_transmittance_bias, float p_soft_shadow_scale, float p_fade_start) {
	if (!has_light(p_light)) {
		return;
	}
	LightState &state = lights[p_light];
	PatchData data = {};
	for (int i = 0; i < 4; i++) {
		data.atlas[i][0] = p_atlas_rects[i].position.x;
		data.atlas[i][1] = p_atlas_rects[i].position.y;
		data.atlas[i][2] = p_atlas_rects[i].size.x;
		data.atlas[i][3] = p_atlas_rects[i].size.y;
	}
	data.settings[0] = p_bias / 100.0f;
	data.settings[1] = p_normal_bias;
	data.settings[2] = p_transmittance_bias / 100.0f;
	data.settings[3] = p_soft_shadow_scale;
	data.fade[0] = MIN(p_fade_start, 0.999f);
	RD::get_singleton()->buffer_update(state.patch, 0, sizeof(data), &data);
	PushConstant params = {};
	params.output_offset = p_index * 116;
	dispatch(PATCH_LIGHT, p_buffer, state.result, state.data, state.patch, params);
}
