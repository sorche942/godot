#[compute]

#version 450

#VERSION_DEFINES

#include "../sdsm_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
struct Bounds { vec4 minimum; vec4 maximum; };
struct Record { Bounds cascade[4]; };
struct Result { vec4 splits; Bounds bounds[4]; vec4 texel; };
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

#if defined(MAKE_SPLITS) || defined(FIT_BOUNDS) || defined(PATCH_SCENE) || defined(PATCH_LIGHT)
#define SINGLE_STAGE
#endif
#if !defined(REDUCE_DEPTH) && !defined(REDUCE_RECORDS)
layout(set = 1, binding = 2, std140) uniform Light {
	mat4 camera_to_light;
	mat4 light_to_world;
	vec4 world_origin_low;
	vec4 corners[8];
	vec4 ranges;
	vec4 extra_range;
	vec4 settings;
	vec4 fog;
	vec4 fog_sizes;
	uvec4 counts;
} light;
#endif
#if defined(MAKE_SPLITS) || defined(FIT_BOUNDS)
layout(set = 0, binding = 0, std430) buffer Output { Result value; } output_data;
layout(set = 1, binding = 0, std430) readonly buffer Input { Record records[]; } input_data;
#ifdef FIT_BOUNDS
layout(set = 1, binding = 3, std430) readonly buffer Inputs { Bounds bounds[]; } inputs;
#endif
#elif defined(PATCH_SCENE) || defined(PATCH_LIGHT)
layout(set = 0, binding = 0, std430) buffer Output { float words[]; } output_data;
layout(set = 1, binding = 0, std430) readonly buffer Input { Result value; } input_data;
#ifdef PATCH_LIGHT
layout(set = 1, binding = 3, std140) uniform Patch { vec4 atlas[4]; vec4 settings; vec4 fade; } patch_data;
#endif
#else
layout(set = 0, binding = 0, std430) writeonly buffer Output { Record records[]; } output_data;
#ifdef REDUCE_RECORDS
layout(set = 1, binding = 0, std430) readonly buffer Input { Record records[]; } input_data;
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
} camera;
#ifndef REDUCE_DEPTH
layout(set = 1, binding = 3, std430) readonly buffer Fitted { Result value; } fitted;
#endif
#endif
#endif

