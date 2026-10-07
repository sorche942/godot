// Shared by the RD compute fitter and the GLES fragment fitter.
// All bounds are in light space relative to the main camera's world origin.

vec4 sdsm_splits(vec2 depth_range, vec2 extra_range, float camera_near, float shadow_far, int cascade_count) {
	float sample_min = shadow_far;
	float sample_max = camera_near;
	if (depth_range.x <= depth_range.y && depth_range.x <= shadow_far && depth_range.y >= camera_near) {
		sample_min = max(depth_range.x, camera_near);
		sample_max = min(depth_range.y, shadow_far);
	}
	if (extra_range.x <= extra_range.y && extra_range.x <= shadow_far && extra_range.y >= camera_near) {
		sample_min = min(sample_min, max(extra_range.x, camera_near));
		sample_max = max(sample_max, min(extra_range.y, shadow_far));
	}
	if (sample_min > sample_max) {
		// An empty distribution keeps the original CSM cameras and metadata.
		return vec4(0.0);
	}
	float minimum_range = max(0.001, abs(sample_min) * 0.0001);
	if (sample_max - sample_min < minimum_range) {
		sample_min = max(camera_near, sample_min - minimum_range * 0.5);
		sample_max = min(shadow_far, sample_max + minimum_range * 0.5);
	}
	vec4 splits = vec4(shadow_far);
	for (int i = 1; i < cascade_count; i++) {
		float fraction = float(i) / float(cascade_count);
		splits[i - 1] = sample_min > 0.0 ? sample_min * pow(sample_max / sample_min, fraction) : mix(sample_min, sample_max, fraction);
	}
	return splits;
}

float sdsm_begin(vec4 splits, float camera_near, int cascade, bool blend) {
	int previous = cascade - (blend ? 2 : 1);
	return previous >= 0 ? splits[previous] : camera_near;
}

float sdsm_expand_xy(inout vec3 lower, inout vec3 upper, float caster_far, float resolution, float blur, float normal_bias, float angular_size, float pancake) {
	vec2 size = upper.xy - lower.xy;
	float initial_texel = max(max(size.x, size.y), 0.001) / resolution;
	float filter_border = initial_texel * (2.0 + 8.0 * blur + normal_bias);
	float soft_border = angular_size * blur * max(caster_far - lower.z, pancake);
	vec2 border = max(size * 0.01, vec2(0.001)) + vec2(filter_border + soft_border);
	lower.xy -= border;
	upper.xy += border;
	float texel = max(upper.x - lower.x, upper.y - lower.y) / resolution;
	lower.xy = floor(lower.xy / texel) * texel;
	upper.xy = ceil(upper.xy / texel) * texel;
	return texel;
}

void sdsm_expand_z(inout vec3 lower, inout vec3 upper, float texel, float normal_bias) {
	float border = max((upper.z - lower.z) * 0.01, 0.001) + normal_bias * texel;
	lower.z -= border;
	upper.z += border;
}

mat4 sdsm_projection(vec3 lower, vec3 upper) {
	vec3 extent = upper - lower;
	return mat4(
			vec4(2.0 / extent.x, 0.0, 0.0, 0.0),
			vec4(0.0, 2.0 / extent.y, 0.0, 0.0),
			vec4(0.0, 0.0, -2.0 / extent.z, 0.0),
			vec4(0.0, 0.0, -1.0, 1.0));
}
