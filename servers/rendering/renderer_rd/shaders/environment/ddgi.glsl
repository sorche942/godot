#[raygen]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require

#include "../oct_inc.glsl"

// The probe atlases are bound as storage images (not sampled textures): they
// are also written as storage images by the blend passes in the same frame,
// and keeping a single usage type avoids layout transitions inside the
// raytracing list. Bilinear filtering is done manually in ddgi_inc.glsl.
// The trace runs before the blend passes write the atlases this frame, so it
// reads last frame's data as sampled textures (hardware bilinear is ~4x fewer
// fetches than manual filtering of storage images).
layout(set = 0, binding = 5) uniform texture2DArray ddgi_irradiance_texture;
layout(set = 0, binding = 6) uniform texture2DArray ddgi_distance_texture;
layout(set = 0, binding = 7) uniform texture2DArray ddgi_probe_data_texture;

layout(set = 0, binding = 8) uniform sampler linear_sampler;

#define DDGI_INC_SAMPLING
#include "ddgi_inc.glsl"

#define LIGHT_TYPE_DIRECTIONAL 0
#define LIGHT_TYPE_OMNI 1
#define LIGHT_TYPE_SPOT 2
// Virtual light for a bright emissive surface (emissive next-event estimation).
#define LIGHT_TYPE_EMISSIVE 3

struct DDGIRayPayload {
	float hit_t; // > 0: frontface hit, < 0: backface hit (negated), 1e27: miss.
	uint instance_index;
	uint primitive_index;
	vec2 barycentrics;
};

layout(location = 0) rayPayloadEXT DDGIRayPayload payload;

// Stream of [position.xyz, uv.xy, normal.xyz] floats, 8 words per vertex.
layout(buffer_reference, buffer_reference_align = 4, std430) readonly buffer TrianglePositions {
	float data[];
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std140) uniform DDGIVolume {
	DDGIVolumeData data;
}
ddgi;

struct InstanceData {
	vec4 xform[3]; // World transform rows.
	uvec2 vertex_buffer_address;
	uint albedo_tex_index; // Index into the material texture table, 0xFFFFFFFF if none.
	uint clearcoat_tex_index; // Index into the material texture table, 0xFFFFFFFF if none.
	vec4 albedo;
	vec4 emission;
	vec4 uv_scale_offset; // Material uv1 scale.xy + offset.xy.
	// x = metallic, y = roughness, z/w = packed texture slots for the metallic
	// and roughness maps (index * 4 + channel, or -1).
	vec4 metallic_roughness;
	// x = clearcoat, y = clearcoat roughness, z/w = unused.
	vec4 clearcoat;
};

layout(set = 0, binding = 2, std430) restrict readonly buffer Instances {
	InstanceData data[];
}
instances;

struct DDGILight {
	vec3 color;
	float energy;

	vec3 direction;
	uint has_shadow;

	vec3 position;
	float attenuation;

	uint type;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float radius;
};

layout(set = 0, binding = 3, std430) restrict readonly buffer Lights {
	DDGILight data[];
}
lights;

layout(rgba32f, set = 0, binding = 4) uniform restrict writeonly image2DArray ray_data;

// Godot's sky radiance is an octahedral-mapped texture; either a plain 2D
// texture or a 2D array depending on the reflection settings. Both are bound
// and the volume's sky_mode selects which one to sample (with sky_color
// holding the octmap border size in that case).
layout(set = 0, binding = 9) uniform texture2D sky_texture;
layout(set = 0, binding = 10) uniform texture2DArray sky_texture_array;

#define MAX_ALBEDO_TEXTURES 32
layout(set = 0, binding = 11) uniform texture2D albedo_textures[MAX_ALBEDO_TEXTURES];
layout(set = 0, binding = 12) uniform sampler material_sampler;

float get_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd; // nd^4
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2
	return nd * pow(max(distance, 0.0001), -decay);
}

float schlick_fresnel(float u) {
	float m = clamp(1.0 - u, 0.0, 1.0);
	float m2 = m * m;
	return m2 * m2 * m;
}

bool trace_shadow_ray(vec3 origin, vec3 direction, float max_distance) {
	payload.hit_t = 0.0;
	traceRayEXT(tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsOpaqueEXT,
			0xFF, 0, 0, 0, origin, 0.0, direction, max_distance, 0);
	// The miss shader sets hit_t to 1e27; any hit terminates without running
	// the closest hit shader, leaving hit_t at 0.
	return payload.hit_t > 0.0;
}

