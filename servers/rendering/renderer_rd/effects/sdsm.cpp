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
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

#include <cfloat>

using namespace RendererRD;

void SDSM::initialize() {
	if (shader_version.is_valid()) {
		return;
	}
	RD *rd = RD::get_singleton();
	String defines;
	if ((rd->limit_get(RD::LIMIT_SUBGROUP_IN_SHADERS) & RD::SHADER_STAGE_COMPUTE_BIT) &&
			(rd->limit_get(RD::LIMIT_SUBGROUP_OPERATIONS) & RD::SUBGROUP_ARITHMETIC_BIT)) {
		defines += "\n#define USE_SUBGROUPS\n";
		const uint32_t minimum_size = MAX(1u, uint32_t(rd->limit_get(RD::LIMIT_SUBGROUP_MIN_SIZE)));
		defines += "#define SDSM_MAX_SUBGROUPS " + itos((WORKGROUP_SIZE + minimum_size - 1) / minimum_size) + "\n";
	}
#ifdef REAL_T_IS_DOUBLE
	defines += "\n#define DOUBLE_WORLD\n";
#endif
	Vector<String> modes = {
		"\n#define TILE_SUMMARY\n", "\n#define TILE_SUMMARY\n#define MULTISAMPLE\n",
		"\n#define REDUCE_SCALARS\n", "\n#define CLASSIFY_TILES\n", "\n#define BUILD_RESCAN_ARGS\n",
		"\n#define RESCAN_TILES\n", "\n#define RESCAN_TILES\n#define MULTISAMPLE\n",
		"\n#define FULL_BOUNDS\n", "\n#define FULL_BOUNDS\n#define MULTISAMPLE\n",
		"\n#define REDUCE_BOUNDS\n", "\n#define MAKE_SPLITS\n", "\n#define TRANSFORM_CASTERS\n",
		"\n#define FIT_RECEIVERS\n", "\n#define FIT_CASTERS\n", "\n#define PUBLISH_LIGHT\n"
	};
	for (String &mode : modes) {
		mode += defines;
	}
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = rd->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	depth_range = rd->storage_buffer_create(sizeof(float) * 2);
	rescan_counts = rd->storage_buffer_create(sizeof(uint32_t) * 4);
	rescan_args = rd->storage_buffer_create(RendererSceneRender::MAX_RENDER_VIEWS * sizeof(uint32_t) * 6, {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	directional_results = rd->uniform_buffer_create(sizeof(Result) * RendererSceneRender::MAX_DIRECTIONAL_LIGHTS, {}, RD::BUFFER_CREATION_AS_STORAGE_BIT);
	rd->buffer_clear(directional_results, 0, sizeof(Result) * RendererSceneRender::MAX_DIRECTIONAL_LIGHTS);
}

void SDSM::begin_frame() {
	for (KeyValue<RID, LightState> &entry : lights) {
		entry.value.active = false;
	}
	depth_views.clear();
	depth_ready = false;
}

bool SDSM::has_light(RID p_light) const {
	const LightState *state = lights.getptr(p_light);
	return state && state->active;
}

RID SDSM::get_result_buffer(RID p_light) const {
	const LightState *state = lights.getptr(p_light);
	return state && state->active ? state->result : RID();
}

RID SDSM::get_caster_buffer(RID p_light) const {
	const LightState *state = lights.getptr(p_light);
	return state && state->active ? state->inputs : RID();
}

void SDSM::free_light(RID p_light) {
	LightState *state = lights.getptr(p_light);
	if (!state) {
		return;
	}
	for (RID buffer : { state->data, state->inputs, state->caster_data, state->result, state->publish }) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	lights.erase(p_light);
}

SDSM::~SDSM() {
	RD *rd = RD::get_singleton();
	for (const KeyValue<RID, LightState> &entry : lights) {
		for (RID buffer : { entry.value.data, entry.value.inputs, entry.value.caster_data, entry.value.result, entry.value.publish }) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
	}
	for (RID buffer : camera_buffers) {
		rd->free_rid(buffer);
	}
	for (RID buffer : { summary_buffer, reduction_buffers[0], reduction_buffers[1], scalar_buffers[0], scalar_buffers[1],
				 depth_range, rescan_tiles, rescan_counts, rescan_args, directional_results }) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

void SDSM::ensure_buffers(uint32_t p_record_count, uint32_t p_scalar_count) {
	RD *rd = RD::get_singleton();
	if (p_record_count > buffer_capacity) {
		for (RID buffer : { summary_buffer, reduction_buffers[0], reduction_buffers[1], rescan_tiles }) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		buffer_capacity = p_record_count;
		summary_buffer = rd->storage_buffer_create(buffer_capacity * sizeof(Bounds));
		reduction_buffers[0] = rd->storage_buffer_create(buffer_capacity * sizeof(Bounds) * 4);
		reduction_buffers[1] = rd->storage_buffer_create(MAX(1u, (buffer_capacity + REDUCTION_FAN_IN - 1) / REDUCTION_FAN_IN) * sizeof(Bounds) * 4);
		rescan_tiles = rd->storage_buffer_create(buffer_capacity * sizeof(uint32_t));
	}
	if (p_scalar_count > scalar_capacity) {
		for (RID buffer : scalar_buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		scalar_capacity = p_scalar_count;
		scalar_buffers[0] = rd->storage_buffer_create(scalar_capacity * sizeof(float) * 2);
		scalar_buffers[1] = rd->storage_buffer_create(MAX(1u, (scalar_capacity + REDUCTION_FAN_IN - 1) / REDUCTION_FAN_IN) * sizeof(float) * 2);
	}
}

void SDSM::bind_stage(RD::ComputeListID p_list, Mode p_mode, RID p_output, std::initializer_list<RD::Uniform> p_inputs, const PushConstant &p_params) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID shader_rid = shader.version_get_shader(shader_version, p_mode);
	RD::Uniform output(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_output);
	const RD::Uniform *u = p_inputs.begin();
	RID input_set;
	// Stack-resident Uniforms keep this hot path allocation-free for buffers.
	switch (p_inputs.size()) {
		case 1:
			input_set = cache->get_cache(shader_rid, 1, u[0]);
			break;
		case 2:
			input_set = cache->get_cache(shader_rid, 1, u[0], u[1]);
			break;
		case 3:
			input_set = cache->get_cache(shader_rid, 1, u[0], u[1], u[2]);
			break;
		case 4:
			input_set = cache->get_cache(shader_rid, 1, u[0], u[1], u[2], u[3]);
			break;
		case 5:
			input_set = cache->get_cache(shader_rid, 1, u[0], u[1], u[2], u[3], u[4]);
			break;
		case 6:
			input_set = cache->get_cache(shader_rid, 1, u[0], u[1], u[2], u[3], u[4], u[5]);
			break;
		default:
			ERR_FAIL_MSG("Invalid SDSM stage inputs.");
	}
	rd->compute_list_bind_compute_pipeline(p_list, pipelines[p_mode]);
	rd->compute_list_bind_uniform_set(p_list, cache->get_cache(shader_rid, 0, output), 0);
	rd->compute_list_bind_uniform_set(p_list, input_set, 1);
	rd->compute_list_set_push_constant(p_list, &p_params, sizeof(p_params));
}

RID SDSM::reduce_scalar(RD::ComputeListID p_list, RID p_source, uint32_t p_count, PushConstant p_params, RID p_destination) {
	RD *rd = RD::get_singleton();
	bool final_write = p_destination.is_valid();
	while (p_count > 1 || final_write) {
		rd->compute_list_add_barrier(p_list);
		const uint32_t groups = MAX(1u, (p_count + REDUCTION_FAN_IN - 1) / REDUCTION_FAN_IN);
		RID destination = groups == 1 && p_destination.is_valid() ? p_destination : (p_source == scalar_buffers[0] ? scalar_buffers[1] : scalar_buffers[0]);
		p_params.input_count = p_count;
		p_params.groups_x = MIN(groups, uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X)));
		bind_stage(p_list, SCALAR_REDUCE, destination, { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_source) }, p_params);
		rd->compute_list_dispatch(p_list, p_params.groups_x, (groups + p_params.groups_x - 1) / p_params.groups_x, 1);
		p_count = groups;
		p_source = destination;
		if (groups == 1) {
			break;
		}
	}
	return p_source;
}

