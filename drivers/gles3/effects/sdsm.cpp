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
#include "drivers/gles3/storage/config.h"
#include "drivers/gles3/storage/light_storage.h"
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
	Level *targets[] = { &depth_ranges, &splits, &bounds, &fitted_bounds, &cameras };
	for (Level *target : targets) {
		glDeleteTextures(1, &target->texture);
		glDeleteFramebuffers(1, &target->framebuffer);
		*target = Level();
	}
	if (!depth_textures.is_empty()) {
		glDeleteTextures(depth_textures.size(), depth_textures.ptr());
		depth_textures.clear();
	}
	glDeleteTextures(1, &records_texture);
	records_texture = 0;
	records_height = 0;
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
	WARN_PRINT("Compatibility SDSM requires renderable RGBA32F and sampleable depth framebuffers. This device does not support those framebuffer operations; using regular CSM.");
	return false;
}

bool SDSM::_create_target(Level &r_target, const Size2i &p_size) {
	r_target.size = p_size;
	glGenTextures(1, &r_target.texture);
	glBindTexture(GL_TEXTURE_2D, r_target.texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, p_size.x, p_size.y, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &r_target.framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, r_target.framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r_target.texture, 0);
	GLenum color = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &color);
	glReadBuffer(color);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

bool SDSM::prepare(const Size2i &p_size, uint32_t p_views, const Rect2i &p_region) {
	light_count = 0;
	if (unsupported || p_size.x <= 0 || p_size.y <= 0 || p_views == 0 || p_views > RendererSceneRender::MAX_RENDER_VIEWS) {
		return false;
	}
	region = p_region.has_area() ? p_region.intersection(Rect2i(Point2i(), p_size)) : Rect2i(Point2i(), p_size);
	for (int mode = SdsmShaderGLES3::MODE_SAMPLE; mode <= SdsmShaderGLES3::MODE_CAMERA; mode++) {
		if (!shader.version_bind_shader(shader_version, SdsmShaderGLES3::ShaderVariant(mode))) {
			ERR_PRINT_ONCE("Compatibility SDSM fitting shader could not be compiled.");
			return false;
		}
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
		bool supported = _create_target(level, next_size);
		levels.push_back(level);
		if (!supported) {
			return _unsupported();
		}
	} while (next_size != Size2i(1, 1));
	if (!_create_target(depth_ranges, Size2i(RendererSceneRender::MAX_RENDER_VIEWS, 1)) ||
			!_create_target(splits, Size2i(1, 1)) ||
			!_create_target(bounds, Size2i(RendererSceneRender::MAX_RENDER_VIEWS * 2, 4)) ||
			!_create_target(fitted_bounds, Size2i(2, 4)) ||
			!_create_target(cameras, Size2i(16, RendererSceneRender::MAX_DIRECTIONAL_LIGHTS * 4))) {
		return _unsupported();
	}
	glGenTextures(1, &records_texture);
	glBindTexture(GL_TEXTURE_2D, records_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, TextureStorage::system_fbo);
	return true;
}

void SDSM::bind_depth(uint32_t p_view) {
	glBindFramebuffer(GL_FRAMEBUFFER, depth_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_textures[p_view], 0);
	glViewport(0, 0, size.x, size.y);
}

void SDSM::_bind_source(GLuint p_texture, int p_unit) {
	glActiveTexture(GL_TEXTURE0 + p_unit);
	glBindTexture(GL_TEXTURE_2D, p_texture);
}

void SDSM::_draw(const Level &p_target, const Rect2i &p_rect) {
	glBindFramebuffer(GL_FRAMEBUFFER, p_target.framebuffer);
	glViewport(p_rect.position.x, p_rect.position.y, p_rect.size.x, p_rect.size.y);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SDSM::_reduce(GLuint p_depth, const Projection &p_inverse_projection, const Transform3D &p_view_to_camera, const Transform3D &p_camera_to_light, int p_operation, int p_cascade) {
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindVertexArray(vertex_array);
	shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::INVERSE_PROJECTION, p_inverse_projection, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::VIEW_TO_CAMERA, p_view_to_camera, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::CAMERA_TO_LIGHT, p_camera_to_light, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::REGION, Vector4(region.position.x, region.position.y, region.get_end().x, region.get_end().y), shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::OPERATION, p_operation, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	shader.version_set_uniform(SdsmShaderGLES3::CASCADE_INDEX, p_cascade, shader_version, SdsmShaderGLES3::MODE_SAMPLE);
	_bind_source(splits.texture, 1);
	GLuint source = p_depth;
	for (int i = 0; i < levels.size(); i++) {
		if (i == 1) {
			shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_REDUCE);
		}
		_bind_source(source, 0);
		_draw(levels[i], Rect2i(Point2i(), levels[i].size));
		source = levels[i].texture;
	}
}

