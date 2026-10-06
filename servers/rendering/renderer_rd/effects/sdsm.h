/**************************************************************************/
/*  sdsm.h                                                                */
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

#include "servers/rendering/renderer_rd/shaders/effects/sdsm.glsl.gen.h"
#include "servers/rendering/renderer_sdsm.h"

namespace RendererRD {

class SDSM {
	enum Mode {
		DEPTH,
		BOUNDS,
		REDUCE,
		DEPTH_MSAA,
		BOUNDS_MSAA,
		MODE_MAX,
	};

	// std430: four pairs of vec4 minima/maxima. Empty intervals have min > max.
	struct Reduction {
		float minimum[4];
		float maximum[4];
	};
	struct CameraData {
		float inverse_projection[16];
		float view_to_camera[16];
		float view_to_light[16];
		float cascade_begin[4];
		float cascade_end[4];
	};
	struct PushConstant {
		int32_t size[2];
		uint32_t groups_x;
		uint32_t output_offset;
		uint32_t input_count;
		uint32_t cascade_count;
		uint32_t sample_count;
		uint32_t pad;
		int32_t region_position[2];
		int32_t region_size[2];
	};

	SdsmShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID reduction_buffers[2];
	Vector<RID> camera_buffers;
	uint32_t buffer_capacity = 0;

	void initialize();
	Vector<uint8_t> reduce(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region, const RendererSDSM::Light *p_light);

public:
	// Include every eye, with the inverse of its actual corrected reverse-Z raster
	// projection. Multisample depth views are read without resolving their samples.
	Vector2 reduce_depth(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region);
	void reduce_bounds(const Vector<RID> &p_depth, const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const Size2i &p_size, const Rect2i &p_region, const RendererSDSM::Light &p_light, RendererSDSM::Bounds *r_bounds);
	~SDSM();
};

} // namespace RendererRD
