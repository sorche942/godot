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

#ifdef MODE_IRRADIANCE
// Low-hysteresis atlas feeding the infinite-bounce loop (see gi.h).
layout(rgba16f, set = 0, binding = 4) uniform restrict coherent image2DArray fast_output_texture;
// Per-probe mean pending change (r) and brightness (g) from last frame.
layout(rg16f, set = 0, binding = 5) uniform restrict coherent image2DArray probe_change;

// Probe-wide reduction of the pending change: real lighting changes are
// coherent across a probe's texels while noise is independent per texel, so
// the probe mean separates them with a ~6x lower noise floor. Accumulated with
// atomics (no mid-flow barrier needed) and published for the next frame.
shared int s_gap_sum_fp;
shared int s_brightness_sum_fp;
#define PROBE_STAT_FP_SCALE 4096.0
#endif

// Cooperative ray cache: every interior texel folds in the same rays, so each
// chunk of ray directions (trig) and ray samples (image loads) is computed and
// loaded once per workgroup instead of once per texel.
#define BLEND_WG_THREADS (OCT_TOTAL_TEXELS * OCT_TOTAL_TEXELS)
shared vec3 s_ray_dir_cache[BLEND_WG_THREADS];
shared vec4 s_ray_sample_cache[BLEND_WG_THREADS];

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
#ifdef MODE_IRRADIANCE
	imageStore(fast_output_texture, thread_coords, imageLoad(fast_output_texture, copy_coords));
#endif
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

#ifdef MODE_IRRADIANCE
	if (gl_LocalInvocationIndex == 0) {
		s_gap_sum_fp = 0;
		s_brightness_sum_fp = 0;
	}
#endif
	barrier();

	// Probes inside a motion region (geometry moved nearby this frame) bypass
	// the convergence freeze and the firefly envelope so moving objects keep
	// responsive, coherent GI shadows.
	bool probe_in_motion = false;
	if (ddgi.data.motion_region_count > 0) {
		ivec3 storage_grid_coords = ddgi_probe_coords(probe_index, ddgi.data);
		ivec3 spatial_coords = ((storage_grid_coords - ddgi.data.probe_scroll_offsets) % ddgi.data.probe_counts + ddgi.data.probe_counts) % ddgi.data.probe_counts;
		vec3 probe_world = ddgi_probe_world_position_base(spatial_coords, ddgi.data);
		if ((ddgi.data.flags & DDGI_FLAG_PROBE_RELOCATION) != 0) {
			probe_world += imageLoad(probe_data, ddgi_probe_texel_coords(probe_index, ddgi.data)).xyz * ddgi.data.probe_spacing;
		}
		for (int r = 0; r < ddgi.data.motion_region_count; r++) {
			if (all(greaterThanEqual(probe_world, ddgi.data.motion_region_min[r].xyz)) && all(lessThanEqual(probe_world, ddgi.data.motion_region_max[r].xyz))) {
				probe_in_motion = true;
				break;
			}
		}
	}

	ivec3 storage_coords = ddgi_probe_texel_coords(probe_index, ddgi.data);

	// Per-probe (workgroup-uniform) early conditions, hoisted out of the
	// border-texel divergence so the cooperative loop's barriers stay in
	// uniform control flow.
	bool scroll_cleared = ddgi_probe_scroll_cleared(storage_coords, ddgi.data);
	bool use_classification = (ddgi.data.flags & DDGI_FLAG_PROBE_CLASSIFICATION) != 0;
	bool probe_inactive = use_classification && imageLoad(probe_data, storage_coords).w == DDGI_PROBE_STATE_INACTIVE;
	bool blending = !is_border_texel && !scroll_cleared && !probe_inactive;

	vec3 probe_ray_direction = vec3(0.0);
	if (blending) {
		ivec2 interior_coords = group_thread.xy - ivec2(1);
		vec2 probe_octant_uv = ddgi_normalized_oct_coord(interior_coords, OCT_INTERIOR_TEXELS);
		probe_ray_direction = ddgi_oct_direction(probe_octant_uv);
	}

	int ray_start = 0;
	if ((ddgi.data.flags & (DDGI_FLAG_PROBE_RELOCATION | DDGI_FLAG_PROBE_CLASSIFICATION)) != 0) {
		// Fixed rays would bias the result, don't blend them.
		ray_start = DDGI_NUM_FIXED_RAYS;
	}

#ifdef MODE_IRRADIANCE
	int backfaces = 0;
	int max_backfaces = int(float(ddgi.data.probe_ray_count - ray_start) * ddgi.data.random_backface_threshold);
