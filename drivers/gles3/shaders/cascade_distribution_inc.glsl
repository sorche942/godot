vec3 cascade_distribution_debug(highp float depth) {
	highp vec4 splits = vec4(0.0);
	bool selected = false;
	for (highp uint i = 0u; i < scene_data_block.data.directional_shadow_count; i++) {
		highp uint index = uint(MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS) - 1u - i;
		highp uvec4 metadata = scene_data_block.data.debug_cascade_metadata[index];
		if (!bool(metadata.x & layer_mask)) {
			continue;
		}
#if defined(USE_LIGHTMAP) && !defined(DISABLE_LIGHTMAP)
		if (metadata.y == 1u) {
			continue; // Static light on a lightmapped receiver.
		}
#endif
		splits = scene_data_block.data.debug_cascade_splits[index];
#ifdef USE_SDSM
		highp int row = int(metadata.z);
		if (row >= 0 && texelFetch(sdsm_camera_texture, ivec2(0, row), 0).w > 0.0) {
			splits = texelFetch(sdsm_camera_texture, ivec2(2, row), 0);
		}
#endif
		selected = true;
		break;
	}
	highp float shadow_distance = max(max(splits.x, splits.y), max(splits.z, splits.w));
	if (!selected || depth < 0.0 || depth >= shadow_distance) {
		return vec3(0.25);
	}
	vec3 color = depth < splits.x ? vec3(1.0, 0.0, 0.0) : (depth < splits.y ? vec3(0.0, 1.0, 0.0) : (depth < splits.z ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 1.0, 0.0)));
	highp float pixel_depth = max(length(vec2(dFdx(depth), dFdy(depth))), 0.000001);
	highp float boundary_distance = abs(depth - shadow_distance);
	for (int cascade = 0; cascade < 3; cascade++) {
		if (splits[cascade] > 0.0 && splits[cascade] < shadow_distance) {
			boundary_distance = min(boundary_distance, abs(depth - splits[cascade]));
		}
	}
	return mix(color, vec3(1.0), 1.0 - smoothstep(pixel_depth * 0.5, pixel_depth * 1.5, boundary_distance));
}
