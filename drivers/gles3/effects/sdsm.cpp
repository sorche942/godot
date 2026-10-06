/**************************************************************************/
/*  sdsm.cpp                                                              */
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

#include "sdsm.h"

#ifdef GLES3_ENABLED
#include "drivers/gles3/storage/texture_storage.h"

#include <cfloat>

namespace GLES3 {

SDSM::SDSM() {
	shader.initialize();
	shader_version = shader.version_create();
	const float vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers(1, &triangle);
	glGenVertexArrays(1, &vertex_array);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, triangle);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SDSM::_free_buffers() {
	for (const Level &level : levels) {
		glDeleteTextures(1, &level.texture);
		glDeleteFramebuffers(1, &level.framebuffer);
	}
	levels.clear();
	if (!depth_textures.is_empty()) {
		glDeleteTextures(depth_textures.size(), depth_textures.ptr());
		depth_textures.clear();
	}
	glDeleteFramebuffers(1, &depth_framebuffer);
	depth_framebuffer = 0;
	size = Size2i();
}

SDSM::~SDSM() {
	_free_buffers();
	glDeleteBuffers(1, &triangle);
	glDeleteVertexArrays(1, &vertex_array);
	shader.version_free(shader_version);
}

bool SDSM::_unsupported() {
	unsupported = true;
	_free_buffers();
	glBindFramebuffer(GL_FRAMEBUFFER, TextureStorage::system_fbo);
	WARN_PRINT("Compatibility SDSM requires renderable RGBA32F with RGBA/FLOAT readback and a sampleable depth framebuffer. This device does not support those framebuffer operations; using regular CSM.");
	return false;
}

bool SDSM::prepare(const Size2i &p_size, uint32_t p_views) {
	if (unsupported || p_size.x <= 0 || p_size.y <= 0) {
		return false;
	}
	if (!shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_SAMPLE) || !shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_REDUCE)) {
		ERR_PRINT_ONCE("Compatibility SDSM reduction shader could not be compiled.");
		return false;
	}
	if (size == p_size && depth_textures.size() == int(p_views)) {
		return true;
	}
	_free_buffers();
	size = p_size;
	glActiveTexture(GL_TEXTURE0);
	glGenFramebuffers(1, &depth_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, depth_framebuffer);
	depth_textures.resize(p_views);
	glGenTextures(p_views, depth_textures.ptrw());
	GLenum none = GL_NONE;
	glDrawBuffers(1, &none);
	glReadBuffer(GL_NONE);
	for (uint32_t view = 0; view < p_views; view++) {
		glBindTexture(GL_TEXTURE_2D, depth_textures[view]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size.x, size.y, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_textures[view], 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			return _unsupported();
		}
	}
	Size2i next_size = size;
	do {
		next_size = Size2i((next_size.x + 3) / 4, (next_size.y + 3) / 4);
		Level level;
		level.size = next_size;
		glGenTextures(1, &level.texture);
		glBindTexture(GL_TEXTURE_2D, level.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, next_size.x, next_size.y, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &level.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, level.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level.texture, 0);
		GLenum color = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &color);
		glReadBuffer(color);
		levels.push_back(level);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			return _unsupported();
		}
	} while (next_size != Size2i(1, 1));
	// GLES implementations may restrict readback even when float rendering works.
	// Probe the exact one-pixel operation, not an extension-name approximation.
	float probe[4];
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, probe);
	if (glGetError() != GL_NO_ERROR) {
		return _unsupported();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, TextureStorage::system_fbo);
	return true;
}

void SDSM::bind_depth(uint32_t p_view) {
	glBindFramebuffer(GL_FRAMEBUFFER, depth_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_textures[p_view], 0);
	glViewport(0, 0, size.x, size.y);
}

void SDSM::_reduce(GLuint p_depth, const Projection &p_inverse_projection, const Transform3D &p_view_to_camera, const Transform3D &p_camera_to_light, const Vector2 &p_partition, int p_operation, float *r_result) {
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindVertexArray(vertex_array);
	glActiveTexture(GL_TEXTURE0);
	shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::INVERSE_PROJECTION, p_inverse_projection, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::VIEW_TO_CAMERA, p_view_to_camera, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::CAMERA_TO_LIGHT, p_camera_to_light, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::PARTITION, p_partition, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::OPERATION, p_operation, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	GLuint source = p_depth;
	for (int i = 0; i < levels.size(); i++) {
		if (i == 1) {
			shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_REDUCE);
		}
		const Level &level = levels[i];
		glBindFramebuffer(GL_FRAMEBUFFER, level.framebuffer);
		glViewport(0, 0, level.size.x, level.size.y);
		glBindTexture(GL_TEXTURE_2D, source);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		source = level.texture;
	}
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, r_result);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, TextureStorage::system_fbo);
}

Vector2 SDSM::reduce_depth(const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera) {
	Vector2 range(FLT_MAX, -FLT_MAX);
	for (int view = 0; view < depth_textures.size(); view++) {
		float value[4];
		_reduce(depth_textures[view], p_inverse_projection[view], p_view_to_camera[view], Transform3D(), Vector2(), 0, value);
		range.x = MIN(range.x, value[0]);
		range.y = MAX(range.y, -value[1]);
	}
	return range;
}

void SDSM::reduce_bounds(const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const RendererSDSM::Light &p_light, RendererSDSM::Bounds *r_bounds) {
	Transform3D camera_to_light = p_light.light_to_world.affine_inverse() * p_light.camera_transform;
	for (uint32_t cascade = 0; cascade < p_light.cascade_count; cascade++) {
		Vector2 partition(RendererSDSM::get_cascade_begin(p_light, cascade), p_light.distances[cascade + 1]);
		Vector3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
		Vector3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (int view = 0; view < depth_textures.size(); view++) {
			float low[4], high[4];
			_reduce(depth_textures[view], p_inverse_projection[view], p_view_to_camera[view], camera_to_light, partition, 1, low);
			_reduce(depth_textures[view], p_inverse_projection[view], p_view_to_camera[view], camera_to_light, partition, 2, high);
			minimum = minimum.min(Vector3(low[0], low[1], low[2]));
			maximum = maximum.max(Vector3(-high[0], -high[1], -high[2]));
		}
		r_bounds[cascade].valid = minimum.x <= maximum.x;
		if (r_bounds[cascade].valid) {
			r_bounds[cascade].aabb = AABB(minimum, maximum - minimum);
		}
	}
}

} // namespace GLES3
#endif
