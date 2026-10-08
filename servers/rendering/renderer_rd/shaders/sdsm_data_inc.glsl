// Dedicated GPU-produced shadow data. All members are 16-byte aggregates,
// so this layout is identical in std140 and std430 (1664 bytes per result).
#ifndef SDSM_DATA_INCLUDED
#define SDSM_DATA_INCLUDED

struct SdsmBounds {
	vec4 minimum;
	vec4 maximum;
};

struct SdsmResult {
	vec4 splits;
	SdsmBounds bounds[4];
	vec4 texel;
	mat4 projection[4];
	mat4 inv_projection[4];
	mat4 inv_view[4];
	mat4 view[4];
	vec4 inv_view_precision[4];
	mat4 shadow_matrix[4];
	vec4 shadow_params[4];
	vec4 shadow_z_range;
	vec4 shadow_range_begin;
	vec4 shadow_uv_scale[2];
	vec4 fade;
	uvec4 meta;
};

#endif