#ifdef SINGLE_STAGE
void merge_point(inout vec3 lower, inout vec3 upper, vec3 point) {
	lower = min(lower, point);
	upper = max(upper, point);
}
void store_matrix(uint offset, mat4 matrix) {
#if defined(PATCH_SCENE) || defined(PATCH_LIGHT)
	for (uint column = 0u; column < 4u; column++) {
		for (uint row = 0u; row < 4u; row++) {
			output_data.words[params.output_offset + offset + column * 4u + row] = matrix[column][row];
		}
	}
#endif
}
mat4 shadow_view(vec3 lower, vec3 upper) {
	vec3 center = vec3((lower.xy + upper.xy) * 0.5, upper.z);
	mat4 view = light.camera_to_light;
	view[3].xyz -= center;
	return view;
}
void main() {
	if (gl_LocalInvocationIndex != 0u) {
		return;
	}
#ifdef MAKE_SPLITS
	output_data.value.splits = sdsm_splits(vec2(input_data.records[0].cascade[0].minimum.x, input_data.records[0].cascade[0].maximum.x), light.extra_range.xy, light.ranges.x, light.ranges.z, int(light.counts.x));
#elif defined(FIT_BOUNDS)
	vec4 splits = output_data.value.splits;
	if (splits[light.counts.x - 1u] <= 0.0) {
		return;
	}
	for (uint cascade = 0u; cascade < light.counts.x; cascade++) {
		float begin = sdsm_begin(splits, light.ranges.x, int(cascade), light.ranges.w != 0.0);
		float end = splits[cascade];
		vec3 lower = input_data.records[0].cascade[cascade].minimum.xyz;
		vec3 upper = input_data.records[0].cascade[cascade].maximum.xyz;
		for (uint i = 0u; i < light.counts.z; i++) {
			Bounds extra = inputs.bounds[light.counts.y + i];
			if (extra.maximum.w >= begin && extra.minimum.w <= end) {
				merge_point(lower, upper, extra.minimum.xyz);
				merge_point(lower, upper, extra.maximum.xyz);
			}
		}
		float fog_begin = cascade == 0u ? 0.0 : begin;
		float fog_end = min(end, light.fog.x);
		if (light.fog.x > 0.0 && fog_begin < fog_end) {
			for (int plane = 0; plane < 2; plane++) {
				float depth = plane == 0 ? fog_begin : fog_end;
				vec2 extent = mix(light.fog_sizes.xy, light.fog_sizes.zw, depth / light.fog.x);
				for (int corner = 0; corner < 4; corner++) {
					vec2 signs = vec2((corner & 1) == 0 ? -1.0 : 1.0, (corner & 2) == 0 ? -1.0 : 1.0);
					merge_point(lower, upper, (light.camera_to_light * vec4(extent * signs, -depth, 1.0)).xyz);
				}
			}
		}
		if (any(greaterThan(lower, upper))) {
			for (int corner = 0; corner < 4; corner++) {
				vec3 near_point = light.corners[corner + 4].xyz;
				vec3 far_point = light.corners[corner].xyz;
				merge_point(lower, upper, mix(near_point, far_point, (begin - light.ranges.x) / (light.ranges.y - light.ranges.x)));
				merge_point(lower, upper, mix(near_point, far_point, (end - light.ranges.x) / (light.ranges.y - light.ranges.x)));
			}
		}
		float caster_far = upper.z;
		for (uint i = 0u; i < light.counts.y; i++) {
			caster_far = max(caster_far, inputs.bounds[i].maximum.z);
		}
		float texel = sdsm_expand_xy(lower, upper, caster_far, light.settings.x, light.settings.y, light.settings.z, light.settings.w, light.fog.y);
		for (uint i = 0u; i < light.counts.y; i++) {
			Bounds caster = inputs.bounds[i];
			if (all(lessThanEqual(caster.minimum.xy, upper.xy)) && all(greaterThanEqual(caster.maximum.xy, lower.xy)) && caster.maximum.z >= lower.z - light.settings.z * texel) {
				upper.z = max(upper.z, caster.maximum.z);
			}
		}
		sdsm_expand_z(lower, upper, texel, light.settings.z);
		output_data.value.bounds[cascade].minimum = vec4(lower, begin);
		output_data.value.bounds[cascade].maximum = vec4(upper, end);
		output_data.value.texel[cascade] = texel;
	}
#else
	if (input_data.value.splits[light.counts.x - 1u] <= 0.0) {
		return;
	}
#ifdef PATCH_SCENE
	uint cascade = params.cascade_count;
	vec3 lower = input_data.value.bounds[cascade].minimum.xyz;
	vec3 upper = input_data.value.bounds[cascade].maximum.xyz;
	mat4 projection = sdsm_projection(lower, upper);
	// Vulkan reverse Z, with the same Y correction as the uploaded SceneData.
	mat4 correction = mat4(1.0);
	correction[1][1] = params.pad != 0u ? -1.0 : 1.0;
	correction[2][2] = -0.5;
	correction[3][2] = 0.5;
	projection = correction * projection;
	store_matrix(0u, projection);
	store_matrix(16u, inverse(projection));
	vec3 center = vec3((lower.xy + upper.xy) * 0.5, upper.z);
	mat4 inv_view = light.light_to_world;
	vec3 relative_origin = mat3(inv_view) * center;
	inv_view[3].xyz += relative_origin;
	mat4 view = inverse(inv_view);
#ifdef DOUBLE_WORLD
	// Shader large-world paths subtract encoded negative camera origin before
	// rotation; do not round a small fitted offset into the world high part.
	vec3 origin_high = light.light_to_world[3].xyz;
	vec3 origin_low = light.world_origin_low.xyz + relative_origin;
	vec3 sum = origin_high + origin_low;
	vec3 residual = (origin_high - sum) + origin_low;
#endif
	for (uint row = 0u; row < 3u; row++) {
		for (uint column = 0u; column < 4u; column++) {
			output_data.words[params.output_offset + 32u + row * 4u + column] = inv_view[column][row];
			output_data.words[params.output_offset + 44u + row * 4u + column] = view[column][row];
		}
#ifdef DOUBLE_WORLD
		output_data.words[params.output_offset + 32u + row * 4u + 3u] = -sum[row];
		output_data.words[params.output_offset + 56u + row] = -residual[row];
#endif
	}
#ifdef DOUBLE_WORLD
	const uint view_start = 60u;
#else
	const uint view_start = 56u;
#endif
	for (uint eye = 0u; eye < params.sample_count; eye++) {
		store_matrix(view_start + eye * 16u, projection);
		store_matrix(view_start + 32u + eye * 16u, inverse(projection));
	}
	// Prefix through the fixed two-view arrays and the four 32-vec4 kernels.
	output_data.words[params.output_offset + view_start + 614u] = upper.z - lower.z;
	output_data.words[params.output_offset + view_start + 615u] = 0.0;
#elif defined(PATCH_LIGHT)
	uint offset = params.output_offset;
	uint count = light.counts.x;
	output_data.words[offset + 14u] = -input_data.value.splits[count - 1u] * patch_data.fade.x;
	output_data.words[offset + 15u] = -input_data.value.splits[count - 1u];
	for (uint cascade = 0u; cascade < 4u; cascade++) {
		uint source = min(cascade, count - 1u);
		vec3 lower = input_data.value.bounds[source].minimum.xyz;
		vec3 upper = input_data.value.bounds[source].maximum.xyz;
		float texel = input_data.value.texel[source];
		vec2 extent = upper.xy - lower.xy;
		mat4 projection = sdsm_projection(lower, upper);
		mat4 correction = mat4(1.0);
		correction[2][2] = -1.0;
		mat4 bias = mat4(1.0);
		bias[0][0] = 0.5;
		bias[1][1] = 0.5;
		bias[2][2] = 0.5;
		bias[3].xyz = vec3(0.5);
		mat4 atlas = mat4(1.0);
		atlas[0][0] = patch_data.atlas[cascade].z;
		atlas[1][1] = patch_data.atlas[cascade].w;
		atlas[3].xy = patch_data.atlas[cascade].xy;
		store_matrix(44u + cascade * 16u, atlas * bias * correction * projection * shadow_view(lower, upper));
		output_data.words[offset + 20u + cascade] = patch_data.settings.x * (upper.z - lower.z) * patch_data.settings.w;
		output_data.words[offset + 24u + cascade] = patch_data.settings.y * texel;
		output_data.words[offset + 28u + cascade] = patch_data.settings.z * (upper.z - lower.z) * patch_data.settings.w;
		output_data.words[offset + 32u + cascade] = upper.z - lower.z;
		output_data.words[offset + 36u + cascade] = upper.z;
		output_data.words[offset + 40u + cascade] = cascade < count ? input_data.value.splits[cascade] : 0.0;
		output_data.words[offset + 108u + cascade * 2u] = patch_data.atlas[cascade].z / extent.x;
		output_data.words[offset + 109u + cascade * 2u] = patch_data.atlas[cascade].w / extent.y;
	}
#endif
#endif
}
#else
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
			// Actual MSAA sample locations are unknown: bound the pixel footprint.
			for (uint corner = 0u; corner < 4u; corner++) {
				vec2 offset = vec2(float(corner & 1u), float(corner >> 1u));
#else
			// A single sample is at the pixel center. Its inverse projection
			// already includes raster jitter; reconstruct it only once.
			for (uint corner = 0u; corner < 1u; corner++) {
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
				float begin = sdsm_begin(fitted.value.splits, light.ranges.x, int(cascade), light.ranges.w != 0.0);
				if (maximum_depth >= begin && minimum_depth <= fitted.value.splits[cascade]) {
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
#endif