vec3 evaluate_direct_light(vec3 position, vec3 normal, uint shaded_instance) {
	vec3 light_accum = vec3(0.0);

	for (int i = 0; i < ddgi.data.light_count; i++) {
		vec3 direction;
		float attenuation = 1.0;
		float light_distance = 1e27;

		switch (lights.data[i].type) {
			case LIGHT_TYPE_DIRECTIONAL: {
				direction = -lights.data[i].direction;
			} break;
			case LIGHT_TYPE_OMNI: {
				vec3 rel_vec = lights.data[i].position - position;
				light_distance = length(rel_vec);
				direction = rel_vec / light_distance;
				attenuation = get_omni_attenuation(light_distance, 1.0 / lights.data[i].radius, lights.data[i].attenuation);
			} break;
			case LIGHT_TYPE_SPOT: {
				vec3 rel_vec = lights.data[i].position - position;
				light_distance = length(rel_vec);
				direction = rel_vec / light_distance;
				attenuation = get_omni_attenuation(light_distance, 1.0 / lights.data[i].radius, lights.data[i].attenuation);

				float cos_spot_angle = lights.data[i].cos_spot_angle;
				float cos_angle = dot(-direction, lights.data[i].direction);
				if (cos_angle < cos_spot_angle) {
					continue;
				}

				float scos = max(cos_angle, cos_spot_angle);
				float spot_rim = max(0.0001, (1.0 - scos) / (1.0 - cos_spot_angle));
				attenuation *= 1.0 - pow(spot_rim, lights.data[i].inv_spot_attenuation);
			} break;
			case LIGHT_TYPE_EMISSIVE: {
				if (uint(lights.data[i].cos_spot_angle) == shaded_instance) {
					// Don't light the emitter with itself.
					continue;
				}
				vec3 rel_vec = lights.data[i].position - position;
				float center_distance = length(rel_vec);
				direction = rel_vec / center_distance;
				float r = lights.data[i].radius;
				// Irradiance from a sphere with uniform radiance L: E = L*PI*r^2/d^2.
				attenuation = (DDGI_PI * r * r) / max(center_distance * center_distance, r * r);
				// Shadow rays must stop at the emitter's surface, not its center.
				light_distance = max(center_distance - r, 0.0);
			} break;
			default: {
				continue;
			}
		}

		float n_dot_l = dot(normal, direction);
		attenuation *= max(0.0, n_dot_l);

		if (attenuation < 0.001) {
			continue;
		}

		if (lights.data[i].has_shadow != 0) {
			float shadow_bias = 0.05;
			vec3 shadow_origin = position + normal * shadow_bias;
			float shadow_max = min(light_distance - shadow_bias, ddgi.data.probe_max_ray_distance);
			if (shadow_max > 0.0 && !trace_shadow_ray(shadow_origin, direction, shadow_max)) {
				continue;
			}
		}

		light_accum += lights.data[i].color * lights.data[i].energy * attenuation;
	}

	return light_accum;
}

