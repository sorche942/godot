#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Unrolls an (optionally indexed, optionally compressed) mesh surface vertex
// stream into a flat, non-indexed float32x3 position stream usable both as
// BLAS build input and for fetching triangle data in raytracing shaders.

layout(buffer_reference, buffer_reference_align = 4, std430) readonly buffer IndexBufferRef {
	uint data[];
};

layout(set = 0, binding = 0, std430) restrict readonly buffer SourceVertices {
	uint data[];
}
src_vertices;

layout(set = 0, binding = 1, std430) restrict writeonly buffer DestVertices {
	float data[];
}
dst_vertices;

#define FLAG_INDEXED (1 << 0)
#define FLAG_INDEX_16 (1 << 1)
#define FLAG_COMPRESSED (1 << 2)

layout(push_constant, std430) uniform Params {
	uvec2 index_buffer_address;
	uint output_vertex_count;
	uint vertex_stride_words;

	uint flags;
	uint pad0;
	uint pad1;
	uint pad2;

	vec4 aabb_position;
	vec4 aabb_size;
}
params;

void main() {
	uint out_vtx = gl_GlobalInvocationID.x;
	if (out_vtx >= params.output_vertex_count) {
		return;
	}

	uint src_idx = out_vtx;
	if (bool(params.flags & FLAG_INDEXED)) {
		IndexBufferRef indices = IndexBufferRef(params.index_buffer_address);
		if (bool(params.flags & FLAG_INDEX_16)) {
			uint w = indices.data[out_vtx >> 1];
			src_idx = ((out_vtx & 1) == 1) ? (w >> 16) : (w & 0xFFFF);
		} else {
			src_idx = indices.data[out_vtx];
		}
	}

	uint ofs = src_idx * params.vertex_stride_words;
	vec3 pos;
	if (bool(params.flags & FLAG_COMPRESSED)) {
		// Positions are unorm16 x/y/z (w packed with other data), normalized to the surface AABB.
		uint w0 = src_vertices.data[ofs];
		uint w1 = src_vertices.data[ofs + 1];
		vec3 unorm_pos = vec3(float(w0 & 0xFFFF), float(w0 >> 16), float(w1 & 0xFFFF)) / 65535.0;
		pos = unorm_pos * params.aabb_size.xyz + params.aabb_position.xyz;
	} else {
		pos = vec3(uintBitsToFloat(src_vertices.data[ofs]), uintBitsToFloat(src_vertices.data[ofs + 1]), uintBitsToFloat(src_vertices.data[ofs + 2]));
	}

	dst_vertices.data[out_vtx * 3 + 0] = pos.x;
	dst_vertices.data[out_vtx * 3 + 1] = pos.y;
	dst_vertices.data[out_vtx * 3 + 2] = pos.z;
}
