#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Unrolls an (optionally indexed, optionally compressed) mesh surface vertex
// stream into a flat, non-indexed stream of [position.xyz, uv.xy, normal.xyz]
// (8 floats per vertex), usable both as BLAS build input (positions at offset
// 0, stride 32 bytes) and for fetching triangle data in raytracing shaders.

layout(buffer_reference, buffer_reference_align = 4, std430) readonly buffer WordBufferRef {
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
#define FLAG_HAS_UV (1 << 3)
#define FLAG_UV_COMPRESSED (1 << 4)
#define FLAG_HAS_NORMAL (1 << 5)

layout(push_constant, std430) uniform Params {
	uvec2 index_buffer_address;
	uvec2 attribute_buffer_address;

	uint output_vertex_count;
	uint vertex_stride_words;
	uint attribute_stride_words;
	uint uv_offset_words;

	uint flags;
	uint normal_offset_words; // Start of the normal/tangent block within the vertex buffer.
	vec2 uv_scale;

	uint normal_stride_words;
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
		WordBufferRef indices = WordBufferRef(params.index_buffer_address);
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

	vec3 normal = vec3(0.0, 1.0, 0.0);
	if (bool(params.flags & FLAG_HAS_NORMAL)) {
		// Octahedral snorm normal in the first word of each normal/tangent entry.
		uint w = src_vertices.data[params.normal_offset_words + src_idx * params.normal_stride_words];
		vec2 e = unpackUnorm2x16(w) * 2.0 - 1.0;
		vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
		float t = max(-v.z, 0.0);
		v.x += (v.x >= 0.0) ? -t : t;
		v.y += (v.y >= 0.0) ? -t : t;
		normal = normalize(v);
	}

	vec2 uv = vec2(0.0);
	if (bool(params.flags & FLAG_HAS_UV)) {
		WordBufferRef attributes = WordBufferRef(params.attribute_buffer_address);
		uint attr_ofs = src_idx * params.attribute_stride_words + params.uv_offset_words;
		if (bool(params.flags & FLAG_UV_COMPRESSED)) {
			uv = unpackUnorm2x16(attributes.data[attr_ofs]);
		} else {
			uv = vec2(uintBitsToFloat(attributes.data[attr_ofs]), uintBitsToFloat(attributes.data[attr_ofs + 1]));
		}
		if (params.uv_scale != vec2(0.0)) {
			// Compressed UVs are packed around the 0.5 center.
			uv = (uv - 0.5) * params.uv_scale;
		}
	}

	dst_vertices.data[out_vtx * 8 + 0] = pos.x;
	dst_vertices.data[out_vtx * 8 + 1] = pos.y;
	dst_vertices.data[out_vtx * 8 + 2] = pos.z;
	dst_vertices.data[out_vtx * 8 + 3] = uv.x;
	dst_vertices.data[out_vtx * 8 + 4] = uv.y;
	dst_vertices.data[out_vtx * 8 + 5] = normal.x;
	dst_vertices.data[out_vtx * 8 + 6] = normal.y;
	dst_vertices.data[out_vtx * 8 + 7] = normal.z;
}
