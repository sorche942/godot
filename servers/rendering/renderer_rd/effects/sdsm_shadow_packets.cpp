/**************************************************************************/
/*  sdsm_shadow_packets.cpp                                               */
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

#include "sdsm_shadow_packets.h"

#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

SDSMShadowPackets::SDSMShadowPackets() {
	static_assert(sizeof(Surface) == 32);
	static_assert(sizeof(Packet) == 32);
	static_assert(sizeof(LOD) == 16);
	static_assert(sizeof(PushConstant) == 32);
	RD *rd = RD::get_singleton();
	const uint32_t limit = MIN(uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_INVOCATIONS)), uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X)));
	while (workgroup_size > limit && workgroup_size > 1) {
		workgroup_size >>= 1;
	}
	const char *mode_names[MODE_MAX] = { "CLEAR", "CLASSIFY", "SCAN", "SCAN_SUMS", "ADD_SUMS", "SCATTER", "NESTED", "FINALIZE" };
	Vector<String> modes;
	for (uint32_t mode = 0; mode < MODE_MAX; mode++) {
		modes.push_back("\n#define MODE_" + String(mode_names[mode]) + "\n#define THREADS " + itos(workgroup_size) + "\n");
	}
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (uint32_t mode = 0; mode < MODE_MAX; mode++) {
		pipelines[mode] = rd->compute_pipeline_create(shader.version_get_shader(shader_version, mode));
	}
}

SDSMShadowPackets::~SDSMShadowPackets() {
	shader.version_free(shader_version);
}

void SDSMShadowPackets::prepare(Prepared &r_prepared, const LocalVector<Surface> &p_surfaces, const LocalVector<Packet> &p_packets, const LocalVector<LOD> &p_lods, uint32_t p_cascade_count) {
	ERR_FAIL_COND(p_cascade_count < 1 || p_cascade_count > 4);
	const uint64_t slots = uint64_t(p_packets.size()) * p_cascade_count;
	const uint64_t entries = uint64_t(p_surfaces.size()) * p_cascade_count;
	const uint64_t surface_bytes = uint64_t(p_surfaces.size()) * sizeof(Surface);
	const uint64_t packet_bytes = uint64_t(p_packets.size()) * sizeof(Packet);
	const uint64_t lod_bytes = uint64_t(p_lods.size()) * sizeof(LOD);
	uint64_t scratch_words = entries + slots;
	for (uint64_t count = slots; count > 1;) {
		count = (count + workgroup_size - 1) / workgroup_size;
		scratch_words += count;
	}
	// A one-element scan still writes its block total.
	if (slots <= 1) {
		scratch_words++;
	}
	ERR_FAIL_COND((4 + slots + entries) * sizeof(uint32_t) > UINT32_MAX || slots * 5 * sizeof(uint32_t) > UINT32_MAX || scratch_words * sizeof(uint32_t) > UINT32_MAX || surface_bytes + packet_bytes + lod_bytes > UINT32_MAX);
	RD *rd = RD::get_singleton();
	const uint32_t sizes[4] = {
		uint32_t(MAX(uint64_t(sizeof(Surface)), surface_bytes + packet_bytes + lod_bytes)),
		uint32_t((4 + slots + entries) * sizeof(uint32_t)),
		uint32_t(MAX(uint64_t(5 * sizeof(uint32_t)), slots * 5 * sizeof(uint32_t))),
		uint32_t(MAX(uint64_t(sizeof(uint32_t)), scratch_words * sizeof(uint32_t))),
	};
	RID *buffers[4] = { &r_prepared.metadata, &r_prepared.visibility, &r_prepared.commands, &r_prepared.scratch };
	const bool layout_changed = r_prepared.surface_count != p_surfaces.size() || r_prepared.packet_count != p_packets.size();
	const bool metadata_recreated = sizes[0] > r_prepared.capacity[0];
	for (uint32_t i = 0; i < 4; i++) {
		if (sizes[i] > r_prepared.capacity[i]) {
			if (buffers[i]->is_valid()) {
				rd->free_rid(*buffers[i]);
			}
			r_prepared.capacity[i] = sizes[i];
			*buffers[i] = rd->storage_buffer_create(sizes[i], {}, i == 2 ? RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT : 0);
		}
	}
	r_prepared.surface_count = p_surfaces.size();
	r_prepared.packet_count = p_packets.size();
	r_prepared.cascade_count = p_cascade_count;
	auto upload = [&](const void *p_data, uint32_t p_offset, uint32_t p_size, LocalVector<uint8_t> &r_cached) {
		if (!p_size) {
			r_cached.clear();
			return;
		}
		if (metadata_recreated || layout_changed || r_cached.size() != p_size || memcmp(r_cached.ptr(), p_data, p_size) != 0) {
			rd->buffer_update(r_prepared.metadata, p_offset, p_size, p_data);
			r_cached.resize(p_size);
			memcpy(r_cached.ptr(), p_data, p_size);
		}
	};
	upload(p_surfaces.ptr(), 0, surface_bytes, r_prepared.cached_surfaces);
	upload(p_packets.ptr(), surface_bytes, packet_bytes, r_prepared.cached_packets);
	upload(p_lods.ptr(), surface_bytes + packet_bytes, lod_bytes, r_prepared.cached_lods);
}

