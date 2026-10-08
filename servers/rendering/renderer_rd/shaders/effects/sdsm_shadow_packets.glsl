#[compute]

#version 450

#VERSION_DEFINES

#include "../sdsm_data_inc.glsl"

layout(local_size_x = THREADS, local_size_y = 1, local_size_z = 1) in;

struct Surface {
	uint caster;
	uint instance;
	uint packet;
	uint lod_count;
	float model_scale;
	float lod_threshold;
	uint instance_count;
	uint flags;
};
struct Packet {
	uint count;
	uint first_index;
	int vertex_offset;
	uint first_instance;
	uint lod;
	uint flags;
	uint source_offset;
	uint instance_multiplier;
};
struct LOD {
	float edge_length;
	uint packet;
	uint index_count;
	uint pad;
};

// Each variant declares only its actual inputs. No stage exceeds the Vulkan
// baseline of four compute SSBOs, including a nested command producer.
#if defined(MODE_CLEAR) || defined(MODE_CLASSIFY) || defined(MODE_SCAN) || defined(MODE_SCATTER) || defined(MODE_NESTED) || defined(MODE_FINALIZE)
layout(set = 0, binding = 0, std430) readonly restrict buffer Metadata { uint data[]; } metadata;
#endif
#if defined(MODE_CLEAR) || defined(MODE_SCAN) || defined(MODE_ADD_SUMS) || defined(MODE_SCATTER)
// Header: slot count, packet count, surface count, reserved. Then slot offsets,
// then up to cascade_count * surface_count uploaded instance-data indices.
layout(set = 0, binding = 1, std430) restrict buffer Visibility { uint data[]; } visibility;
#endif
#if defined(MODE_CLEAR) || defined(MODE_CLASSIFY) || defined(MODE_SCAN) || defined(MODE_NESTED) || defined(MODE_FINALIZE)
layout(set = 0, binding = 2, std430) restrict buffer Commands { uint data[]; } commands;
#endif
#if defined(MODE_CLEAR) || defined(MODE_CLASSIFY) || defined(MODE_SCAN) || defined(MODE_SCAN_SUMS) || defined(MODE_ADD_SUMS) || defined(MODE_SCATTER)
// Decisions, scatter cursors, then the logarithmic scan hierarchy.
layout(set = 0, binding = 3, std430) restrict buffer Scratch { uint data[]; } scratch;
#endif
#ifdef MODE_CLASSIFY
layout(set = 0, binding = 1, std430) readonly restrict buffer Casters { SdsmBounds data[]; } casters;
layout(set = 0, binding = 4, std140) uniform Result { SdsmResult data; } fitted;
#endif
#ifdef MODE_NESTED
layout(set = 0, binding = 3, std430) readonly restrict buffer NestedCommands { uint data[]; } nested;
#endif

layout(push_constant, std430) uniform Params {
	uint surface_count;
	uint packet_count;
	uint cascade_count;
	uint element_count;
	uint input_offset;
	uint output_offset;
	uint parent_offset;
	uint packet;
} params;

#if defined(MODE_CLASSIFY) || defined(MODE_SCATTER)
Surface get_surface(uint index) {
	uint base = index * 8u;
	return Surface(metadata.data[base], metadata.data[base + 1u], metadata.data[base + 2u], metadata.data[base + 3u],
		uintBitsToFloat(metadata.data[base + 4u]), uintBitsToFloat(metadata.data[base + 5u]), metadata.data[base + 6u], metadata.data[base + 7u]);
}
#endif
#if defined(MODE_CLEAR) || defined(MODE_CLASSIFY) || defined(MODE_SCAN) || defined(MODE_NESTED) || defined(MODE_FINALIZE)
Packet get_packet(uint index) {
	uint base = (params.surface_count + index) * 8u;
	return Packet(metadata.data[base], metadata.data[base + 1u], int(metadata.data[base + 2u]), metadata.data[base + 3u],
		metadata.data[base + 4u], metadata.data[base + 5u], metadata.data[base + 6u], metadata.data[base + 7u]);
}
#endif
#ifdef MODE_CLASSIFY
LOD get_lod(uint index) {
	uint base = (params.surface_count + params.packet_count) * 8u + index * 4u;
	return LOD(uintBitsToFloat(metadata.data[base]), metadata.data[base + 1u], metadata.data[base + 2u], metadata.data[base + 3u]);
}
#endif
#if defined(MODE_SCAN) || defined(MODE_SCAN_SUMS)
shared uint scan_data[THREADS];
#endif

