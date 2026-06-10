#[compute]

#version 450

#VERSION_DEFINES

#include "ddgi_inc.glsl"

// Blends the raytraced per-probe radiance (or hit distances) into the
// octahedral irradiance (or filtered distance) atlas, then updates the
// 1-texel border for bilinear filtering.
//
// One workgroup processes one probe, covering its full octahedral footprint
// including borders.

#ifdef MODE_IRRADIANCE
#define OCT_INTERIOR_TEXELS DDGI_IRRADIANCE_OCT_SIZE
#else
#define OCT_INTERIOR_TEXELS DDGI_DISTANCE_OCT_SIZE
#endif

#define OCT_TOTAL_TEXELS (OCT_INTERIOR_TEXELS + 2)

layout(local_size_x = OCT_TOTAL_TEXELS, local_size_y = OCT_TOTAL_TEXELS, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform DDGIVolume {
	DDGIVolumeData data;
}
ddgi;

layout(rgba32f, set = 0, binding = 1) uniform restrict readonly image2DArray ray_data;

#ifdef MODE_IRRADIANCE
layout(rgba16f, set = 0, binding = 2) uniform restrict coherent image2DArray output_texture;
#else
layout(rg16f, set = 0, binding = 2) uniform restrict coherent image2DArray output_texture;
#endif

layout(rgba16f, set = 0, binding = 3) uniform restrict readonly image2DArray probe_data;

float linear_rgb_to_luminance(vec3 rgb) {
	return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

float max_component(vec3 v) {
	return max(v.x, max(v.y, v.z));
}

void update_border_texel(ivec3 thread_coords, ivec3 group_thread, ivec3 group_id) {
	bool is_corner_texel = (group_thread.x == 0 || group_thread.x == (OCT_TOTAL_TEXELS - 1)) && (group_thread.y == 0 || group_thread.y == (OCT_TOTAL_TEXELS - 1));
	bool is_row_texel = (group_thread.x > 0 && group_thread.x < (OCT_TOTAL_TEXELS - 1));

	ivec3 copy_coords = ivec3(group_id.x * OCT_TOTAL_TEXELS, group_id.y * OCT_TOTAL_TEXELS, thread_coords.z);

	if (is_corner_texel) {
		copy_coords.x += (group_thread.x > 0) ? 1 : OCT_INTERIOR_TEXELS;
		copy_coords.y += (group_thread.y > 0) ? 1 : OCT_INTERIOR_TEXELS;
	} else if (is_row_texel) {
		copy_coords.x += (OCT_TOTAL_TEXELS - 1) - group_thread.x;
		copy_coords.y += group_thread.y + ((group_thread.y > 0) ? -1 : 1);
	} else { // Column texel.
		copy_coords.x += group_thread.x + ((group_thread.x > 0) ? -1 : 1);
		copy_coords.y += (OCT_TOTAL_TEXELS - 1) - group_thread.y;
	}

	imageStore(output_texture, thread_coords, imageLoad(output_texture, copy_coords));
}

void main() {
	ivec3 thread_coords = ivec3(gl_GlobalInvocationID);
	ivec3 group_thread = ivec3(gl_LocalInvocationID);
	ivec3 group_id = ivec3(gl_WorkGroupID);

	bool is_border_texel = (group_thread.x == 0 || group_thread.x == (OCT_TOTAL_TEXELS - 1));
	is_border_texel = is_border_texel || (group_thread.y == 0 || group_thread.y == (OCT_TOTAL_TEXELS - 1));

	// The storage index of the probe this workgroup processes.
	// Workgroups are dispatched as (probe_counts.x, probe_counts.z, probe_counts.y).
	int probe_index = group_id.x + (group_id.y * ddgi.data.probe_counts.x) + (group_id.z * ddgi_probes_per_plane(ddgi.data));

	int num_probes = ddgi.data.probe_counts.x * ddgi.data.probe_counts.y * ddgi.data.probe_counts.z;
	if (probe_index >= num_probes || probe_index < 0) {
		return;
	}

	if (!is_border_texel) {
		ivec3 storage_coords = ddgi_probe_texel_coords(probe_index, ddgi.data);

		// Clear and skip blending for probes that scrolled to a new position this frame.
		if (ddgi_probe_scroll_cleared(storage_coords, ddgi.data)) {
			imageStore(output_texture, thread_coords, vec4(0.0, 0.0, 0.0, 1.0));
		} else {
			bool use_classification = (ddgi.data.flags & DDGI_FLAG_PROBE_CLASSIFICATION) != 0;
			if (use_classification && imageLoad(probe_data, storage_coords).w == DDGI_PROBE_STATE_INACTIVE) {
				// Don't blend rays for inactive probes.
			} else {
				ivec2 interior_coords = group_thread.xy - ivec2(1);
				vec2 probe_octant_uv = ddgi_normalized_oct_coord(interior_coords, OCT_INTERIOR_TEXELS);
				vec3 probe_ray_direction = ddgi_oct_direction(probe_octant_uv);

				int ray_index = 0;
				if ((ddgi.data.flags & (DDGI_FLAG_PROBE_RELOCATION | DDGI_FLAG_PROBE_CLASSIFICATION)) != 0) {
					// Fixed rays would bias the result, don't blend them.
					ray_index = DDGI_NUM_FIXED_RAYS;
				}

#ifdef MODE_IRRADIANCE
				int backfaces = 0;
				int max_backfaces = int(float(ddgi.data.probe_ray_count - ray_index) * ddgi.data.random_backface_threshold);
#endif

				vec4 result = vec4(0.0);
				bool early_out = false;
				for (; ray_index < ddgi.data.probe_ray_count; ray_index++) {
					vec3 ray_direction = ddgi_probe_ray_direction(ray_index, ddgi.data);
					float weight = max(0.0, dot(probe_ray_direction, ray_direction));

					ivec3 ray_tex_coords = ddgi_ray_data_texel_coords(ray_index, probe_index, ddgi.data);
					vec4 ray_sample = imageLoad(ray_data, ray_tex_coords);

#ifdef MODE_IRRADIANCE
					if (ray_sample.w < 0.0) {
						// Backface hit, don't blend.
						backfaces++;
						if (backfaces >= max_backfaces) {
							// Probe is likely inside geometry: don't blend anything this frame.
							early_out = true;
							break;
						}
						continue;
					}

					result += vec4(ray_sample.rgb * weight, weight);
#else
					// Initialize to ~50% larger than the probe grid cell diagonal.
					float probe_max_ray_distance = length(ddgi.data.probe_spacing) * 1.5;

					// Increase or decrease the filtered distance value's "sharpness".
					weight = pow(weight, ddgi.data.probe_distance_exponent);

					// Distance is negative on backface hits, take the absolute value.
					float ray_distance = min(abs(ray_sample.w), probe_max_ray_distance);

					result += vec4(ray_distance * weight, (ray_distance * ray_distance) * weight, 0.0, weight);
#endif
				}

				if (!early_out) {
					int blended_ray_count = ddgi.data.probe_ray_count;
					if ((ddgi.data.flags & (DDGI_FLAG_PROBE_RELOCATION | DDGI_FLAG_PROBE_CLASSIFICATION)) != 0) {
						blended_ray_count -= DDGI_NUM_FIXED_RAYS;
					}
					float epsilon = float(blended_ray_count) * 1e-9;

					// Dividing by the sum of cosine weights (instead of N) reduces variance;
					// the 1/2 factor accounts for that. Distance values must be multiplied
					// by 2 when sampled.
					result.rgb *= 1.0 / (2.0 * max(result.a, epsilon));
					result.a = 1.0;

					vec3 previous = imageLoad(output_texture, thread_coords).rgb;

					// Freshly cleared probes (reset or scrolled) adopt the new value
					// immediately instead of slowly converging from black.
					bool probe_reset = dot(previous, previous) == 0.0;
					float hysteresis = probe_reset ? 0.0 : ddgi.data.probe_hysteresis;

#ifdef MODE_IRRADIANCE
					// Tone-mapping gamma adjustment.
					result.rgb = pow(result.rgb, vec3(1.0 / ddgi.data.irradiance_gamma));

					vec3 delta = result.rgb - previous;

					if (!probe_reset) {
						if (max_component(previous - result.rgb) > ddgi.data.irradiance_threshold) {
							// Lower the hysteresis when a large lighting change is detected.
							hysteresis = max(0.0, hysteresis - 0.75);
						}

						if (linear_rgb_to_luminance(delta) > ddgi.data.brightness_threshold) {
							// Clamp the maximum per-update change when a large brightness change is detected.
							delta *= 0.25;
						}
					}

					// Step at least the minimum representable value when darkening so
					// convergence doesn't stall in low bit-depth formats.
					const float c_threshold = 1.0 / 1024.0;
					vec3 lerp_delta = (1.0 - hysteresis) * delta;
					if (max_component(result.rgb) < max_component(previous)) {
						lerp_delta = min(max(vec3(c_threshold), abs(lerp_delta)), abs(delta)) * sign(lerp_delta);
					}
					result = vec4(previous + lerp_delta, 1.0);
#else
					result = vec4(mix(result.rg, previous.rg, hysteresis), 0.0, 1.0);
#endif

					imageStore(output_texture, thread_coords, result);
				}
			}
		}
	}

	// Wait for all interior texels of this probe, then fill the border.
	memoryBarrierImage();
	barrier();

	if (is_border_texel) {
		update_border_texel(thread_coords, group_thread, group_id);
	}
}