void SDSMShadowPackets::dispatch(Prepared &p_prepared, RID p_result, RID p_casters, const LocalVector<RID> &p_nested_command_buffers) {
	ERR_FAIL_COND(p_prepared.visibility.is_null() || p_result.is_null() || p_casters.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RD::Uniform metadata(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_prepared.metadata);
	RD::Uniform visibility(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_prepared.visibility);
	RD::Uniform commands(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_prepared.commands);
	RD::Uniform scratch(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_prepared.scratch);
	RD::Uniform casters(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_casters);
	RD::Uniform result(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, p_result);
	RID sets[MODE_MAX];
	sets[CLEAR] = cache->get_cache(shader.version_get_shader(shader_version, CLEAR), 0, metadata, visibility, commands, scratch);
	sets[CLASSIFY] = cache->get_cache(shader.version_get_shader(shader_version, CLASSIFY), 0, metadata, casters, commands, scratch, result);
	sets[SCAN] = cache->get_cache(shader.version_get_shader(shader_version, SCAN), 0, metadata, visibility, commands, scratch);
	sets[SCAN_SUMS] = cache->get_cache(shader.version_get_shader(shader_version, SCAN_SUMS), 0, scratch);
	sets[ADD_SUMS] = cache->get_cache(shader.version_get_shader(shader_version, ADD_SUMS), 0, visibility, scratch);
	sets[SCATTER] = cache->get_cache(shader.version_get_shader(shader_version, SCATTER), 0, metadata, visibility, scratch);
	sets[FINALIZE] = cache->get_cache(shader.version_get_shader(shader_version, FINALIZE), 0, metadata, commands);
	RENDER_TIMESTAMP("SDSM Visibility and LOD");
	RD::ComputeListID list = rd->compute_list_begin();
	auto stage = [&](const char *p_name) {
		if (RSG::utilities->capturing_timestamps) {
			rd->compute_list_end();
			RENDER_TIMESTAMP(p_name);
			list = rd->compute_list_begin();
		}
	};
	PushConstant push = {};
	push.surface_count = p_prepared.surface_count;
	push.packet_count = p_prepared.packet_count;
	push.cascade_count = p_prepared.cascade_count;
	const uint32_t slots = push.packet_count * push.cascade_count;
	const uint32_t entries = push.surface_count * push.cascade_count;
	auto run = [&](Mode p_mode, uint32_t p_count) {
		rd->compute_list_bind_compute_pipeline(list, pipelines[p_mode]);
		rd->compute_list_bind_uniform_set(list, sets[p_mode], 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		const uint32_t groups = MAX(1u, (p_count + workgroup_size - 1u) / workgroup_size);
		const uint32_t groups_x = MIN(groups, uint32_t(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X)));
		rd->compute_list_dispatch(list, groups_x, (groups + groups_x - 1u) / groups_x, 1);
		rd->compute_list_add_barrier(list);
	};
	run(CLEAR, slots);
	if (slots == 0 || entries == 0) {
		rd->compute_list_end();
		RENDER_TIMESTAMP("SDSM Packets Ready");
		return;
	}
	run(CLASSIFY, entries);
	stage("SDSM Packet Prefix");
	struct ScanLevel {
		uint32_t offset;
		uint32_t count;
	};
	ScanLevel levels[16];
	uint32_t level_count = 0;
	uint32_t offset = entries + slots;
	uint32_t count = (slots + workgroup_size - 1u) / workgroup_size;
	levels[level_count++] = { offset, count };
	push.element_count = slots;
	push.output_offset = offset;
	run(SCAN, slots);
	while (count > 1) {
		push.input_offset = offset;
		push.element_count = count;
		offset += count;
		push.output_offset = offset;
		run(SCAN_SUMS, count);
		count = (count + workgroup_size - 1u) / workgroup_size;
		levels[level_count++] = { offset, count };
	}
	// Each level is exclusive within its own block. Propagate higher prefixes
	// downwards, then add the first scanned level to the packet offsets.
	for (int32_t level = int32_t(level_count) - 3; level >= 0; --level) {
		push.element_count = levels[level].count;
		push.output_offset = levels[level].offset;
		push.parent_offset = levels[level + 1].offset;
		run(ADD_SUMS, push.element_count);
	}
	if (slots > workgroup_size) {
		push.element_count = slots;
		push.output_offset = UINT32_MAX;
		push.parent_offset = levels[0].offset;
		run(ADD_SUMS, slots);
	}
	stage("SDSM Packet Scatter");
	run(SCATTER, entries);
	run(FINALIZE, slots);
	stage("SDSM Nested Counts");
	// Separate source bindings preserve GPU-generated MultiMesh counts while
	// keeping this variant below the portable four-SSBO compute limit.
	for (uint32_t packet = 0; packet < p_nested_command_buffers.size(); ++packet) {
		if (p_nested_command_buffers[packet].is_null()) {
			continue;
		}
		ERR_CONTINUE(packet >= p_prepared.packet_count);
		RD::Uniform source(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_nested_command_buffers[packet]);
		sets[NESTED] = cache->get_cache(shader.version_get_shader(shader_version, NESTED), 0, metadata, commands, source);
		push.packet = packet;
		run(NESTED, push.cascade_count);
	}
	rd->compute_list_end();
	RENDER_TIMESTAMP("SDSM Packets Ready");
}

void SDSMShadowPackets::free(Prepared &p_prepared) {
	for (RID buffer : { p_prepared.metadata, p_prepared.visibility, p_prepared.commands, p_prepared.scratch }) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	p_prepared = Prepared();
}