RID SDSM::reduce_bounds(RD::ComputeListID p_list, uint32_t p_count, PushConstant p_params) {
	RD *rd = RD::get_singleton();
	RID source = reduction_buffers[0];
	while (p_count > 1) {
		rd->compute_list_add_barrier(p_list);
		const uint32_t groups = (p_count + REDUCTION_FAN_IN - 1) / REDUCTION_FAN_IN;
		RID destination = source == reduction_buffers[0] ? reduction_buffers[1] : reduction_buffers[0];
		p_params.input_count = p_count;
		p_params.groups_x = MIN(groups, uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X)));
		bind_stage(p_list, BOUNDS_REDUCE, destination, { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, source) }, p_params);
		rd->compute_list_dispatch(p_list, p_params.groups_x, (groups + p_params.groups_x - 1) / p_params.groups_x, 1);
		p_count = groups;
		source = destination;
	}
	return source;
}

void SDSM::reduce_depth(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region) {
	ERR_FAIL_COND(p_depth.is_empty() || p_depth.size() > RendererSceneRender::MAX_RENDER_VIEWS);
	ERR_FAIL_COND(p_depth.size() != p_inverse_projection.size() || p_depth.size() != p_view_to_camera.size());
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0);
	begin_frame();
	depth_views = p_depth;
	inverse_projections = p_inverse_projection;
	view_to_cameras = p_view_to_camera;
	depth_size = p_size;
	depth_region = p_region.has_area() ? p_region.intersection(Rect2i(Point2i(), p_size)) : Rect2i(Point2i(), p_size);
	// The first light's tile analysis also produces the shared depth extrema.
	// Do not add a separate full-resolution depth traversal.
	if (directional_results.is_valid()) {
		RD::get_singleton()->buffer_clear(directional_results, 0, sizeof(Result) * RendererSceneRender::MAX_DIRECTIONAL_LIGHTS);
	}
}

