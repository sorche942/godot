/**************************************************************************/
/*  renderer_sdsm.h                                                       */
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

#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_scene_render.h"

// Prepare camera-relative inputs and conservative CPU caster lists. Partition
// placement and shadow-camera fitting remain entirely on the GPU.
class RendererSDSM {
public:
	struct Caster {
		RenderGeometryInstance *instance = nullptr;
		AABB bounds;
	};

	struct Light {
		RID instance;
		RID base;
		Transform3D light_to_world;
		Projection camera_projection;
		Transform3D camera_transform;
		float camera_near = 0.0f;
		float camera_far = 0.0f;
		float shadow_far = 0.0f;
		int shadow_indices[4] = { -1, -1, -1, -1 };
		uint32_t cascade_count = 0;
		bool blend_splits = false;
		bool orthogonal = false;
	};

	static bool is_enabled();
	static bool prepare_light(Light &r_light, RID p_instance, RID p_base, const Transform3D &p_light_transform, const Projection &p_camera_projection, const Transform3D &p_camera_transform, bool p_orthogonal, RendererSceneRender::RenderShadowData *p_shadows, int p_shadow_count);

	// Full-distance receiver footprint extruded upstream toward the light.
	static Vector<Plane> candidate_planes(const Projection &p_camera_projection, const Transform3D &p_camera_transform, const Basis &p_light_basis, real_t p_shadow_far, real_t p_texture_size, real_t p_normal_bias, real_t p_soft_angle, real_t p_blur);

	// Candidates are unique and remain owned by the cull invocation. RD derives
	// bounds on the GPU; Compatibility requests exact camera-relative bounds.
	const LocalVector<Caster> &prepare_casters(const Light &p_light, const PagedArray<RenderGeometryInstance *> &p_candidates, bool p_light_space_bounds);

private:
	LocalVector<Caster> casters;
};
