/**************************************************************************/
/*  test_dlss.cpp                                                         */
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

#include "servers/rendering/renderer_rd/effects/dlss.h"

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_dlss)

namespace TestDLSS {

TEST_CASE("[Rendering][DLSS] Motion vector scale converts Godot UVs to DLSS pixels") {
	const Vector2 scale = RendererRD::dlss_get_motion_vector_scale(Size2i(1920, 1080));

	CHECK_MESSAGE(scale.x == doctest::Approx(1920.0), "DLSS X motion keeps Godot's UV direction and scales to render pixels.");
	CHECK_MESSAGE(scale.y == doctest::Approx(1080.0), "DLSS Y motion keeps Godot's top-left screen-space direction and scales to render pixels.");
}

TEST_CASE("[Rendering][DLSS] Zero render size keeps zero motion scale") {
	const Vector2 scale = RendererRD::dlss_get_motion_vector_scale(Size2i());

	CHECK_MESSAGE(scale.is_zero_approx(), "Degenerate render sizes must not synthesize motion.");
}

} // namespace TestDLSS
