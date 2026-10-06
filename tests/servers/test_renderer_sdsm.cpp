/**************************************************************************/
/*  test_renderer_sdsm.cpp                                                */
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

#include "servers/rendering/renderer_sdsm.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_renderer_sdsm)

namespace TestRendererSDSM {

TEST_CASE("[Rendering][SDSM] Nondepth receivers span partition boundaries") {
	RendererSDSM::Light light;
	light.cascade_count = 3;
	light.distances[0] = 0.1f;
	light.distances[1] = 10;
	light.distances[2] = 30;
	light.distances[3] = 90;
	RendererSDSM::Bounds bounds[4];
	Vector<AABB> receivers;
	receivers.push_back(AABB(Vector3(-1, -1, -12), Vector3(2, 2, 4)));
	RendererSDSM::include_extra_receivers(light, receivers, bounds);
	CHECK(bounds[0].valid);
	CHECK(bounds[1].valid);
	CHECK_FALSE(bounds[2].valid);
	CHECK(bounds[0].aabb.has_point(Vector3(0, 0, -11)));
	CHECK(bounds[1].aabb.has_point(Vector3(0, 0, -9)));

	// Receivers outside the shadow distance must not enlarge any map.
	receivers.clear();
	receivers.push_back(AABB(Vector3(-100, -100, -110), Vector3(200, 200, 5)));
	RendererSDSM::include_extra_receivers(light, receivers, bounds);
	CHECK_FALSE(bounds[2].valid);
	CHECK(bounds[0].aabb.size.is_equal_approx(Vector3(2, 2, 4)));
}

TEST_CASE("[Rendering][SDSM] Cascade blending retains earlier nondepth receivers") {
	RendererSDSM::Light light;
	light.cascade_count = 3;
	light.blend_splits = true;
	light.distances[0] = 0.1f;
	light.distances[1] = 10;
	light.distances[2] = 30;
	light.distances[3] = 90;
	RendererSDSM::Bounds bounds[4];
	Vector<AABB> receivers;
	receivers.push_back(AABB(Vector3(-1, -1, -6), Vector3(2, 2, 2)));
	RendererSDSM::include_extra_receivers(light, receivers, bounds);
	CHECK(bounds[0].valid);
	CHECK(bounds[1].valid);
	CHECK_FALSE(bounds[2].valid);

	receivers.clear();
	receivers.push_back(AABB(Vector3(-1, -1, -18), Vector3(2, 2, 2)));
	RendererSDSM::include_extra_receivers(light, receivers, bounds);
	CHECK(bounds[2].valid);
	CHECK(bounds[1].aabb.has_point(Vector3(0, 0, -17)));
	CHECK(bounds[2].aabb.has_point(Vector3(0, 0, -17)));
}

TEST_CASE("[Rendering][SDSM] Receiver bounds remain camera-relative in rotated light space") {
	RendererSDSM::Light light;
	light.cascade_count = 1;
	light.distances[0] = 0.1f;
	light.distances[1] = 90;
	light.camera_transform.origin = Vector3(10000, 0, 0);
	light.light_to_world = Transform3D(Basis(Vector3(0, 1, 0), Math::PI / 2), light.camera_transform.origin);
	RendererSDSM::Bounds bounds[4];
	Vector<AABB> receivers;
	receivers.push_back(AABB(Vector3(9999, -1, -6), Vector3(2, 2, 2)));
	RendererSDSM::include_extra_receivers(light, receivers, bounds);
	CHECK(bounds[0].valid);
	CHECK(bounds[0].aabb.position.is_equal_approx(Vector3(4, -1, -1)));
	CHECK(bounds[0].aabb.size.is_equal_approx(Vector3(2, 2, 2)));
}

TEST_CASE("[Rendering][SDSM] Fog volume retains shadow coverage away from visible surfaces") {
	RendererSDSM::Light light;
	light.cascade_count = 3;
	light.orthogonal = true;
	light.camera_projection.set_orthogonal(-10, 10, -5, 5, 1, 100);
	light.distances[0] = 1;
	light.distances[1] = 10;
	light.distances[2] = 20;
	light.distances[3] = 100;
	RendererSDSM::Bounds bounds[4];
	RendererSDSM::include_camera_volume(light, 30, bounds);
	CHECK(bounds[0].aabb.has_point(Vector3(9, 4, 0)));
	CHECK(bounds[1].aabb.has_point(Vector3(9, 4, -15)));
	CHECK(bounds[2].aabb.has_point(Vector3(9, 4, -29)));
	CHECK_FALSE(bounds[2].aabb.has_point(Vector3(9, 4, -40)));
}

} // namespace TestRendererSDSM
