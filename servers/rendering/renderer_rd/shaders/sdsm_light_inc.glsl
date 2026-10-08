DirectionalLightData sdsm_apply_directional_light(DirectionalLightData light, SdsmResult fitted) {
	if (fitted.meta.x == 0u) {
		return light;
	}
	light.shadow_matrix1 = fitted.shadow_matrix[0];
	light.shadow_matrix2 = fitted.shadow_matrix[1];
	light.shadow_matrix3 = fitted.shadow_matrix[2];
	light.shadow_matrix4 = fitted.shadow_matrix[3];
	light.shadow_split_offsets = fitted.splits;
	light.shadow_bias = vec4(fitted.shadow_params[0].x, fitted.shadow_params[1].x, fitted.shadow_params[2].x, fitted.shadow_params[3].x);
	light.shadow_normal_bias = vec4(fitted.shadow_params[0].y, fitted.shadow_params[1].y, fitted.shadow_params[2].y, fitted.shadow_params[3].y);
	light.shadow_transmittance_bias = vec4(fitted.shadow_params[0].z, fitted.shadow_params[1].z, fitted.shadow_params[2].z, fitted.shadow_params[3].z);
	light.shadow_z_range = fitted.shadow_z_range;
	light.shadow_range_begin = fitted.shadow_range_begin;
	light.uv_scale1 = fitted.shadow_uv_scale[0].xy;
	light.uv_scale2 = fitted.shadow_uv_scale[0].zw;
	light.uv_scale3 = fitted.shadow_uv_scale[1].xy;
	light.uv_scale4 = fitted.shadow_uv_scale[1].zw;
	light.fade_from = fitted.fade.x;
	light.fade_to = fitted.fade.y;
	return light;
}
