// Scene matrix builtins must see the fitted camera before custom shader code.
SceneData sdsm_scene_data;
#ifdef USE_SDSM
uniform highp sampler2D sdsm_camera_texture; // texunit:-1
uniform int sdsm_shadow_row;
uniform int sdsm_light_row;
uniform mat4 sdsm_atlas_rects;
uniform vec3 sdsm_camera_origin;
uniform vec3 sdsm_model_origin;
mat4 sdsm_camera_matrix(int column, int row) {
	return mat4(texelFetch(sdsm_camera_texture, ivec2(column, row), 0),
			texelFetch(sdsm_camera_texture, ivec2(column + 1, row), 0),
			texelFetch(sdsm_camera_texture, ivec2(column + 2, row), 0),
			texelFetch(sdsm_camera_texture, ivec2(column + 3, row), 0));
}
void sdsm_set_camera(inout SceneData data) {
	if (sdsm_shadow_row >= 0 && texelFetch(sdsm_camera_texture, ivec2(0, sdsm_shadow_row), 0).w > 0.0) {
		data.projection_matrix = sdsm_camera_matrix(4, sdsm_shadow_row);
		data.inv_projection_matrix = inverse(data.projection_matrix);
		data.view_matrix = sdsm_camera_matrix(8, sdsm_shadow_row);
		data.inv_view_matrix = sdsm_camera_matrix(12, sdsm_shadow_row);
		// Stored matrices are main-camera-relative; builtins still describe world space.
		data.view_matrix[3].xyz -= mat3(data.view_matrix) * sdsm_camera_origin;
		data.inv_view_matrix[3].xyz += sdsm_camera_origin;
		data.shadow_bias = texelFetch(sdsm_camera_texture, ivec2(3, sdsm_shadow_row), 0).y;
	}
}
#endif
