/**************************************************************************/
/*  dlss.h                                                                */
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

#ifdef DLSS_ENABLED

#include "core/math/vector2.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device.h"

// NVIDIA NGX forward declarations (avoid pulling the SDK headers in here).
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace RendererRD {

class DLSSEffect;

// One DLSS Super Resolution feature instance; owned by the render buffers,
// recreated when the internal or target size changes.
class DLSSContext {
	friend class DLSSEffect;

public:
	struct EvalPayload {
		DLSSContext *context = nullptr;

		struct TextureHandles {
			uint64_t image = 0;
			uint64_t image_view = 0;
			uint32_t vk_format = 0;
			uint32_t width = 0;
			uint32_t height = 0;
		};

		TextureHandles color;
		TextureHandles depth;
		TextureHandles velocity;
		TextureHandles exposure;
		TextureHandles output;

		Vector2 jitter;
		Size2i render_size;
		uint32_t view = 0;
		bool reset = false;
	};

private:
	// One feature per view: DLSS keeps temporal state per feature, so stereo
	// rendering needs an independent instance per eye.
	enum {
		MAX_VIEWS = 2,
	};
	NVSDK_NGX_Handle *features[MAX_VIEWS] = {};
	NVSDK_NGX_Parameter *ngx_parameters = nullptr;
	Size2i internal_size;
	Size2i target_size;
	bool create_failed = false;

	// Payloads passed to the driver callback; ring buffered because the
	// callback executes later, when the render graph is replayed into a
	// command buffer.
	static const uint32_t PAYLOAD_RING_SIZE = 4;
	EvalPayload payload_ring[PAYLOAD_RING_SIZE];
	uint32_t payload_cursor = 0;

public:
	~DLSSContext();
};

class DLSSEffect {
	static DLSSEffect *singleton;

	bool init_attempted = false;
	bool ngx_initialized = false;
	bool dlss_available = false;
	NVSDK_NGX_Parameter *capability_parameters = nullptr;

	bool _ensure_initialized();

	static void _eval_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
	static void _fill_texture_handles(RID p_texture, DLSSContext::EvalPayload::TextureHandles &r_handles);

public:
	static DLSSEffect *get_singleton();

	bool is_available();

	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size);

	struct Parameters {
		DLSSContext *context = nullptr;
		Size2i internal_size;
		uint32_t view = 0;
		RID color;
		RID depth;
		RID velocity;
		RID exposure;
		RID output;
		Vector2 jitter; // In pixels, projection convention.
		bool reset_accumulation = false;
	};

	void upscale(const Parameters &p_params);

	DLSSEffect();
	~DLSSEffect();
};

} // namespace RendererRD

#endif // DLSS_ENABLED
