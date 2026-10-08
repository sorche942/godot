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

bool RendererSDSM::prepare_light(Light &r_light, RID p_instance, RID p_base, const Transform3D &p_light_transform, const Projection &p_camera_projection, const Transform3D &p_camera_transform, bool p_orthogonal, RendererSceneRender::RenderShadowData *p_shadows, int p_shadow_count) {
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
		if (p_shadows[i].sdsm && p_shadows[i].light == p_instance && p_shadows[i].pass >= 0 && p_shadows[i].pass < 4) {
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

	r_light.camera_near = camera_near;
	r_light.camera_far = p_camera_projection.get_z_far();
	r_light.shadow_far = shadow_far;
	return true;
}

Vector<Plane> RendererSDSM::candidate_planes(const Projection &p_camera_projection, const Transform3D &p_camera_transform, const Basis &p_light_basis, real_t p_shadow_far, real_t p_texture_size, real_t p_normal_bias, real_t p_soft_angle, real_t p_blur) {
	Vector3 endpoints[8];
	ERR_FAIL_COND_V(!p_camera_projection.get_endpoints(Transform3D(), endpoints), Vector<Plane>());
	const real_t near_distance = p_camera_projection.get_z_near();
	const real_t far_distance = p_camera_projection.get_z_far();
	const real_t fraction = CLAMP((p_shadow_far - near_distance) / (far_distance - near_distance), real_t(0), real_t(1));
	Vector3 center;
	for (int i = 0; i < 4; i++) {
		endpoints[i] = endpoints[i + 4].lerp(endpoints[i], fraction);
	}
	// Work relative to the camera origin before projecting onto the light axes.
	// This also preserves asymmetric and canted projections without rebuilding FOV.
	for (Vector3 &endpoint : endpoints) {
		endpoint = p_camera_transform.basis.xform(endpoint);
		center += endpoint;
	}
	center /= 8;
	real_t radius = 0;
	Vector3 minimum;
	Vector3 maximum;
	for (int i = 0; i < 8; i++) {
		radius = MAX(radius, center.distance_to(endpoints[i]));
		Vector3 point = p_light_basis.xform_inv(endpoints[i]);
		if (i == 0) {
			minimum = maximum = point;
		} else {
			minimum = minimum.min(point);
			maximum = maximum.max(point);
		}
	}
	// Cover filter taps, stabilization and normal offset using the largest
	// possible full-distance texel, rather than the old split texel sizes.
	const real_t texel = radius * 2 / MAX(p_texture_size - 2, real_t(1));
	// Eight texels covers the largest directional filter quality radius.
	const real_t padding = texel * (4 + 64 * MAX(p_blur, real_t(0)) + Math::abs(p_normal_bias));
	minimum -= Vector3(padding, padding, padding);
	maximum += Vector3(padding, padding, padding);
	Vector<Plane> planes;
	// DynamicBVH does not expose root bounds. Angular PCSS expansion depends
	// on the furthest upstream caster, not just receiver depth, so no finite
	// receiver-only XY box can conservatively reject soft-shadow candidates.
	const bool angular_soft_shadows = p_soft_angle > 0 && p_blur > 0;
	planes.resize(angular_soft_shadows ? 1 : 5);
	for (int axis = 0; axis < (angular_soft_shadows ? 0 : 2); axis++) {
		const Vector3 normal = p_light_basis.get_column(axis);
		const real_t origin = normal.dot(p_camera_transform.origin);
		planes.write[axis * 2] = Plane(normal, maximum[axis] + origin);
		planes.write[axis * 2 + 1] = Plane(-normal, -minimum[axis] - origin);
	}
	const Vector3 z = p_light_basis.get_column(2);
	planes.write[planes.size() - 1] = Plane(-z, -minimum.z - z.dot(p_camera_transform.origin));
	// No upstream plane: off-screen occluders can cast onto receivers from any
	// distance along the directional light, including beyond the camera far plane.
	return planes;
}

const LocalVector<RendererSDSM::Caster> &RendererSDSM::prepare_casters(const Light &p_light, const PagedArray<RenderGeometryInstance *> &p_candidates, bool p_light_space_bounds) {
	casters.clear();
	const Transform3D world_to_light = p_light_space_bounds ? p_light.light_to_world.affine_inverse() : Transform3D();
	for (uint64_t i = 0; i < p_candidates.size(); i++) {
		RenderGeometryInstance *instance = p_candidates[i];
		casters.push_back({ instance, p_light_space_bounds ? (world_to_light * instance->get_transform()).xform(instance->get_aabb()) : AABB() });
	}
	return casters;
}