#endif

	vec4 result = vec4(0.0);
	bool early_out = false;

	if (!scroll_cleared && !probe_inactive) {
		for (int chunk_start = ray_start; chunk_start < ddgi.data.probe_ray_count; chunk_start += BLEND_WG_THREADS) {
			int load_index = chunk_start + int(gl_LocalInvocationIndex);
			if (load_index < ddgi.data.probe_ray_count) {
				s_ray_dir_cache[gl_LocalInvocationIndex] = ddgi_probe_ray_direction(load_index, ddgi.data);
				s_ray_sample_cache[gl_LocalInvocationIndex] = imageLoad(ray_data, ddgi_ray_data_texel_coords(load_index, probe_index, ddgi.data));
			}
			barrier();

			if (blending && !early_out) {
				int chunk_count = min(BLEND_WG_THREADS, ddgi.data.probe_ray_count - chunk_start);
				for (int i = 0; i < chunk_count; i++) {
					vec4 ray_sample = s_ray_sample_cache[i];
					float weight = max(0.0, dot(probe_ray_direction, s_ray_dir_cache[i]));

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
			}
			barrier();
		}
	}

	if (!is_border_texel) {
		// Clear and skip blending for probes that scrolled to a new position this frame.
		if (scroll_cleared) {
			imageStore(output_texture, thread_coords, vec4(0.0, 0.0, 0.0, 1.0));
#ifdef MODE_IRRADIANCE
			imageStore(fast_output_texture, thread_coords, vec4(0.0, 0.0, 0.0, 1.0));
#endif
		} else if (!probe_inactive) {
			{
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

					vec4 previous_texel = imageLoad(output_texture, thread_coords);
					vec3 previous = previous_texel.rgb;

					// Freshly cleared probes (reset or scrolled) adopt the new value
					// immediately instead of slowly converging from black.
#ifdef MODE_IRRADIANCE
					// The alpha channel is never written as zero by the blend, so it
					// doubles as the "cleared" marker; testing rgb would re-trigger
					// for texels whose converged value is genuinely black.
					bool probe_reset = previous_texel.a == 0.0;
#else
					bool probe_reset = dot(previous, previous) == 0.0;
#endif
					float hysteresis = probe_reset ? 0.0 : ddgi.data.probe_hysteresis;

#ifdef MODE_IRRADIANCE
					// Tone-mapping gamma adjustment.
					result.rgb = pow(result.rgb, vec3(1.0 / ddgi.data.irradiance_gamma));

					// Fast feedback atlas: converges in a few frames so the bounce
					// chain isn't throttled by the display hysteresis. Mild blending
					// keeps its noise from echoing into the estimates.
					vec3 fast_previous = imageLoad(fast_output_texture, thread_coords).rgb;
					{
						float fast_hysteresis = probe_reset ? 0.0 : 0.8;
						imageStore(fast_output_texture, thread_coords, vec4(mix(result.rgb, fast_previous, fast_hysteresis), 1.0));
					}

					if (!probe_reset && !probe_in_motion) {
						// Firefly suppression: rare lucky rays hitting small bright
						// emitters spike the estimate of otherwise dim texels and keep
						// them from ever converging. Envelope the estimate relative to
						// the converged value (encoded space, so 2x encoded = 32x
						// linear: a persistent change blasts through in a few frames).
						// Skipped during motion: it slows shadow recovery.
						result.rgb = min(result.rgb, previous * 2.0 + (1.0 / 64.0));
					}

					vec3 delta = result.rgb - previous;

					// Convergence detection: an EMA of the *signed* per-frame change
					// (stored in the spare alpha channel). The per-frame ray estimate
					// is noisy but zero-mean once converged, so the EMA decays towards
					// zero; a real lighting change pushes it consistently in one
					// direction and revives it within a few frames.
					// Encoded at 2x in alpha to keep EMA steps above fp16 quantization.
					float signed_change = clamp(delta.r + delta.g + delta.b, -0.5, 0.5);
					float change_ema = probe_reset ? 0.5 : mix((previous_texel.a - 0.5), signed_change, 0.06);

					// Relative knee: per-frame estimate noise is roughly proportional
					// to the texel's brightness, so the "converged" threshold must be
					// too. Real lighting changes above ~8% revive the EMA and bring
					// back the responsive hysteresis within a few frames.
					float previous_sum = previous.r + previous.g + previous.b;
					float convergence_knee = max(0.01, 0.03 * previous_sum);

					// Two independent change statistics must agree to unfreeze:
					// - the signed-delta EMA (long window; zero-mean noise decays),
					// - the gap between the fast feedback atlas and the displayed
					//   value (short window; measures the pending change directly).
					// Their false positives multiply, so each knee can be sensitive
					// without reintroducing shimmer.
					float signed_gap = (fast_previous.r + fast_previous.g + fast_previous.b) - previous_sum;
					float gap_evidence = smoothstep(convergence_knee * 0.75, convergence_knee * 2.5, abs(signed_gap));
					float ema_evidence = smoothstep(convergence_knee * 0.25, convergence_knee, abs(change_ema));
					// A real change drives both statistics in the same direction;
					// noise only agrees half the time.
					float sign_agreement = (signed_gap * change_ema > 0.0) ? 1.0 : 0.0;
					float change_evidence = ema_evidence * gap_evidence * sign_agreement;

					// Probe-coherent pending change (previous frame's reduction):
					// unfreezes and accelerates the whole probe through the slow
					// settling tail that per-texel statistics cannot separate from
					// noise.
					atomicAdd(s_gap_sum_fp, int(signed_gap * PROBE_STAT_FP_SCALE));
					atomicAdd(s_brightness_sum_fp, int(previous_sum * PROBE_STAT_FP_SCALE));
					vec2 probe_stat = imageLoad(probe_change, storage_coords).rg;
					float probe_rel_gap = abs(probe_stat.r) / max(0.05, probe_stat.g);
					float probe_evidence = smoothstep(0.02, 0.06, probe_rel_gap);
					change_evidence = max(change_evidence, probe_evidence);

					float convergence = 1.0 - change_evidence;
					// Strong agreement proves the change is real and substantial;
					// blend much faster to collapse the convergence tail.
					float consistency = change_evidence * max(smoothstep(convergence_knee, convergence_knee * 4.0, abs(change_ema)), probe_evidence);

					if (!probe_reset) {
						if (max_component(previous - result.rgb) > ddgi.data.irradiance_threshold) {
							// Lower the hysteresis when a large lighting change is detected.
							hysteresis = max(0.0, hysteresis - 0.75);
							convergence = 0.0;
						}

						if (consistency < 0.5 && linear_rgb_to_luminance(delta) > ddgi.data.brightness_threshold) {
							// Clamp the maximum per-update change when a large
							// brightness change is detected, unless the EMA already
							// vouches that the change is consistent.
							delta *= 0.25;
						}
					}

					// Converged texels FREEZE (hysteresis 1): the per-frame ray
					// estimate never stops being noisy, so any blend-through keeps a
					// visible random walk alive. The change EMA in alpha keeps
					// updating regardless, and any consistent drift of the estimates
					// (even a slow one: slow changes have a consistent sign) revives
					// the responsive hysteresis within a few frames.
					// Verified-change acceleration first, then the freeze; the two
					// regimes are mutually exclusive by construction.
					hysteresis = mix(hysteresis, 0.5, consistency);
					hysteresis = mix(hysteresis, 1.0, convergence);

					if (probe_in_motion) {
						// Track moving geometry quickly and coherently; the extra
						// noise is masked by the motion itself.
						hysteresis = min(hysteresis, 0.95);
					}

					// Step at least the minimum representable value when darkening so
					// convergence doesn't stall in low bit-depth formats. Skipped for
					// converged texels (it would force a visible oscillation).
					const float c_threshold = 1.0 / 1024.0;
					vec3 lerp_delta = (1.0 - hysteresis) * delta;
					if (convergence < 0.5 && max_component(result.rgb) < max_component(previous)) {
						lerp_delta = min(max(vec3(c_threshold), abs(lerp_delta)), abs(delta)) * sign(lerp_delta);
					}
					// Alpha 0 is reserved as the cleared marker.
					result = vec4(previous + lerp_delta, clamp(change_ema + 0.5, 1.0 / 255.0, 1.0));
#else
					if (probe_in_motion) {
						hysteresis = min(hysteresis, 0.9);
					}
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

#ifdef MODE_IRRADIANCE
	if (gl_LocalInvocationIndex == 0) {
		float inv_texels = 1.0 / float(OCT_INTERIOR_TEXELS * OCT_INTERIOR_TEXELS);
		float mean_gap = (float(s_gap_sum_fp) / PROBE_STAT_FP_SCALE) * inv_texels;
		float mean_brightness = (float(s_brightness_sum_fp) / PROBE_STAT_FP_SCALE) * inv_texels;
		ivec3 stat_coords = ddgi_probe_texel_coords(probe_index, ddgi.data);
		// Temporal smoothing: low-amplitude ringing of the fast feedback loop
		// is coherent but alternating, so a short EMA suppresses it while a
		// genuine pending change persists through it.
		float previous_gap = imageLoad(probe_change, stat_coords).r;
		mean_gap = mix(previous_gap, mean_gap, 0.35);
		imageStore(probe_change, stat_coords, vec4(mean_gap, mean_brightness, 0.0, 0.0));
	}
#endif

	if (is_border_texel) {
		update_border_texel(thread_coords, group_thread, group_id);
	}
}
