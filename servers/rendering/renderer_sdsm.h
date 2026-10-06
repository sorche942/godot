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

#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_scene_render.h"

// Backend-independent partition placement and shadow-camera fitting. GPU backends
// supply current-frame depth extrema and per-partition receiver bounds.
class RendererSDSM {
	struct Caster {
		RenderGeometryInstance *instance = nullptr;
		AABB bounds;
	};

	HashSet<RenderGeometryInstance *> caster_set;
	LocalVector<Caster> casters;

public:
	struct Bounds {
		AABB aabb;
		bool valid = false;
	};

	struct Light {
		RID instance;
		RID base;
		Transform3D light_to_world;
		Projection camera_projection;
		Transform3D camera_transform;
		float distances[5] = {};
		int shadow_indices[4] = { -1, -1, -1, -1 };
		uint32_t cascade_count = 0;
		bool blend_splits = false;
		bool orthogonal = false;
	};

	static bool is_enabled();

	// Extra receivers cover transparent and other surfaces absent from camera depth.
	// Their world-space bounds contribute to both depth placement and receiver fitting.
	static bool initialize_light(Light &r_light, RID p_instance, RID p_base, const Transform3D &p_light_transform, const Projection &p_camera_projection, const Transform3D &p_camera_transform, bool p_orthogonal, const Vector2 &p_depth_range, const Vector<AABB> &p_extra_receivers, RendererSceneRender::RenderShadowData *p_shadows, int p_shadow_count);
	static float get_cascade_begin(const Light &p_light, uint32_t p_cascade);
	static void include_extra_receivers(const Light &p_light, const Vector<AABB> &p_extra_receivers, Bounds *r_bounds);
	static void include_camera_volume(const Light &p_light, real_t p_length, Bounds *r_bounds);
	void apply_light(const Light &p_light, const Bounds *p_bounds, RendererSceneRender::RenderShadowData *p_shadows);
};
