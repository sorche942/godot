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

	Transform3D get_transform() override { return transform; }
	AABB get_aabb() override { return bounds; }
};

TEST_CASE("[Rendering][SDSM] Dynamic cascades retain every original caster") {
	PagedArrayPool<RenderGeometryInstance *> pool;
	RendererSceneRender::RenderShadowData shadows[4];
	for (auto &shadow : shadows) {
		shadow.instances.set_page_pool(&pool);
	}
	CasterInstance near_caster;
	CasterInstance middle_caster;
	CasterInstance far_caster;
	CasterInstance other_light_caster;
	shadows[0].instances.push_back(&near_caster);
	shadows[1].instances.push_back(&middle_caster);
	shadows[2].instances.push_back(&near_caster);
	shadows[2].instances.push_back(&far_caster);
	shadows[3].instances.push_back(&other_light_caster);

	RendererSDSM::Light light;
	light.cascade_count = 3;
	for (int i = 0; i < 3; i++) {
		light.shadow_indices[i] = i;
	}
	RendererSDSM sdsm;
	const auto &casters = sdsm.collect_casters(light, shadows);
	REQUIRE(casters.size() == 3);
	for (int i = 0; i < 3; i++) {
		HashSet<RenderGeometryInstance *> members;
		for (uint64_t j = 0; j < shadows[i].instances.size(); j++) {
			members.insert(shadows[i].instances[j]);
		}
		CHECK(members.size() == 3);
		CHECK(members.has(&near_caster));
		CHECK(members.has(&middle_caster));
		CHECK(members.has(&far_caster));
		CHECK_FALSE(members.has(&other_light_caster));
	}
	REQUIRE(shadows[3].instances.size() == 1);
	CHECK(shadows[3].instances[0] == &other_light_caster);

	// A subsequent light or frame must not inherit the previous union.
	light.cascade_count = 1;
	light.shadow_indices[0] = 3;
	const auto &next_casters = sdsm.collect_casters(light, shadows);
	REQUIRE(next_casters.size() == 1);
	CHECK(next_casters[0].instance == &other_light_caster);
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
	const auto &casters = sdsm.collect_casters(light, &shadow);
	REQUIRE(casters.size() == 1);
	CHECK(casters[0].bounds.position.is_equal_approx(Vector3(4, -1, -1)));
	CHECK(casters[0].bounds.size.is_equal_approx(Vector3(2, 2, 2)));
}

} // namespace TestRendererSDSM
