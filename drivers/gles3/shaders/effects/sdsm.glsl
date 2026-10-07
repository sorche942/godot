/* clang-format off */
#[modes]
mode_sample = #define MODE_SAMPLE
mode_reduce = #define MODE_REDUCE
mode_splits = #define MODE_SPLITS
mode_fit = #define MODE_FIT
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
uniform int cascade_index;
uniform int row_offset;
uniform int operation;
layout(location = 0) out highp vec4 frag_color;
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
	float caster_far = upper.z;
	for (int caster = 0; caster < caster_count; caster++) {
		caster_far = max(caster_far, record_texel(17 + (receiver_count + caster) * 2).z);
	}
	float texel = sdsm_expand_xy(lower, upper, caster_far, fit_parameters.x, fit_parameters.y, fit_parameters.z, fit_parameters.w, shadow_parameters.x);
	for (int caster = 0; caster < caster_count; caster++) {
		vec3 low = record_texel(16 + (receiver_count + caster) * 2).xyz;
		vec3 high = record_texel(17 + (receiver_count + caster) * 2).xyz;
		if (all(lessThanEqual(low.xy, upper.xy)) && all(greaterThanEqual(high.xy, lower.xy))) {
			upper.z = max(upper.z, high.z);
		}
	}
	upper.z = max(upper.z, lower.z + shadow_parameters.x);
	sdsm_expand_z(lower, upper, texel, fit_parameters.z);
	frag_color = int(gl_FragCoord.x) == 0 ? vec4(lower, 1.0) : vec4(upper, texel);
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
	} else {
		frag_color = camera[column - 12];
	}
#else
	ivec2 size = textureSize(source_texture, 0);
	ivec2 begin_pixel = ivec2(gl_FragCoord.xy) * 4;
	vec4 result = vec4(3.402823e38);
#ifdef MODE_SAMPLE
	vec2 partition = vec2(0.0);
	if (operation != 0) {
		vec4 splits = texelFetch(split_texture, ivec2(0), 0);
		partition = vec2(sdsm_begin(splits, camera_parameters.x, cascade_index, shadow_parameters.w > 0.5), splits[cascade_index]);
	}
#endif
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			ivec2 pixel = begin_pixel + ivec2(x, y);
			if (any(greaterThanEqual(pixel, size))) {
				continue;
			}
#ifdef MODE_SAMPLE
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
			if (operation == 0) {
				result = min(result, vec4(depths.x, -depths.y, 0.0, 0.0));
			} else if (depths.y >= partition.x && depths.x <= partition.y) {
				for (int corner = 0; corner < 4; corner++) {
					vec3 light_position = (camera_to_light * vec4(positions[corner], 1.0)).xyz;
					result = min(result, vec4(operation == 1 ? light_position : -light_position, 0.0));
				}
			}
#else
			result = min(result, texelFetch(source_texture, pixel, 0));
#endif
		}
	}
	frag_color = result;
#endif
}
