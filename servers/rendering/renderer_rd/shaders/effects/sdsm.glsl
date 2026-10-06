#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Bounds {
	vec4 minimum;
	vec4 maximum;
};
struct Record {
	Bounds cascade[4];
};

layout(set = 0, binding = 0, std430) restrict writeonly buffer Output {
	Record records[];
} output_data;

#ifdef REDUCE_RECORDS
layout(set = 1, binding = 0, std430) restrict readonly buffer Input {
	Record records[];
} input_data;
#else
#ifdef MULTISAMPLE
layout(set = 1, binding = 0) uniform sampler2DMS source_depth;
#else
layout(set = 1, binding = 0) uniform sampler2D source_depth;
#endif
layout(set = 1, binding = 1, std140) uniform Camera {
	mat4 inverse_projection;
	mat4 view_to_camera;
	mat4 view_to_light;
	vec4 cascade_begin;
	vec4 cascade_end;
} camera;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 size;
	uint groups_x;
	uint output_offset;
	uint input_count;
	uint cascade_count;
	uint sample_count;
	uint pad;
	ivec2 region_position;
	ivec2 region_size;
} params;

shared Record tile[64];

void main() {
	uint lane = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;
#ifdef REDUCE_RECORDS
	uint group_index = gl_WorkGroupID.y * params.groups_x + gl_WorkGroupID.x;
	if (group_index * 64u >= params.input_count) {
		return;
	}
#endif
	Record value;
	for (uint cascade = 0u; cascade < 4u; cascade++) {
		value.cascade[cascade].minimum = vec4(3.402823466e+38);
		value.cascade[cascade].maximum = vec4(-3.402823466e+38);
	}
#ifdef REDUCE_RECORDS
	uint index = group_index * 64u + lane;
	if (index < params.input_count) {
		value = input_data.records[index];
	}
#else
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (all(lessThan(pixel, params.size)) && all(greaterThanEqual(pixel, params.region_position)) && all(lessThan(pixel, params.region_position + params.region_size))) {
		for (uint sample_index = 0u; sample_index < params.sample_count; sample_index++) {
#ifdef MULTISAMPLE
			float depth = texelFetch(source_depth, pixel, int(sample_index)).r;
#else
			float depth = texelFetch(source_depth, pixel, 0).r;
#endif
			// Reverse-Z clear represents no receiver. Inverse homogeneous projection
			// handles perspective, off-axis/jittered, and orthographic cameras alike.
			if (depth <= 0.0) {
				continue;
			}
			float minimum_depth = 3.402823466e+38;
			float maximum_depth = -3.402823466e+38;
#ifndef REDUCE_DEPTH
			vec3 minimum_light = vec3(3.402823466e+38);
			vec3 maximum_light = vec3(-3.402823466e+38);
#endif
#ifdef MULTISAMPLE
			// RD does not expose fixed sample locations. Bounding the full pixel
			// footprint includes every possible location, also for rotated XR eyes.
			const uint corner_count = 4u;
#else
			const uint corner_count = 1u;
#endif
			for (uint corner = 0u; corner < corner_count; corner++) {
#ifdef MULTISAMPLE
				vec2 offset = vec2(float(corner & 1u), float(corner >> 1u));
#else
				vec2 offset = vec2(0.5);
#endif
				vec2 ndc = (vec2(pixel - params.region_position) + offset) / vec2(params.region_size) * 2.0 - 1.0;
				vec4 view_h = camera.inverse_projection * vec4(ndc, depth, 1.0);
				vec4 view = vec4(view_h.xyz / view_h.w, 1.0);
				float view_depth = -(camera.view_to_camera * view).z;
				minimum_depth = min(minimum_depth, view_depth);
				maximum_depth = max(maximum_depth, view_depth);
#ifndef REDUCE_DEPTH
				vec3 light_position = (camera.view_to_light * view).xyz;
				minimum_light = min(minimum_light, light_position);
				maximum_light = max(maximum_light, light_position);
#endif
			}
#ifdef REDUCE_DEPTH
			value.cascade[0].minimum.x = min(value.cascade[0].minimum.x, minimum_depth);
			value.cascade[0].maximum.x = max(value.cascade[0].maximum.x, maximum_depth);
#else
			for (uint cascade = 0u; cascade < params.cascade_count; cascade++) {
				if (maximum_depth >= camera.cascade_begin[cascade] && minimum_depth <= camera.cascade_end[cascade]) {
					value.cascade[cascade].minimum.xyz = min(value.cascade[cascade].minimum.xyz, minimum_light);
					value.cascade[cascade].maximum.xyz = max(value.cascade[cascade].maximum.xyz, maximum_light);
				}
			}
#endif
		}
	}
#endif
	tile[lane] = value;
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (lane < stride) {
			for (uint cascade = 0u; cascade < 4u; cascade++) {
				tile[lane].cascade[cascade].minimum = min(tile[lane].cascade[cascade].minimum, tile[lane + stride].cascade[cascade].minimum);
				tile[lane].cascade[cascade].maximum = max(tile[lane].cascade[cascade].maximum, tile[lane + stride].cascade[cascade].maximum);
			}
		}
		barrier();
	}
	if (lane == 0u) {
#ifdef REDUCE_RECORDS
		uint output_index = group_index;
#else
		uint output_index = params.output_offset + gl_WorkGroupID.y * params.groups_x + gl_WorkGroupID.x;
#endif
		output_data.records[output_index] = tile[0];
	}
}
