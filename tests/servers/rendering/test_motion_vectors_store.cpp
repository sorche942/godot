/**************************************************************************/
/*  test_motion_vectors_store.cpp                                         */
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
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "servers/rendering/renderer_rd/effects/motion_vectors_store.h"

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_motion_vectors_store)

namespace TestMotionVectorsStore {

static bool projections_are_equal_approx(const Projection &p_a, const Projection &p_b) {
	for (uint32_t column = 0; column < 4; column++) {
		for (uint32_t row = 0; row < 4; row++) {
			if (!Math::is_equal_approx(real_t(p_a.columns[column][row]), real_t(p_b.columns[column][row]))) {
				return false;
			}
		}
	}
	return true;
}

TEST_CASE("[Rendering][MotionVectorsStore] Mono reprojection matches camera projection formula") {
	Projection current_projection;
	current_projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 100.0);
	Projection previous_projection;
	previous_projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 100.0);

	Transform3D current_transform;
	current_transform.origin = Vector3(1.0, 2.0, 3.0);
	Transform3D previous_transform;
	previous_transform.origin = Vector3(0.75, 2.0, 3.5);

	Projection correction;
	correction.set_depth_correction(true, true, false);
	const Projection expected = (correction * previous_projection) * Projection(previous_transform.affine_inverse()) * Projection(current_transform) * (correction * current_projection).inverse();
	const Projection actual = RendererRD::MotionVectorsStore::get_reprojection(current_projection, current_transform, previous_projection, previous_transform);

	CHECK_MESSAGE(projections_are_equal_approx(actual, expected), "Mono must collapse to the pre-stereo camera reprojection formula.");
}

TEST_CASE("[Rendering][MotionVectorsStore] Stereo reprojection uses previous view projection") {
	Projection current_view_projection;
	current_view_projection.set_frustum(-0.9, 1.1, -1.0, 1.0, 0.05, 100.0);
	Projection previous_view_projection;
	previous_view_projection.set_frustum(-1.1, 0.9, -1.0, 1.0, 0.05, 100.0);

	Transform3D current_transform;
	current_transform.origin = Vector3(0.0, 0.0, 1.0);
	Transform3D previous_transform;
	previous_transform.origin = Vector3(0.2, 0.0, 1.0);

	const Projection actual = RendererRD::MotionVectorsStore::get_reprojection(current_view_projection, current_transform, previous_view_projection, previous_transform);
	const Projection buggy_current_reused_for_previous = RendererRD::MotionVectorsStore::get_reprojection(current_view_projection, current_transform, current_view_projection, previous_transform);

	CHECK_FALSE_MESSAGE(projections_are_equal_approx(actual, buggy_current_reused_for_previous), "Stereo reprojection must use previous_view_projection, not reuse current_view_projection for the previous frame.");
}

} // namespace TestMotionVectorsStore
