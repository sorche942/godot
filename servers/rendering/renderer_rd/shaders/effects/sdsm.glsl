#[compute]

#version 450

#VERSION_DEFINES

#ifdef USE_SUBGROUPS
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#endif

#include "../sdsm_inc.glsl"
#include "../sdsm_data_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

const float MAX_VALUE = 3.402823466e+38;
struct Record { SdsmBounds cascade[4]; };
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
	uint records_per_view;
	uint view_count;
	uint dispatch_limit;
	uint reserved;
} params;

#if defined(CLASSIFY_TILES) || defined(RESCAN_TILES) || defined(FULL_BOUNDS) || defined(MAKE_SPLITS) || defined(TRANSFORM_CASTERS) || defined(FIT_RECEIVERS) || defined(FIT_CASTERS) || defined(PUBLISH_LIGHT)
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

#if defined(TILE_SUMMARY)
layout(set = 0, binding = 0, std430) writeonly buffer Output { SdsmBounds records[]; } output_data;
layout(set = 1, binding = 4, std430) writeonly buffer DepthRanges { vec2 ranges[]; } depth_ranges;
#elif defined(REDUCE_SCALARS)
layout(set = 0, binding = 0, std430) writeonly buffer Output { vec2 ranges[]; } output_data;
layout(set = 1, binding = 0, std430) readonly buffer Input { vec2 ranges[]; } input_data;
#elif defined(CLASSIFY_TILES) || defined(RESCAN_TILES) || defined(FULL_BOUNDS) || defined(REDUCE_BOUNDS)
layout(set = 0, binding = 0, std430) writeonly buffer Output { Record records[]; } output_data;
#ifdef CLASSIFY_TILES
layout(set = 1, binding = 0, std430) readonly buffer Input { SdsmBounds records[]; } input_data;
layout(set = 1, binding = 4, std430) writeonly buffer TileList { uint tiles[]; } tile_list;
layout(set = 1, binding = 5, std430) buffer Counts { uint counts[]; } rescan_counts;
#elif defined(REDUCE_BOUNDS)
layout(set = 1, binding = 0, std430) readonly buffer Input { Record records[]; } input_data;
#elif defined(RESCAN_TILES)
layout(set = 1, binding = 4, std430) readonly buffer TileList { uint tiles[]; } tile_list;
layout(set = 1, binding = 5, std430) readonly buffer Counts { uint counts[]; } rescan_counts;
#endif
#if defined(CLASSIFY_TILES) || defined(RESCAN_TILES) || defined(FULL_BOUNDS)
layout(set = 1, binding = 3, std140) uniform Fitted { SdsmResult value; } fitted;
#endif
#elif defined(BUILD_RESCAN_ARGS)
layout(set = 0, binding = 0, std430) writeonly buffer Output { uint words[]; } output_data;
layout(set = 1, binding = 0, std430) readonly buffer Counts { uint counts[]; } rescan_counts;
#elif defined(MAKE_SPLITS) || defined(FIT_RECEIVERS) || defined(FIT_CASTERS)
layout(set = 0, binding = 0, std430) buffer Output { SdsmResult value; } output_data;
#ifdef MAKE_SPLITS
layout(set = 1, binding = 0, std430) readonly buffer Input { vec2 ranges[]; } input_data;
#elif defined(FIT_RECEIVERS)
layout(set = 1, binding = 0, std430) readonly buffer Input { Record records[]; } input_data;
layout(set = 1, binding = 4, std430) readonly buffer CasterRange { vec2 ranges[]; } caster_range;
#endif
#if defined(FIT_RECEIVERS) || defined(FIT_CASTERS)
layout(set = 1, binding = 3, std430) readonly buffer Inputs { SdsmBounds bounds[]; } inputs;
#endif
#elif defined(TRANSFORM_CASTERS)
struct CasterInput { vec4 transform[3]; SdsmBounds local_bounds; };
layout(set = 0, binding = 0, std430) writeonly buffer Output { SdsmBounds bounds[]; } output_data;
layout(set = 1, binding = 0, std430) readonly buffer Input { CasterInput casters[]; } input_data;
layout(set = 1, binding = 4, std430) writeonly buffer CasterRange { vec2 ranges[]; } caster_range;
#elif defined(PUBLISH_LIGHT)
layout(set = 0, binding = 0, std430) writeonly buffer Output { SdsmResult values[]; } output_data;
layout(set = 1, binding = 0, std140) uniform Input { SdsmResult value; } input_data;
layout(set = 1, binding = 3, std140) uniform Publish { vec4 atlas[4]; vec4 settings; vec4 fade; } publish_data;
#endif

