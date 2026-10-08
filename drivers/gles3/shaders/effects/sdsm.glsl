/* clang-format off */
#[modes]
mode_sample = #define MODE_SAMPLE
mode_summary = #define MODE_SUMMARY
mode_reduce = #define MODE_REDUCE
mode_splits = #define MODE_SPLITS
mode_fit = #define MODE_FIT
mode_casters = #define MODE_CASTERS
mode_caster_reduce = #define MODE_CASTER_REDUCE
mode_finalize = #define MODE_FINALIZE
mode_camera = #define MODE_CAMERA
#[vertex]
layout(location = 0) in vec2 vertex_attrib;
void main() {
	gl_Position = vec4(vertex_attrib, 0.0, 1.0);
}
#[fragment]
/* clang-format on */
#include "../../../../servers/rendering/renderer_rd/shaders/sdsm_inc.glsl"
uniform highp sampler2D source_texture; // texunit:0
uniform highp sampler2D split_texture; // texunit:1
uniform highp sampler2D records_texture; // texunit:2
#ifdef MODE_SAMPLE
uniform highp sampler2D tile_depth_texture; // texunit:3
uniform highp sampler2D tile_min_texture; // texunit:4
uniform highp sampler2D tile_max_texture; // texunit:5
#endif
#ifdef MODE_REDUCE
uniform highp sampler2D source_max_texture; // texunit:3
#endif
#ifdef MODE_FINALIZE
uniform highp sampler2D caster_texture; // texunit:3
#endif
uniform mat4 inverse_projection;
uniform mat4 view_to_camera;
uniform mat4 camera_to_light;
uniform mat4 light_to_world;
uniform vec4 region;
// Near, far, shadow far, cascade count.
uniform vec4 camera_parameters;
// Resolution, blur, normal bias, angular size.
uniform vec4 fit_parameters;
// Pancake, depth bias, fade start, blend.
uniform vec4 shadow_parameters;
uniform vec2 extra_range;
uniform int view_count;
uniform int receiver_count;
uniform int caster_count;
uniform float caster_max_z;
uniform int caster_group_size;
uniform int cascade_index;
uniform int row_offset;
uniform int operation;
layout(location = 0) out highp vec4 frag_color;
#if defined(MODE_SAMPLE) || defined(MODE_REDUCE)
layout(location = 1) out highp vec4 frag_upper;
#endif
#ifdef MODE_SUMMARY
layout(location = 1) out highp vec4 frag_min;
layout(location = 2) out highp vec4 frag_max;
#endif
vec4 record_texel(int index) {
	ivec2 dimensions = textureSize(records_texture, 0);
	return texelFetch(records_texture, ivec2(index % dimensions.x, index / dimensions.x), 0);
}
void main() {
#ifdef MODE_SPLITS
	vec2 range = vec2(3.402823e38, -3.402823e38);
	for (int view = 0; view < view_count; view++) {
		vec2 value = texelFetch(source_texture, ivec2(view, 0), 0).xy;
		range.x = min(range.x, value.x);
		range.y = max(range.y, -value.y);
	}
	frag_color = sdsm_splits(range, extra_range, camera_parameters.x, camera_parameters.z, int(camera_parameters.w));
#elif defined(MODE_FIT)
	int cascade = int(gl_FragCoord.y);
	vec4 splits = texelFetch(split_texture, ivec2(0), 0);
	if (cascade >= int(camera_parameters.w) || splits[int(camera_parameters.w) - 1] <= 0.0) {
		frag_color = vec4(0.0);
		return;
	}
	float begin = sdsm_begin(splits, camera_parameters.x, cascade, shadow_parameters.w > 0.5);
	float end = splits[cascade];
	vec3 lower = vec3(3.402823e38);
	vec3 upper = vec3(-3.402823e38);
	for (int view = 0; view < view_count; view++) {
		lower = min(lower, texelFetch(source_texture, ivec2(view * 2, cascade), 0).xyz);
		upper = max(upper, -texelFetch(source_texture, ivec2(view * 2 + 1, cascade), 0).xyz);
	}
	for (int receiver = 0; receiver < receiver_count; receiver++) {
		vec4 low = record_texel(16 + receiver * 2);
		vec4 high = record_texel(17 + receiver * 2);
		if (high.w >= begin && low.w <= end) {
			lower = min(lower, low.xyz);
			upper = max(upper, high.xyz);
		}
	}
	if (lower.x > upper.x) {
		// Empty intervals still cover their camera frustum, not a stale fit.
		for (int view = 0; view < view_count; view++) {
			for (int corner = 0; corner < 4; corner++) {
				vec3 near_corner = record_texel((view * 4 + corner) * 2).xyz;
				vec3 far_corner = record_texel((view * 4 + corner) * 2 + 1).xyz;
				vec3 first = mix(near_corner, far_corner, (begin - camera_parameters.x) / (camera_parameters.y - camera_parameters.x));
				vec3 last = mix(near_corner, far_corner, (end - camera_parameters.x) / (camera_parameters.y - camera_parameters.x));
				lower = min(lower, min(first, last));
				upper = max(upper, max(first, last));
			}
		}
	}
	float caster_far = max(upper.z, caster_max_z);
	float texel = sdsm_expand_xy(lower, upper, caster_far, fit_parameters.x, fit_parameters.y, fit_parameters.z, fit_parameters.w, shadow_parameters.x);
	// Caster overlap is reduced in parallel after XY padding is known.
	frag_color = int(gl_FragCoord.x) == 0 ? vec4(lower, 1.0) : vec4(upper, texel);
#elif defined(MODE_CASTERS)
	int cascade = int(gl_FragCoord.y);
	vec4 lower = texelFetch(source_texture, ivec2(0, cascade), 0);
	vec4 upper = texelFetch(source_texture, ivec2(1, cascade), 0);
	float maximum = upper.z;
	int first = int(gl_FragCoord.x) * caster_group_size;
	int last = min(first + caster_group_size, caster_count);
	if (lower.w > 0.0) {
		for (int caster = first; caster < last; caster++) {
			vec3 low = record_texel(16 + (receiver_count + caster) * 2).xyz;
			vec3 high = record_texel(17 + (receiver_count + caster) * 2).xyz;
			if (all(lessThanEqual(low.xy, upper.xy)) && all(greaterThanEqual(high.xy, lower.xy))) {
				maximum = max(maximum, high.z);
			}
		}
	}
	frag_color = vec4(maximum);
#elif defined(MODE_CASTER_REDUCE)
	int first = int(gl_FragCoord.x) * 4;
	float maximum = -3.402823e38;
	for (int i = 0; i < 4; i++) {
		if (first + i < textureSize(source_texture, 0).x) {
			maximum = max(maximum, texelFetch(source_texture, ivec2(first + i, int(gl_FragCoord.y)), 0).x);
		}
	}
	frag_color = vec4(maximum);
#elif defined(MODE_FINALIZE)
	int cascade = int(gl_FragCoord.y);
	vec4 lower = texelFetch(source_texture, ivec2(0, cascade), 0);
	vec4 upper = texelFetch(source_texture, ivec2(1, cascade), 0);
	if (lower.w > 0.0) {
		upper.z = max(texelFetch(caster_texture, ivec2(0, cascade), 0).x, lower.z + shadow_parameters.x);
		vec3 low = lower.xyz;
		vec3 high = upper.xyz;
		sdsm_expand_z(low, high, upper.w, fit_parameters.z);
		lower.xyz = low;
		upper.xyz = high;
	}
	frag_color = int(gl_FragCoord.x) == 0 ? lower : upper;
#elif defined(MODE_CAMERA)
	int cascade = int(gl_FragCoord.y) - row_offset;
	int column = int(gl_FragCoord.x);
	vec4 lower = texelFetch(source_texture, ivec2(0, cascade), 0);
	vec4 upper = texelFetch(source_texture, ivec2(1, cascade), 0);
	vec4 splits = texelFetch(split_texture, ivec2(0), 0);
	if (lower.w <= 0.0) {
		frag_color = vec4(0.0);
		return;
	}
	vec3 center = vec3((lower.xy + upper.xy) * 0.5, upper.z);
	mat4 camera = light_to_world;
	camera[3] = light_to_world * vec4(center, 1.0);
	mat4 projection = sdsm_projection(lower.xyz, upper.xyz);
	// Compatibility uses reversed depth in the OpenGL [-1,1] clip range.
	projection[0].z = -projection[0].z;
	projection[1].z = -projection[1].z;
	projection[2].z = -projection[2].z;
	projection[3].z = -projection[3].z;
	if (column == 0) {
		frag_color = lower;
	} else if (column == 1) {
		frag_color = upper;
	} else if (column == 2) {
		frag_color = splits;
	} else if (column == 3) {
		frag_color = vec4(fit_parameters.z * upper.w, shadow_parameters.y * (upper.z - lower.z), -splits.w * min(shadow_parameters.z, 0.999), -splits.w);
	} else if (column < 8) {
		frag_color = projection[column - 4];
	} else if (column < 12) {
		frag_color = inverse(camera)[column - 8];
	} else if (column < 16) {
		frag_color = camera[column - 12];
	} else if (column < 20) {
		frag_color = inverse(projection)[column - 16];
	} else {
		// Precompute the reusable main-camera-space -> shadow clip matrix.
		// Atlas placement is assigned later by the shadow renderer.
		frag_color = (projection * inverse(camera) * view_to_camera)[column - 20];
	}
#else
	ivec2 size = textureSize(source_texture, 0);
	ivec2 begin_pixel = ivec2(gl_FragCoord.xy) * 4;
	vec4 result = vec4(3.402823e38);
#if defined(MODE_SAMPLE) || defined(MODE_REDUCE)
	vec4 upper_result = vec4(3.402823e38);
#endif
#ifdef MODE_SUMMARY
	vec3 light_min = vec3(3.402823e38);
	vec3 light_max = vec3(-3.402823e38);
#endif
#ifdef MODE_SAMPLE
	vec2 cascade_range = vec2(0.0);
	if (operation != 0) {
		vec4 splits = texelFetch(split_texture, ivec2(0), 0);
		cascade_range = vec2(sdsm_begin(splits, camera_parameters.x, cascade_index, shadow_parameters.w > 0.5), splits[cascade_index]);
	}
	if (operation != 0) {
		ivec2 tile = ivec2(gl_FragCoord.xy);
		vec2 interval = texelFetch(tile_depth_texture, tile, 0).xy;
		if (-interval.y < cascade_range.x || interval.x > cascade_range.y) {
			frag_color = result;
			frag_upper = upper_result;
			return;
		}
		if (interval.x >= cascade_range.x && -interval.y <= cascade_range.y) {
			frag_color = texelFetch(tile_min_texture, tile, 0);
			frag_upper = texelFetch(tile_max_texture, tile, 0);
			return;
		}
	}
#endif
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			ivec2 pixel = begin_pixel + ivec2(x, y);
			if (any(greaterThanEqual(pixel, size))) {
				continue;
			}
#if defined(MODE_SAMPLE) || defined(MODE_SUMMARY)
			if (any(lessThan(vec2(pixel), region.xy)) || any(greaterThanEqual(vec2(pixel), region.zw))) {
				continue;
			}
			float depth = texelFetch(source_texture, pixel, 0).r;
			if (depth <= 0.0) {
				continue;
			}
			vec3 positions[4];
			vec2 depths = vec2(3.402823e38, -3.402823e38);
			for (int corner = 0; corner < 4; corner++) {
				vec2 uv = (vec2(pixel) + vec2(corner & 1, corner >> 1)) / vec2(size);
				vec4 position = inverse_projection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
				position /= position.w;
				position = view_to_camera * position;
				positions[corner] = position.xyz;
				depths = vec2(min(depths.x, -position.z), max(depths.y, -position.z));
			}
#ifdef MODE_SUMMARY
			result = min(result, vec4(depths.x, -depths.y, 0.0, 0.0));
			for (int corner = 0; corner < 4; corner++) {
				vec3 position = (camera_to_light * vec4(positions[corner], 1.0)).xyz;
				light_min = min(light_min, position);
				light_max = max(light_max, position);
			}
#else
			if (operation == 0) {
				result = min(result, vec4(depths.x, -depths.y, 0.0, 0.0));
			} else if (depths.y >= cascade_range.x && depths.x <= cascade_range.y) {
				for (int corner = 0; corner < 4; corner++) {
					vec3 light_position = (camera_to_light * vec4(positions[corner], 1.0)).xyz;
					result = min(result, vec4(light_position, 0.0));
					upper_result = min(upper_result, vec4(-light_position, 0.0));
				}
			}
#endif
#else
			result = min(result, texelFetch(source_texture, pixel, 0));
#ifdef MODE_REDUCE
			if (operation != 0) {
				upper_result = min(upper_result, texelFetch(source_max_texture, pixel, 0));
			}
#endif
#endif
		}
	}
	frag_color = result;
#if defined(MODE_SAMPLE) || defined(MODE_REDUCE)
	frag_upper = upper_result;
#endif
#ifdef MODE_SUMMARY
	frag_min = vec4(light_min, 0.0);
	frag_max = vec4(-light_max, 0.0);
#endif
#endif
}