void SDSM::_copy_result(const Level &p_target, int p_x, int p_y) {
	// Copies within GPU memory; never maps or downloads a reduction.
	_bind_source(p_target.texture, 0);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, p_x, p_y, 0, 0, 1, 1);
}

void SDSM::reduce_depth(const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera) {
	for (int view = 0; view < depth_textures.size(); view++) {
		_reduce(depth_textures[view], p_inverse_projection[view], p_view_to_camera[view], Transform3D(), 0, 0);
		_copy_result(depth_ranges, view, 0);
	}
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, TextureStorage::system_fbo);
}

bool SDSM::fit_light(const Vector<Projection> &p_inverse_projection, const Vector<Transform3D> &p_view_to_camera, const RendererSDSM::Light &p_light, const Vector<AABB> &p_receivers, const LocalVector<RendererSDSM::Caster> &p_casters) {
	if (light_count >= RendererSceneRender::MAX_DIRECTIONAL_LIGHTS) {
		return false;
	}
	const int record_width = 256;
	const int needed_height = (16 + (p_receivers.size() + p_casters.size()) * 2 + record_width - 1) / record_width;
	if (needed_height > Config::get_singleton()->max_texture_size) {
		ERR_PRINT_ONCE("Compatibility SDSM receiver/caster bounds exceed the device texture capacity; using CSM for this light.");
		return false;
	}
	if (needed_height > records_height) {
		records_height = MIN(int(Math::next_power_of_2(uint32_t(needed_height))), Config::get_singleton()->max_texture_size);
		_bind_source(records_texture, 2);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, record_width, records_height, 0, GL_RGBA, GL_FLOAT, nullptr);
		records.resize(record_width * records_height * 4);
	}
	float *data = records.ptrw();
	const Transform3D world_to_light = p_light.light_to_world.affine_inverse();
	const Transform3D world_to_camera = p_light.camera_transform.affine_inverse();
	const Transform3D camera_to_light = world_to_light * p_light.camera_transform;
	for (int view = 0; view < depth_textures.size(); view++) {
		for (int corner = 0; corner < 4; corner++) {
			const Vector2 xy((corner & 1) ? 1.0 : -1.0, (corner & 2) ? 1.0 : -1.0);
			const Vector4 near_h = p_inverse_projection[view].xform(Vector4(xy.x, xy.y, 1.0, 1.0));
			const Vector4 far_h = p_inverse_projection[view].xform(Vector4(xy.x, xy.y, -1.0, 1.0));
			const Vector3 near_point = p_view_to_camera[view].xform(Vector3(near_h.x, near_h.y, near_h.z) / near_h.w);
			const Vector3 far_point = p_view_to_camera[view].xform(Vector3(far_h.x, far_h.y, far_h.z) / far_h.w);
			for (int plane = 0; plane < 2; plane++) {
				const real_t depth = plane == 0 ? p_light.camera_near : p_light.camera_far;
				const real_t weight = (depth + near_point.z) / (near_point.z - far_point.z);
				const Vector3 point = camera_to_light.xform(near_point.lerp(far_point, weight));
				const int offset = ((view * 4 + corner) * 2 + plane) * 4;
				data[offset] = point.x;
				data[offset + 1] = point.y;
				data[offset + 2] = point.z;
				data[offset + 3] = 0.0f;
			}
		}
	}
	Vector2 extra(FLT_MAX, -FLT_MAX);
	for (int receiver = 0; receiver < p_receivers.size(); receiver++) {
		const AABB light_bounds = world_to_light.xform(p_receivers[receiver]);
		const AABB camera_bounds = world_to_camera.xform(p_receivers[receiver]);
		const Vector3 low = light_bounds.position;
		const Vector3 high = light_bounds.get_end();
		const float near = -camera_bounds.get_end().z;
		const float far = -camera_bounds.position.z;
		const int offset = (16 + receiver * 2) * 4;
		data[offset] = low.x;
		data[offset + 1] = low.y;
		data[offset + 2] = low.z;
		data[offset + 3] = near;
		data[offset + 4] = high.x;
		data[offset + 5] = high.y;
		data[offset + 6] = high.z;
		data[offset + 7] = far;
		if (far >= p_light.camera_near && near <= p_light.shadow_far) {
			extra.x = MIN(extra.x, MAX(near, p_light.camera_near));
			extra.y = MAX(extra.y, MIN(far, p_light.shadow_far));
		}
	}
	for (uint32_t caster = 0; caster < p_casters.size(); caster++) {
		const Vector3 low = p_casters[caster].bounds.position;
		const Vector3 high = p_casters[caster].bounds.get_end();
		const int offset = (16 + (p_receivers.size() + caster) * 2) * 4;
		data[offset] = low.x;
		data[offset + 1] = low.y;
		data[offset + 2] = low.z;
		data[offset + 3] = 0.0f;
		data[offset + 4] = high.x;
		data[offset + 5] = high.y;
		data[offset + 6] = high.z;
		data[offset + 7] = 0.0f;
	}
	_bind_source(records_texture, 2);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, record_width, needed_height, GL_RGBA, GL_FLOAT, data);
	LightStorage *storage = LightStorage::get_singleton();
	const Vector4 camera_parameters(p_light.camera_near, p_light.camera_far, p_light.shadow_far, p_light.cascade_count);
	const Vector4 fit_parameters(MAX(storage->get_directional_light_shadow_size(p_light.instance), 1), MAX(storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_BLUR), 0.0f), storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_NORMAL_BIAS), Math::tan(Math::deg_to_rad(storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SIZE))));
	const Vector4 shadow_parameters(storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_PANCAKE_SIZE), storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_BIAS) / 100.0f, storage->light_get_param(p_light.base, RSE::LIGHT_PARAM_SHADOW_FADE_START), p_light.blend_splits ? 1.0f : 0.0f);
	glBindVertexArray(vertex_array);
	for (int mode = SdsmShaderGLES3::MODE_SAMPLE; mode <= SdsmShaderGLES3::MODE_CAMERA; mode++) {
		const SdsmShaderGLES3::ShaderVariant variant = SdsmShaderGLES3::ShaderVariant(mode);
		shader.version_bind_shader(shader_version, variant);
		shader.version_set_uniform(SdsmShaderGLES3::CAMERA_PARAMETERS, camera_parameters, shader_version, variant);
		shader.version_set_uniform(SdsmShaderGLES3::FIT_PARAMETERS, fit_parameters, shader_version, variant);
		shader.version_set_uniform(SdsmShaderGLES3::SHADOW_PARAMETERS, shadow_parameters, shader_version, variant);
		shader.version_set_uniform(SdsmShaderGLES3::VIEW_COUNT, int(depth_textures.size()), shader_version, variant);
		shader.version_set_uniform(SdsmShaderGLES3::RECEIVER_COUNT, int(p_receivers.size()), shader_version, variant);
		shader.version_set_uniform(SdsmShaderGLES3::CASTER_COUNT, int(p_casters.size()), shader_version, variant);
	}
	shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_SPLITS);
	shader.version_set_uniform(SdsmShaderGLES3::EXTRA_RANGE, extra, shader_version, SdsmShaderGLES3::MODE_SPLITS);
	_bind_source(depth_ranges.texture, 0);
	_draw(splits, Rect2i(Point2i(), splits.size));
	for (uint32_t cascade = 0; cascade < p_light.cascade_count; cascade++) {
		for (int view = 0; view < depth_textures.size(); view++) {
			for (int operation = 1; operation <= 2; operation++) {
				_reduce(depth_textures[view], p_inverse_projection[view], p_view_to_camera[view], camera_to_light, operation, cascade);
				_copy_result(bounds, view * 2 + operation - 1, cascade);
			}
		}
	}
	shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_FIT);
	_bind_source(bounds.texture, 0);
	_bind_source(splits.texture, 1);
	_bind_source(records_texture, 2);
	_draw(fitted_bounds, Rect2i(Point2i(), fitted_bounds.size));
	shader.version_bind_shader(shader_version, SdsmShaderGLES3::MODE_CAMERA);
	Transform3D relative_light = p_light.light_to_world;
	relative_light.origin = Vector3();
	shader.version_set_uniform(SdsmShaderGLES3::LIGHT_TO_WORLD, relative_light, shader_version, SdsmShaderGLES3::MODE_CAMERA);
	shader.version_set_uniform(SdsmShaderGLES3::ROW_OFFSET, light_count * 4, shader_version, SdsmShaderGLES3::MODE_CAMERA);
	_bind_source(fitted_bounds.texture, 0);
	_draw(cameras, Rect2i(0, light_count * 4, 16, 4));
	lights[light_count++] = p_light.instance;
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, TextureStorage::system_fbo);
	return true;
}

int SDSM::get_light_index(RID p_light) const {
	for (int i = 0; i < light_count; i++) {
		if (lights[i] == p_light) {
			return i;
		}
	}
	return -1;
}

} // namespace GLES3
#endif
