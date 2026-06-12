#[compute]

#version 450

#VERSION_DEFINES

#include "ddgi_inc.glsl"

// Per-probe maintenance passes (one thread per probe):
// - MODE_RELOCATE: moves probes out of geometry using the fixed rays
//   (probe placement tuning).
// - MODE_CLASSIFY: marks probes with no nearby geometry as inactive so their
//   rays can be skipped (performance).
// - MODE_RESET: resets offsets and states (used when settings change).

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform DDGIVolume {
	DDGIVolumeData data;
}
ddgi;

layout(rgba32f, set = 0, binding = 1) uniform restrict readonly image2DArray ray_data;

layout(rgba16f, set = 0, binding = 2) uniform restrict image2DArray probe_data;

void main() {
	int probe_index = int(gl_GlobalInvocationID.x);

	int num_probes = ddgi.data.probe_counts.x * ddgi.data.probe_counts.y * ddgi.data.probe_counts.z;
	if (probe_index >= num_probes) {
		return;
	}

	ivec3 output_coords = ddgi_probe_texel_coords(probe_index, ddgi.data);

#ifdef MODE_RESET
	imageStore(probe_data, output_coords, vec4(0.0, 0.0, 0.0, DDGI_PROBE_STATE_ACTIVE));
#else

	// Probes that scrolled to a new position start over.
	if (ddgi_probe_scroll_cleared(output_coords, ddgi.data)) {
		imageStore(probe_data, output_coords, vec4(0.0, 0.0, 0.0, DDGI_PROBE_STATE_ACTIVE));
		return;
	}

	int num_rays = min(ddgi.data.probe_ray_count, DDGI_NUM_FIXED_RAYS);

#ifdef MODE_RELOCATE
	vec4 current = imageLoad(probe_data, output_coords);
	vec3 offset = current.xyz * ddgi.data.probe_spacing;

	int closest_backface_index = -1;
	int closest_frontface_index = -1;
	int farthest_frontface_index = -1;
	float closest_backface_distance = 1e27;
	float closest_frontface_distance = 1e27;
	float farthest_frontface_distance = 0.0;
	float backface_count = 0.0;

	for (int ray_index = 0; ray_index < num_rays; ray_index++) {
		ivec3 ray_coords = ddgi_ray_data_texel_coords(ray_index, probe_index, ddgi.data);
		float hit_distance = imageLoad(ray_data, ray_coords).w;

		if (hit_distance < 0.0) {
			backface_count += 1.0;
			// Undo the 80% shortening applied on backface hits.
			hit_distance = hit_distance * -5.0;
			if (hit_distance < closest_backface_distance) {
				closest_backface_distance = hit_distance;
				closest_backface_index = ray_index;
			}
		} else {
			if (hit_distance < closest_frontface_distance) {
				closest_frontface_distance = hit_distance;
				closest_frontface_index = ray_index;
			} else if (hit_distance > farthest_frontface_distance) {
				farthest_frontface_distance = hit_distance;
				farthest_frontface_index = ray_index;
			}
		}
	}

	vec3 full_offset = vec3(1e27);

	if (closest_backface_index != -1 && (backface_count / float(num_rays)) > ddgi.data.fixed_backface_threshold) {
		// Probe is likely inside geometry: move it out through the closest backface.
		vec3 closest_backface_direction = ddgi_probe_ray_direction(closest_backface_index, ddgi.data);
		full_offset = offset + closest_backface_direction * (closest_backface_distance + ddgi.data.probe_min_frontface_distance * 0.5);
	} else if (closest_frontface_distance < ddgi.data.probe_min_frontface_distance) {
		// Probe is too close to a frontface: move it away, but only if that
		// doesn't bring it closer to the nearest frontface.
		vec3 closest_frontface_direction = ddgi_probe_ray_direction(closest_frontface_index, ddgi.data);
		vec3 farthest_frontface_direction = ddgi_probe_ray_direction(farthest_frontface_index, ddgi.data);

		if (dot(closest_frontface_direction, farthest_frontface_direction) <= 0.0) {
			// Never move through the farthest frontface.
			farthest_frontface_direction *= min(farthest_frontface_distance, 1.0);
			full_offset = offset + farthest_frontface_direction;
		}
	} else if (closest_frontface_distance > ddgi.data.probe_min_frontface_distance) {
		// Nothing nearby: move back towards zero offset.
		float move_back_margin = min(closest_frontface_distance - ddgi.data.probe_min_frontface_distance, length(offset));
		if (length(offset) > 0.0) {
			full_offset = offset + move_back_margin * normalize(-offset);
		}
	}

	// Clamp the offset so probes stay within their grid cell (ellipsoid bound).
	vec3 normalized_offset = full_offset / ddgi.data.probe_spacing;
	if (dot(normalized_offset, normalized_offset) < 0.2025) { // 0.45 * 0.45
		offset = full_offset;
	}

	imageStore(probe_data, output_coords, vec4(offset / ddgi.data.probe_spacing, current.w));
#endif // MODE_RELOCATE

#ifdef MODE_CLASSIFY
	vec4 current = imageLoad(probe_data, output_coords);
	bool was_inactive = current.w == DDGI_PROBE_STATE_INACTIVE;

	int backface_count = 0;

	for (int ray_index = 0; ray_index < num_rays; ray_index++) {
		ivec3 ray_coords = ddgi_ray_data_texel_coords(ray_index, probe_index, ddgi.data);
		backface_count += int(imageLoad(ray_data, ray_coords).w < 0.0);
	}

	// Too many backface hits: the probe is probably inside geometry.
	if ((float(backface_count) / float(num_rays)) > ddgi.data.fixed_backface_threshold) {
		imageStore(probe_data, output_coords, vec4(current.xyz, DDGI_PROBE_STATE_INACTIVE));
		return;
	}

	// Determine if there is geometry within this probe's voxel by comparing
	// ray hit distances against the distances to the voxel planes. The 32
	// fixed rays alone miss small geometry (and the miss pattern repeats with
	// the grid, leaving holes of dead probes at regular intervals), so active
	// probes scan their full rotating ray set: over a few frames it covers the
	// sphere densely enough to catch anything that matters. Inactive probes
	// only have fresh data for the fixed rays.
	int scan_rays = was_inactive ? num_rays : ddgi.data.probe_ray_count;
	bool geometry_in_voxel = false;
	for (int ray_index = 0; ray_index < scan_rays; ray_index++) {
		ivec3 ray_coords = ddgi_ray_data_texel_coords(ray_index, probe_index, ddgi.data);
		float hit_distance = imageLoad(ray_data, ray_coords).w;
		if (hit_distance < 0.0) {
			continue;
		}

		vec3 direction = ddgi_probe_ray_direction(ray_index, ddgi.data);

		// Distance along the ray to each axis-aligned voxel plane in the ray's octant.
		// (Independent of the probe position: the planes sit one probe spacing away.)
		vec3 distances = vec3(
				ddgi.data.probe_spacing.x / max(abs(direction.x), 0.000001),
				ddgi.data.probe_spacing.y / max(abs(direction.y), 0.000001),
				ddgi.data.probe_spacing.z / max(abs(direction.z), 0.000001));

		float max_distance = min(distances.x, min(distances.y, distances.z));

		if (hit_distance <= max_distance) {
			geometry_in_voxel = true;
			break;
		}
	}

	if (geometry_in_voxel) {
		// Fade back in rather than snapping to active: borderline probes whose
		// rotating rays only intermittently catch geometry would otherwise
		// oscillate between states and modulate the sampled lighting.
		imageStore(probe_data, output_coords, vec4(current.xyz, max(current.w - 0.25, DDGI_PROBE_STATE_ACTIVE)));
		return;
	}

	// Probes inside a motion region wake up: something moved (or spawned) here
	// and inactive probes' sparse fixed rays may never see it on their own.
	if (ddgi.data.motion_region_count > 0) {
		// Recover the probe's world position from its storage coordinates.
		ivec3 storage_coords = ddgi_probe_coords(probe_index, ddgi.data);
		ivec3 spatial_coords = ((storage_coords - ddgi.data.probe_scroll_offsets) % ddgi.data.probe_counts + ddgi.data.probe_counts) % ddgi.data.probe_counts;
		vec3 probe_world = ddgi_probe_world_position_base(spatial_coords, ddgi.data) + current.xyz * ddgi.data.probe_spacing;
		for (int r = 0; r < ddgi.data.motion_region_count; r++) {
			if (all(greaterThanEqual(probe_world, ddgi.data.motion_region_min[r].xyz)) && all(lessThanEqual(probe_world, ddgi.data.motion_region_max[r].xyz))) {
				imageStore(probe_data, output_coords, vec4(current.xyz, DDGI_PROBE_STATE_ACTIVE));
				return;
			}
		}
	}

	// No geometry in sight: deactivate, but only after a streak of consecutive
	// empty frames. Small objects are only caught by the rotating rays every
	// few frames; instant deactivation would flicker them in and out.
	float new_state = min(current.w + (1.0 / 16.0), DDGI_PROBE_STATE_INACTIVE);
	imageStore(probe_data, output_coords, vec4(current.xyz, new_state));
#endif // MODE_CLASSIFY

#endif // !MODE_RESET
}