#if defined(TILE_SUMMARY) || defined(RESCAN_TILES) || defined(FULL_BOUNDS)
#ifdef MULTISAMPLE
layout(set = 1, binding = 0) uniform sampler2DMS source_depth;
#else
layout(set = 1, binding = 0) uniform sampler2D source_depth;
#endif
layout(set = 1, binding = 1, std140) uniform Camera {
	mat4 clip_to_light;
	vec4 camera_depth_row;
} camera;

bool sample_bounds(ivec2 pixel, uint sample_index, out vec4 lower, out vec4 upper) {
	lower = vec4(MAX_VALUE);
	upper = vec4(-MAX_VALUE);
	if (any(greaterThanEqual(pixel, params.size)) || any(lessThan(pixel, params.region_position)) || any(greaterThanEqual(pixel, params.region_position + params.region_size))) {
		return false;
	}
#ifdef MULTISAMPLE
	float depth = texelFetch(source_depth, pixel, int(sample_index)).r;
#else
	float depth = texelFetch(source_depth, pixel, 0).r;
#endif
	if (depth <= 0.0) {
		return false;
	}
#ifdef MULTISAMPLE
	// The raster sample locations are not exposed. Preserve the conservative
	// per-sample pixel footprint rather than resolve or select one sample.
	const uint corners = 4u;
#else
	const uint corners = 1u;
#endif
	for (uint corner = 0u; corner < corners; corner++) {
#ifdef MULTISAMPLE
		vec2 offset = vec2(float(corner & 1u), float(corner >> 1u));
#else
		vec2 offset = vec2(0.5);
#endif
		vec2 ndc = (vec2(pixel - params.region_position) + offset) / vec2(params.region_size) * 2.0 - 1.0;
		vec4 clip = vec4(ndc, depth, 1.0);
		vec4 light_h = camera.clip_to_light * clip;
		float inverse_w = 1.0 / light_h.w;
		vec4 point = vec4(light_h.xyz * inverse_w, -dot(camera.camera_depth_row, clip) * inverse_w);
		lower = min(lower, point);
		upper = max(upper, point);
	}
	return true;
}
#endif

#if defined(RESCAN_TILES) || defined(FULL_BOUNDS) || defined(REDUCE_BOUNDS)
#define REDUCTION_CASCADES 4
#else
#define REDUCTION_CASCADES 1
#endif

#if !defined(CLASSIFY_TILES) && !defined(BUILD_RESCAN_ARGS) && !defined(MAKE_SPLITS) && !defined(PUBLISH_LIGHT)
#ifdef USE_SUBGROUPS
#ifndef SDSM_MAX_SUBGROUPS
#define SDSM_MAX_SUBGROUPS 64
#endif
#define SHARED_LANES SDSM_MAX_SUBGROUPS
#else
#define SHARED_LANES 64
#endif
shared vec4 minimum_values[REDUCTION_CASCADES][SHARED_LANES];
shared vec4 maximum_values[REDUCTION_CASCADES][SHARED_LANES];

