#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	float exposure_scale;
	float pad[3];
} params;

layout(set = 0, binding = 0) uniform sampler2D source_luminance;
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_exposure;

void main() {
	float avg_luminance = texelFetch(source_luminance, ivec2(0, 0), 0).r;
	float exposure = params.exposure_scale / max(avg_luminance, 1e-6);
	imageStore(dest_exposure, ivec2(0, 0), vec4(exposure, 0.0, 0.0, 0.0));
}