void main() {
	uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
	uint lane = gl_LocalInvocationIndex;
	uint i = group * THREADS + lane;
	uint slots = params.packet_count * params.cascade_count;
	uint decisions = params.surface_count * params.cascade_count;
#ifdef MODE_CLEAR
	if (i == 0u) {
		visibility.data[0] = slots;
		visibility.data[1] = params.packet_count;
		visibility.data[2] = params.surface_count;
		visibility.data[3] = 0u;
	}
	if (i < slots) {
		Packet packet = get_packet(i % params.packet_count);
		uint dst = i * 5u;
		commands.data[dst] = packet.count;
		commands.data[dst + 1u] = 0u;
		commands.data[dst + 2u] = packet.first_index;
		commands.data[dst + 3u] = (packet.flags & 1u) != 0u ? uint(packet.vertex_offset) : packet.first_instance;
		commands.data[dst + 4u] = (packet.flags & 1u) != 0u ? packet.first_instance : 0u;
		scratch.data[decisions + i] = 0u;
	}
#endif
#ifdef MODE_CLASSIFY
	if (i >= decisions) { return; }
	uint cascade = i / params.surface_count;
	Surface surface = get_surface(i % params.surface_count);
	SdsmBounds bounds = casters.data[surface.caster];
	SdsmBounds volume = fitted.data.bounds[cascade];
	bool visible = fitted.data.meta.x != 0u && cascade < fitted.data.meta.y && surface.instance_count != 0u &&
		all(greaterThanEqual(bounds.maximum.xyz, volume.minimum.xyz)) && all(lessThanEqual(bounds.minimum.xyz, volume.maximum.xyz));
	uint selected = 0xffffffffu;
	if (visible) {
		uint lod = 0u;
		if (surface.lod_threshold > 0.0) {
			float error_limit = fitted.data.texel[cascade] * surface.lod_threshold;
			for (uint level = 1u; level < surface.lod_count; ++level) {
				if (get_lod(surface.packet + level).edge_length * surface.model_scale > error_limit) { break; }
				lod = level;
			}
		}
		selected = cascade * params.packet_count + get_lod(surface.packet + lod).packet;
		Packet packet = get_packet(selected % params.packet_count);
		uint count = (packet.flags & 2u) != 0u ? surface.instance_count : 1u;
		atomicAdd(commands.data[selected * 5u + 1u], count);
	}
	scratch.data[i] = selected;
#endif
#if defined(MODE_SCAN) || defined(MODE_SCAN_SUMS)
	uint value = 0u;
	if (i < params.element_count) {
#ifdef MODE_SCAN
		value = commands.data[i * 5u + 1u];
		if ((get_packet(i % params.packet_count).flags & 2u) != 0u) { value = uint(value != 0u); }
#else
		value = scratch.data[params.input_offset + i];
#endif
	}
	scan_data[lane] = value;
	barrier();
	for (uint offset = 1u; offset < THREADS; offset <<= 1u) {
		uint addition = lane >= offset ? scan_data[lane - offset] : 0u;
		barrier();
		scan_data[lane] += addition;
		barrier();
	}
	if (i < params.element_count) {
		uint exclusive = scan_data[lane] - value;
#ifdef MODE_SCAN
		visibility.data[4u + i] = exclusive;
#else
		scratch.data[params.input_offset + i] = exclusive;
#endif
	}
	if (lane == THREADS - 1u && group < (params.element_count + THREADS - 1u) / THREADS) {
		scratch.data[params.output_offset + group] = scan_data[THREADS - 1u];
	}
#endif
#ifdef MODE_ADD_SUMS
	if (i >= params.element_count) { return; }
	uint addition = scratch.data[params.parent_offset + i / THREADS];
	if (params.output_offset == 0xffffffffu) { visibility.data[4u + i] += addition; }
	else { scratch.data[params.output_offset + i] += addition; }
#endif
#ifdef MODE_SCATTER
	if (i >= decisions) { return; }
	uint slot = scratch.data[i];
	if (slot == 0xffffffffu) { return; }
	uint dst = visibility.data[4u + slot] + atomicAdd(scratch.data[decisions + slot], 1u);
	visibility.data[4u + slots + dst] = get_surface(i % params.surface_count).instance;
#endif
#ifdef MODE_NESTED
	if (i >= params.cascade_count) { return; }
	uint dst = (i * params.packet_count + params.packet) * 5u;
	Packet packet = get_packet(params.packet);
	bool point = (packet.flags & 8u) != 0u;
	bool visible = commands.data[dst + (point ? 0u : 1u)] != 0u;
	uint src = packet.source_offset;
	uint first_instance = nested.data[src + ((packet.flags & 16u) != 0u ? 4u : 3u)];
	if (point) {
		commands.data[dst] = visible ? nested.data[src + 1u] * packet.count : 0u;
		commands.data[dst + 2u] = first_instance * packet.count;
	} else {
		commands.data[dst + 1u] = visible ? nested.data[src + 1u] * packet.instance_multiplier : 0u;
		commands.data[dst + ((packet.flags & 1u) != 0u ? 4u : 3u)] = first_instance;
	}
#endif
#ifdef MODE_FINALIZE
	if (i >= slots) { return; }
	Packet packet = get_packet(i % params.packet_count);
	uint dst = i * 5u;
	uint count = commands.data[dst + 1u];
	if ((packet.flags & 8u) != 0u) {
		commands.data[dst] = count * packet.count;
		commands.data[dst + 1u] = packet.instance_multiplier;
	} else {
		commands.data[dst + 1u] = count * packet.instance_multiplier;
	}
#endif
}
