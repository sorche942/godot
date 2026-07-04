#[raygen]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require

#include "../oct_inc.glsl"

// Raytraced reflections: one mirror ray per (reflection buffer) pixel,
// reconstructed from the depth and normal-roughness buffers. Hits are shaded
// with direct light (raytraced shadows) plus irradiance from the DDGI probes,
// so reflections include global illumination. The result replaces the probe
// based glossy fallback for smooth surfaces and fades back into it as
// roughness approaches the cutoff.

// The DDGI update has fully finished by the time reflections run; sampled
// textures give hardware bilinear at ~4x fewer fetches.
layout(set = 0, binding = 4) uniform texture2DArray ddgi_irradiance_texture;
layout(set = 0, binding = 5) uniform texture2DArray ddgi_distance_texture;
layout(set = 0, binding = 6) uniform texture2DArray ddgi_probe_data_texture;

layout(set = 0, binding = 7) uniform sampler linear_sampler;

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

layout(set = 0, binding = 8) uniform texture2D sky_texture;
layout(set = 0, binding = 9) uniform texture2DArray sky_texture_array;

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

layout(rgba16f, set = 0, binding = 13) uniform restrict image2D reflection_buffer;

#define MAX_ALBEDO_TEXTURES 32
layout(set = 0, binding = 15) uniform texture2D albedo_textures[MAX_ALBEDO_TEXTURES];
layout(set = 0, binding = 16) uniform sampler material_sampler;

// Note: parameters live in a UBO; push constants in raytracing lists do not
// reach the shader correctly through the current RenderingDevice.
layout(set = 0, binding = 14, std140) uniform Params {
	uint view_index;
	uint half_res;
	uint orthogonal;
	float max_roughness;

	float ray_bias;
	float pad0;
	float pad1;
	float pad2;
}
params;

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

vec3 f0_clear_coat_to_surface(vec3 f0) {
	// Approximation of iorTof0(f0ToIor(f0), 1.5), matching scene_forward_lights_inc.glsl.
	return clamp(f0 * (f0 * (0.941892 - 0.263008 * f0) + 0.346479) - 0.0285998, vec3(0.0), vec3(1.0));
}

bool trace_shadow_ray(vec3 origin, vec3 direction, float max_distance) {
	payload.hit_t = 0.0;
	traceRayEXT(tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsOpaqueEXT,
			0xFF, 0, 0, 0, origin, 0.0, direction, max_distance, 0);
	return payload.hit_t > 0.0;
}

vec3 evaluate_direct_light(vec3 position, vec3 normal, uint shaded_instance, vec3 view_direction, float spec_roughness, float clearcoat, float clearcoat_roughness, out vec3 r_specular, out vec3 r_clearcoat_specular) {
	vec3 light_accum = vec3(0.0);
	r_specular = vec3(0.0);
	r_clearcoat_specular = vec3(0.0);
	// Blinn-Phong-style lobes; enough for recognizable highlights in reflections.
	float shininess = clamp(2.0 / max(spec_roughness * spec_roughness, 0.005) - 2.0, 4.0, 1024.0);
	float clearcoat_lobe_roughness = mix(0.001, 0.1, clearcoat_roughness);
	float clearcoat_shininess = clamp(2.0 / max(clearcoat_lobe_roughness * clearcoat_lobe_roughness, 0.005) - 2.0, 4.0, 1024.0);

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
					continue;
				}
				vec3 rel_vec = lights.data[i].position - position;
				float center_distance = length(rel_vec);
				direction = rel_vec / center_distance;
				float r = lights.data[i].radius;
				attenuation = (DDGI_PI * r * r) / max(center_distance * center_distance, r * r);
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

		vec3 light_value = lights.data[i].color * lights.data[i].energy * attenuation;
		light_accum += light_value;

		vec3 half_vector = normalize(direction - view_direction);
		float n_dot_h = max(0.0, dot(normal, half_vector));
		r_specular += light_value * pow(n_dot_h, shininess) * ((shininess + 8.0) / (8.0 * DDGI_PI));
		if (clearcoat > 0.0) {
			float l_dot_h = clamp(dot(direction, half_vector), 0.0, 1.0);
			float clearcoat_fresnel = mix(0.04, 1.0, schlick_fresnel(l_dot_h)) * clearcoat;
			r_clearcoat_specular += light_value * pow(n_dot_h, clearcoat_shininess) * ((clearcoat_shininess + 8.0) / (8.0 * DDGI_PI)) * clearcoat_fresnel;
		}
	}

	return light_accum;
}

