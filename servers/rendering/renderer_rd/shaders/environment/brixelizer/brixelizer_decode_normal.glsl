#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D normal_roughness_tex;
layout(set = 0, binding = 1, rgba8) uniform image2D world_normal_out;

layout(push_constant, std430) uniform Params {
	mat4 inv_view;
}
params;

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(world_normal_out);
	if (any(greaterThanEqual(coord, size))) {
		return;
	}

	vec3 normal = texelFetch(normal_roughness_tex, coord, 0).xyz * 2.0 - 1.0;
	normal = normalize(normal);
	vec3 world_normal = normalize((params.inv_view * vec4(normal, 0.0)).xyz);
	vec3 enc = world_normal * 0.5 + 0.5;
	imageStore(world_normal_out, coord, vec4(enc, 1.0));
}