void reduce_value(vec4 lower[REDUCTION_CASCADES], vec4 upper[REDUCTION_CASCADES]) {
	uint lane = gl_LocalInvocationIndex;
#ifdef USE_SUBGROUPS
	for (uint cascade = 0u; cascade < REDUCTION_CASCADES; cascade++) {
		vec4 minimum_value = subgroupMin(lower[cascade]);
		vec4 maximum_value = subgroupMax(upper[cascade]);
		if (subgroupElect()) {
			minimum_values[cascade][gl_SubgroupID] = minimum_value;
			maximum_values[cascade][gl_SubgroupID] = maximum_value;
		}
	}
	barrier();
	if (lane == 0u) {
		for (uint cascade = 0u; cascade < REDUCTION_CASCADES; cascade++) {
			vec4 minimum_value = minimum_values[cascade][0];
			vec4 maximum_value = maximum_values[cascade][0];
			for (uint subgroup = 1u; subgroup < gl_NumSubgroups; subgroup++) {
				minimum_value = min(minimum_value, minimum_values[cascade][subgroup]);
				maximum_value = max(maximum_value, maximum_values[cascade][subgroup]);
			}
			minimum_values[cascade][0] = minimum_value;
			maximum_values[cascade][0] = maximum_value;
		}
	}
#else
	for (uint cascade = 0u; cascade < REDUCTION_CASCADES; cascade++) {
		minimum_values[cascade][lane] = lower[cascade];
		maximum_values[cascade][lane] = upper[cascade];
	}
	barrier();
	for (uint stride = 32u; stride != 0u; stride >>= 1u) {
		if (lane < stride) {
			for (uint cascade = 0u; cascade < REDUCTION_CASCADES; cascade++) {
				minimum_values[cascade][lane] = min(minimum_values[cascade][lane], minimum_values[cascade][lane + stride]);
				maximum_values[cascade][lane] = max(maximum_values[cascade][lane], maximum_values[cascade][lane + stride]);
			}
		}
		barrier();
	}
#endif
}
#endif

#ifdef FIT_CASTERS
void store_camera(uint cascade, vec3 lower, vec3 upper) {
	vec3 extent = upper - lower;
	mat4 projection = mat4(1.0);
	projection[0][0] = 2.0 / extent.x;
	projection[1][1] = 2.0 / extent.y;
	projection[2][2] = 1.0 / extent.z;
	projection[3][2] = 1.0;
	mat4 inv_projection = mat4(1.0);
	inv_projection[0][0] = extent.x * 0.5;
	inv_projection[1][1] = extent.y * 0.5;
	inv_projection[2][2] = extent.z;
	inv_projection[3][2] = -extent.z;
	vec3 center = vec3((lower.xy + upper.xy) * 0.5, upper.z);
	mat4 inv_view = light.light_to_world;
	vec3 relative_origin = mat3(inv_view) * center;
	inv_view[3].xyz += relative_origin;
	mat4 view = mat4(transpose(mat3(inv_view)));
	view[3].xyz = -mat3(view) * inv_view[3].xyz;
	vec4 origin_precision = vec4(0.0);
#ifdef DOUBLE_WORLD
	vec3 high = light.light_to_world[3].xyz;
	vec3 low = light.world_origin_low.xyz + relative_origin;
	// Reassociation would cancel the residual and move shadows in large worlds.
	precise vec3 sum = high + low;
	precise vec3 residual = (high - sum) + low;
	// Matches the large-world SceneData encoded negative origin convention.
	inv_view[3].xyz = -sum;
	origin_precision.xyz = -residual;
#endif
	output_data.value.projection[cascade] = projection;
	output_data.value.inv_projection[cascade] = inv_projection;
	output_data.value.inv_view[cascade] = inv_view;
	output_data.value.view[cascade] = view;
	output_data.value.inv_view_precision[cascade] = origin_precision;
}
#endif

