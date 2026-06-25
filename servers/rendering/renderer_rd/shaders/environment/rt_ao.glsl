#[raygen]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require

// Ray-traced ambient occlusion: traces one short cosine-weighted ray per
// (half-res) pixel to capture sub-probe-spacing occlusion that DDGI's
// probes can't resolve. Reuses the DDGI TLAS. Output is a scalar AO factor
// [0,1] that multiplies the DDGI ambient in the apply pass.

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 7) uniform sampler linear_sampler;

layout(set = 0, binding = 10, std140) uniform SceneData {
	mat4x4 inv_projection[2];
	mat4x4 cam_transform;
	vec4 eye_offset[2];
	ivec2 screen_size;
	float pad1;
	float pad2;
}
scene_data;

layout(set = 0, binding = 11) uniform texture2D depth_buffer;
layout(set = 0, binding = 12) uniform texture2D normal_roughness_buffer;

layout(r16f, set = 0, binding = 13) uniform restrict image2D output_ao;

layout(set = 0, binding = 14, std140) uniform Params {
	uint view_index;
	uint half_res;
	uint orthogonal;
	float ray_radius;

	float ray_bias;
	float intensity;
	float frame_rand;
	float blend_factor;
}
params;

layout(set = 0, binding = 15) uniform texture2D history_ao_texture;

struct AoPayload {
	float hit_t;
};

layout(location = 0) rayPayloadEXT AoPayload payload;

float hash_uint(uint x) {
	x = ((x >> 16u) ^ x) * 0x45d9f3bu;
	x = ((x >> 16u) ^ x) * 0x45d9f3bu;
	x = (x >> 16u) ^ x;
	return float(x) * (1.0 / 4294967296.0);
}

vec3 cosine_weighted_hemisphere(float r1, float r2, vec3 normal) {
	float phi = 2.0 * 3.14159265 * r1;
	float cos_theta = sqrt(1.0 - r2);
	float sin_theta = sqrt(r2);

	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);

	return tangent * (cos(phi) * sin_theta) + bitangent * (sin(phi) * sin_theta) + normal * cos_theta;
}

void main() {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	ivec2 src_pos = (params.half_res != 0) ? (pixel << 1) : pixel;
	if (any(greaterThanEqual(src_pos, scene_data.screen_size))) {
		imageStore(output_ao, pixel, vec4(1.0));
		return;
	}

	// Depth + normal from G-buffer.
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), src_pos, 0).r;
	if (depth == 0.0) {
		imageStore(output_ao, pixel, vec4(1.0));
		return;
	}

	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), src_pos, 0);
	vec3 view_normal = normal_roughness.xyz * 2.0 - 1.0;
	if (dot(view_normal, view_normal) < 0.01) {
		imageStore(output_ao, pixel, vec4(1.0));
		return;
	}
	view_normal = normalize(view_normal);

	// Reconstruct world position.
	vec4 pos;
	pos.xy = (2.0 * vec2(src_pos) / vec2(scene_data.screen_size)) - 1.0;
	pos.z = depth;
	pos.w = 1.0;
	pos = scene_data.inv_projection[params.view_index] * pos;
	vec3 vertex = pos.xyz / pos.w;
	vec3 world_position = (scene_data.cam_transform * vec4(vertex, 1.0)).xyz;
	vec3 world_normal = normalize(mat3(scene_data.cam_transform) * view_normal);

	// Random cosine-weighted hemisphere direction (changes per frame).
	uint seed = uint(src_pos.x) * 1973u + uint(src_pos.y) * 9277u + uint(params.frame_rand * 65535.0);
	float r1 = hash_uint(seed);
	float r2 = hash_uint(seed * 747796405u + 2891336453u);
	vec3 ray_dir = cosine_weighted_hemisphere(r1, r2, world_normal);

	vec3 origin = world_position + world_normal * params.ray_bias;

	payload.hit_t = params.ray_radius;
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, 0, 0, 0,
			origin, 0.001, ray_dir, params.ray_radius, 0);

	float ao = clamp(payload.hit_t / params.ray_radius, 0.0, 1.0);
	ao = mix(1.0, ao, params.intensity);

	// Temporal accumulation: blend with previous frame's result to reduce
	// 1-ray-per-pixel noise. The history texture is this same image from the
	// previous frame (bound as a sampled texture). At blend_factor=0.15, the
	// noise floor drops ~7x within ~15 frames.
	vec2 ao_uv = (vec2(pixel) + 0.5) / vec2(imageSize(output_ao));
	float history = textureLod(sampler2D(history_ao_texture, linear_sampler), ao_uv, 0.0).r;
	ao = mix(history, ao, params.blend_factor);

	imageStore(output_ao, pixel, vec4(ao));
}

#[closest_hit]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require

struct AoPayload {
	float hit_t;
};

layout(location = 0) rayPayloadInEXT AoPayload payload;

void main() {
	payload.hit_t = gl_HitTEXT;
}

#[miss]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require

struct AoPayload {
	float hit_t;
};

layout(set = 0, binding = 14, std140) uniform Params {
	uint view_index;
	uint half_res;
	uint orthogonal;
	float ray_radius;

	float ray_bias;
	float intensity;
	float frame_rand;
	float blend_factor;
}
params;

layout(location = 0) rayPayloadInEXT AoPayload payload;

void main() {
	payload.hit_t = params.ray_radius;
}
