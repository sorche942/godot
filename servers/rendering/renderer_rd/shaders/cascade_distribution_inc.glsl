// Fragment-only visualization of the same directional data used by shadow sampling.
vec3 cascade_distribution_color(float depth, vec4 splits) {
	float shadow_distance = max(max(splits.x, splits.y), max(splits.z, splits.w));
	if (depth < 0.0 || depth >= shadow_distance) {
		return vec3(0.25);
	}
	vec3 color = depth < splits.x ? vec3(1.0, 0.0, 0.0) : (depth < splits.y ? vec3(0.0, 1.0, 0.0) : (depth < splits.z ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 1.0, 0.0)));
	// One pixel on either side of each distinct cascade end, including shadow distance.
	float pixel_depth = max(length(vec2(dFdx(depth), dFdy(depth))), 0.000001);
	float boundary_distance = abs(depth - shadow_distance);
	for (int cascade = 0; cascade < 3; cascade++) {
		if (splits[cascade] > 0.0 && splits[cascade] < shadow_distance) {
			boundary_distance = min(boundary_distance, abs(depth - splits[cascade]));
		}
	}
	return mix(color, vec3(1.0), 1.0 - smoothstep(pixel_depth * 0.5, pixel_depth * 1.5, boundary_distance));
}

vec3 cascade_distribution_debug(float depth, uint layer_mask, bool uses_lightmap) {
	for (uint i = 0u; i < scene_data_block.data.directional_light_count; i++) {
		DirectionalLightData light = sdsm_directional_light(i);
		if (light.shadow_opacity <= 0.001 || !bool(light.mask & layer_mask) || (uses_lightmap && light.bake_mode == LIGHT_BAKE_STATIC)) {
			continue;
		}
		return cascade_distribution_color(depth, light.shadow_split_offsets);
	}
	return vec3(0.25);
}