void main() {
	int ray_index = int(gl_LaunchIDEXT.x);
	int probe_plane_index = int(gl_LaunchIDEXT.y);
	int full_plane_index = int(gl_LaunchIDEXT.z);

	// Extract cascade index and cascade-local plane index.
	int cascade_index = full_plane_index / ddgi.data.probe_counts.y;
	int plane_index = full_plane_index % ddgi.data.probe_counts.y;

	int probes_per_plane = ddgi_probes_per_plane(ddgi.data);
	int probe_index = (plane_index * probes_per_plane) + probe_plane_index;

	ivec3 probe_coords = ddgi_probe_coords(probe_index, ddgi.data);

	// The storage (texture) index accounts for the scrolling offsets.
	int storage_index = ddgi_scrolling_probe_index_cascade(probe_coords, cascade_index, ddgi.data);

	ivec3 probe_data_coords = ddgi_probe_texel_coords_cascade(storage_index, cascade_index, ddgi.data);
	vec4 probe_data = ddgi_fetch_probe_data(probe_data_coords);

	bool use_relocation = (ddgi.data.flags & DDGI_FLAG_PROBE_RELOCATION) != 0;
	bool use_classification = (ddgi.data.flags & DDGI_FLAG_PROBE_CLASSIFICATION) != 0;
	bool fixed_rays = use_relocation || use_classification;

	// Scroll-cleared probes may inherit stale probe_data (INACTIVE state,
	// relocation offset) from the previous occupant of their toroidal slot.
	bool scroll_cleared = ddgi_probe_scroll_cleared_cascade(probe_data_coords, cascade_index, ddgi.data);

	// Inactive probes only trace the fixed rays used by classification.
	// Scroll-cleared probes force a full trace regardless of stale state.
	if (!scroll_cleared && use_classification && probe_data.w == DDGI_PROBE_STATE_INACTIVE && ray_index >= DDGI_NUM_FIXED_RAYS) {
		return;
	}

	vec3 probe_world_position = ddgi_probe_world_position_base_cascade(probe_coords, cascade_index, ddgi.data);
	if (use_relocation && !scroll_cleared) {
		probe_world_position += probe_data.xyz * ddgi_cascade_spacing(cascade_index, ddgi.data);
	}

	vec3 probe_ray_direction = ddgi_probe_ray_direction(ray_index, ddgi.data);

	ivec3 output_coords = ddgi_ray_data_texel_coords_cascade(ray_index, storage_index, cascade_index, ddgi.data);

	payload.hit_t = 1e27;
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, probe_world_position, 0.0, probe_ray_direction, ddgi.data.probe_max_ray_distance, 0);

	if (payload.hit_t >= 1e27) {
		// Miss: store the sky radiance.
		vec3 sky_radiance = ddgi.data.sky_color;
		if (ddgi.data.sky_mode != DDGI_SKY_MODE_COLOR) {
			// sky_color.xy holds the octmap border size in sky modes.
			vec2 sky_uv = vec3_to_oct_with_border(probe_ray_direction, ddgi.data.sky_color.xy);
			if (ddgi.data.sky_mode == DDGI_SKY_MODE_SKY_ARRAY) {
				// Roughness levels are stored in the array layers; use a slightly rough one.
				sky_radiance = textureLod(sampler2DArray(sky_texture_array, linear_sampler), vec3(sky_uv, 2.0), 0.0).rgb;
			} else {
				sky_radiance = textureLod(sampler2D(sky_texture, linear_sampler), sky_uv, 2.0).rgb;
			}
		}
		sky_radiance *= ddgi.data.sky_energy;
		imageStore(ray_data, output_coords, vec4(sky_radiance, 1e27));
		return;
	}

	if (payload.hit_t < 0.0) {
		// Backface hit: negative distance marks it for blending, relocation and
		// classification. Shorten by 80% to reduce the probe's influence.
		imageStore(ray_data, output_coords, vec4(0.0, 0.0, 0.0, payload.hit_t * 0.2));
		return;
	}

	float hit_t = payload.hit_t;

	// Fixed rays only contribute distance data; lighting them would bias irradiance.
	if (fixed_rays && ray_index < DDGI_NUM_FIXED_RAYS) {
		imageStore(ray_data, output_coords, vec4(0.0, 0.0, 0.0, hit_t));
		return;
	}

	InstanceData instance = instances.data[payload.instance_index];

	// Fetch the triangle and compute the geometric normal in world space.
	TrianglePositions tri = TrianglePositions(instance.vertex_buffer_address);
	uint base = payload.primitive_index * 24;
	vec3 v0 = vec3(tri.data[base + 0], tri.data[base + 1], tri.data[base + 2]);
	vec3 v1 = vec3(tri.data[base + 8], tri.data[base + 9], tri.data[base + 10]);
	vec3 v2 = vec3(tri.data[base + 16], tri.data[base + 17], tri.data[base + 18]);

	mat4 to_world = mat4(
			vec4(instance.xform[0].x, instance.xform[1].x, instance.xform[2].x, 0.0),
			vec4(instance.xform[0].y, instance.xform[1].y, instance.xform[2].y, 0.0),
			vec4(instance.xform[0].z, instance.xform[1].z, instance.xform[2].z, 0.0),
			vec4(instance.xform[0].w, instance.xform[1].w, instance.xform[2].w, 1.0));

	vec3 w0 = (to_world * vec4(v0, 1.0)).xyz;
	vec3 w1 = (to_world * vec4(v1, 1.0)).xyz;
	vec3 w2 = (to_world * vec4(v2, 1.0)).xyz;

	vec3 geometric_normal = normalize(cross(w1 - w0, w2 - w0));
	// Make sure the normal faces against the ray (handles mirrored transforms).
	if (dot(geometric_normal, probe_ray_direction) > 0.0) {
		geometric_normal = -geometric_normal;
	}

	// Interpolated smooth vertex normal for shading.
	vec3 bary = vec3(1.0 - payload.barycentrics.x - payload.barycentrics.y, payload.barycentrics.x, payload.barycentrics.y);
	vec3 n0 = vec3(tri.data[base + 5], tri.data[base + 6], tri.data[base + 7]);
	vec3 n1 = vec3(tri.data[base + 13], tri.data[base + 14], tri.data[base + 15]);
	vec3 n2 = vec3(tri.data[base + 21], tri.data[base + 22], tri.data[base + 23]);
	vec3 normal = normalize(mat3(to_world) * (n0 * bary.x + n1 * bary.y + n2 * bary.z));
	if (dot(normal, geometric_normal) < 0.0) {
		normal = -normal;
	}

	vec3 hit_position = probe_world_position + probe_ray_direction * hit_t;

	// Direct lighting with raytraced shadows.
	vec3 direct = evaluate_direct_light(hit_position, normal, payload.instance_index);

	// Indirect lighting: recursively sample the probes (previous frame's data),
	// giving DDGI its infinite bounces.
	vec3 irradiance = vec3(0.0);
	float volume_weight = ddgi_volume_blend_weight(hit_position, ddgi.data);
	if (volume_weight > 0.0) {
		vec3 surface_bias = ddgi_surface_bias(normal, probe_ray_direction, ddgi.data);
		irradiance = ddgi_sample_irradiance_single(hit_position, surface_bias, normal, ddgi.data);
		irradiance *= volume_weight;
	}

	vec2 uv = vec2(0.0);
	bool has_uv = false;
	if (instance.albedo_tex_index != 0xFFFFFFFF || instance.clearcoat_tex_index != 0xFFFFFFFF || instance.metallic_roughness.z >= 0.0 || instance.metallic_roughness.w >= 0.0) {
		vec2 uv0 = vec2(tri.data[base + 3], tri.data[base + 4]);
		vec2 uv1 = vec2(tri.data[base + 11], tri.data[base + 12]);
		vec2 uv2 = vec2(tri.data[base + 19], tri.data[base + 20]);
		uv = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
		uv = uv * instance.uv_scale_offset.xy + instance.uv_scale_offset.zw;
		has_uv = true;
	}

	// Probe rays are heavily blurred by the probe integration; a high mip is plenty.
	const float tex_lod = 4.0;
	vec3 albedo = instance.albedo.rgb;
	if (instance.albedo_tex_index != 0xFFFFFFFF) {
		albedo *= textureLod(sampler2D(albedo_textures[nonuniformEXT(instance.albedo_tex_index)], material_sampler), uv, tex_lod).rgb;
	}
	// Perfectly diffuse reflectors don't exist; clamp albedo to limit energy gain.
	albedo = min(albedo, vec3(0.9));

	float metallic = instance.metallic_roughness.x;
	float surface_roughness = instance.metallic_roughness.y;
	if (has_uv && instance.metallic_roughness.z >= 0.0) {
		int packed = int(instance.metallic_roughness.z);
		metallic *= textureLod(sampler2D(albedo_textures[nonuniformEXT(packed >> 2)], material_sampler), uv, tex_lod)[packed & 3];
	}
	if (has_uv && instance.metallic_roughness.w >= 0.0) {
		int packed = int(instance.metallic_roughness.w);
		surface_roughness *= textureLod(sampler2D(albedo_textures[nonuniformEXT(packed >> 2)], material_sampler), uv, tex_lod)[packed & 3];
	}

	float clearcoat_strength = clamp(instance.clearcoat.x, 0.0, 1.0);
	if (has_uv && instance.clearcoat_tex_index != 0xFFFFFFFF) {
		clearcoat_strength = clamp(clearcoat_strength * textureLod(sampler2D(albedo_textures[nonuniformEXT(instance.clearcoat_tex_index)], material_sampler), uv, tex_lod).r, 0.0, 1.0);
	}

	// For probe bounce purposes a metal behaves like a tinted diffuse
	// reflector of equivalent energy: kd + f0 = albedo*(1-m) + mix(0.04,
	// albedo, m) ~= albedo. Directional specular detail is invisible after the
	// probe integration, and sampling it per ray would re-introduce estimate
	// noise; the directional metallic workflow lives in the reflections pass.
	float dielectric_boost = 0.04 * (1.0 - metallic);
	vec3 bounce_color = min(albedo + vec3(dielectric_boost), vec3(0.9));

	if (clearcoat_strength > 0.0) {
		float coat_n_dot_v = max(dot(normal, -probe_ray_direction), 0.0001);
		float coat_fresnel = mix(0.04, 1.0, schlick_fresnel(coat_n_dot_v)) * clearcoat_strength;
		bounce_color *= 1.0 - coat_fresnel;
	}

	// emission.a is zero for emitters handled by the virtual lights above.
	vec3 radiance = instance.emission.rgb * instance.emission.a + (bounce_color / DDGI_PI) * (direct + irradiance);

	imageStore(ray_data, output_coords, vec4(radiance, hit_t));
}

#[closest_hit]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require

struct DDGIRayPayload {
	float hit_t;
	uint instance_index;
	uint primitive_index;
	vec2 barycentrics;
};

layout(location = 0) rayPayloadInEXT DDGIRayPayload payload;

hitAttributeEXT vec2 hit_attribs;

void main() {
	payload.hit_t = (gl_HitKindEXT == gl_HitKindBackFacingTriangleEXT) ? -gl_HitTEXT : gl_HitTEXT;
	payload.instance_index = uint(gl_InstanceID);
	payload.primitive_index = uint(gl_PrimitiveID);
	payload.barycentrics = hit_attribs;
}

#[miss]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require

struct DDGIRayPayload {
	float hit_t;
	uint instance_index;
	uint primitive_index;
	vec2 barycentrics;
};

layout(location = 0) rayPayloadInEXT DDGIRayPayload payload;

void main() {
	payload.hit_t = 1e27;
}
