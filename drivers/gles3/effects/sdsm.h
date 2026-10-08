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

#ifdef GLES3_ENABLED

#include "drivers/gles3/shaders/effects/sdsm.glsl.gen.h"
#include "servers/rendering/renderer_sdsm.h"

namespace GLES3 {

// Pixel-shader reductions work on GLES 3 without compute, readback or float blending.
class SDSM {
	struct Level {
		Size2i size;
		GLuint texture = 0;
		GLuint framebuffer = 0;
	};
	SdsmShaderGLES3 shader;
	RID shader_version;
	GLuint triangle = 0;
	GLuint vertex_array = 0;
	GLuint depth_framebuffer = 0;
	Vector<GLuint> depth_textures;
	Vector<Level> levels;
	Level depth_ranges;
	Vector<Level> paired_levels; // Receiver maxima reduced alongside minima.
	Level tile_summary[3]; // Depth interval, light-space minimum, light-space maximum.
	Level splits;
	Level bounds;
	Level fitted_bounds;
	Level final_bounds;
	Vector<Level> caster_levels;
	Level cameras;
	GLuint records_texture = 0;
	int records_height = 0;
	Vector<float> records;
	RID lights[RendererSceneRender::MAX_DIRECTIONAL_LIGHTS];
	int light_count = 0;
	Size2i size;
	Rect2i region;
	bool unsupported = false;
	void _free_buffers();
	bool _unsupported();
	bool _create_target(Level &r_target, const Size2i &p_size);
	void _bind_source(GLuint p_texture, int p_unit);
	void _draw(const Level &p_target, const Rect2i &p_rect, int p_color_count = 1);
	void _reduce(GLuint p_depth, const Projection &p_inverse_projection, const Transform3D &p_view_to_camera, const Transform3D &p_camera_to_light, int p_operation, int p_cascade);
	void _copy_result(const Level &p_target, int p_x, int p_y);

public:
	SDSM();
	~SDSM();
	bool prepare(const Size2i &p_size, uint32_t p_views, const Rect2i &p_region);
	void bind_depth(uint32_t p_view);
	void reduce_depth(const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera);
	bool fit_light(const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const RendererSDSM::Light &p_light, const Vector<AABB> &p_receivers, const LocalVector<RendererSDSM::Caster> &p_casters);
	int get_light_index(RID p_light) const;
	GLuint get_camera_texture() const { return cameras.texture; }
};

} // namespace GLES3
#endif
