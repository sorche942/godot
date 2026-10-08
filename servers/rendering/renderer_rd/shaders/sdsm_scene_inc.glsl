#include "sdsm_data_inc.glsl"
#include "sdsm_light_inc.glsl"

layout(set = 1, binding = 39, std430) restrict readonly buffer SdsmVisibility {
	uint words[];
} sdsm_visibility;

layout(set = 1, binding = 40, std140) uniform SdsmShadowResult {
	SdsmResult data;
} sdsm_shadow_result;

layout(set = 1, binding = 41, std140) uniform SdsmDirectionalResults {
	SdsmResult data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
} sdsm_directional_results;

uint sdsm_instance_index(uint ordinary_index, uint local_index) {
	if (draw_call.sdsm_packet == 0xffffffffu) {
		return ordinary_index;
	}
	uint slot = draw_call.sdsm_cascade * sdsm_visibility.words[1] + draw_call.sdsm_packet;
	return sdsm_visibility.words[4u + sdsm_visibility.words[0] + sdsm_visibility.words[4u + slot] + local_index];
}

DirectionalLightData sdsm_directional_light(uint index) {
	DirectionalLightData light = directional_lights.data[index];
	if (bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_USE_SDSM_SHADOWS)) {
		light = sdsm_apply_directional_light(light, sdsm_directional_results.data[index]);
	}
	return light;
}

SceneData sdsm_scene_data(SceneData scene) {
	if (draw_call.sdsm_cascade == 0xffffffffu) {
		return scene;
	}
	uint cascade = draw_call.sdsm_cascade;
	SdsmResult fitted = sdsm_shadow_result.data;
	mat4 flip = mat4(1.0);
	flip[1][1] = bool(scene.flags & SCENE_DATA_FLAGS_SDSM_SHADOW_FLIP_Y) ? -1.0 : 1.0;
	scene.projection_matrix = flip * fitted.projection[cascade];
	scene.inv_projection_matrix = fitted.inv_projection[cascade] * flip;
	scene.view_matrix = mat3x4(transpose(fitted.view[cascade]));
	scene.inv_view_matrix = mat3x4(transpose(fitted.inv_view[cascade]));
#ifdef USE_DOUBLE_PRECISION
	scene.inv_view_precision = fitted.inv_view_precision[cascade];
#endif
	for (uint view = 0u; view < MAX_VIEWS; view++) {
		scene.projection_matrix_view[view] = scene.projection_matrix;
		scene.inv_projection_matrix_view[view] = scene.inv_projection_matrix;
		scene.eye_offset[view] = vec4(0.0);
	}
	scene.z_near = 0.0;
	scene.z_far = fitted.bounds[cascade].maximum.z - fitted.bounds[cascade].minimum.z;
	// Keep main_cam_inv_view_matrix unchanged: billboards face the main camera.
	return scene;
}
