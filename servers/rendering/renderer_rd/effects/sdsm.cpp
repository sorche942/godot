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
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

SDSM::~SDSM() {
	RD *rd = RD::get_singleton();
	for (RID buffer : camera_buffers) {
		rd->free_rid(buffer);
	}
	for (RID buffer : reduction_buffers) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

Vector<uint8_t> SDSM::reduce(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region, const RendererSDSM::Light *p_light) {
	ERR_FAIL_COND_V(p_depth.size() != p_inverse_projection.size() || p_depth.size() != p_view_to_camera.size(), Vector<uint8_t>());
	if (p_depth.is_empty() || p_size.x <= 0 || p_size.y <= 0) {
		return Vector<uint8_t>();
	}
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL_V(cache, Vector<uint8_t>());
	MaterialStorage *materials = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(materials, Vector<uint8_t>());
	initialize();

	const uint32_t groups_x = (p_size.x + 7) / 8;
	const uint32_t groups_y = (p_size.y + 7) / 8;
	const uint32_t records_per_view = groups_x * groups_y;
	const uint32_t record_count = records_per_view * p_depth.size();
	const uint32_t record_size = sizeof(Reduction) * 4;
	if (record_count > buffer_capacity) {
		for (RID &buffer : reduction_buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
			buffer = rd->storage_buffer_create(record_count * record_size);
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
			for (uint32_t cascade = 0; cascade < p_light->cascade_count; cascade++) {
				data.cascade_begin[cascade] = RendererSDSM::get_cascade_begin(*p_light, cascade);
				data.cascade_end[cascade] = p_light->distances[cascade + 1];
			}
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
		rd->compute_list_bind_uniform_set(list, cache->get_cache(shader_rid, 1, depth, camera), 1);
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
	// Only one 128-byte record crosses to the CPU, never the depth image or tiles.
	return rd->buffer_get_data(reduction_buffers[source], 0, record_size);
}

Vector2 SDSM::reduce_depth(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region) {
	Vector<uint8_t> data = reduce(p_depth, p_inverse_projection, p_view_to_camera, p_size, p_region, nullptr);
	if (data.is_empty()) {
		return Vector2(1, 0);
	}
	Reduction result;
	memcpy(&result, data.ptr(), sizeof(result));
	return Vector2(result.minimum[0], result.maximum[0]);
}

void SDSM::reduce_bounds(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region, const RendererSDSM::Light &p_light, RendererSDSM::Bounds *r_bounds) {
	ERR_FAIL_NULL(r_bounds);
	ERR_FAIL_COND(p_light.cascade_count > 4);
	for (uint32_t cascade = 0; cascade < p_light.cascade_count; cascade++) {
		r_bounds[cascade] = RendererSDSM::Bounds();
	}
	Vector<uint8_t> data = reduce(p_depth, p_inverse_projection, p_view_to_camera, p_size, p_region, &p_light);
	if (data.is_empty()) {
		return;
	}
	Reduction results[4];
	memcpy(results, data.ptr(), sizeof(results));
	for (uint32_t cascade = 0; cascade < p_light.cascade_count; cascade++) {
		Vector3 minimum(results[cascade].minimum[0], results[cascade].minimum[1], results[cascade].minimum[2]);
		Vector3 maximum(results[cascade].maximum[0], results[cascade].maximum[1], results[cascade].maximum[2]);
		if (minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z) {
			r_bounds[cascade].aabb = AABB(minimum, maximum - minimum);
			r_bounds[cascade].valid = true;
		}
	}
}
