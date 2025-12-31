#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D normal_roughness_tex;
layout(set = 0, binding = 1, r8) uniform image2D roughness_out;

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(roughness_out);
	if (any(greaterThanEqual(coord, size))) {
		return;
	}

	float enc = texelFetch(normal_roughness_tex, coord, 0).a;
	float roughness = (enc > 0.5) ? (1.0 - enc) : enc;
	roughness /= (127.0 / 255.0);
	roughness = clamp(roughness, 0.0, 1.0);

	imageStore(roughness_out, coord, vec4(roughness, 0.0, 0.0, 1.0));
}