void SDSM::fit_light(const RendererSDSM::Light &p_light, const LocalVector<RendererSDSM::Caster> &p_casters, const Vector<AABB> &p_extra_receivers, const FitSettings &p_settings) {
	ERR_FAIL_COND(depth_views.is_empty());
	RD *rd = RD::get_singleton();
	initialize();
	LightState &state = lights[p_light.instance];
	if (state.data.is_null()) {
		state.data = rd->uniform_buffer_create(sizeof(LightData));
		state.result = rd->uniform_buffer_create(sizeof(Result), {}, RD::BUFFER_CREATION_AS_STORAGE_BIT);
		state.publish = rd->uniform_buffer_create(sizeof(PublishData));
	}
	const uint32_t input_count = MAX(1u, uint32_t(p_casters.size() + p_extra_receivers.size()));
	if (state.capacity < input_count) {
		if (state.inputs.is_valid()) {
			rd->free_rid(state.inputs);
		}
		state.capacity = MAX(input_count, state.capacity * 2);
		state.inputs = rd->storage_buffer_create(state.capacity * sizeof(Bounds));
	}
	const uint32_t caster_count = MAX(1u, uint32_t(p_casters.size()));
	if (state.caster_capacity < caster_count) {
		if (state.caster_data.is_valid()) {
			rd->free_rid(state.caster_data);
		}
		state.caster_capacity = MAX(caster_count, state.caster_capacity * 2);
		state.caster_data = rd->storage_buffer_create(state.caster_capacity * sizeof(CasterInput));
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

	caster_inputs.resize(p_casters.size());
	for (uint32_t i = 0; i < p_casters.size(); i++) {
		RenderGeometryInstance *instance = p_casters[i].instance;
		Transform3D transform = instance->get_transform();
		transform.origin -= p_light.camera_transform.origin;
		CasterInput &caster = caster_inputs.write[i];
		MaterialStorage::store_transform_transposed_3x4(transform, caster.transform);
		const AABB bounds = instance->get_aabb();
		for (int axis = 0; axis < 3; axis++) {
			caster.local_bounds.minimum[axis] = bounds.position[axis];
			caster.local_bounds.maximum[axis] = bounds.get_end()[axis];
		}
		caster.local_bounds.minimum[3] = caster.local_bounds.maximum[3] = 0;
	}
	if (!caster_inputs.is_empty()) {
		rd->buffer_update(state.caster_data, 0, caster_inputs.size() * sizeof(CasterInput), caster_inputs.ptr());
	}
	inputs.resize(p_extra_receivers.size());
	const Transform3D world_to_light = p_light.light_to_world.affine_inverse();
	const Transform3D world_to_camera = p_light.camera_transform.affine_inverse();
	for (int i = 0; i < p_extra_receivers.size(); i++) {
		const AABB &world = p_extra_receivers[i];
		const AABB light_bounds = world_to_light.xform(world);
		const AABB camera_bounds = world_to_camera.xform(world);
		Bounds &bounds = inputs.write[i];
		for (int axis = 0; axis < 3; axis++) {
			bounds.minimum[axis] = light_bounds.position[axis];
			bounds.maximum[axis] = light_bounds.get_end()[axis];
		}
		bounds.minimum[3] = -camera_bounds.get_end().z;
		bounds.maximum[3] = -camera_bounds.position.z;
		if (bounds.maximum[3] >= p_light.camera_near && bounds.minimum[3] <= p_light.shadow_far) {
			data.extra_range[0] = MIN(data.extra_range[0], MAX(bounds.minimum[3], p_light.camera_near));
			data.extra_range[1] = MAX(data.extra_range[1], MIN(bounds.maximum[3], p_light.shadow_far));
		}
	}
	if (!inputs.is_empty()) {
		rd->buffer_update(state.inputs, p_casters.size() * sizeof(Bounds), inputs.size() * sizeof(Bounds), inputs.ptr());
	}
	if (p_settings.fog_length > p_light.camera_near) {
		data.extra_range[0] = MIN(data.extra_range[0], p_light.camera_near);
		data.extra_range[1] = MAX(data.extra_range[1], MIN(p_settings.fog_length, p_light.shadow_far));
	}
	rd->buffer_update(state.data, 0, sizeof(data), &data);

	const uint32_t groups_x = (depth_size.x + TILE_SIZE - 1) / TILE_SIZE;
	const uint32_t groups_y = (depth_size.y + TILE_SIZE - 1) / TILE_SIZE;
	const uint32_t records_per_view = groups_x * groups_y;
	const uint32_t record_count = records_per_view * depth_views.size();
	const uint32_t caster_groups = MAX(1u, (uint32_t(p_casters.size()) + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE);
	ensure_buffers(record_count, MAX(record_count, caster_groups));
	while (camera_buffers.size() < depth_views.size()) {
		camera_buffers.push_back(rd->uniform_buffer_create(sizeof(CameraData)));
	}
	for (int view = 0; view < depth_views.size(); view++) {
		CameraData camera = {};
		const Projection clip_to_camera = Projection(view_to_cameras[view]) * inverse_projections[view];
		MaterialStorage::store_camera(Projection(camera_to_light) * clip_to_camera, camera.clip_to_light);
		for (int column = 0; column < 4; column++) {
			camera.camera_depth_row[column] = clip_to_camera[column][2];
		}
		rd->buffer_update(camera_buffers[view], 0, sizeof(camera), &camera);
	}
	PushConstant params = {};
	params.size[0] = depth_size.x;
	params.size[1] = depth_size.y;
	params.groups_x = groups_x;
	params.input_count = record_count;
	params.cascade_count = p_light.cascade_count;
	params.region_position[0] = depth_region.position.x;
	params.region_position[1] = depth_region.position.y;
	params.region_size[0] = depth_region.size.x;
	params.region_size[1] = depth_region.size.y;
	params.records_per_view = records_per_view;
	params.view_count = depth_views.size();
	params.dispatch_limit = rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X);
	const RD::Uniform light_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, state.data);
	const RD::Uniform result_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, state.result);
	const RD::Uniform input_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, state.inputs);
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RENDER_TIMESTAMP("SDSM Tile Summaries");
	RD::ComputeListID list = rd->compute_list_begin();
	auto stage = [&](const char *p_name) {
		if (RSG::utilities->capturing_timestamps) {
			rd->compute_list_end();
			RENDER_TIMESTAMP(p_name);
			list = rd->compute_list_begin();
		} else {
			rd->compute_list_add_barrier(list);
		}
	};
	for (int view = 0; view < depth_views.size(); view++) {
		RD::Uniform &depth_uniform = depth_uniforms[view];
		if (depth_uniform.get_id_count() != 2 || depth_uniform.get_id(1) != depth_views[view]) {
			depth_uniform = RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth_views[view] }));
		}
		const RD::TextureSamples samples = rd->texture_get_format(depth_views[view]).samples;
		params.sample_count = 1u << samples;
		params.output_offset = view * records_per_view;
		params.pad = depth_ready ? 0 : 1;
		bind_stage(list, samples == RD::TEXTURE_SAMPLES_1 ? SUMMARY : SUMMARY_MSAA, summary_buffer, { depth_uniform, RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, camera_buffers[view]), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, scalar_buffers[0]) }, params);
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
	}
	stage("SDSM Splits");
	if (!depth_ready) {
		reduce_scalar(list, scalar_buffers[0], record_count, params, depth_range);
		depth_ready = true;
	}
	rd->compute_list_add_barrier(list);
	params.output_offset = 0;
	bind_stage(list, SPLITS, state.result, { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, depth_range), light_uniform }, params);
	rd->compute_list_dispatch(list, 1, 1, 1);
	// Clear only the tiny counters, outside the active compute list.
	rd->compute_list_end();
	rd->buffer_clear(rescan_counts, 0, sizeof(uint32_t) * 4);
	RENDER_TIMESTAMP("SDSM Tile Classification");
	list = rd->compute_list_begin();
	params.input_count = record_count;
	const uint32_t classify_groups = (record_count + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
	params.groups_x = MIN(classify_groups, params.dispatch_limit);
	bind_stage(list, CLASSIFY, reduction_buffers[0], { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, summary_buffer), light_uniform, result_uniform, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, rescan_tiles), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, rescan_counts) }, params);
	rd->compute_list_dispatch(list, params.groups_x, (classify_groups + params.groups_x - 1) / params.groups_x, 1);
	rd->compute_list_add_barrier(list);
	bind_stage(list, BUILD_RESCAN_ARGS, rescan_args, { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, rescan_counts) }, params);
	rd->compute_list_dispatch(list, 1, 1, 1);
	stage("SDSM Boundary Rescan");
	for (int view = 0; view < depth_views.size(); view++) {
		const RD::TextureSamples samples = rd->texture_get_format(depth_views[view]).samples;
		params.sample_count = 1u << samples;
		params.output_offset = view * records_per_view;
		for (int full = 0; full < 2; full++) {
			params.groups_x = full ? groups_x : params.dispatch_limit;
			const Mode mode = full ? (samples == RD::TEXTURE_SAMPLES_1 ? FULL_BOUNDS : FULL_BOUNDS_MSAA) : (samples == RD::TEXTURE_SAMPLES_1 ? RESCAN : RESCAN_MSAA);
			const RD::Uniform &depth = depth_uniforms[view];
			RD::Uniform camera(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, camera_buffers[view]);
			if (full) {
				bind_stage(list, mode, reduction_buffers[0], { depth, camera, light_uniform, result_uniform }, params);
			} else {
				bind_stage(list, mode, reduction_buffers[0], { depth, camera, light_uniform, result_uniform, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, rescan_tiles), RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, rescan_counts) }, params);
			}
			rd->compute_list_dispatch_indirect(list, rescan_args, (view * 6 + full * 3) * sizeof(uint32_t));
		}
	}
	stage("SDSM Receiver Reduction");
	RID receiver_bounds = reduce_bounds(list, record_count, params);
	stage("SDSM Caster Bounds");
	params.input_count = p_casters.size();
	params.groups_x = MIN(caster_groups, params.dispatch_limit);
	bind_stage(list, CASTERS, state.inputs, { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, state.caster_data), light_uniform, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, scalar_buffers[0]) }, params);
	rd->compute_list_dispatch(list, params.groups_x, (caster_groups + params.groups_x - 1) / params.groups_x, 1);
	RID caster_range = reduce_scalar(list, scalar_buffers[0], caster_groups, params);
	stage("SDSM Cascade Fit");
	bind_stage(list, FIT_RECEIVERS, state.result, { RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, receiver_bounds), light_uniform, input_uniform, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, caster_range) }, params);
	rd->compute_list_dispatch(list, p_light.cascade_count, 1, 1);
	rd->compute_list_add_barrier(list);
	bind_stage(list, FIT_CASTERS, state.result, { light_uniform, input_uniform }, params);
	rd->compute_list_dispatch(list, p_light.cascade_count, 1, 1);
	rd->compute_list_end();
	RENDER_TIMESTAMP("SDSM Fit Complete");
	state.active = true;
}

void SDSM::publish_directional_light(RID p_light, uint32_t p_index, const Rect2 *p_atlas_rects, float p_bias, float p_normal_bias, float p_transmittance_bias, float p_soft_shadow_scale, float p_fade_start) {
	if (!has_light(p_light)) {
		return;
	}
	ERR_FAIL_COND(p_index >= RendererSceneRender::MAX_DIRECTIONAL_LIGHTS);
	LightState &state = lights[p_light];
	PublishData data = {};
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
	RD *rd = RD::get_singleton();
	rd->buffer_update(state.publish, 0, sizeof(data), &data);
	PushConstant params = {};
	params.output_offset = p_index;
	RD::ComputeListID list = rd->compute_list_begin();
	bind_stage(list, PUBLISH_LIGHT, directional_results, { RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, state.result), RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, state.data), RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, state.publish) }, params);
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
}
