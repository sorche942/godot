// DDGI (Dynamic Diffuse Global Illumination) shared helpers.
// Implements the probe grid math of NVIDIA's RTXGI DDGI:
// octahedral probe encoding, spherical fibonacci ray distribution,
// infinite scrolling volume indexing and irradiance sampling with
// Chebyshev visibility (statistical occlusion) testing.

#define DDGI_PI 3.141592653589793
#define DDGI_2PI 6.283185307179586

// Texels per probe, excluding the 1-texel border on each side.
#define DDGI_IRRADIANCE_OCT_SIZE 6
#define DDGI_DISTANCE_OCT_SIZE 14

// Fixed (non-rotating) rays used by probe relocation and classification.
#define DDGI_NUM_FIXED_RAYS 32

#define DDGI_PROBE_STATE_ACTIVE 0.0
#define DDGI_PROBE_STATE_INACTIVE 1.0

#define DDGI_FLAG_PROBE_RELOCATION (1 << 0)
#define DDGI_FLAG_PROBE_CLASSIFICATION (1 << 1)
#define DDGI_FLAG_RT_REFLECTIONS (1 << 2)

#define DDGI_SKY_MODE_COLOR 0
#define DDGI_SKY_MODE_SKY_2D 1
#define DDGI_SKY_MODE_SKY_ARRAY 2

#define DDGI_MAX_CASCADES 4

// Mirrors GI::DDGIVolumeDataUBO (std140).
struct DDGIVolumeData {
	vec3 probe_spacing;
	float probe_max_ray_distance;

	ivec3 probe_counts;
	int probe_ray_count;

	ivec3 probe_scroll_offsets;
	int light_count;

	ivec3 scroll_delta;
	uint flags;

	vec3 origin;
	float probe_hysteresis;

	vec4 probe_ray_rotation; // Quaternion (xyz, w).

	float probe_normal_bias;
	float probe_view_bias;
	float probe_distance_exponent;
	float probe_min_frontface_distance;

	float energy;
	float irradiance_gamma;
	float random_backface_threshold;
	float fixed_backface_threshold;

	float irradiance_threshold;
	float brightness_threshold;
	float sky_energy;
	uint sky_mode;

	vec3 sky_color;
	float exposure_normalization;
	vec4 motion_region_min[8];
	vec4 motion_region_max[8];

	int motion_region_count;
	// Roughness below which the RT reflections pass fully overwrites the
	// probe-glossy reflection (so the apply pass can skip computing it).
	float rt_reflections_fade_start;
	int motion_pad1;
	int motion_pad2;

	// Cascade data (mirrors VolumeDataUBO).
	int cascade_count;
	float cascade_spacing_ratio;
	ivec2 cascade_ubo_pad;
	ivec4 cascade_scroll_offsets[DDGI_MAX_CASCADES];
	ivec4 cascade_scroll_delta[DDGI_MAX_CASCADES];
};

/* Quaternion helpers */

vec3 ddgi_quaternion_rotate(vec3 v, vec4 q) {
	vec3 b = q.xyz;
	float b2 = dot(b, b);
	return (v * (q.w * q.w - b2) + b * (dot(v, b) * 2.0) + cross(b, v) * (q.w * 2.0));
}

vec4 ddgi_quaternion_conjugate(vec4 q) {
	return vec4(-q.xyz, q.w);
}

/* Octahedral mapping (RTXGI convention, coordinates in [-1, 1]) */

vec2 ddgi_sign_not_zero(vec2 v) {
	return vec2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? 1.0 : -1.0);
}

// Texel coordinates (within the probe interior) to normalized octahedral coordinates.
vec2 ddgi_normalized_oct_coord(ivec2 tex_coords, int num_texels) {
	vec2 oct_coord = vec2(tex_coords.x % num_texels, tex_coords.y % num_texels);
	oct_coord += 0.5;
	oct_coord /= float(num_texels);
	return oct_coord * 2.0 - 1.0;
}