vec3 sample_sky(vec3 direction, float lod) {
	vec3 sky_radiance = ddgi.data.sky_color;
	if (ddgi.data.sky_mode != DDGI_SKY_MODE_COLOR) {
		// sky_color.xy holds the octmap border size in sky modes.
		vec2 sky_uv = vec3_to_oct_with_border(direction, ddgi.data.sky_color.xy);
		if (ddgi.data.sky_mode == DDGI_SKY_MODE_SKY_ARRAY) {
			sky_radiance = textureLod(sampler2DArray(sky_texture_array, linear_sampler), vec3(sky_uv, lod), 0.0).rgb;
		} else {
			sky_radiance = textureLod(sampler2D(sky_texture, linear_sampler), sky_uv, lod).rgb;
		}
	}
	return sky_radiance * ddgi.data.sky_energy;
}

void main() {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	ivec2 buffer_size = imageSize(reflection_buffer);
	if (any(greaterThanEqual(pixel, buffer_size))) {
		return;
	}


	ivec2 src_pos = (params.half_res != 0) ? (pixel << 1) : pixel;
	if (any(greaterThanEqual(src_pos, scene_data.screen_size))) {
		return;
	}

	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), src_pos, 0);
	vec3 normal = normal_roughness.xyz * 2.0 - 1.0;
	if (length(normal) < 0.1) {
		// No geometry here. Note: valid normals can be well below unit length,
		// since the buffer stores Kaplanyan best-fit normals (the length is
		// the lattice scale, only the direction matters).
		return;
	}
	normal = normalize(normal);

	float roughness = normal_roughness.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);

	if (roughness > params.max_roughness) {
		// Leave the probe based glossy fallback in place.
		return;
	}

	// Reconstruct the view-space position from the depth buffer.
	vec4 pos;
	pos.xy = (2.0 * vec2(src_pos) / vec2(scene_data.screen_size)) - 1.0;
	pos.z = texelFetch(sampler2D(depth_buffer, linear_sampler), src_pos, 0).r;
	pos.w = 1.0;
	pos = scene_data.inv_projection[params.view_index] * pos;
	vec3 vertex = pos.xyz / pos.w;

	vec3 world_position = (scene_data.cam_transform * vec4(vertex, 1.0)).xyz;
	vec3 world_normal = normalize(mat3(scene_data.cam_transform) * normal);

	vec3 view_dir;
	if (params.orthogonal != 0) {
		view_dir = -normalize(mat3(scene_data.cam_transform)[2]);
	} else {
		vec3 cam_origin = (scene_data.cam_transform * vec4(scene_data.eye_offset[params.view_index].xyz, 1.0)).xyz;
		view_dir = normalize(world_position - cam_origin);
	}

	// The G-buffer stores normals in 8 bits per axis; reflect() doubles the
	// quantization error and mirror reflections on curved surfaces show it as
	// banding. Refine the normal with a short primary re-trace: the RT vertex
	// stream holds full precision smooth normals. The refined normal is only
	// adopted when it agrees with the G-buffer one within quantization
	// tolerance, so normal mapped detail is preserved.
	{
		vec3 retrace_origin = world_position - view_dir * 0.5;
		payload.hit_t = 1e27;
		traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, retrace_origin, 0.3, view_dir, 0.7, 0);
		if (payload.hit_t > 0.0 && payload.hit_t < 1e27) {
			InstanceData re_instance = instances.data[payload.instance_index];
			TrianglePositions re_tri = TrianglePositions(re_instance.vertex_buffer_address);
			uint re_base = payload.primitive_index * 24;
			vec3 re_bary = vec3(1.0 - payload.barycentrics.x - payload.barycentrics.y, payload.barycentrics.x, payload.barycentrics.y);
			vec3 rn0 = vec3(re_tri.data[re_base + 5], re_tri.data[re_base + 6], re_tri.data[re_base + 7]);
			vec3 rn1 = vec3(re_tri.data[re_base + 13], re_tri.data[re_base + 14], re_tri.data[re_base + 15]);
			vec3 rn2 = vec3(re_tri.data[re_base + 21], re_tri.data[re_base + 22], re_tri.data[re_base + 23]);
			mat3 re_rot = mat3(
					vec3(re_instance.xform[0].x, re_instance.xform[1].x, re_instance.xform[2].x),
					vec3(re_instance.xform[0].y, re_instance.xform[1].y, re_instance.xform[2].y),
					vec3(re_instance.xform[0].z, re_instance.xform[1].z, re_instance.xform[2].z));
			vec3 precise_normal = normalize(re_rot * (rn0 * re_bary.x + rn1 * re_bary.y + rn2 * re_bary.z));
			if (dot(precise_normal, view_dir) > 0.0) {
				precise_normal = -precise_normal;
			}
			if (dot(precise_normal, world_normal) > 0.999) {
				world_normal = precise_normal;
			}
			// The exact hit point is far more precise than the position
			// reconstructed from the depth buffer; reflection rays starting
			// from the reconstructed point can re-enter curved surfaces
			// (visible as dark speckles).
			world_position = retrace_origin + view_dir * payload.hit_t;
		}
	}

	vec3 reflect_dir = normalize(reflect(view_dir, world_normal));

	vec3 origin = world_position + world_normal * params.ray_bias;

	payload.hit_t = 1e27;
	// The t-min skips residual self-intersections with the originating surface.
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, origin, params.ray_bias, reflect_dir, ddgi.data.probe_max_ray_distance, 0);

	vec3 radiance;
	if (payload.hit_t >= 1e27) {
		// Sharp sky for mirror reflections.
		radiance = sample_sky(reflect_dir, 0.0);
	} else if (payload.hit_t < 0.0) {
		// Backface: inside geometry, return no reflection.
		radiance = vec3(0.0);
	} else {
		float hit_t = payload.hit_t;
		InstanceData instance = instances.data[payload.instance_index];

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
		if (dot(geometric_normal, reflect_dir) > 0.0) {
			geometric_normal = -geometric_normal;
		}

		// Interpolated smooth vertex normal for shading.
		vec3 bary = vec3(1.0 - payload.barycentrics.x - payload.barycentrics.y, payload.barycentrics.x, payload.barycentrics.y);
		vec3 n0 = vec3(tri.data[base + 5], tri.data[base + 6], tri.data[base + 7]);
		vec3 n1 = vec3(tri.data[base + 13], tri.data[base + 14], tri.data[base + 15]);
		vec3 n2 = vec3(tri.data[base + 21], tri.data[base + 22], tri.data[base + 23]);
		vec3 hit_normal = normalize(mat3(to_world) * (n0 * bary.x + n1 * bary.y + n2 * bary.z));
		if (dot(hit_normal, geometric_normal) < 0.0) {
			hit_normal = -hit_normal;
		}

		vec3 hit_position = origin + reflect_dir * hit_t;

		// Material properties at the hit, including maps.
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
		// Crude ray-cone style mip selection: sharper up close, blurrier far away.
		float tex_lod = clamp(log2(max(hit_t, 0.01)) + 1.0, 0.0, 6.0);

		float metallic = instance.metallic_roughness.x;
		float hit_roughness = instance.metallic_roughness.y;
		if (has_uv && instance.metallic_roughness.z >= 0.0) {
			int packed = int(instance.metallic_roughness.z);
			metallic *= textureLod(sampler2D(albedo_textures[nonuniformEXT(packed >> 2)], material_sampler), uv, tex_lod)[packed & 3];
		}
		if (has_uv && instance.metallic_roughness.w >= 0.0) {
			int packed = int(instance.metallic_roughness.w);
			hit_roughness *= textureLod(sampler2D(albedo_textures[nonuniformEXT(packed >> 2)], material_sampler), uv, tex_lod)[packed & 3];
		}

		float clearcoat_strength = clamp(instance.clearcoat.x, 0.0, 1.0);
		float clearcoat_roughness = clamp(instance.clearcoat.y, 0.0, 1.0);
		if (has_uv && instance.clearcoat_tex_index != 0xFFFFFFFF) {
			vec2 clearcoat_tex = textureLod(sampler2D(albedo_textures[nonuniformEXT(instance.clearcoat_tex_index)], material_sampler), uv, tex_lod).rg;
			clearcoat_strength = clamp(clearcoat_strength * clearcoat_tex.r, 0.0, 1.0);
			clearcoat_roughness = clamp(clearcoat_roughness * clearcoat_tex.g, 0.0, 1.0);
		}

		vec3 direct_specular;
		vec3 direct_clearcoat_specular;
		vec3 direct = evaluate_direct_light(hit_position, hit_normal, payload.instance_index, reflect_dir, hit_roughness, clearcoat_strength, clearcoat_roughness, direct_specular, direct_clearcoat_specular);

		vec3 irradiance = vec3(0.0);
		float volume_weight = ddgi_volume_blend_weight(hit_position, ddgi.data);
		if (volume_weight > 0.0) {
			vec3 surface_bias = ddgi_surface_bias(hit_normal, reflect_dir, ddgi.data);
			irradiance = ddgi_sample_irradiance_single(hit_position, surface_bias, hit_normal, ddgi.data);
			irradiance *= volume_weight * ddgi.data.energy;
		}

		vec3 albedo = instance.albedo.rgb;
		if (instance.albedo_tex_index != 0xFFFFFFFF) {
			albedo *= textureLod(sampler2D(albedo_textures[nonuniformEXT(instance.albedo_tex_index)], material_sampler), uv, tex_lod).rgb;
		}
		albedo = min(albedo, vec3(0.9));

		// Metallic workflow: metals have no diffuse and a tinted specular response.
		vec3 diffuse_color = albedo * (1.0 - metallic);
		vec3 f0 = mix(vec3(0.04), albedo, metallic);
		if (clearcoat_strength > 0.0) {
			f0 = mix(f0, f0_clear_coat_to_surface(f0), clearcoat_strength);
		}

		// Environment specular from the probe field along this hit's own
		// reflection direction (no recursion); rougher surfaces fit the blurry
		// probe data naturally, smooth ones at least pick up tinted ambience.
		vec3 specular_env = vec3(0.0);
		vec3 clearcoat_env = vec3(0.0);
		vec3 hit_reflect = normalize(reflect(reflect_dir, hit_normal));
		float spec_volume_weight = ddgi_volume_blend_weight(hit_position, ddgi.data);
		if (spec_volume_weight > 0.0) {
			vec3 spec_bias = ddgi_surface_bias(hit_normal, reflect_dir, ddgi.data);
			specular_env = ddgi_sample_irradiance_single(hit_position, spec_bias, hit_reflect, ddgi.data);
			specular_env *= spec_volume_weight * ddgi.data.energy / DDGI_2PI;

			if (clearcoat_strength > 0.0) {
				vec3 clearcoat_direction = normalize(mix(hit_reflect, hit_normal, mix(0.001, 0.1, clearcoat_roughness)));
				clearcoat_env = ddgi_sample_irradiance_single(hit_position, spec_bias, clearcoat_direction, ddgi.data);
				clearcoat_env *= spec_volume_weight * ddgi.data.energy / DDGI_2PI;
			}
		}

		vec3 base_radiance = (diffuse_color / DDGI_PI) * (direct + irradiance) +
				f0 * (specular_env + direct_specular);
		if (clearcoat_strength > 0.0) {
			float coat_n_dot_v = max(dot(hit_normal, -reflect_dir), 0.0001);
			float coat_fresnel = mix(0.04, 1.0, schlick_fresnel(coat_n_dot_v)) * clearcoat_strength;
			base_radiance *= 1.0 - coat_fresnel;
			base_radiance += clearcoat_env * coat_fresnel + direct_clearcoat_specular;
		}
		radiance = instance.emission.rgb + base_radiance;
	}

	// Fade towards the existing probe based glossy result as roughness
	// approaches the cutoff so the transition is seamless.
	float fade = smoothstep(params.max_roughness * 0.7, params.max_roughness, roughness);
	vec4 previous = imageLoad(reflection_buffer, pixel);
	vec3 rgb = mix(radiance, previous.rgb, fade);
	float alpha = mix(1.0, previous.a, fade);
	imageStore(reflection_buffer, pixel, vec4(rgb, alpha));
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
