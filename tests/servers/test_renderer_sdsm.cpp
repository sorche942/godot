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

#include "servers/rendering/dummy/rasterizer_scene_dummy.h"
#include "servers/rendering/renderer_sdsm.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_renderer_sdsm)

namespace TestRendererSDSM {

class CasterInstance : public RasterizerSceneDummy::GeometryInstanceDummy {
public:
	Transform3D transform;
	AABB bounds = AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2));

	Transform3D get_transform() override {
		return transform;
	}
	AABB get_aabb() override {
		return bounds;
	}
};

static bool inside_volume(const Vector<Plane> &p_planes, const Vector3 &p_point) {
	for (const Plane &plane : p_planes) {
		if (plane.distance_to(p_point) > 0.001) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[Rendering][SDSM] Full-distance volume retains off-screen upstream casters") {
	Projection projection;
	projection.set_perspective(70, 1.7, 0.1, 100);
	const Transform3D camera(Basis(Vector3(1, 0, 0), 0.3), Vector3(10000, 20, -5000));
	const Basis light_basis(Vector3(0, 1, 0), 0.6);
	const Vector<Plane> planes = RendererSDSM::candidate_planes(projection, camera, light_basis, 100, 2048, 2, 0, 1);
	Vector3 endpoints[8];
	REQUIRE(projection.get_endpoints(camera, endpoints));
	const Vector3 upstream = light_basis.get_column(2) * 2000000;
	for (const Vector3 &endpoint : endpoints) {
		CHECK(inside_volume(planes, endpoint));
		CHECK(inside_volume(planes, endpoint + upstream));
	}
	CHECK_FALSE(inside_volume(planes, camera.origin - upstream));
}

TEST_CASE("[Rendering][SDSM] Angular soft shadows retain arbitrarily distant lateral occluders") {
	Projection projection;
	projection.set_orthogonal(20, 1.5, 0.1, 50);
	const Vector<Plane> planes = RendererSDSM::candidate_planes(projection, Transform3D(), Basis(), 50, 1024, 2, 2, 1);
	CHECK(inside_volume(planes, Vector3(100000, -100000, 1000000)));
	CHECK_FALSE(inside_volume(planes, Vector3(0, 0, -1000)));
}

TEST_CASE("[Rendering][SDSM] Asymmetric camera footprint and normal padding remain conservative") {
	Projection projection;
	projection.set_frustum(-0.3, 0.8, -0.2, 0.4, 0.5, 40);
	const Vector<Plane> planes = RendererSDSM::candidate_planes(projection, Transform3D(), Basis(), 20, 512, 3, 0, 0);
	Vector3 endpoints[8];
	REQUIRE(projection.get_endpoints(Transform3D(), endpoints));
	for (int i = 0; i < 4; i++) {
		const Vector3 far_point = endpoints[i + 4].lerp(endpoints[i], real_t(19.5 / 39.5));
		CHECK(inside_volume(planes, far_point));
		CHECK(inside_volume(planes, far_point + Vector3(0.01, 0, 0)));
	}
	CHECK_FALSE(inside_volume(planes, Vector3(1000, 0, -10)));
}

TEST_CASE("[Rendering][SDSM] Caster fitting inputs retain rotated large-world bounds") {
	PagedArrayPool<RenderGeometryInstance *> pool;
	RendererSceneRender::RenderShadowData shadow;
	shadow.instances.set_page_pool(&pool);
	CasterInstance caster;
	caster.transform.origin = Vector3(10000, 0, -5);
	shadow.instances.push_back(&caster);
	RendererSDSM::Light light;
	light.cascade_count = 1;
	light.shadow_indices[0] = 0;
	light.light_to_world = Transform3D(Basis(Vector3(0, 1, 0), Math::PI / 2), Vector3(10000, 0, 0));
	RendererSDSM sdsm;
	const auto &casters = sdsm.prepare_casters(light, shadow.instances, true);
	REQUIRE(casters.size() == 1);
	CHECK(casters[0].bounds.position.is_equal_approx(Vector3(4, -1, -1)));
	CHECK(casters[0].bounds.size.is_equal_approx(Vector3(2, 2, 2)));
}

} // namespace TestRendererSDSM
