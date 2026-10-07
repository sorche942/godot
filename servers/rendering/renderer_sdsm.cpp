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

	r_light.camera_near = camera_near;
	r_light.camera_far = p_camera_projection.get_z_far();
	r_light.shadow_far = shadow_far;
	return true;
}

const LocalVector<RendererSDSM::Caster> &RendererSDSM::collect_casters(const Light &p_light, RendererSceneRender::RenderShadowData *p_shadows) {
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

	for (uint32_t i = 0; i < p_light.cascade_count; i++) {
		PagedArray<RenderGeometryInstance *> &instances = p_shadows[p_light.shadow_indices[i]].instances;
		instances.clear();
		for (const Caster &caster : casters) {
			instances.push_back(caster.instance);
		}
	}
	return casters;
}