vec3 ddgi_oct_direction(vec2 coords) {
	vec3 direction = vec3(coords.x, coords.y, 1.0 - abs(coords.x) - abs(coords.y));
	if (direction.z < 0.0) {
		direction.xy = (1.0 - abs(direction.yx)) * ddgi_sign_not_zero(direction.xy);
	}
	return normalize(direction);
}

vec2 ddgi_oct_coord(vec3 direction) {
	float l1norm = abs(direction.x) + abs(direction.y) + abs(direction.z);
	vec2 uv = direction.xy * (1.0 / l1norm);
	if (direction.z < 0.0) {
		uv = (1.0 - abs(uv.yx)) * ddgi_sign_not_zero(uv.xy);
	}
	return uv;
}

/* Probe ray directions */

vec3 ddgi_spherical_fibonacci(float sample_index, float num_samples) {
	const float b = (sqrt(5.0) * 0.5 + 0.5) - 1.0;
	float phi = DDGI_2PI * fract(sample_index * b);
	float cos_theta = 1.0 - (2.0 * sample_index + 1.0) * (1.0 / num_samples);
	float sin_theta = sqrt(clamp(1.0 - (cos_theta * cos_theta), 0.0, 1.0));
	return vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

vec3 ddgi_probe_ray_direction(int ray_index, DDGIVolumeData volume) {
	bool is_fixed_ray = false;
	int sample_index = ray_index;
	int num_rays = volume.probe_ray_count;

	if ((volume.flags & (DDGI_FLAG_PROBE_RELOCATION | DDGI_FLAG_PROBE_CLASSIFICATION)) != 0) {
		is_fixed_ray = (ray_index < DDGI_NUM_FIXED_RAYS);
		sample_index = is_fixed_ray ? ray_index : (ray_index - DDGI_NUM_FIXED_RAYS);
		num_rays = is_fixed_ray ? DDGI_NUM_FIXED_RAYS : (num_rays - DDGI_NUM_FIXED_RAYS);
	}

	vec3 direction = ddgi_spherical_fibonacci(float(sample_index), float(num_rays));

	if (is_fixed_ray) {
		// Fixed rays don't rotate so relocation/classification stay temporally stable.
		return normalize(direction);
	}

	return normalize(ddgi_quaternion_rotate(direction, ddgi_quaternion_conjugate(volume.probe_ray_rotation)));
}

/* Probe grid indexing (right-handed Y-up: planes are vertical slices along Y) */

int ddgi_probes_per_plane(DDGIVolumeData volume) {
	return volume.probe_counts.x * volume.probe_counts.z;
}

int ddgi_probe_index(ivec3 probe_coords, DDGIVolumeData volume) {
	int probes_per_plane = ddgi_probes_per_plane(volume);
	int plane_index = probe_coords.y;
	int index_in_plane = probe_coords.x + (volume.probe_counts.x * probe_coords.z);
	return (plane_index * probes_per_plane) + index_in_plane;
}

ivec3 ddgi_probe_coords(int probe_index, DDGIVolumeData volume) {
	ivec3 coords;
	coords.x = probe_index % volume.probe_counts.x;
	coords.y = probe_index / (volume.probe_counts.x * volume.probe_counts.z);
	coords.z = (probe_index / volume.probe_counts.x) % volume.probe_counts.z;
	return coords;
}

// Storage (texture) index for a probe at the given spatial grid coordinates,
// accounting for infinite scrolling offsets.
int ddgi_scrolling_probe_index(ivec3 probe_coords, DDGIVolumeData volume) {
	return ddgi_probe_index(((probe_coords + volume.probe_scroll_offsets) % volume.probe_counts + volume.probe_counts) % volume.probe_counts, volume);
}

// Texel coordinates (x, y, layer) of a probe in the probe data texture
// (and the per-probe base in the atlas textures), from its storage index.
ivec3 ddgi_probe_texel_coords(int probe_index, DDGIVolumeData volume) {
	int probes_per_plane = ddgi_probes_per_plane(volume);
	int plane_index = probe_index / probes_per_plane;
	int x = probe_index % volume.probe_counts.x;
	int y = (probe_index / volume.probe_counts.x) % volume.probe_counts.z;
	return ivec3(x, y, plane_index + volume.cascade_ubo_pad.x);
}

// Coordinates of a ray in the ray data texture, from a probe storage index.
ivec3 ddgi_ray_data_texel_coords(int ray_index, int probe_index, DDGIVolumeData volume) {
	int probes_per_plane = ddgi_probes_per_plane(volume);
	ivec3 coords;
	coords.x = ray_index;
	coords.z = probe_index / probes_per_plane;
	coords.y = probe_index - (coords.z * probes_per_plane);
	return coords;
}

// Cascade-aware version: adds cascade layer offset to Z.
ivec3 ddgi_ray_data_texel_coords_cascade(int ray_index, int probe_index, int cascade_index, DDGIVolumeData volume) {
	int probes_per_plane = ddgi_probes_per_plane(volume);
	ivec3 coords;
	coords.x = ray_index;
	coords.z = probe_index / probes_per_plane + cascade_index * volume.probe_counts.y;
	coords.y = probe_index - ((probe_index / probes_per_plane) * probes_per_plane);
	return coords;
}

// Normalized atlas UV (and layer) for sampling a probe octant.
vec3 ddgi_probe_uv(int probe_index, vec2 octant_coords, int num_interior_texels, DDGIVolumeData volume) {
	ivec3 coords = ddgi_probe_texel_coords(probe_index, volume);
	float num_texels = float(num_interior_texels) + 2.0;

	float texture_width = num_texels * float(volume.probe_counts.x);
	float texture_height = num_texels * float(volume.probe_counts.z);

	vec2 uv = vec2(float(coords.x) * num_texels, float(coords.y) * num_texels) + (num_texels * 0.5);
	uv += octant_coords * (float(num_interior_texels) * 0.5);
	uv /= vec2(texture_width, texture_height);
	return vec3(uv, float(coords.z));
}

/* Probe positions */

// World position of a probe from its spatial grid coordinates, ignoring relocation.
vec3 ddgi_probe_world_position_base(ivec3 probe_coords, DDGIVolumeData volume) {
	vec3 grid_world_position = vec3(probe_coords) * volume.probe_spacing;
	vec3 grid_shift = (volume.probe_spacing * vec3(volume.probe_counts - ivec3(1))) * 0.5;
	return (grid_world_position - grid_shift) + volume.origin + (vec3(volume.probe_scroll_offsets) * volume.probe_spacing);
}

// Spatial grid coordinates of the base probe of the 8-probe cube around a world position.
ivec3 ddgi_base_probe_coords(vec3 world_position, DDGIVolumeData volume) {
	vec3 position = world_position - (volume.origin + vec3(volume.probe_scroll_offsets) * volume.probe_spacing);
	position += (volume.probe_spacing * vec3(volume.probe_counts - ivec3(1))) * 0.5;
	ivec3 probe_coords = ivec3(position / volume.probe_spacing);
	return clamp(probe_coords, ivec3(0), volume.probe_counts - ivec3(1));
}

// True when a probe (given by *storage* texel coords) was scrolled to a new
// position this frame and its accumulated data must be discarded.
bool ddgi_probe_scroll_cleared(ivec3 storage_coords, DDGIVolumeData volume) {
	for (int axis = 0; axis < 3; axis++) {
		int d = volume.scroll_delta[axis];
		if (d == 0) {
			continue;
		}
		int n = volume.probe_counts[axis];
		if (abs(d) >= n) {
			return true;
		}
		// Recover the spatial coordinate of this storage slot.
		int s = (axis == 0) ? storage_coords.x : ((axis == 1) ? storage_coords.z : storage_coords.y);
		int o = volume.probe_scroll_offsets[axis];
		int rel = ((s - o) % n + n) % n;
		if (d > 0 && rel >= n - d) {
			return true;
		}
		if (d < 0 && rel < -d) {
			return true;
		}
	}
	return false;
}

/* Cascade helpers -- per-cascade versions of the probe grid functions.
   Cascade 0 mirrors the existing behavior (backward compatible). */

vec3 ddgi_cascade_spacing(int cascade_index, DDGIVolumeData volume) {
	return volume.probe_spacing * pow(volume.cascade_spacing_ratio, float(cascade_index));
}

ivec3 ddgi_cascade_scroll_offsets(int cascade_index, DDGIVolumeData volume) {
	if (cascade_index == 0) {
		return volume.probe_scroll_offsets;
	}
	return volume.cascade_scroll_offsets[cascade_index].xyz;
}

ivec3 ddgi_cascade_scroll_delta(int cascade_index, DDGIVolumeData volume) {
	if (cascade_index == 0) {
		return volume.scroll_delta;
	}
	return volume.cascade_scroll_delta[cascade_index].xyz;
}

ivec3 ddgi_probe_texel_coords_cascade(int probe_index, int cascade_index, DDGIVolumeData volume) {
	int probes_per_plane = ddgi_probes_per_plane(volume);
	int plane_index = probe_index / probes_per_plane;
	int x = probe_index % volume.probe_counts.x;
	int y = (probe_index / volume.probe_counts.x) % volume.probe_counts.z;
	return ivec3(x, y, plane_index + cascade_index * volume.probe_counts.y);
}

int ddgi_scrolling_probe_index_cascade(ivec3 probe_coords, int cascade_index, DDGIVolumeData volume) {
	ivec3 scroll = ddgi_cascade_scroll_offsets(cascade_index, volume);
	return ddgi_probe_index(((probe_coords + scroll) % volume.probe_counts + volume.probe_counts) % volume.probe_counts, volume);
}

vec3 ddgi_probe_world_position_base_cascade(ivec3 probe_coords, int cascade_index, DDGIVolumeData volume) {
	vec3 spacing = ddgi_cascade_spacing(cascade_index, volume);
	ivec3 scroll = ddgi_cascade_scroll_offsets(cascade_index, volume);
	vec3 grid_world_position = vec3(probe_coords) * spacing;
	vec3 grid_shift = (spacing * vec3(volume.probe_counts - ivec3(1))) * 0.5;
	return (grid_world_position - grid_shift) + volume.origin + (vec3(scroll) * spacing);
}

ivec3 ddgi_base_probe_coords_cascade(vec3 world_position, int cascade_index, DDGIVolumeData volume) {
	vec3 spacing = ddgi_cascade_spacing(cascade_index, volume);
	ivec3 scroll = ddgi_cascade_scroll_offsets(cascade_index, volume);
	vec3 position = world_position - (volume.origin + vec3(scroll) * spacing);
	position += (spacing * vec3(volume.probe_counts - ivec3(1))) * 0.5;
	ivec3 probe_coords = ivec3(position / spacing);
	return clamp(probe_coords, ivec3(0), volume.probe_counts - ivec3(1));
}

bool ddgi_probe_scroll_cleared_cascade(ivec3 storage_coords, int cascade_index, DDGIVolumeData volume) {
	ivec3 delta = ddgi_cascade_scroll_delta(cascade_index, volume);
	ivec3 scroll = ddgi_cascade_scroll_offsets(cascade_index, volume);
	for (int axis = 0; axis < 3; axis++) {
		int d = delta[axis];
		if (d == 0) {
			continue;
		}
		int n = volume.probe_counts[axis];
		if (abs(d) >= n) {
			return true;
		}
		int s = (axis == 0) ? storage_coords.x : ((axis == 1) ? storage_coords.z : storage_coords.y);
		int o = scroll[axis];
		int rel = ((s - o) % n + n) % n;
		if (d > 0 && rel >= n - d) {
			return true;
		}
		if (d < 0 && rel < -d) {
			return true;
		}
	}
	return false;
}

/* Surface bias for sampling (avoids self-shadowing artifacts) */

vec3 ddgi_surface_bias(vec3 surface_normal, vec3 camera_direction, DDGIVolumeData volume) {
	return (surface_normal * volume.probe_normal_bias) + (-camera_direction * volume.probe_view_bias);
}

// Blend weight in [0, 1]: 1 inside the volume, fading out over
// DDGI_EDGE_FADE_SPACINGS probe spacings outside of it. Wider than the
// original 1-spacing fade to soften the hard boundary sweep when the grid
// scrolls. The outer probes have less-converged data, so the fade shouldn't
// be too wide -- 3 spacings is a good balance for single-grid volumes.
#define DDGI_EDGE_FADE_SPACINGS 3.0
float ddgi_volume_blend_weight(vec3 world_position, DDGIVolumeData volume) {
	vec3 origin = volume.origin + (vec3(volume.probe_scroll_offsets) * volume.probe_spacing);
	vec3 extent = (volume.probe_spacing * vec3(volume.probe_counts - ivec3(1))) * 0.5;

	vec3 delta = abs(world_position - origin) - extent;
	if (all(lessThan(delta, vec3(0.0)))) {
		return 1.0;
	}

	vec3 fade_extent = volume.probe_spacing * DDGI_EDGE_FADE_SPACINGS;
	float weight = 1.0;
	weight *= (1.0 - clamp(delta.x / fade_extent.x, 0.0, 1.0));
	weight *= (1.0 - clamp(delta.y / fade_extent.y, 0.0, 1.0));
	weight *= (1.0 - clamp(delta.z / fade_extent.z, 0.0, 1.0));
	return weight;
}

/* Irradiance sampling (the core of DDGI: 8-probe bilinear blend with
   wrap-shading backface term and Chebyshev visibility weights).

   GLSL doesn't allow passing combined sampler constructors as function
   arguments, so the sampling function requires the including shader to
   declare bindings before including this file, in one of two flavors:

   DDGI_INC_SAMPLING (sampled textures, for raster/compute passes):
     uniform texture2DArray ddgi_irradiance_texture;
     uniform texture2DArray ddgi_distance_texture;
     uniform texture2DArray ddgi_probe_data_texture;
     uniform sampler linear_sampler;

   DDGI_INC_SAMPLING_IMAGE (storage images with manual bilinear filtering,
   for the raytracing pass where the same images are written by the blend
   passes within the same frame):
     layout(rgba16f) uniform readonly image2DArray ddgi_irradiance_image;
     layout(rg16f) uniform readonly image2DArray ddgi_distance_image;
     layout(rgba16f) uniform readonly image2DArray ddgi_probe_data_image; */

#if defined(DDGI_INC_SAMPLING)

vec3 ddgi_fetch_irradiance(vec3 probe_uv) {
	return textureLod(sampler2DArray(ddgi_irradiance_texture, linear_sampler), probe_uv, 0.0).rgb;
}

vec2 ddgi_fetch_distance(vec3 probe_uv) {
	return textureLod(sampler2DArray(ddgi_distance_texture, linear_sampler), probe_uv, 0.0).rg;
}

vec4 ddgi_fetch_probe_data(ivec3 coords) {
	return texelFetch(sampler2DArray(ddgi_probe_data_texture, linear_sampler), coords, 0);
}

#elif defined(DDGI_INC_SAMPLING_IMAGE)

vec3 ddgi_fetch_irradiance(vec3 probe_uv) {
	ivec3 size = imageSize(ddgi_irradiance_image);
	vec2 p = probe_uv.xy * vec2(size.xy) - 0.5;
	ivec2 base = ivec2(floor(p));
	vec2 f = p - vec2(base);
	int layer = int(probe_uv.z);
	vec3 t00 = imageLoad(ddgi_irradiance_image, ivec3(base, layer)).rgb;
	vec3 t10 = imageLoad(ddgi_irradiance_image, ivec3(base + ivec2(1, 0), layer)).rgb;
	vec3 t01 = imageLoad(ddgi_irradiance_image, ivec3(base + ivec2(0, 1), layer)).rgb;
	vec3 t11 = imageLoad(ddgi_irradiance_image, ivec3(base + ivec2(1, 1), layer)).rgb;
	return mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
}

vec2 ddgi_fetch_distance(vec3 probe_uv) {
	ivec3 size = imageSize(ddgi_distance_image);
	vec2 p = probe_uv.xy * vec2(size.xy) - 0.5;
	ivec2 base = ivec2(floor(p));
	vec2 f = p - vec2(base);
	int layer = int(probe_uv.z);
	vec2 t00 = imageLoad(ddgi_distance_image, ivec3(base, layer)).rg;
	vec2 t10 = imageLoad(ddgi_distance_image, ivec3(base + ivec2(1, 0), layer)).rg;
	vec2 t01 = imageLoad(ddgi_distance_image, ivec3(base + ivec2(0, 1), layer)).rg;
	vec2 t11 = imageLoad(ddgi_distance_image, ivec3(base + ivec2(1, 1), layer)).rg;
	return mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
}

vec4 ddgi_fetch_probe_data(ivec3 coords) {
	return imageLoad(ddgi_probe_data_image, coords);
}

#endif

#if defined(DDGI_INC_SAMPLING) || defined(DDGI_INC_SAMPLING_IMAGE)

vec3 ddgi_sample_irradiance_single(vec3 world_position, vec3 surface_bias, vec3 direction, DDGIVolumeData volume) {
	vec3 irradiance = vec3(0.0);
	float accumulated_weights = 0.0;
	// Fallback accumulator with the RTXGI 5% visibility floor; only used when
	// no probe passes the strict visibility test. Keeping the strict sum free
	// of the floor prevents bright occluded probes (e.g. sky-lit probes behind
	// a wall) from leaking into fully enclosed interiors.
	vec3 fallback_irradiance = vec3(0.0);
	float fallback_weights = 0.0;
	// Last resort: stored irradiance of classification-inactive probes (stale
	// but valid open-air data). Only used when every probe got skipped, which
	// happens for small geometry floating in a dead (geometry-free) cell that
	// classification hasn't caught up with; returning black there blacks out
	// whole objects.
	vec3 last_resort_irradiance = vec3(0.0);
	float last_resort_weights = 0.0;

	vec3 biased_world_position = world_position + surface_bias;

	ivec3 base_probe_coords = ddgi_base_probe_coords(biased_world_position, volume);
	vec3 base_probe_world_position = ddgi_probe_world_position_base(base_probe_coords, volume);

	vec3 grid_space_distance = biased_world_position - base_probe_world_position;
	vec3 alpha = clamp(grid_space_distance / volume.probe_spacing, vec3(0.0), vec3(1.0));

	bool use_classification = (volume.flags & DDGI_FLAG_PROBE_CLASSIFICATION) != 0;
	bool use_relocation = (volume.flags & DDGI_FLAG_PROBE_RELOCATION) != 0;

	for (int probe = 0; probe < 8; probe++) {
		ivec3 adjacent_offset = (ivec3(probe) >> ivec3(0, 1, 2)) & ivec3(1);
		ivec3 adjacent_coords = clamp(base_probe_coords + adjacent_offset, ivec3(0), volume.probe_counts - ivec3(1));
		int adjacent_index = ddgi_scrolling_probe_index(adjacent_coords, volume);

		ivec3 probe_data_coords = ddgi_probe_texel_coords(adjacent_index, volume);
		vec4 probe_data = ddgi_fetch_probe_data(probe_data_coords);

		vec3 adjacent_world_position = ddgi_probe_world_position_base(adjacent_coords, volume);
		if (use_relocation) {
			adjacent_world_position += probe_data.xyz * volume.probe_spacing;
		}

		vec3 pos_to_probe_unnormalized = adjacent_world_position - world_position;
		float pos_to_probe_dist = length(pos_to_probe_unnormalized);
		vec3 world_pos_to_probe = (pos_to_probe_dist > 0.0) ? (pos_to_probe_unnormalized / pos_to_probe_dist) : vec3(0.0);

		// The classification state doubles as a miss-streak counter: 0 = active,
		// 1 = fully inactive, in-between = fading out. Weigh the probe down
		// smoothly so state transitions don't pop.
		float probe_liveness = use_classification ? (1.0 - clamp(probe_data.w, 0.0, 1.0)) : 1.0;

		if (probe_liveness <= 0.0) {
			vec3 alpha_mix = mix(1.0 - alpha, alpha, vec3(adjacent_offset));
			float inactive_trilinear = max(0.001, alpha_mix.x * alpha_mix.y * alpha_mix.z);
			float inactive_wrap = (dot(world_pos_to_probe, direction) + 1.0) * 0.5;
			float inactive_weight = inactive_trilinear * ((inactive_wrap * inactive_wrap) + 0.2);
			vec2 inactive_octant = ddgi_oct_coord(direction);
			vec3 inactive_uv = ddgi_probe_uv(adjacent_index, inactive_octant, DDGI_IRRADIANCE_OCT_SIZE, volume);
			vec3 inactive_irradiance = max(ddgi_fetch_irradiance(inactive_uv), vec3(0.0));
			last_resort_irradiance += inactive_weight * pow(inactive_irradiance, vec3(volume.irradiance_gamma * 0.5));
			last_resort_weights += inactive_weight;
			continue;
		}
		vec3 biased_pos_to_probe = adjacent_world_position - biased_world_position;
		float biased_pos_to_probe_dist = length(biased_pos_to_probe);
		biased_pos_to_probe = (biased_pos_to_probe_dist > 0.0) ? (biased_pos_to_probe / biased_pos_to_probe_dist) : vec3(0.0);

		vec3 trilinear = max(vec3(0.001), mix(1.0 - alpha, alpha, vec3(adjacent_offset)));
		float trilinear_weight = trilinear.x * trilinear.y * trilinear.z;
		float weight = 1.0;

		// "Wrap shading" backface test: don't fully reject probes behind the
		// surface so small geometry doesn't lose all valid probes.
		float wrap_shading = (dot(world_pos_to_probe, direction) + 1.0) * 0.5;
		weight *= (wrap_shading * wrap_shading) + 0.2;

		// Chebyshev visibility (the DDGI occlusion fix): use mean/mean-squared
		// distances stored in the probe to statistically reject occluded probes.
		// Note: unlike the RTXGI reference, the mean is used unscaled (RTXGI
		// multiplies it by 2, which lets any sample within twice the occluder
		// distance pass as visible and leaks sky-lit probes through walls into
		// enclosed interiors), and the variance is the proper E[d^2] - E[d]^2.
		vec2 octant_coords = ddgi_oct_coord(-biased_pos_to_probe);
		vec3 probe_uv = ddgi_probe_uv(adjacent_index, octant_coords, DDGI_DISTANCE_OCT_SIZE, volume);
		vec2 filtered_distance = ddgi_fetch_distance(probe_uv);

		float variance = abs(filtered_distance.y - (filtered_distance.x * filtered_distance.x));

		float chebyshev_weight = 1.0;
		if (biased_pos_to_probe_dist > filtered_distance.x) {
			float v = biased_pos_to_probe_dist - filtered_distance.x;
			chebyshev_weight = variance / (variance + (v * v));
			chebyshev_weight = max(chebyshev_weight * chebyshev_weight * chebyshev_weight, 0.0);
		}

		float strict_weight = max(0.000001, weight * chebyshev_weight);
		float fallback_weight = max(0.000001, weight * max(0.05, chebyshev_weight));

		// Crush tiny weights (logarithmic perception).
		const float crush_threshold = 0.2;
		if (strict_weight < crush_threshold) {
			strict_weight *= (strict_weight * strict_weight) * (1.0 / (crush_threshold * crush_threshold));
		}
		if (fallback_weight < crush_threshold) {
			fallback_weight *= (fallback_weight * fallback_weight) * (1.0 / (crush_threshold * crush_threshold));
		}

		strict_weight *= trilinear_weight * probe_liveness;
		fallback_weight *= trilinear_weight * probe_liveness;

		octant_coords = ddgi_oct_coord(direction);
		probe_uv = ddgi_probe_uv(adjacent_index, octant_coords, DDGI_IRRADIANCE_OCT_SIZE, volume);
		vec3 probe_irradiance = max(ddgi_fetch_irradiance(probe_uv), vec3(0.0));

		// Decode the tone curve, leaving a gamma = 2 curve to approximate sRGB blending.
		probe_irradiance = pow(probe_irradiance, vec3(volume.irradiance_gamma * 0.5));

		irradiance += strict_weight * probe_irradiance;
		accumulated_weights += strict_weight;
		fallback_irradiance += fallback_weight * probe_irradiance;
		fallback_weights += fallback_weight;
	}

	// Blend towards the floored fallback as the strict weights vanish, so the
	// no-visibility case stays stable without leaking through occluders.
	const float fallback_threshold = 0.01;
	if (accumulated_weights < fallback_threshold && fallback_weights > 0.0) {
		float t = accumulated_weights / fallback_threshold;
		vec3 fallback_avg = fallback_irradiance / fallback_weights;
		vec3 strict_avg = (accumulated_weights > 0.0) ? (irradiance / accumulated_weights) : fallback_avg;
		vec3 blended = mix(fallback_avg, strict_avg, t);
		blended *= blended;
		blended *= DDGI_2PI;
		return blended;
	}

	if (accumulated_weights == 0.0) {
		if (last_resort_weights > 0.0) {
			vec3 blended = last_resort_irradiance / last_resort_weights;
			blended *= blended;
			blended *= DDGI_2PI;
			return blended;
		}
		return vec3(0.0);
	}

	irradiance *= (1.0 / accumulated_weights);
	irradiance *= irradiance; // Back to linear irradiance.
	irradiance *= DDGI_2PI; // Complete the Monte Carlo estimator (hemisphere area).

	return irradiance;
}

// Cascade selection by view distance (view_relative = sample_point - camera).
float ddgi_select_cascade(vec3 view_relative, DDGIVolumeData volume) {
	if (volume.cascade_count <= 1) {
		return 0.0;
	}
	vec3 cascade0_range = volume.probe_spacing * vec3(volume.probe_counts / 2 - ivec3(2));
	vec3 normalized_dist = abs(view_relative) / max(cascade0_range, vec3(0.001));
	float dist = length(normalized_dist);
	float cascade = log2(max(dist, 0.001)) + 1.0;
	float blend_start = float(volume.cascade_count - 1) * 0.7;
	return mix(cascade, float(volume.cascade_count - 1), smoothstep(blend_start, float(volume.cascade_count - 1), cascade));
}

// Cascade-aware wrapper: selects cascade by view distance, creates a
// per-cascade volume copy with the correct spacing/scroll/layer-offset,
// and blends between adjacent cascades in the overlap band.
vec3 ddgi_sample_irradiance(vec3 world_position, vec3 surface_bias, vec3 direction, vec3 view_relative, DDGIVolumeData volume) {
	if (volume.cascade_count <= 1) {
		return ddgi_sample_irradiance_single(world_position, surface_bias, direction, volume);
	}

	float cascade_f = ddgi_select_cascade(view_relative, volume);
	int lower = int(floor(cascade_f));
	int upper = min(lower + 1, volume.cascade_count - 1);

	DDGIVolumeData lower_vol = volume;
	lower_vol.probe_spacing = ddgi_cascade_spacing(lower, volume);
	lower_vol.probe_scroll_offsets = ddgi_cascade_scroll_offsets(lower, volume);
	lower_vol.scroll_delta = ddgi_cascade_scroll_delta(lower, volume);
	lower_vol.cascade_ubo_pad.x = lower * volume.probe_counts.y;
	vec3 lower_irradiance = ddgi_sample_irradiance_single(world_position, surface_bias, direction, lower_vol);

	if (lower >= upper) {
		return lower_irradiance;
	}

	DDGIVolumeData upper_vol = volume;
	upper_vol.probe_spacing = ddgi_cascade_spacing(upper, volume);
	upper_vol.probe_scroll_offsets = ddgi_cascade_scroll_offsets(upper, volume);
	upper_vol.scroll_delta = ddgi_cascade_scroll_delta(upper, volume);
	upper_vol.cascade_ubo_pad.x = upper * volume.probe_counts.y;
	vec3 upper_irradiance = ddgi_sample_irradiance_single(world_position, surface_bias, direction, upper_vol);

	return mix(lower_irradiance, upper_irradiance, fract(cascade_f));
}

#endif // DDGI_INC_SAMPLING || DDGI_INC_SAMPLING_IMAGE
