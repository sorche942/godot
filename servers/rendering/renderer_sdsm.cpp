/**************************************************************************/
/*  renderer_sdsm.cpp                                                     */
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

#include "renderer_sdsm.h"

#include "core/config/project_settings.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/light_storage.h"

bool RendererSDSM::is_enabled() {
	return GLOBAL_GET_CACHED(int, "rendering/lights_and_shadows/directional_shadow/mode") == 1;
}

bool RendererSDSM::initialize_light(Light &r_light, RID p_instance, RID p_base, const Transform3D &p_light_transform, const Projection &p_camera_projection, const Transform3D &p_camera_transform, bool p_orthogonal, const Vector2 &p_depth_range, const Vector<AABB> &p_extra_receivers, RendererSceneRender::RenderShadowData *p_shadows, int p_shadow_count) {
	r_light = Light();
	r_light.instance = p_instance;
	r_light.base = p_base;
	r_light.camera_projection = p_camera_projection;
	r_light.camera_transform = p_camera_transform;
	r_light.orthogonal = p_orthogonal;
	r_light.light_to_world = p_light_transform;
	r_light.light_to_world.orthonormalize();
	// Keep GPU receiver coordinates camera-relative, including in large worlds.
	r_light.light_to_world.origin = p_camera_transform.origin;
	r_light.blend_splits = RSG::light_storage->light_directional_get_blend_splits(p_base);

	for (int i = 0; i < p_shadow_count; i++) {
		if (p_shadows[i].light == p_instance && p_shadows[i].pass >= 0 && p_shadows[i].pass < 4) {
			r_light.shadow_indices[p_shadows[i].pass] = i;
			r_light.cascade_count = MAX(r_light.cascade_count, uint32_t(p_shadows[i].pass + 1));
		}
	}
	if (r_light.cascade_count == 0) {
		return false;
	}
	for (uint32_t i = 0; i < r_light.cascade_count; i++) {
		if (r_light.shadow_indices[i] < 0) {
			return false;
		}
	}

	const real_t camera_near = p_camera_projection.get_z_near();
	real_t shadow_far = p_camera_projection.get_z_far();
	const real_t shadow_max = RSG::light_storage->light_get_param(p_base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
	if (!p_orthogonal && shadow_max > 0) {
		shadow_far = MIN(shadow_far, shadow_max);
	}
	shadow_far = MAX(shadow_far, camera_near + real_t(0.001));

	real_t sample_min = shadow_far;
	real_t sample_max = camera_near;
	if (Math::is_finite(p_depth_range.x) && Math::is_finite(p_depth_range.y) && p_depth_range.x <= p_depth_range.y && p_depth_range.x <= shadow_far && p_depth_range.y >= camera_near) {
		sample_min = MAX(real_t(p_depth_range.x), camera_near);
		sample_max = MIN(real_t(p_depth_range.y), shadow_far);
	}
	const Transform3D world_to_camera = p_camera_transform.affine_inverse();
	for (const AABB &receiver : p_extra_receivers) {
		const AABB view_bounds = world_to_camera.xform(receiver);
		const real_t begin = -view_bounds.get_end().z;
		const real_t end = -view_bounds.position.z;
		if (end >= camera_near && begin <= shadow_far) {
			sample_min = MIN(sample_min, MAX(begin, camera_near));
			sample_max = MAX(sample_max, MIN(end, shadow_far));
		}
	}

	// An empty camera has no distribution to fit. Leave its existing CSM intact.
	if (sample_min > sample_max) {
		return false;
	}
	// Planar receivers still need distinct, finite interior split locations.
	const real_t minimum_range = MAX(real_t(0.001), Math::abs(sample_min) * real_t(0.0001));
	if (sample_max - sample_min < minimum_range) {
		sample_min = MAX(camera_near, sample_min - minimum_range * real_t(0.5));
		sample_max = MIN(shadow_far, sample_max + minimum_range * real_t(0.5));
	}

	// Intel's reduction-based logarithmic partitioning. Keep the outer intervals
	// at the camera/shadow limits so fade distance and receiver coverage don't move.
	r_light.distances[0] = camera_near;
	for (uint32_t i = 1; i < r_light.cascade_count; i++) {
		const real_t fraction = real_t(i) / r_light.cascade_count;
		r_light.distances[i] = sample_min > 0 ? sample_min * Math::pow(sample_max / sample_min, fraction) : Math::lerp(sample_min, sample_max, fraction);
	}
	r_light.distances[r_light.cascade_count] = shadow_far;
	return true;
}

float RendererSDSM::get_cascade_begin(const Light &p_light, uint32_t p_cascade) {
	return p_light.distances[p_light.blend_splits && p_cascade > 0 ? p_cascade - 1 : p_cascade];
}

void RendererSDSM::include_extra_receivers(const Light &p_light, const Vector<AABB> &p_extra_receivers, Bounds *r_bounds) {
	const Transform3D world_to_camera = p_light.camera_transform.affine_inverse();
	const Transform3D world_to_light = p_light.light_to_world.affine_inverse();
	for (const AABB &receiver : p_extra_receivers) {
		const AABB view_bounds = world_to_camera.xform(receiver);
		const real_t begin = -view_bounds.get_end().z;
		const real_t end = -view_bounds.position.z;
		const AABB light_bounds = world_to_light.xform(receiver);
		for (uint32_t i = 0; i < p_light.cascade_count; i++) {
			if (end >= get_cascade_begin(p_light, i) && begin <= p_light.distances[i + 1]) {
				if (r_bounds[i].valid) {
					r_bounds[i].aabb.merge_with(light_bounds);
				} else {
					r_bounds[i].aabb = light_bounds;
					r_bounds[i].valid = true;
				}
			}
		}
	}
}

void RendererSDSM::include_camera_volume(const Light &p_light, real_t p_length, Bounds *r_bounds) {
	if (p_length <= 0) {
		return;
	}
	const real_t camera_near = p_light.camera_projection.get_z_near();
	const real_t camera_far = p_light.camera_projection.get_z_far();
	const Vector2 near_size = p_light.camera_projection.get_viewport_half_extents();
	const Vector2 far_size = p_light.camera_projection.get_far_plane_half_extents();
	const Vector2 volume_far = near_size.lerp(far_size, (p_length - camera_near) / (camera_far - camera_near));
	const Vector2 volume_near = p_light.orthogonal ? volume_far : near_size.maxf(0.001);
	const Transform3D camera_to_light = p_light.light_to_world.affine_inverse() * p_light.camera_transform;
	for (uint32_t i = 0; i < p_light.cascade_count; i++) {
		const real_t begin = i == 0 ? 0 : get_cascade_begin(p_light, i);
		const real_t end = MIN(real_t(p_light.distances[i + 1]), p_length);
		if (begin > end) {
			continue;
		}
		for (int plane = 0; plane < 2; plane++) {
			const real_t depth = plane == 0 ? begin : end;
			const Vector2 half_size = volume_near.lerp(volume_far, depth / p_length);
			for (int corner = 0; corner < 4; corner++) {
				const Vector3 point = camera_to_light.xform(Vector3(corner & 1 ? half_size.x : -half_size.x, corner & 2 ? half_size.y : -half_size.y, -depth));
				if (r_bounds[i].valid) {
					r_bounds[i].aabb.expand_to(point);
				} else {
					r_bounds[i].aabb = AABB(point, Vector3());
					r_bounds[i].valid = true;
				}
			}
		}
	}
}

void RendererSDSM::apply_light(const Light &p_light, const Bounds *p_bounds, RendererSceneRender::RenderShadowData *p_shadows) {
	caster_set.clear();
	casters.clear();
	const Transform3D world_to_light = p_light.light_to_world.affine_inverse();
	// Dynamic partition boundaries may cross every original CSM split. A new
	// cascade must consider the union, not merely its old per-split caster list.
	for (uint32_t i = 0; i < p_light.cascade_count; i++) {
		const PagedArray<RenderGeometryInstance *> &instances = p_shadows[p_light.shadow_indices[i]].instances;
		for (uint64_t j = 0; j < instances.size(); j++) {
			RenderGeometryInstance *instance = instances[j];
			if (!caster_set.has(instance)) {
				caster_set.insert(instance);
				casters.push_back({ instance, (world_to_light * instance->get_transform()).xform(instance->get_aabb()) });
			}
		}
	}

	const real_t resolution = MAX(RSG::light_storage->get_directional_light_shadow_size(p_light.instance), 16);
	const real_t blur = MAX(RSG::light_storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_BLUR), 0.0f);
	const real_t normal_bias = Math::abs(RSG::light_storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_NORMAL_BIAS));
	const real_t pancake = MAX(RSG::light_storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE), 0.0f);
	const real_t angular_size = Math::tan(Math::deg_to_rad(MAX(RSG::light_storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SIZE), 0.0f)));

	Vector3 frustum_corners[8];
	const bool has_frustum = p_light.camera_projection.get_endpoints(p_light.camera_transform, frustum_corners);
	ERR_FAIL_COND(!has_frustum);
	const real_t camera_near = p_light.camera_projection.get_z_near();
	const real_t camera_range = p_light.camera_projection.get_z_far() - camera_near;
	ERR_FAIL_COND(camera_range <= 0);

	for (uint32_t i = 0; i < p_light.cascade_count; i++) {
		AABB receivers;
		if (p_bounds[i].valid && p_bounds[i].aabb.position.is_finite() && p_bounds[i].aabb.size.is_finite()) {
			receivers = p_bounds[i].aabb;
		} else {
			// Empty intervals still have valid shadow cameras and matching split
			// metadata (needed by blending and consumers that don't write depth).
			const real_t begin = (get_cascade_begin(p_light, i) - camera_near) / camera_range;
			const real_t end = (p_light.distances[i + 1] - camera_near) / camera_range;
			for (int corner = 0; corner < 4; corner++) {
				const Vector3 near_corner = frustum_corners[corner + 4];
				const Vector3 ray = frustum_corners[corner] - near_corner;
				const Vector3 a = world_to_light.xform(near_corner + ray * begin);
				const Vector3 b = world_to_light.xform(near_corner + ray * end);
				if (corner == 0) {
					receivers = AABB(a, Vector3());
				} else {
					receivers.expand_to(a);
				}
				receivers.expand_to(b);
			}
		}

		Vector3 lower = receivers.position;
		Vector3 upper = receivers.get_end();
		real_t caster_far = upper.z;
		for (const Caster &caster : casters) {
			caster_far = MAX(caster_far, caster.bounds.get_end().z);
		}
		const real_t initial_texel = MAX(MAX(receivers.size.x, receivers.size.y), real_t(0.001)) / resolution;
		const real_t filter_border = initial_texel * (real_t(2) + real_t(8) * blur + normal_bias);
		const real_t soft_border = angular_size * blur * MAX(caster_far - lower.z, pancake);
		const real_t x_border = MAX(receivers.size.x * real_t(0.01), real_t(0.001)) + filter_border + soft_border;
		const real_t y_border = MAX(receivers.size.y * real_t(0.01), real_t(0.001)) + filter_border + soft_border;
		lower.x -= x_border;
		upper.x += x_border;
		lower.y -= y_border;
		upper.y += y_border;

		const real_t texel = MAX(upper.x - lower.x, upper.y - lower.y) / resolution;
		lower.x = Math::floor(lower.x / texel) * texel;
		lower.y = Math::floor(lower.y / texel) * texel;
		upper.x = Math::ceil(upper.x / texel) * texel;
		upper.y = Math::ceil(upper.y / texel) * texel;

		PagedArray<RenderGeometryInstance *> &instances = p_shadows[p_light.shadow_indices[i]].instances;
		instances.clear();
		for (const Caster &caster : casters) {
			const Vector3 caster_end = caster.bounds.get_end();
			if (caster_end.x < lower.x || caster.bounds.position.x > upper.x || caster_end.y < lower.y || caster.bounds.position.y > upper.y || caster_end.z < lower.z - normal_bias * texel) {
				continue;
			}
			instances.push_back(caster.instance);
			upper.z = MAX(upper.z, caster_end.z);
		}

		const real_t depth_border = MAX((upper.z - lower.z) * real_t(0.01), real_t(0.001)) + normal_bias * texel;
		lower.z -= depth_border;
		upper.z += depth_border;
		const Vector3 extent = upper - lower;
		Projection projection;
		projection.set_orthogonal(-extent.x * real_t(0.5), extent.x * real_t(0.5), -extent.y * real_t(0.5), extent.y * real_t(0.5), 0, extent.z);
		Transform3D transform = p_light.light_to_world;
		transform.origin = p_light.light_to_world.xform(Vector3((lower.x + upper.x) * real_t(0.5), (lower.y + upper.y) * real_t(0.5), upper.z));
		const Vector2 uv_scale(real_t(1) / extent.x, real_t(1) / extent.y);
		RSG::light_storage->light_instance_set_shadow_transform(p_light.instance, projection, transform, extent.z, p_light.distances[i + 1], i, texel, extent.z, upper.z, uv_scale);
	}
}