void main() {
	uint lane = gl_LocalInvocationIndex;
#if defined(CLASSIFY_TILES)
	uint group = gl_WorkGroupID.y * params.groups_x + gl_WorkGroupID.x;
	uint index = group * 64u + lane;
	if (index >= params.input_count) {
		return;
	}
	SdsmBounds tile = input_data.records[index];
	Record value;
	bool ambiguous = false;
	for (uint cascade = 0u; cascade < 4u; cascade++) {
		value.cascade[cascade].minimum = vec4(MAX_VALUE);
		value.cascade[cascade].maximum = vec4(-MAX_VALUE);
		if (cascade >= params.cascade_count || tile.minimum.w > tile.maximum.w) {
			continue;
		}
		float begin = sdsm_begin(fitted.value.splits, light.ranges.x, int(cascade), light.ranges.w != 0.0);
		float end = fitted.value.splits[cascade];
		if (tile.maximum.w < begin || tile.minimum.w > end) {
			continue;
		}
		if (tile.minimum.w >= begin && tile.maximum.w <= end) {
			value.cascade[cascade].minimum.xyz = tile.minimum.xyz;
			value.cascade[cascade].maximum.xyz = tile.maximum.xyz;
		} else {
			ambiguous = true;
		}
	}
	output_data.records[index] = value;
	if (ambiguous) {
		uint view = index / params.records_per_view;
		uint slot = atomicAdd(rescan_counts.counts[view], 1u);
		tile_list.tiles[view * params.records_per_view + slot] = index % params.records_per_view;
	}
#elif defined(BUILD_RESCAN_ARGS)
	if (lane < params.view_count) {
		uint count = rescan_counts.counts[lane];
		bool bulk = count * 2u >= params.records_per_view;
		uint sparse_count = bulk ? 0u : count;
		uint x = min(sparse_count, params.dispatch_limit);
		uint base = lane * 6u;
		output_data.words[base] = x;
		output_data.words[base + 1u] = max(1u, (sparse_count + params.dispatch_limit - 1u) / params.dispatch_limit);
		output_data.words[base + 2u] = 1u;
		output_data.words[base + 3u] = bulk ? uint((params.size.x + 15) / 16) : 0u;
		output_data.words[base + 4u] = uint((params.size.y + 15) / 16);
		output_data.words[base + 5u] = 1u;
	}
#elif defined(MAKE_SPLITS)
	if (lane == 0u) {
		vec4 splits = sdsm_splits(input_data.ranges[0], light.extra_range.xy, light.ranges.x, light.ranges.z, int(light.counts.x));
		if (splits[light.counts.x - 1u] <= 0.0) {
			// Empty depth has a complete analytic fallback, not old CSM cameras.
			splits = sdsm_splits(light.ranges.xz, vec2(MAX_VALUE, -MAX_VALUE), light.ranges.x, light.ranges.z, int(light.counts.x));
		}
		output_data.value.splits = splits;
		output_data.value.meta = uvec4(1u, light.counts.x, 0u, 0u);
	}
#elif defined(PUBLISH_LIGHT)
	if (lane != 0u) {
		return;
	}
	SdsmResult result = input_data.value;
	uint count = light.counts.x;
	result.fade = vec4(-result.splits[count - 1u] * publish_data.fade.x, -result.splits[count - 1u], 0.0, 0.0);
	for (uint cascade = 0u; cascade < 4u; cascade++) {
		uint source = min(cascade, count - 1u);
		vec3 lower = result.bounds[source].minimum.xyz;
		vec3 upper = result.bounds[source].maximum.xyz;
		vec3 center = vec3((lower.xy + upper.xy) * 0.5, upper.z);
		mat4 view = light.camera_to_light;
		view[3].xyz -= center;
		mat4 atlas_bias = mat4(1.0);
		atlas_bias[0][0] = publish_data.atlas[cascade].z * 0.5;
		atlas_bias[1][1] = publish_data.atlas[cascade].w * 0.5;
		atlas_bias[3].xy = publish_data.atlas[cascade].xy + publish_data.atlas[cascade].zw * 0.5;
		result.shadow_matrix[cascade] = atlas_bias * result.projection[source] * view;
		float depth_range = upper.z - lower.z;
		result.shadow_params[cascade] = vec4(publish_data.settings.x * depth_range * publish_data.settings.w,
				publish_data.settings.y * result.texel[source], publish_data.settings.z * depth_range * publish_data.settings.w, depth_range);
		result.shadow_z_range[cascade] = depth_range;
		result.shadow_range_begin[cascade] = upper.z;
		vec2 uv_scale = publish_data.atlas[cascade].zw / (upper.xy - lower.xy);
		result.shadow_uv_scale[cascade / 2u][(cascade % 2u) * 2u] = uv_scale.x;
		result.shadow_uv_scale[cascade / 2u][(cascade % 2u) * 2u + 1u] = uv_scale.y;
		if (cascade >= count) {
			result.splits[cascade] = 0.0;
		}
	}
	output_data.values[params.output_offset] = result;
#else
	vec4 lower[REDUCTION_CASCADES];
	vec4 upper[REDUCTION_CASCADES];
	for (uint cascade = 0u; cascade < REDUCTION_CASCADES; cascade++) {
		lower[cascade] = vec4(MAX_VALUE);
		upper[cascade] = vec4(-MAX_VALUE);
	}
	uint group = gl_WorkGroupID.y * params.groups_x + gl_WorkGroupID.x;
#if defined(REDUCE_SCALARS) || defined(REDUCE_BOUNDS)
	if (group * 256u >= params.input_count) {
		return;
	}
#elif defined(TRANSFORM_CASTERS)
	if (group * 64u >= max(params.input_count, 1u)) {
		return;
	}
#endif
#ifdef REDUCE_SCALARS
	for (uint item = 0u; item < 4u; item++) {
		uint index = group * 256u + lane + item * 64u;
		if (index < params.input_count) {
			vec2 range = input_data.ranges[index];
			lower[0].x = min(lower[0].x, range.x);
			upper[0].x = max(upper[0].x, range.y);
		}
	}
#elif defined(REDUCE_BOUNDS)
	for (uint item = 0u; item < 4u; item++) {
		uint index = group * 256u + lane + item * 64u;
		if (index < params.input_count) {
			for (uint cascade = 0u; cascade < 4u; cascade++) {
				lower[cascade] = min(lower[cascade], input_data.records[index].cascade[cascade].minimum);
				upper[cascade] = max(upper[cascade], input_data.records[index].cascade[cascade].maximum);
			}
		}
	}
#elif defined(TILE_SUMMARY) || defined(RESCAN_TILES) || defined(FULL_BOUNDS)
#ifdef RESCAN_TILES
	if (group >= rescan_counts.counts[params.output_offset / params.records_per_view]) {
		return;
	}
	uint tile = tile_list.tiles[params.output_offset + group];
	uint tiles_x = uint((params.size.x + 15) / 16);
	ivec2 tile_position = ivec2(tile % tiles_x, tile / tiles_x) * 16;
#else
	uint tile = gl_WorkGroupID.y * uint((params.size.x + 15) / 16) + gl_WorkGroupID.x;
	ivec2 tile_position = ivec2(gl_WorkGroupID.xy) * 16;
#endif
	for (uint item = 0u; item < 4u; item++) {
		ivec2 pixel = tile_position + ivec2(gl_LocalInvocationID.xy) + ivec2(int(item & 1u), int(item >> 1u)) * 8;
		for (uint sample_index = 0u; sample_index < params.sample_count; sample_index++) {
			vec4 sample_lower;
			vec4 sample_upper;
			if (!sample_bounds(pixel, sample_index, sample_lower, sample_upper)) {
				continue;
			}
#ifdef TILE_SUMMARY
			lower[0] = min(lower[0], sample_lower);
			upper[0] = max(upper[0], sample_upper);
#else
			for (uint cascade = 0u; cascade < params.cascade_count; cascade++) {
				float begin = sdsm_begin(fitted.value.splits, light.ranges.x, int(cascade), light.ranges.w != 0.0);
				if (sample_upper.w >= begin && sample_lower.w <= fitted.value.splits[cascade]) {
					lower[cascade].xyz = min(lower[cascade].xyz, sample_lower.xyz);
					upper[cascade].xyz = max(upper[cascade].xyz, sample_upper.xyz);
				}
			}
#endif
		}
	}
#elif defined(TRANSFORM_CASTERS)
	uint index = group * 64u + lane;
	if (index < params.input_count) {
		CasterInput caster = input_data.casters[index];
		mat3 model = transpose(mat3(caster.transform[0].xyz, caster.transform[1].xyz, caster.transform[2].xyz));
		vec3 origin = vec3(caster.transform[0].w, caster.transform[1].w, caster.transform[2].w);
		vec3 center = (caster.local_bounds.minimum.xyz + caster.local_bounds.maximum.xyz) * 0.5;
		vec3 half_size = (caster.local_bounds.maximum.xyz - caster.local_bounds.minimum.xyz) * 0.5;
		mat3 world_to_light = transpose(mat3(light.light_to_world));
		mat3 basis = world_to_light * model;
		vec3 light_center = basis * center + world_to_light * origin;
		vec3 extent = abs(basis[0]) * half_size.x + abs(basis[1]) * half_size.y + abs(basis[2]) * half_size.z;
		// Outward roundoff allowance prevents the visibility test from clipping
		// a transformed vertex at a fitted border, including large coordinates.
		vec3 error = max(vec3(0.00001), (abs(light_center) + extent) * 0.000002);
		vec3 minimum_value = light_center - extent - error;
		vec3 maximum_value = light_center + extent + error;
		output_data.bounds[index].minimum = vec4(minimum_value, 0.0);
		output_data.bounds[index].maximum = vec4(maximum_value, 0.0);
		lower[0].x = minimum_value.z;
		upper[0].x = maximum_value.z;
	}
#elif defined(FIT_RECEIVERS)
	uint cascade = gl_WorkGroupID.x;
	vec4 splits = output_data.value.splits;
	float begin = sdsm_begin(splits, light.ranges.x, int(cascade), light.ranges.w != 0.0);
	float end = splits[cascade];
	if (lane == 0u) {
		lower[0].xyz = input_data.records[0].cascade[cascade].minimum.xyz;
		upper[0].xyz = input_data.records[0].cascade[cascade].maximum.xyz;
	}
	for (uint index = lane; index < light.counts.z; index += 64u) {
		SdsmBounds extra = inputs.bounds[light.counts.y + index];
		if (extra.maximum.w >= begin && extra.minimum.w <= end) {
			lower[0].xyz = min(lower[0].xyz, extra.minimum.xyz);
			upper[0].xyz = max(upper[0].xyz, extra.maximum.xyz);
		}
	}
	if (lane < 8u && light.fog.x > 0.0) {
		float fog_begin = cascade == 0u ? 0.0 : begin;
		float fog_end = min(end, light.fog.x);
		if (fog_begin < fog_end) {
			float depth = lane < 4u ? fog_begin : fog_end;
			vec2 signs = vec2((lane & 1u) == 0u ? -1.0 : 1.0, (lane & 2u) == 0u ? -1.0 : 1.0);
			vec2 extent = mix(light.fog_sizes.xy, light.fog_sizes.zw, depth / light.fog.x);
			vec3 point = (light.camera_to_light * vec4(extent * signs, -depth, 1.0)).xyz;
			lower[0].xyz = min(lower[0].xyz, point);
			upper[0].xyz = max(upper[0].xyz, point);
		}
	}
#elif defined(FIT_CASTERS)
	uint cascade = gl_WorkGroupID.x;
	vec3 minimum_value = output_data.value.bounds[cascade].minimum.xyz;
	vec3 maximum_value = output_data.value.bounds[cascade].maximum.xyz;
	float texel = output_data.value.texel[cascade];
	upper[0].x = maximum_value.z;
	for (uint index = lane; index < light.counts.y; index += 64u) {
		SdsmBounds caster = inputs.bounds[index];
		if (all(lessThanEqual(caster.minimum.xy, maximum_value.xy)) && all(greaterThanEqual(caster.maximum.xy, minimum_value.xy)) && caster.maximum.z >= minimum_value.z - light.settings.z * texel) {
			upper[0].x = max(upper[0].x, caster.maximum.z);
		}
	}
#endif
	reduce_value(lower, upper);
	if (lane == 0u) {
#ifdef REDUCE_SCALARS
		output_data.ranges[group] = vec2(minimum_values[0][0].x, maximum_values[0][0].x);
#elif defined(REDUCE_BOUNDS)
		for (uint cascade = 0u; cascade < 4u; cascade++) {
			output_data.records[group].cascade[cascade].minimum = minimum_values[cascade][0];
			output_data.records[group].cascade[cascade].maximum = maximum_values[cascade][0];
		}
#elif defined(TILE_SUMMARY)
		uint index = params.output_offset + tile;
		output_data.records[index].minimum = minimum_values[0][0];
		output_data.records[index].maximum = maximum_values[0][0];
		if (params.pad != 0u) {
			depth_ranges.ranges[index] = vec2(minimum_values[0][0].w, maximum_values[0][0].w);
		}
#elif defined(RESCAN_TILES) || defined(FULL_BOUNDS)
		for (uint cascade = 0u; cascade < 4u; cascade++) {
			output_data.records[params.output_offset + tile].cascade[cascade].minimum = minimum_values[cascade][0];
			output_data.records[params.output_offset + tile].cascade[cascade].maximum = maximum_values[cascade][0];
		}
#elif defined(TRANSFORM_CASTERS)
		caster_range.ranges[group] = vec2(minimum_values[0][0].x, maximum_values[0][0].x);
#elif defined(FIT_RECEIVERS)
		vec3 minimum_value = minimum_values[0][0].xyz;
		vec3 maximum_value = maximum_values[0][0].xyz;
		if (any(greaterThan(minimum_value, maximum_value))) {
			for (uint corner = 0u; corner < 4u; corner++) {
				vec3 near_point = light.corners[corner + 4u].xyz;
				vec3 far_point = light.corners[corner].xyz;
				vec3 first = mix(near_point, far_point, (begin - light.ranges.x) / (light.ranges.y - light.ranges.x));
				vec3 last = mix(near_point, far_point, (end - light.ranges.x) / (light.ranges.y - light.ranges.x));
				minimum_value = min(minimum_value, min(first, last));
				maximum_value = max(maximum_value, max(first, last));
			}
		}
		float caster_far = max(maximum_value.z, caster_range.ranges[0].y);
		float texel = sdsm_expand_xy(minimum_value, maximum_value, caster_far, light.settings.x, light.settings.y, light.settings.z, light.settings.w, light.fog.y);
		output_data.value.bounds[cascade].minimum = vec4(minimum_value, begin);
		output_data.value.bounds[cascade].maximum = vec4(maximum_value, end);
		output_data.value.texel[cascade] = texel;
#elif defined(FIT_CASTERS)
		maximum_value.z = maximum_values[0][0].x;
		sdsm_expand_z(minimum_value, maximum_value, texel, light.settings.z);
		output_data.value.bounds[cascade].minimum.xyz = minimum_value;
		output_data.value.bounds[cascade].maximum.xyz = maximum_value;
		store_camera(cascade, minimum_value, maximum_value);
		if (cascade + 1u == light.counts.x) {
			for (uint unused = light.counts.x; unused < 4u; unused++) {
				output_data.value.bounds[unused] = output_data.value.bounds[cascade];
				output_data.value.texel[unused] = texel;
				output_data.value.projection[unused] = output_data.value.projection[cascade];
				output_data.value.inv_projection[unused] = output_data.value.inv_projection[cascade];
				output_data.value.inv_view[unused] = output_data.value.inv_view[cascade];
				output_data.value.view[unused] = output_data.value.view[cascade];
				output_data.value.inv_view_precision[unused] = output_data.value.inv_view_precision[cascade];
			}
		}
#endif
	}
#endif
}
