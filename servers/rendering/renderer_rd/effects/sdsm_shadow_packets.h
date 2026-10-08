/**************************************************************************/
/*  sdsm_shadow_packets.h                                                 */
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

#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/sdsm_shadow_packets.glsl.gen.h"

namespace RendererRD {

// Invocation snapshots reuse a bounded renderer-owned pool. RD tracks writes
// after earlier consumers, including indirect-command and vertex-buffer reads.
class SDSMShadowPackets {
public:
	struct Surface {
		uint32_t caster = 0;
		uint32_t instance = 0;
		uint32_t packet = 0; // First LOD record, not the output packet index.
		uint32_t lod_count = 1;
		float model_scale = 1.0f;
		float lod_threshold = 0.0f; // Existing error threshold, expressed in pixels.
		uint32_t instance_count = 1;
		uint32_t flags = 0;
	};
	struct Packet {
		uint32_t count = 0;
		uint32_t first_index = 0;
		int32_t vertex_offset = 0;
		uint32_t first_instance = 0;
		uint32_t lod = 0;
		uint32_t flags = 0; // Indexed=1, nested=2, indirect source=4, point emulation=8, indexed source=16.
		uint32_t source_offset = 0; // Source indirect command offset in uint words.
		uint32_t instance_multiplier = 1;
	};
	struct LOD {
		float edge_length = 0.0f;
		uint32_t packet = 0;
		uint32_t index_count = 0;
		uint32_t pad = 0;
	};
	struct Prepared {
		RID metadata; // Packed Surface[], Packet[], LOD[]; no padding between arrays.
		RID visibility;
		RID commands;
		RID scratch;
		uint32_t surface_count = 0;
		uint32_t packet_count = 0;
		uint32_t cascade_count = 0;
		uint32_t capacity[4] = {};
		LocalVector<uint8_t> cached_surfaces;
		LocalVector<uint8_t> cached_packets;
		LocalVector<uint8_t> cached_lods;
	};

	void prepare(Prepared &r_prepared, const LocalVector<Surface> &p_surfaces, const LocalVector<Packet> &p_packets, const LocalVector<LOD> &p_lods, uint32_t p_cascade_count);
	void dispatch(Prepared &p_prepared, RID p_result, RID p_casters, const LocalVector<RID> &p_nested_command_buffers);
	void free(Prepared &p_prepared);
	SDSMShadowPackets();
	~SDSMShadowPackets();

private:
	enum Mode { CLEAR,
		CLASSIFY,
		SCAN,
		SCAN_SUMS,
		ADD_SUMS,
		SCATTER,
		NESTED,
		FINALIZE,
		MODE_MAX };
	struct PushConstant {
		uint32_t surface_count;
		uint32_t packet_count;
		uint32_t cascade_count;
		uint32_t element_count;
		uint32_t input_offset;
		uint32_t output_offset;
		uint32_t parent_offset;
		uint32_t packet;
	};
	SdsmShadowPacketsShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	uint32_t workgroup_size = 256;
};

} // namespace RendererRD
