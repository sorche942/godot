// Include Brixelizer SDK FIRST to avoid type conflicts with FSR2
#include "thirdparty/amd-brixelizer/ffx_brixelizer.h"
#include "thirdparty/amd-brixelizer/ffx_brixelizergi.h"
#include "thirdparty/amd-brixelizer/ffx_brixelizer_raw.h"
#include "thirdparty/amd-brixelizer/shaders/ffx_brixelizer_resources.h"
#include "thirdparty/amd-brixelizer/shaders/ffx_brixelizergi_resources.h"
#include "thirdparty/amd-brixelizer/ffx_brixelizergi_private.h"

#include "brixelizer_gi.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/environment/brixelizer_bindings.gen.h"

// Blue noise sampler data (FidelityFX SDK).
#include "FidelityFX-SDK-v1.1.4/samples/thirdparty/samplercpp/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp.cpp"

// Include all generated shader headers
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_build_tree_aabb_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_clear_brick_storage_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_clear_build_counters_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_clear_job_counter_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_clear_ref_counters_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_coarse_culling_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_compact_references_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_compress_brick_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_emit_sdf_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_free_cascade_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_initialize_cascade_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_invalidate_job_areas_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_mark_cascade_uninitialized_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_reset_cascade_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_scan_jobs_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_scan_references_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_scroll_cascade_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_cascade_ops_voxelize_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_clear_brick_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_clear_counters_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_collect_clear_bricks_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_collect_dirty_bricks_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_eikonal_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_merge_bricks_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_merge_cascades_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_prepare_clear_bricks_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_prepare_eikonal_args_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_context_ops_prepare_merge_bricks_args_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_debug_draw_aabb_tree.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_debug_draw_instance_aabbs.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizer_debug_visualization_pass.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_blur_x.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_blur_y.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_clear_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_debug_visualization.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_downsample.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_emit_irradiance_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_emit_primary_ray_radiance.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_fill_screen_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_generate_disocclusion_mask.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_interpolate_screen_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_prepare_clear_cache.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_project_screen_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_propagate_sh.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_reproject_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_reproject_screen_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_spawn_screen_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_specular_pre_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_specular_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/ffx_brixelizergi_upsample.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/brixelizer_decode_normal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/brixelizer/brixelizer_decode_roughness.glsl.gen.h"

using namespace RendererRD;

#define BRIXELIZER_UBO_RING_BUFFER_SIZE 16
static constexpr uint32_t BRIXELIZER_MAX_VERTEX_BUFFERS = 64;
static constexpr uint32_t BRIXELIZER_VERTEX_BUFFER_BINDING = 0;
static constexpr uint32_t BRIXELIZER_SAMPLER_BINDING_WRAP_LINEAR = 1000;
static constexpr uint32_t BRIXELIZER_GI_SAMPLER_BINDING_CLAMP_LINEAR = 1000;
static constexpr uint32_t BRIXELIZER_GI_SAMPLER_BINDING_CLAMP_NEAREST = 1001;
static constexpr uint32_t BRIXELIZER_GI_SAMPLER_BINDING_WRAP_LINEAR = 1002;
static constexpr uint32_t BRIXELIZER_GI_SAMPLER_BINDING_WRAP_NEAREST = 1003;
static constexpr uint32_t BRIXELIZER_GI_SUPPORTED_PERMUTATIONS =
		BRIXELIZER_GI_SHADER_PERMUTATION_DEPTH_INVERTED |
		BRIXELIZER_GI_SHADER_PERMUTATION_DISABLE_SPECULAR |
		BRIXELIZER_GI_SHADER_PERMUTATION_DISABLE_DENOISER;
static constexpr uint32_t BRIXELIZER_GI_VARIANT_COUNT = 1u << 3;

struct BrixelizerDecodeNormalPushConstant {
	float inv_view[16];
};

// Pipeline info stored for each FFX pass
struct BrixelizerPipeline {
	RID shader_rid;
	RID pipeline_rid;
	RID static_uniform_set;
	uint32_t static_uniform_set_version = 0;
	uint32_t pass_id = 0;
	uint32_t shader_variant = 0;
	bool is_gi = false;
	bool needs_vertex_buffers = false;
};

static uint32_t get_gi_variant(uint32_t p_permutation_options) {
	return p_permutation_options & BRIXELIZER_GI_SUPPORTED_PERMUTATIONS;
}

// Implementation struct - holds all FFX types and Godot shaders
struct RendererRD::BrixelizerGIImpl {
	FfxBrixelizerContext brixelizer_context;
	FfxBrixelizerGIContext brixelizer_gi_context;
	bool initialized = false;

	// Brixelizer shaders (indexed by FfxBrixelizerPass)
	ShaderRD *brixelizer_shaders[FFX_BRIXELIZER_PASS_COUNT] = {};
	RID brixelizer_shader_versions[FFX_BRIXELIZER_PASS_COUNT];
	BrixelizerPipeline brixelizer_pipelines[FFX_BRIXELIZER_PASS_COUNT];

	// BrixelizerGI shaders (indexed by FfxBrixelizerGIPass)
	ShaderRD *gi_shaders[FFX_BRIXELIZER_GI_PASS_COUNT] = {};
	RID gi_shader_versions[FFX_BRIXELIZER_GI_PASS_COUNT];
	BrixelizerPipeline gi_pipelines[FFX_BRIXELIZER_GI_PASS_COUNT];

	// Shader instances (we own these)
	FfxBrixelizerContextOpsClearCountersPassShaderRD context_clear_counters;
	FfxBrixelizerContextOpsCollectClearBricksPassShaderRD context_collect_clear_bricks;
	FfxBrixelizerContextOpsPrepareClearBricksPassShaderRD context_prepare_clear_bricks;
	FfxBrixelizerContextOpsClearBrickPassShaderRD context_clear_brick;
	FfxBrixelizerContextOpsCollectDirtyBricksPassShaderRD context_collect_dirty_bricks;
	FfxBrixelizerContextOpsPrepareEikonalArgsPassShaderRD context_prepare_eikonal_args;
	FfxBrixelizerContextOpsEikonalPassShaderRD context_eikonal;
	FfxBrixelizerContextOpsMergeCascadesPassShaderRD context_merge_cascades;
	FfxBrixelizerContextOpsPrepareMergeBricksArgsPassShaderRD context_prepare_merge_bricks_args;
	FfxBrixelizerContextOpsMergeBricksPassShaderRD context_merge_bricks;
	FfxBrixelizerCascadeOpsClearBuildCountersPassShaderRD cascade_clear_build_counters;
	FfxBrixelizerCascadeOpsResetCascadePassShaderRD cascade_reset_cascade;
	FfxBrixelizerCascadeOpsScrollCascadePassShaderRD cascade_scroll_cascade;
	FfxBrixelizerCascadeOpsClearRefCountersPassShaderRD cascade_clear_ref_counters;
	FfxBrixelizerCascadeOpsClearJobCounterPassShaderRD cascade_clear_job_counter;
	FfxBrixelizerCascadeOpsInvalidateJobAreasPassShaderRD cascade_invalidate_job_areas;
	FfxBrixelizerCascadeOpsCoarseCullingPassShaderRD cascade_coarse_culling;
	FfxBrixelizerCascadeOpsScanJobsPassShaderRD cascade_scan_jobs;
	FfxBrixelizerCascadeOpsVoxelizePassShaderRD cascade_voxelize;
	FfxBrixelizerCascadeOpsScanReferencesPassShaderRD cascade_scan_references;
	FfxBrixelizerCascadeOpsCompactReferencesPassShaderRD cascade_compact_references;
	FfxBrixelizerCascadeOpsClearBrickStoragePassShaderRD cascade_clear_brick_storage;
	FfxBrixelizerCascadeOpsEmitSdfPassShaderRD cascade_emit_sdf;
	FfxBrixelizerCascadeOpsCompressBrickPassShaderRD cascade_compress_brick;
	FfxBrixelizerCascadeOpsInitializeCascadePassShaderRD cascade_initialize_cascade;
	FfxBrixelizerCascadeOpsMarkCascadeUninitializedPassShaderRD cascade_mark_uninitialized;
	FfxBrixelizerCascadeOpsBuildTreeAabbPassShaderRD cascade_build_tree_aabb;
	FfxBrixelizerCascadeOpsFreeCascadePassShaderRD cascade_free_cascade;
	FfxBrixelizerDebugVisualizationPassShaderRD debug_visualization;
	FfxBrixelizerDebugDrawInstanceAabbsShaderRD debug_instance_aabbs;
	FfxBrixelizerDebugDrawAabbTreeShaderRD debug_aabb_tree;

	// GI shaders
	FfxBrixelizergiBlurXShaderRD gi_blur_x;
	FfxBrixelizergiBlurYShaderRD gi_blur_y;
	FfxBrixelizergiClearCacheShaderRD gi_clear_cache;
	FfxBrixelizergiEmitIrradianceCacheShaderRD gi_emit_irradiance_cache;
	FfxBrixelizergiEmitPrimaryRayRadianceShaderRD gi_emit_primary_ray_radiance;
	FfxBrixelizergiFillScreenProbesShaderRD gi_fill_screen_probes;
	FfxBrixelizergiInterpolateScreenProbesShaderRD gi_interpolate_screen_probes;
	FfxBrixelizergiPrepareClearCacheShaderRD gi_prepare_clear_cache;
	FfxBrixelizergiProjectScreenProbesShaderRD gi_project_screen_probes;
	FfxBrixelizergiPropagateShShaderRD gi_propagate_sh;
	FfxBrixelizergiReprojectGiShaderRD gi_reproject_gi;
	FfxBrixelizergiReprojectScreenProbesShaderRD gi_reproject_screen_probes;
	FfxBrixelizergiSpawnScreenProbesShaderRD gi_spawn_screen_probes;
	FfxBrixelizergiSpecularPreTraceShaderRD gi_specular_pre_trace;
	FfxBrixelizergiSpecularTraceShaderRD gi_specular_trace;
	FfxBrixelizergiDebugVisualizationShaderRD gi_debug_visualization;
	FfxBrixelizergiGenerateDisocclusionMaskShaderRD gi_generate_disocclusion_mask;
	FfxBrixelizergiDownsampleShaderRD gi_downsample;
	FfxBrixelizergiUpsampleShaderRD gi_upsample;
	BrixelizerDecodeNormalShaderRD decode_normal;
	RID decode_normal_shader_version;
	RID decode_normal_pipeline;
	BrixelizerDecodeRoughnessShaderRD decode_roughness;
	RID decode_roughness_shader_version;
	RID decode_roughness_pipeline;

	// Resource management
	struct Resource {
		RID rid;
		FfxResourceDescription desc;
		bool is_external;
	};
	HashMap<uint32_t, Resource> resources;
	uint32_t next_resource_id = 1;

	// GPU job queue
	LocalVector<FfxGpuJobDescription> gpu_jobs;
	// Staging data for constant buffers to keep data alive until dispatch
	LocalVector<Vector<uint8_t>> constant_buffer_staging;

	// UBO ring buffer for constant buffers
	RID ubo_ring_buffer[BRIXELIZER_UBO_RING_BUFFER_SIZE];
	uint32_t ubo_ring_buffer_index = 0;

	struct MappedBuffer {
		uint8_t *data = nullptr;
		uint32_t size = 0;
	};

	// CPU-side staging for mapped buffers (upload/readback emulation).
	HashMap<uint32_t, MappedBuffer> mapped_buffers;

	// Samplers
	RID point_clamp_sampler;
	RID linear_clamp_sampler;
	RID point_wrap_sampler;
	RID linear_wrap_sampler;

	// GI output textures
	RID diffuse_gi_texture;
	RID specular_gi_texture;
	RID world_normal_texture;
	RID roughness_texture;
	Size2i output_size;
	Size2i world_normal_size;
	Size2i roughness_size;
	Size2i gi_display_size;

	// History textures
	RID history_depth_texture;
	RID history_normal_roughness_texture;
	RID history_lit_output_texture;
	Size2i history_size;
	bool history_valid = false;
	bool history_lit_valid = false;

	// Fallback textures for missing inputs
	RID fallback_env_map;
	RID fallback_noise;
	RID fallback_prev_lit_output;
	RID fallback_storage_texture;

	// Brixelizer external resources
	RID sdf_atlas;
	RID brick_aabbs;
	RID cascade_aabb_trees[FFX_BRIXELIZER_MAX_CASCADES] = {};
	RID cascade_brick_maps[FFX_BRIXELIZER_MAX_CASCADES] = {};
	uint32_t cascade_count = 0;

	// Backend interface for context recreation
	FfxInterface backend = {};

	// Previous frame data for temporal
	Transform3D prev_cam_transform;
	Projection prev_cam_projection;
	uint32_t frame_index = 0;

	// Track shader initialization separately from context initialization
	bool shaders_initialized = false;
	bool gi_initialized = false;

	// Static vertex buffer table for bindless access
	Vector<RID> static_vertex_buffers;
	uint32_t static_buffers_version = 1;
	uint32_t static_buffer_count = 0;

	// Mesh buffer registration tracking
	struct RegisteredBuffer {
		uint32_t ffx_index;  // Index returned by ffxBrixelizerRegisterBuffers
		uint32_t size;       // Buffer size in bytes
		RID storage_rid;     // Storage buffer used by Brixelizer
	};
	HashMap<RID, RegisteredBuffer> registered_vertex_buffers;
	HashMap<RID, RegisteredBuffer> registered_index_buffers;

	// Instance registration tracking (for static instances)
	// Key: combination of mesh RID + surface index + transform hash
	struct RegisteredInstance {
		FfxBrixelizerInstanceID instance_id;
		RID mesh_rid;
		uint32_t surface_index;
		Transform3D transform;
	};
	LocalVector<RegisteredInstance> registered_instances;

	// Dynamic instances (recreated each frame)
	LocalVector<FfxBrixelizerInstanceID> dynamic_instance_ids;

	uint32_t get_next_resource_id() {
		return next_resource_id++;
	}
};

// Forward declaration for the static backend pointer
static BrixelizerGIImpl *s_current_impl = nullptr;

static RD::TextureType ffx_resource_type_to_rd(FfxResourceType p_type) {
	switch (p_type) {
		case FFX_RESOURCE_TYPE_TEXTURE1D: return RD::TEXTURE_TYPE_1D;
		case FFX_RESOURCE_TYPE_TEXTURE2D: return RD::TEXTURE_TYPE_2D;
		case FFX_RESOURCE_TYPE_TEXTURE_CUBE: return RD::TEXTURE_TYPE_CUBE;
		case FFX_RESOURCE_TYPE_TEXTURE3D: return RD::TEXTURE_TYPE_3D;
		default: return RD::TEXTURE_TYPE_2D;
	}
}

static RD::DataFormat ffx_format_to_rd(FfxSurfaceFormat p_format) {
	switch (p_format) {
		case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		case FFX_SURFACE_FORMAT_R32G32_FLOAT: return RD::DATA_FORMAT_R32G32_SFLOAT;
		case FFX_SURFACE_FORMAT_R32_UINT: return RD::DATA_FORMAT_R32_UINT;
		case FFX_SURFACE_FORMAT_R32_FLOAT: return RD::DATA_FORMAT_R32_SFLOAT;
		case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return RD::DATA_FORMAT_R8G8B8A8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB: return RD::DATA_FORMAT_R8G8B8A8_SRGB;
		case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
		case FFX_SURFACE_FORMAT_R16G16_FLOAT: return RD::DATA_FORMAT_R16G16_SFLOAT;
		case FFX_SURFACE_FORMAT_R16_FLOAT: return RD::DATA_FORMAT_R16_SFLOAT;
		case FFX_SURFACE_FORMAT_R16_UINT: return RD::DATA_FORMAT_R16_UINT;
		case FFX_SURFACE_FORMAT_R16_UNORM: return RD::DATA_FORMAT_R16_UNORM;
		case FFX_SURFACE_FORMAT_R16_SNORM: return RD::DATA_FORMAT_R16_SNORM;
		case FFX_SURFACE_FORMAT_R8_UNORM: return RD::DATA_FORMAT_R8_UNORM;
		case FFX_SURFACE_FORMAT_R8_UINT: return RD::DATA_FORMAT_R8_UINT;
		case FFX_SURFACE_FORMAT_R8G8_UNORM: return RD::DATA_FORMAT_R8G8_UNORM;
		default: return RD::DATA_FORMAT_R8G8B8A8_UNORM;
	}
}

static FfxSurfaceFormat rd_format_to_ffx_surface_format(RD::DataFormat p_format) {
	switch (p_format) {
		case RD::DATA_FORMAT_R32G32B32A32_SFLOAT: return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
		case RD::DATA_FORMAT_R16G16B16A16_SFLOAT: return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		case RD::DATA_FORMAT_R32G32_SFLOAT: return FFX_SURFACE_FORMAT_R32G32_FLOAT;
		case RD::DATA_FORMAT_R32_UINT: return FFX_SURFACE_FORMAT_R32_UINT;
		case RD::DATA_FORMAT_R32_SFLOAT: return FFX_SURFACE_FORMAT_R32_FLOAT;
		case RD::DATA_FORMAT_R8G8B8A8_UNORM: return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case RD::DATA_FORMAT_R8G8B8A8_SRGB: return FFX_SURFACE_FORMAT_R8G8B8A8_SRGB;
		case RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;
		case RD::DATA_FORMAT_R16G16_SFLOAT: return FFX_SURFACE_FORMAT_R16G16_FLOAT;
		case RD::DATA_FORMAT_R16_SFLOAT: return FFX_SURFACE_FORMAT_R16_FLOAT;
		case RD::DATA_FORMAT_R16_UINT: return FFX_SURFACE_FORMAT_R16_UINT;
		case RD::DATA_FORMAT_R16_UNORM: return FFX_SURFACE_FORMAT_R16_UNORM;
		case RD::DATA_FORMAT_R16_SNORM: return FFX_SURFACE_FORMAT_R16_SNORM;
		case RD::DATA_FORMAT_R8_UNORM: return FFX_SURFACE_FORMAT_R8_UNORM;
		case RD::DATA_FORMAT_R8_UINT: return FFX_SURFACE_FORMAT_R8_UINT;
		case RD::DATA_FORMAT_R8G8_UNORM: return FFX_SURFACE_FORMAT_R8G8_UNORM;
		default: return FFX_SURFACE_FORMAT_UNKNOWN;
	}
}

// Backend callbacks
static FfxErrorCode create_backend_context_rd(FfxInterface *backendInterface, FfxEffect effect, FfxEffectBindlessConfig *bindlessConfig, FfxUInt32 *effectContextId) {
	*effectContextId = 0;
	return FFX_OK;
}

static FfxErrorCode destroy_backend_context_rd(FfxInterface *backendInterface, FfxUInt32 effectContextId) {
	return FFX_OK;
}

static FfxErrorCode get_device_capabilities_rd(FfxInterface *backendInterface, FfxDeviceCapabilities *outDeviceCapabilities) {
	outDeviceCapabilities->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_0;
	outDeviceCapabilities->waveLaneCountMin = 32;
	outDeviceCapabilities->waveLaneCountMax = 64;
	outDeviceCapabilities->fp16Supported = true;
	outDeviceCapabilities->raytracingSupported = false;
	outDeviceCapabilities->deviceCoherentMemorySupported = false;
	outDeviceCapabilities->dedicatedAllocationSupported = true;
	outDeviceCapabilities->bufferMarkerSupported = false;
	outDeviceCapabilities->extendedSynchronizationSupported = false;
	outDeviceCapabilities->shaderStorageBufferArrayNonUniformIndexing = true; // Required for Brixelizer
	return FFX_OK;
}

static FfxVersionNumber get_sdk_version_rd(FfxInterface *backendInterface) {
	return FFX_SDK_MAKE_VERSION(1, 1, 4);
}

static void copy_binding_name(wchar_t *dst, size_t dst_size, const char *src) {
	if (!dst || dst_size == 0) {
		return;
	}
	size_t i = 0;
	for (; i + 1 < dst_size && src && src[i] != '\0'; i++) {
		dst[i] = (wchar_t)src[i];
	}
	dst[i] = 0;
}

static void push_binding(FfxResourceBinding *bindings, uint32_t &count, uint32_t max_count, uint32_t binding, uint32_t array_index, const char *name) {
	if (count >= max_count) {
		return;
	}
	bindings[count].slotIndex = binding;
	bindings[count].arrayIndex = array_index;
	bindings[count].resourceIdentifier = 0;
	copy_binding_name(bindings[count].name, FFX_RESOURCE_NAME_SIZE, name);
	count++;
}

static void populate_pipeline_bindings(FfxPipelineState *out_pipeline, const BrixelizerPassBindings &bindings) {
	if (!out_pipeline) {
		return;
	}

	out_pipeline->srvTextureCount = 0;
	out_pipeline->uavTextureCount = 0;
	out_pipeline->srvBufferCount = 0;
	out_pipeline->uavBufferCount = 0;
	out_pipeline->constCount = 0;

	for (uint32_t i = 0; i < bindings.count; i++) {
		const BrixelizerBindingDesc &desc = bindings.bindings[i];
		if (desc.set != 0) {
			continue; // Only set 0 is handled here.
		}
		uint32_t array_size = MAX(desc.array_size, 1u);
		for (uint32_t array_index = 0; array_index < array_size; array_index++) {
			switch (desc.type) {
				case BRIXELIZER_BINDING_CBV:
					push_binding(out_pipeline->constantBufferBindings, out_pipeline->constCount, FFX_MAX_NUM_CONST_BUFFERS, desc.binding, array_index, desc.name);
					break;
				case BRIXELIZER_BINDING_SRV_TEXTURE:
					push_binding(out_pipeline->srvTextureBindings, out_pipeline->srvTextureCount, FFX_MAX_NUM_SRVS, desc.binding, array_index, desc.name);
					break;
				case BRIXELIZER_BINDING_UAV_TEXTURE:
					push_binding(out_pipeline->uavTextureBindings, out_pipeline->uavTextureCount, FFX_MAX_NUM_UAVS, desc.binding, array_index, desc.name);
					break;
				case BRIXELIZER_BINDING_SRV_BUFFER:
					push_binding(out_pipeline->srvBufferBindings, out_pipeline->srvBufferCount, FFX_MAX_NUM_SRVS, desc.binding, array_index, desc.name);
					break;
				case BRIXELIZER_BINDING_UAV_BUFFER:
					push_binding(out_pipeline->uavBufferBindings, out_pipeline->uavBufferCount, FFX_MAX_NUM_UAVS, desc.binding, array_index, desc.name);
					break;
				default:
					break;
			}
		}
	}
}

static FfxErrorCode create_resource_rd(FfxInterface *backendInterface, const FfxCreateResourceDescription *desc, FfxUInt32 effectContextId, FfxResourceInternal *outResource) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;

	RID rid;
	if (desc->resourceDescription.type == FFX_RESOURCE_TYPE_BUFFER) {
		uint32_t size = MAX(desc->resourceDescription.size, 16u);
		BitField<RD::StorageBufferUsage> usage;
		if (desc->resourceDescription.usage & FFX_RESOURCE_USAGE_INDIRECT) {
			usage.set_flag(RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		}
		rid = RD::get_singleton()->storage_buffer_create(size, {}, usage);
	} else {
		RD::TextureFormat tf;
		tf.width = MAX(desc->resourceDescription.width, 1u);
		tf.height = MAX(desc->resourceDescription.height, 1u);
		tf.depth = MAX(desc->resourceDescription.depth, 1u);
		tf.array_layers = (desc->resourceDescription.type == FFX_RESOURCE_TYPE_TEXTURE_CUBE) ? 6 : 1;
		if (desc->resourceDescription.type == FFX_RESOURCE_TYPE_TEXTURE_CUBE) {
			tf.depth = 1;
		}
		tf.mipmaps = MAX(desc->resourceDescription.mipCount, 1u);
		tf.texture_type = ffx_resource_type_to_rd(desc->resourceDescription.type);
		tf.format = ffx_format_to_rd(desc->resourceDescription.format);
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
						RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		rid = RD::get_singleton()->texture_create(tf, RD::TextureView());
	}

	if (!rid.is_valid()) {
		return FFX_ERROR_BACKEND_API_ERROR;
	}

	uint32_t id = impl->get_next_resource_id();
	impl->resources[id] = { rid, desc->resourceDescription, false };
	outResource->internalIndex = id;
	return FFX_OK;
}

static FfxErrorCode register_resource_rd(FfxInterface *backendInterface, const FfxResource *inResource, FfxUInt32 effectContextId, FfxResourceInternal *outResource) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;

	if (inResource->resource == nullptr) {
		outResource->internalIndex = 0;
		return FFX_OK;
	}

	RID rid = RID::from_uint64((uint64_t)(uintptr_t)inResource->resource);
	uint32_t id = impl->get_next_resource_id();
	impl->resources[id] = { rid, inResource->description, true };
	outResource->internalIndex = id;
	return FFX_OK;
}

static FfxErrorCode unregister_resources_rd(FfxInterface *backendInterface, FfxCommandList commandList, FfxUInt32 effectContextId) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	// Remove external resources
	LocalVector<uint32_t> to_remove;
	for (const KeyValue<uint32_t, BrixelizerGIImpl::Resource> &kv : impl->resources) {
		if (kv.value.is_external) {
			to_remove.push_back(kv.key);
		}
	}
	for (uint32_t id : to_remove) {
		impl->resources.erase(id);
	}
	return FFX_OK;
}

static FfxResourceDescription get_resource_description_rd(FfxInterface *backendInterface, FfxResourceInternal resource) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	if (impl->resources.has(resource.internalIndex)) {
		return impl->resources[resource.internalIndex].desc;
	}
	return {};
}

static FfxErrorCode get_effect_gpu_memory_usage_rd(FfxInterface *backendInterface, FfxUInt32 effectContextId, FfxEffectMemoryUsage *outVramUsage) {
	if (outVramUsage) {
		outVramUsage->totalUsageInBytes = 0;
		outVramUsage->aliasableUsageInBytes = 0;
	}
	return FFX_OK;
}

static FfxResource get_resource_rd(FfxInterface *backendInterface, FfxResourceInternal resource) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	FfxResource res = {};
	if (impl->resources.has(resource.internalIndex)) {
		res.resource = (void *)(uintptr_t)impl->resources[resource.internalIndex].rid.get_id();
		res.description = impl->resources[resource.internalIndex].desc;
		res.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	}
	return res;
}

static FfxErrorCode register_static_resource_rd(FfxInterface *backendInterface, const FfxStaticResourceDescription *desc, FfxUInt32 effectContextId) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	if (!desc || !desc->resource || desc->resource->resource == nullptr) {
		return FFX_OK;
	}

	if (desc->descriptorType != FFX_DESCRIPTOR_BUFFER_SRV) {
		return FFX_OK;
	}

	if (desc->descriptorIndex >= BRIXELIZER_MAX_VERTEX_BUFFERS) {
		return FFX_ERROR_OUT_OF_RANGE;
	}

	RID rid = RID::from_uint64((uint64_t)(uintptr_t)desc->resource->resource);
	if (!rid.is_valid()) {
		return FFX_OK;
	}

	if (impl->static_vertex_buffers.is_empty()) {
		impl->static_vertex_buffers.resize(BRIXELIZER_MAX_VERTEX_BUFFERS);
	}

	impl->static_vertex_buffers.write[desc->descriptorIndex] = rid;
	impl->static_buffer_count = MAX(impl->static_buffer_count, desc->descriptorIndex + 1);
	impl->static_buffers_version++;

	return FFX_OK;
}

static FfxErrorCode map_resource_rd(FfxInterface *backendInterface, FfxResourceInternal resource, void **outData) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	if (!outData || !impl->resources.has(resource.internalIndex)) {
		return FFX_ERROR_INVALID_POINTER;
	}

	BrixelizerGIImpl::Resource &res = impl->resources[resource.internalIndex];
	if (res.desc.type != FFX_RESOURCE_TYPE_BUFFER) {
		return FFX_ERROR_BACKEND_API_ERROR;
	}

	BrixelizerGIImpl::MappedBuffer *mapped = impl->mapped_buffers.getptr(resource.internalIndex);
	if (!mapped) {
		BrixelizerGIImpl::MappedBuffer new_mapping;
		new_mapping.size = MAX(res.desc.size, 1u);
		new_mapping.data = (uint8_t *)memalloc(new_mapping.size);
		ERR_FAIL_NULL_V(new_mapping.data, FFX_ERROR_OUT_OF_MEMORY);
		memset(new_mapping.data, 0, new_mapping.size);
		impl->mapped_buffers[resource.internalIndex] = new_mapping;
		mapped = impl->mapped_buffers.getptr(resource.internalIndex);
	}

	*outData = mapped->data;
	return FFX_OK;
}

static FfxErrorCode unmap_resource_rd(FfxInterface *backendInterface, FfxResourceInternal resource) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	if (!impl->mapped_buffers.has(resource.internalIndex)) {
		return FFX_OK;
	}

	BrixelizerGIImpl::MappedBuffer mapping = impl->mapped_buffers[resource.internalIndex];
	if (mapping.data) {
		memfree(mapping.data);
	}
	impl->mapped_buffers.erase(resource.internalIndex);
	return FFX_OK;
}

static FfxErrorCode stage_constant_buffer_data_rd(FfxInterface *backendInterface, void *data, FfxUInt32 size, FfxConstantBuffer *constantBuffer) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	if (!constantBuffer) {
		return FFX_ERROR_INVALID_POINTER;
	}

	constantBuffer->num32BitEntries = size / sizeof(uint32_t);
	if (size == 0 || data == nullptr) {
		constantBuffer->data = nullptr;
		return FFX_OK;
	}

	impl->constant_buffer_staging.push_back(Vector<uint8_t>());
	Vector<uint8_t> &staging = impl->constant_buffer_staging[impl->constant_buffer_staging.size() - 1];
	staging.resize(size);
	memcpy(staging.ptrw(), data, size);
	constantBuffer->data = (uint32_t *)staging.ptrw();

	return FFX_OK;
}

static FfxErrorCode destroy_resource_rd(FfxInterface *backendInterface, FfxResourceInternal resource, FfxUInt32 effectContextId) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	if (impl->resources.has(resource.internalIndex)) {
		BrixelizerGIImpl::Resource &res = impl->resources[resource.internalIndex];
		if (!res.is_external && res.rid.is_valid()) {
			RD::get_singleton()->free(res.rid);
		}
		impl->resources.erase(resource.internalIndex);
	}
	if (impl->mapped_buffers.has(resource.internalIndex)) {
		BrixelizerGIImpl::MappedBuffer mapping = impl->mapped_buffers[resource.internalIndex];
		if (mapping.data) {
			memfree(mapping.data);
		}
		impl->mapped_buffers.erase(resource.internalIndex);
	}
	return FFX_OK;
}

static FfxErrorCode create_pipeline_rd(FfxInterface *backendInterface, FfxEffect effect, FfxPass pass, uint32_t permutationOptions, const FfxPipelineDescription *pipelineDesc, FfxUInt32 effectContextId, FfxPipelineState *outPipeline) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;

	BrixelizerPipeline *pipeline = nullptr;

	if (effect == FFX_EFFECT_BRIXELIZER) {
		if (pass < FFX_BRIXELIZER_PASS_COUNT) {
			pipeline = &impl->brixelizer_pipelines[pass];
			pipeline->is_gi = false;
			pipeline->pass_id = pass;
			pipeline->needs_vertex_buffers = (pass == FFX_BRIXELIZER_PASS_CASCADE_VOXELIZE);
			if (pipeline->pipeline_rid.is_null() && impl->brixelizer_shader_versions[pass].is_valid()) {
				pipeline->shader_rid = impl->brixelizer_shaders[pass]->version_get_shader(impl->brixelizer_shader_versions[pass], 0);
				if (pipeline->shader_rid.is_valid()) {
					pipeline->pipeline_rid = RD::get_singleton()->compute_pipeline_create(pipeline->shader_rid);
				}
			}
		}
	} else if (effect == FFX_EFFECT_BRIXELIZER_GI) {
		if (pass < FFX_BRIXELIZER_GI_PASS_COUNT) {
			pipeline = &impl->gi_pipelines[pass];
			pipeline->is_gi = true;
			pipeline->pass_id = pass;
			pipeline->needs_vertex_buffers = false;
			uint32_t gi_variant = get_gi_variant(permutationOptions);
			if (pipeline->pipeline_rid.is_valid() && pipeline->shader_variant != gi_variant) {
				if (pipeline->static_uniform_set.is_valid()) {
					RD::get_singleton()->free(pipeline->static_uniform_set);
				}
				RD::get_singleton()->free(pipeline->pipeline_rid);
				pipeline->pipeline_rid = RID();
				pipeline->shader_rid = RID();
				pipeline->static_uniform_set = RID();
				pipeline->static_uniform_set_version = 0;
			}
			pipeline->shader_variant = gi_variant;
			if (pipeline->pipeline_rid.is_null() && impl->gi_shader_versions[pass].is_valid()) {
				pipeline->shader_rid = impl->gi_shaders[pass]->version_get_shader(impl->gi_shader_versions[pass], gi_variant);
				if (pipeline->shader_rid.is_valid()) {
					pipeline->pipeline_rid = RD::get_singleton()->compute_pipeline_create(pipeline->shader_rid);
				}
			}
		}
	}

	if (pipeline && pipeline->pipeline_rid.is_valid()) {
		if (effect == FFX_EFFECT_BRIXELIZER && pass < FFX_BRIXELIZER_PASS_COUNT) {
			populate_pipeline_bindings(outPipeline, brixelizer_pass_bindings[pass]);
		} else if (effect == FFX_EFFECT_BRIXELIZER_GI && pass < FFX_BRIXELIZER_GI_PASS_COUNT) {
			populate_pipeline_bindings(outPipeline, brixelizer_gi_pass_bindings[pass]);
		}

		outPipeline->passId = pass;
		// Store pointers in the FFX pipeline state for retrieval during execution
		outPipeline->pipeline = (FfxPipeline)(uintptr_t)pipeline;
		outPipeline->rootSignature = (FfxRootSignature)(uintptr_t)&pipeline->shader_rid;
		return FFX_OK;
	}

	return FFX_ERROR_BACKEND_API_ERROR;
}

static FfxErrorCode destroy_pipeline_rd(FfxInterface *backendInterface, FfxPipelineState *pipeline, FfxUInt32 effectContextId) {
	// Pipelines are managed by impl, don't destroy here
	return FFX_OK;
}

static FfxErrorCode schedule_gpu_job_rd(FfxInterface *backendInterface, const FfxGpuJobDescription *job) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;
	impl->gpu_jobs.push_back(*job);
	return FFX_OK;
}

static FfxErrorCode execute_clear_job(BrixelizerGIImpl *impl, const FfxClearFloatJobDescription &job) {
	if (!impl->resources.has(job.target.internalIndex)) {
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	BrixelizerGIImpl::Resource &res = impl->resources[job.target.internalIndex];
	if (res.desc.type == FFX_RESOURCE_TYPE_BUFFER) {
		return FFX_OK; // Can't clear buffers this way
	}
	Color color(job.color[0], job.color[1], job.color[2], job.color[3]);
	RD::get_singleton()->texture_clear(res.rid, color, 0, res.desc.mipCount, 0, 1);
	return FFX_OK;
}

static FfxErrorCode execute_copy_job(BrixelizerGIImpl *impl, const FfxCopyJobDescription &job) {
	if (!impl->resources.has(job.src.internalIndex) || !impl->resources.has(job.dst.internalIndex)) {
		return FFX_ERROR_INVALID_ARGUMENT;
	}
	BrixelizerGIImpl::Resource &src = impl->resources[job.src.internalIndex];
	BrixelizerGIImpl::Resource &dst = impl->resources[job.dst.internalIndex];

	if (src.desc.type == FFX_RESOURCE_TYPE_BUFFER && dst.desc.type == FFX_RESOURCE_TYPE_BUFFER) {
		uint32_t size = job.size > 0 ? job.size : src.desc.size;
		RD::get_singleton()->buffer_copy(src.rid, dst.rid, job.srcOffset, job.dstOffset, size);
	} else if (src.desc.type != FFX_RESOURCE_TYPE_BUFFER && dst.desc.type != FFX_RESOURCE_TYPE_BUFFER) {
		for (uint32_t mip = 0; mip < src.desc.mipCount; mip++) {
			RD::get_singleton()->texture_copy(src.rid, dst.rid, Vector3(0, 0, 0), Vector3(0, 0, 0),
				Vector3(src.desc.width >> mip, src.desc.height >> mip, src.desc.depth), mip, mip, 0, 0);
		}
	}
	return FFX_OK;
}

static FfxErrorCode execute_compute_job(BrixelizerGIImpl *impl, const FfxComputeJobDescription &job) {
	BrixelizerPipeline *pipeline = (BrixelizerPipeline *)(uintptr_t)job.pipeline.pipeline;
	if (!pipeline || pipeline->pipeline_rid.is_null()) {
		return FFX_ERROR_INVALID_ARGUMENT;
	}

	// Build uniforms for the compute job
	Vector<RD::Uniform> uniforms;

	// Add samplers (bindings are fixed in the shader headers)
	if (pipeline->is_gi) {
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = BRIXELIZER_GI_SAMPLER_BINDING_CLAMP_LINEAR;
			u.append_id(impl->linear_clamp_sampler);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = BRIXELIZER_GI_SAMPLER_BINDING_CLAMP_NEAREST;
			u.append_id(impl->point_clamp_sampler);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = BRIXELIZER_GI_SAMPLER_BINDING_WRAP_LINEAR;
			u.append_id(impl->linear_wrap_sampler);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = BRIXELIZER_GI_SAMPLER_BINDING_WRAP_NEAREST;
			u.append_id(impl->point_wrap_sampler);
			uniforms.push_back(u);
		}
	} else {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.binding = BRIXELIZER_SAMPLER_BINDING_WRAP_LINEAR;
		u.append_id(impl->linear_wrap_sampler);
		uniforms.push_back(u);
	}

	struct UniformAccumulator {
		RD::UniformType type = RD::UNIFORM_TYPE_MAX;
		uint32_t binding = 0;
		Vector<RID> ids;
	};

	HashMap<uint64_t, UniformAccumulator> uniform_map;

	auto accumulate_uniform = [&](RD::UniformType type, uint32_t binding, uint32_t array_index, RID rid) {
		uint64_t key = (uint64_t(type) << 32) | binding;
		UniformAccumulator *acc = uniform_map.getptr(key);
		if (!acc) {
			UniformAccumulator fresh;
			fresh.type = type;
			fresh.binding = binding;
			uniform_map[key] = fresh;
			acc = uniform_map.getptr(key);
		}
		if (!acc) {
			return;
		}
		int needed_size = int(array_index) + 1;
		if (acc->ids.size() < needed_size) {
			acc->ids.resize(needed_size);
		}
		acc->ids.ptrw()[array_index] = rid;
	};

	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RID fallback_buffer = mesh_storage ? mesh_storage->get_default_rd_storage_buffer() : RID();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	RID fallback_srv_texture = texture_storage ? texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK) : RID();
	if (fallback_srv_texture.is_null()) {
		if (impl->fallback_noise.is_valid()) {
			fallback_srv_texture = impl->fallback_noise;
		} else if (impl->fallback_prev_lit_output.is_valid()) {
			fallback_srv_texture = impl->fallback_prev_lit_output;
		}
	}
	RID fallback_uav_texture = impl->fallback_storage_texture.is_valid() ? impl->fallback_storage_texture : RID();
	if (fallback_uav_texture.is_null()) {
		if (impl->diffuse_gi_texture.is_valid()) {
			fallback_uav_texture = impl->diffuse_gi_texture;
		} else if (impl->specular_gi_texture.is_valid()) {
			fallback_uav_texture = impl->specular_gi_texture;
		} else {
			fallback_uav_texture = fallback_srv_texture;
		}
	}

	// Add SRV textures
	for (uint32_t i = 0; i < job.pipeline.srvTextureCount; i++) {
		RID texture_rid;
		if (impl->resources.has(job.srvTextures[i].resource.internalIndex)) {
			texture_rid = impl->resources[job.srvTextures[i].resource.internalIndex].rid;
		}
		if (texture_rid.is_null()) {
			texture_rid = fallback_srv_texture;
		}
		accumulate_uniform(RD::UNIFORM_TYPE_TEXTURE, job.pipeline.srvTextureBindings[i].slotIndex, job.pipeline.srvTextureBindings[i].arrayIndex, texture_rid);
	}

	// Add SRV buffers
	for (uint32_t i = 0; i < job.pipeline.srvBufferCount; i++) {
		RID buffer_rid;
		if (impl->resources.has(job.srvBuffers[i].resource.internalIndex)) {
			buffer_rid = impl->resources[job.srvBuffers[i].resource.internalIndex].rid;
		}
		if (buffer_rid.is_null()) {
			buffer_rid = fallback_buffer;
		}
		accumulate_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, job.pipeline.srvBufferBindings[i].slotIndex, job.pipeline.srvBufferBindings[i].arrayIndex, buffer_rid);
	}

	// Add UAV textures
	for (uint32_t i = 0; i < job.pipeline.uavTextureCount; i++) {
		RID image_rid;
		if (impl->resources.has(job.uavTextures[i].resource.internalIndex)) {
			image_rid = impl->resources[job.uavTextures[i].resource.internalIndex].rid;
		}
		if (image_rid.is_null()) {
			image_rid = fallback_uav_texture;
		}
		accumulate_uniform(RD::UNIFORM_TYPE_IMAGE, job.pipeline.uavTextureBindings[i].slotIndex, job.pipeline.uavTextureBindings[i].arrayIndex, image_rid);
	}

	// Add UAV buffers
	for (uint32_t i = 0; i < job.pipeline.uavBufferCount; i++) {
		RID buffer_rid;
		if (impl->resources.has(job.uavBuffers[i].resource.internalIndex)) {
			buffer_rid = impl->resources[job.uavBuffers[i].resource.internalIndex].rid;
		}
		if (buffer_rid.is_null()) {
			buffer_rid = fallback_buffer;
		}
		accumulate_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, job.pipeline.uavBufferBindings[i].slotIndex, job.pipeline.uavBufferBindings[i].arrayIndex, buffer_rid);
	}

	// Add constant buffers
	for (uint32_t i = 0; i < job.pipeline.constCount; i++) {
		RID ubo_rid = impl->ubo_ring_buffer[impl->ubo_ring_buffer_index];
		impl->ubo_ring_buffer_index = (impl->ubo_ring_buffer_index + 1) % BRIXELIZER_UBO_RING_BUFFER_SIZE;

		uint32_t cb_size = job.cbs[i].num32BitEntries * sizeof(uint32_t);
		if (cb_size > 0) {
			RD::get_singleton()->buffer_update(ubo_rid, 0, cb_size, job.cbs[i].data);
		}

		accumulate_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, job.pipeline.constantBufferBindings[i].slotIndex, job.pipeline.constantBufferBindings[i].arrayIndex, ubo_rid);
	}

	for (const KeyValue<uint64_t, UniformAccumulator> &kv : uniform_map) {
		const UniformAccumulator &acc = kv.value;
		if (acc.ids.is_empty()) {
			continue;
		}
		RD::Uniform u;
		u.uniform_type = acc.type;
		u.binding = acc.binding;
		for (int i = 0; i < acc.ids.size(); i++) {
			u.append_id(acc.ids[i]);
		}
		uniforms.push_back(u);
	}

	if (uniforms.is_empty()) {
		// No uniforms - can't create uniform set, but maybe this pass doesn't need any
		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline->pipeline_rid);
		if (job.cmdArgument.internalIndex != 0 && impl->resources.has(job.cmdArgument.internalIndex)) {
			RID indirect_buffer = impl->resources[job.cmdArgument.internalIndex].rid;
			RD::get_singleton()->compute_list_dispatch_indirect(compute_list, indirect_buffer, job.cmdArgumentOffset);
		} else {
			RD::get_singleton()->compute_list_dispatch(compute_list, job.dimensions[0], job.dimensions[1], job.dimensions[2]);
		}
		RD::get_singleton()->compute_list_end();
		return FFX_OK;
	}

	// Create uniform set
	RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, pipeline->shader_rid, 0);
	if (!uniform_set.is_valid()) {
		ERR_PRINT("Failed to create uniform set for Brixelizer compute job");
		return FFX_ERROR_BACKEND_API_ERROR;
	}

	RID static_uniform_set;
	if (pipeline->needs_vertex_buffers) {
		MeshStorage *mesh_storage = MeshStorage::get_singleton();
		if (!mesh_storage) {
			RD::get_singleton()->free(uniform_set);
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		if (pipeline->static_uniform_set.is_valid() && pipeline->static_uniform_set_version != impl->static_buffers_version) {
			RD::get_singleton()->free(pipeline->static_uniform_set);
			pipeline->static_uniform_set = RID();
		}

		if (pipeline->static_uniform_set.is_null()) {
			if (impl->static_vertex_buffers.is_empty()) {
				impl->static_vertex_buffers.resize(BRIXELIZER_MAX_VERTEX_BUFFERS);
			}

			Vector<RD::Uniform> static_uniforms;
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = BRIXELIZER_VERTEX_BUFFER_BINDING;

			RID fallback_buffer = mesh_storage->get_default_rd_storage_buffer();
			for (uint32_t i = 0; i < BRIXELIZER_MAX_VERTEX_BUFFERS; i++) {
				RID buffer_rid = impl->static_vertex_buffers[i];
				u.append_id(buffer_rid.is_valid() ? buffer_rid : fallback_buffer);
			}

			static_uniforms.push_back(u);
			pipeline->static_uniform_set = RD::get_singleton()->uniform_set_create(static_uniforms, pipeline->shader_rid, 1);
			pipeline->static_uniform_set_version = impl->static_buffers_version;
		}

		static_uniform_set = pipeline->static_uniform_set;
		if (!static_uniform_set.is_valid()) {
			RD::get_singleton()->free(uniform_set);
			ERR_PRINT("Failed to create static uniform set for Brixelizer vertex buffers");
			return FFX_ERROR_BACKEND_API_ERROR;
		}
	}

	// Execute compute
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline->pipeline_rid);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	if (pipeline->needs_vertex_buffers) {
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, static_uniform_set, 1);
	}

	if (job.cmdArgument.internalIndex != 0 && impl->resources.has(job.cmdArgument.internalIndex)) {
		RID indirect_buffer = impl->resources[job.cmdArgument.internalIndex].rid;
		RD::get_singleton()->compute_list_dispatch_indirect(compute_list, indirect_buffer, job.cmdArgumentOffset);
	} else {
		RD::get_singleton()->compute_list_dispatch(compute_list, job.dimensions[0], job.dimensions[1], job.dimensions[2]);
	}

	RD::get_singleton()->compute_list_end();

	// Free uniform set (it's a one-shot)
	RD::get_singleton()->free(uniform_set);

	return FFX_OK;
}

static FfxErrorCode execute_gpu_jobs_rd(FfxInterface *backendInterface, FfxCommandList commandList, FfxUInt32 effectContextId) {
	BrixelizerGIImpl *impl = (BrixelizerGIImpl *)backendInterface->scratchBuffer;

	for (const KeyValue<uint32_t, BrixelizerGIImpl::MappedBuffer> &kv : impl->mapped_buffers) {
		if (!impl->resources.has(kv.key)) {
			continue;
		}
		BrixelizerGIImpl::Resource &res = impl->resources[kv.key];
		const BrixelizerGIImpl::MappedBuffer &mapping = kv.value;
		if (res.desc.type != FFX_RESOURCE_TYPE_BUFFER || !res.rid.is_valid() || !mapping.data || mapping.size == 0) {
			continue;
		}
		RD::get_singleton()->buffer_update(res.rid, 0, mapping.size, mapping.data);
	}

	FfxErrorCode result = FFX_OK;
	for (const FfxGpuJobDescription &job : impl->gpu_jobs) {
		switch (job.jobType) {
			case FFX_GPU_JOB_CLEAR_FLOAT:
				result = execute_clear_job(impl, job.clearJobDescriptor);
				break;
			case FFX_GPU_JOB_COPY:
				result = execute_copy_job(impl, job.copyJobDescriptor);
				break;
			case FFX_GPU_JOB_COMPUTE:
				result = execute_compute_job(impl, job.computeJobDescriptor);
				break;
			default:
				break;
		}
		if (result != FFX_OK) {
			break;
		}
	}

	impl->gpu_jobs.clear();
	impl->constant_buffer_staging.clear();
	return result;
}

static bool ensure_gi_context(BrixelizerGIImpl *impl, const Size2i &p_size) {
	if (impl->gi_initialized && impl->gi_display_size == p_size) {
		return true;
	}

	if (impl->gi_initialized) {
		ffxBrixelizerGIContextDestroy(&impl->brixelizer_gi_context);
		impl->gi_initialized = false;
	}

	FfxBrixelizerGIContextDescription gi_desc = {};
	gi_desc.flags = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED;
	gi_desc.backendInterface = impl->backend;
	gi_desc.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_NATIVE;
	gi_desc.displaySize = { (uint32_t)p_size.x, (uint32_t)p_size.y };

	FfxErrorCode result = ffxBrixelizerGIContextCreate(&impl->brixelizer_gi_context, &gi_desc);
	if (result != FFX_OK) {
		ERR_PRINT("Failed to create BrixelizerGI context: " + itos(result));
		return false;
	}

	impl->gi_display_size = p_size;
	impl->gi_initialized = true;
	return true;
}

static Color get_environment_fallback_color(RID p_env) {
	RendererSceneRenderRD *render_scene = RendererSceneRenderRD::get_singleton();
	if (!render_scene) {
		return Color(0, 0, 0, 1);
	}

	if (p_env.is_null()) {
		return RSG::texture_storage->get_default_clear_color().srgb_to_linear();
	}

	const RS::EnvironmentBG bg_mode = render_scene->environment_get_background(p_env);
	Color bg_color = Color(0, 0, 0, 1);
	if (bg_mode == RS::ENV_BG_CLEAR_COLOR) {
		bg_color = RSG::texture_storage->get_default_clear_color();
	} else if (bg_mode == RS::ENV_BG_COLOR) {
		bg_color = render_scene->environment_get_bg_color(p_env);
	}

	bg_color = bg_color.srgb_to_linear();
	const float bg_energy = render_scene->environment_get_bg_energy_multiplier(p_env);
	bg_color.r *= bg_energy;
	bg_color.g *= bg_energy;
	bg_color.b *= bg_energy;

	const RS::EnvironmentAmbientSource ambient_source = render_scene->environment_get_ambient_source(p_env);
	bool use_ambient_light = ambient_source == RS::ENV_AMBIENT_SOURCE_COLOR ||
			ambient_source == RS::ENV_AMBIENT_SOURCE_SKY ||
			ambient_source == RS::ENV_AMBIENT_SOURCE_BG;

	if (use_ambient_light) {
		Color ambient_color = render_scene->environment_get_ambient_light(p_env);
		ambient_color = ambient_color.srgb_to_linear();
		const float ambient_energy = render_scene->environment_get_ambient_light_energy(p_env);
		ambient_color.r *= ambient_energy;
		ambient_color.g *= ambient_energy;
		ambient_color.b *= ambient_energy;

		if (bg_mode == RS::ENV_BG_SKY) {
			bg_color = ambient_color;
		} else {
			const float ambient_sky_mix = render_scene->environment_get_ambient_sky_contribution(p_env);
			bg_color = ambient_color.lerp(bg_color, ambient_sky_mix);
		}
	}

	return bg_color;
}

void BrixelizerGI::_init_shaders() {
	if (impl->shaders_initialized) {
		return;
	}

	Vector<String> brixelizer_variants;
	brixelizer_variants.push_back("");

	Vector<String> gi_variants;
	for (uint32_t variant = 0; variant < BRIXELIZER_GI_VARIANT_COUNT; variant++) {
		String defines;
		if (variant & BRIXELIZER_GI_SHADER_PERMUTATION_DEPTH_INVERTED) {
			defines += "\n#define FFX_BRIXELIZER_GI_OPTION_DEPTH_INVERTED 1\n";
		}
		if (variant & BRIXELIZER_GI_SHADER_PERMUTATION_DISABLE_SPECULAR) {
			defines += "\n#define FFX_BRIXELIZER_GI_OPTION_DISABLE_SPECULAR 1\n";
		}
		if (variant & BRIXELIZER_GI_SHADER_PERMUTATION_DISABLE_DENOISER) {
			defines += "\n#define FFX_BRIXELIZER_GI_OPTION_DISABLE_DENOISER 1\n";
		}
		gi_variants.push_back(defines);
	}

	// Map Brixelizer passes to shader instances
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_CLEAR_COUNTERS] = &impl->context_clear_counters;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_COLLECT_CLEAR_BRICKS] = &impl->context_collect_clear_bricks;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_PREPARE_CLEAR_BRICKS] = &impl->context_prepare_clear_bricks;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_CLEAR_BRICK] = &impl->context_clear_brick;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_COLLECT_DIRTY_BRICKS] = &impl->context_collect_dirty_bricks;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_PREPARE_EIKONAL_ARGS] = &impl->context_prepare_eikonal_args;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_EIKONAL] = &impl->context_eikonal;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_MERGE_CASCADES] = &impl->context_merge_cascades;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_PREPARE_MERGE_BRICKS_ARGS] = &impl->context_prepare_merge_bricks_args;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CONTEXT_MERGE_BRICKS] = &impl->context_merge_bricks;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_CLEAR_BUILD_COUNTERS] = &impl->cascade_clear_build_counters;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_RESET_CASCADE] = &impl->cascade_reset_cascade;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_SCROLL_CASCADE] = &impl->cascade_scroll_cascade;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_CLEAR_REF_COUNTERS] = &impl->cascade_clear_ref_counters;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_CLEAR_JOB_COUNTER] = &impl->cascade_clear_job_counter;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_INVALIDATE_JOB_AREAS] = &impl->cascade_invalidate_job_areas;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_COARSE_CULLING] = &impl->cascade_coarse_culling;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_SCAN_JOBS] = &impl->cascade_scan_jobs;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_VOXELIZE] = &impl->cascade_voxelize;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_SCAN_REFERENCES] = &impl->cascade_scan_references;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_COMPACT_REFERENCES] = &impl->cascade_compact_references;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_CLEAR_BRICK_STORAGE] = &impl->cascade_clear_brick_storage;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_EMIT_SDF] = &impl->cascade_emit_sdf;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_COMPRESS_BRICK] = &impl->cascade_compress_brick;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_INITIALIZE_CASCADE] = &impl->cascade_initialize_cascade;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_MARK_UNINITIALIZED] = &impl->cascade_mark_uninitialized;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_BUILD_TREE_AABB] = &impl->cascade_build_tree_aabb;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_CASCADE_FREE_CASCADE] = &impl->cascade_free_cascade;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_DEBUG_VISUALIZATION] = &impl->debug_visualization;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_DEBUG_INSTANCE_AABBS] = &impl->debug_instance_aabbs;
	impl->brixelizer_shaders[FFX_BRIXELIZER_PASS_DEBUG_AABB_TREE] = &impl->debug_aabb_tree;

	// Map GI passes to shader instances
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_BLUR_X] = &impl->gi_blur_x;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_BLUR_Y] = &impl->gi_blur_y;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_CLEAR_CACHE] = &impl->gi_clear_cache;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_EMIT_IRRADIANCE_CACHE] = &impl->gi_emit_irradiance_cache;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_EMIT_PRIMARY_RAY_RADIANCE] = &impl->gi_emit_primary_ray_radiance;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_FILL_SCREEN_PROBES] = &impl->gi_fill_screen_probes;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_INTERPOLATE_SCREEN_PROBES] = &impl->gi_interpolate_screen_probes;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_PREPARE_CLEAR_CACHE] = &impl->gi_prepare_clear_cache;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_PROJECT_SCREEN_PROBES] = &impl->gi_project_screen_probes;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_PROPAGATE_SH] = &impl->gi_propagate_sh;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_REPROJECT_GI] = &impl->gi_reproject_gi;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_REPROJECT_SCREEN_PROBES] = &impl->gi_reproject_screen_probes;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_SPAWN_SCREEN_PROBES] = &impl->gi_spawn_screen_probes;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_SPECULAR_PRE_TRACE] = &impl->gi_specular_pre_trace;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_SPECULAR_TRACE] = &impl->gi_specular_trace;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_DEBUG_VISUALIZATION] = &impl->gi_debug_visualization;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_GENERATE_DISOCCLUSION_MASK] = &impl->gi_generate_disocclusion_mask;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_DOWNSAMPLE] = &impl->gi_downsample;
	impl->gi_shaders[FFX_BRIXELIZER_GI_PASS_UPSAMPLE] = &impl->gi_upsample;

	// Initialize all Brixelizer shaders
	for (int i = 0; i < FFX_BRIXELIZER_PASS_COUNT; i++) {
		if (impl->brixelizer_shaders[i]) {
			impl->brixelizer_shaders[i]->initialize(brixelizer_variants);
			impl->brixelizer_shader_versions[i] = impl->brixelizer_shaders[i]->version_create();
		}
	}

	// Initialize all GI shaders
	for (int i = 0; i < FFX_BRIXELIZER_GI_PASS_COUNT; i++) {
		if (impl->gi_shaders[i]) {
			impl->gi_shaders[i]->initialize(gi_variants);
			impl->gi_shader_versions[i] = impl->gi_shaders[i]->version_create();
		}
	}

	{
		Vector<String> decode_variants;
		decode_variants.push_back("");
		impl->decode_roughness.initialize(decode_variants);
		impl->decode_roughness_shader_version = impl->decode_roughness.version_create();
		RID decode_shader = impl->decode_roughness.version_get_shader(impl->decode_roughness_shader_version, 0);
		if (decode_shader.is_valid()) {
			impl->decode_roughness_pipeline = RD::get_singleton()->compute_pipeline_create(decode_shader);
		}
	}
	{
		Vector<String> decode_variants;
		decode_variants.push_back("");
		impl->decode_normal.initialize(decode_variants);
		impl->decode_normal_shader_version = impl->decode_normal.version_create();
		RID decode_shader = impl->decode_normal.version_get_shader(impl->decode_normal_shader_version, 0);
		if (decode_shader.is_valid()) {
			impl->decode_normal_pipeline = RD::get_singleton()->compute_pipeline_create(decode_shader);
		}
	}

	impl->shaders_initialized = true;
}

BrixelizerGI::BrixelizerGI() {
	impl = memnew(BrixelizerGIImpl);
	memset(&impl->brixelizer_context, 0, sizeof(impl->brixelizer_context));
	memset(&impl->brixelizer_gi_context, 0, sizeof(impl->brixelizer_gi_context));
	memset(impl->brixelizer_pipelines, 0, sizeof(impl->brixelizer_pipelines));
	memset(impl->gi_pipelines, 0, sizeof(impl->gi_pipelines));
}

BrixelizerGI::~BrixelizerGI() {
	free();
	if (impl) {
		memdelete(impl);
		impl = nullptr;
	}
}

void BrixelizerGI::init() {
	if (impl->initialized) {
		return;
	}

	// Initialize shaders
	_init_shaders();

	// Create UBO ring buffers (16KB each for constant buffers)
	for (int i = 0; i < BRIXELIZER_UBO_RING_BUFFER_SIZE; i++) {
		impl->ubo_ring_buffer[i] = RD::get_singleton()->uniform_buffer_create(16 * 1024);
	}

	// Create samplers
	RD::SamplerState sampler_state;
	sampler_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	impl->point_clamp_sampler = RD::get_singleton()->sampler_create(sampler_state);

	sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	impl->linear_clamp_sampler = RD::get_singleton()->sampler_create(sampler_state);

	sampler_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_REPEAT;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
	sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_REPEAT;
	impl->point_wrap_sampler = RD::get_singleton()->sampler_create(sampler_state);

	sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	impl->linear_wrap_sampler = RD::get_singleton()->sampler_create(sampler_state);

	// Set up backend interface
	impl->backend = {};
	impl->backend.scratchBuffer = impl;
	impl->backend.scratchBufferSize = sizeof(*impl);
	impl->backend.fpGetSDKVersion = get_sdk_version_rd;
	impl->backend.fpGetEffectGpuMemoryUsage = get_effect_gpu_memory_usage_rd;
	impl->backend.fpCreateBackendContext = create_backend_context_rd;
	impl->backend.fpGetDeviceCapabilities = get_device_capabilities_rd;
	impl->backend.fpDestroyBackendContext = destroy_backend_context_rd;
	impl->backend.fpCreateResource = create_resource_rd;
	impl->backend.fpRegisterResource = register_resource_rd;
	impl->backend.fpGetResource = get_resource_rd;
	impl->backend.fpUnregisterResources = unregister_resources_rd;
	impl->backend.fpRegisterStaticResource = register_static_resource_rd;
	impl->backend.fpGetResourceDescription = get_resource_description_rd;
	impl->backend.fpDestroyResource = destroy_resource_rd;
	impl->backend.fpMapResource = map_resource_rd;
	impl->backend.fpUnmapResource = unmap_resource_rd;
	impl->backend.fpStageConstantBufferDataFunc = stage_constant_buffer_data_rd;
	impl->backend.fpCreatePipeline = create_pipeline_rd;
	impl->backend.fpDestroyPipeline = destroy_pipeline_rd;
	impl->backend.fpScheduleGpuJob = schedule_gpu_job_rd;
	impl->backend.fpExecuteGpuJobs = execute_gpu_jobs_rd;
	impl->backend.device = reinterpret_cast<FfxDevice>(RD::get_singleton());

	// Create Brixelizer context
	FfxBrixelizerContextDescription brixelizer_desc = {};
	brixelizer_desc.backendInterface = impl->backend;
	brixelizer_desc.sdfCenter[0] = 0.0f;
	brixelizer_desc.sdfCenter[1] = 0.0f;
	brixelizer_desc.sdfCenter[2] = 0.0f;
	brixelizer_desc.numCascades = 4;
	brixelizer_desc.flags = (FfxBrixelizerContextFlags)0;

	// Configure cascade descriptions with increasing voxel sizes
	float base_voxel_size = 0.1f; // 10cm base voxel size
	for (uint32_t i = 0; i < brixelizer_desc.numCascades; i++) {
		brixelizer_desc.cascadeDescs[i].flags = (FfxBrixelizerCascadeFlag)(FFX_BRIXELIZER_CASCADE_STATIC | FFX_BRIXELIZER_CASCADE_DYNAMIC);
		brixelizer_desc.cascadeDescs[i].voxelSize = base_voxel_size * (1 << i); // Double size each cascade
	}

	FfxErrorCode result = ffxBrixelizerContextCreate(&brixelizer_desc, &impl->brixelizer_context);
	if (result != FFX_OK) {
		ERR_PRINT("Failed to create Brixelizer context: " + itos(result));
		return;
	}

	impl->cascade_count = brixelizer_desc.numCascades;
	impl->static_vertex_buffers.resize(BRIXELIZER_MAX_VERTEX_BUFFERS);

	if (impl->sdf_atlas.is_null()) {
		RD::TextureFormat sdf_format;
		sdf_format.width = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE;
		sdf_format.height = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE;
		sdf_format.depth = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE;
		sdf_format.array_layers = 1;
		sdf_format.mipmaps = 1;
		sdf_format.texture_type = RD::TEXTURE_TYPE_3D;
		sdf_format.format = RD::DATA_FORMAT_R8_UNORM;
		sdf_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		impl->sdf_atlas = RD::get_singleton()->texture_create(sdf_format, RD::TextureView());
	}

	if (impl->brick_aabbs.is_null()) {
		impl->brick_aabbs = RD::get_singleton()->storage_buffer_create(FFX_BRIXELIZER_BRICK_AABBS_SIZE);
	}

	for (uint32_t i = 0; i < impl->cascade_count; i++) {
		if (impl->cascade_aabb_trees[i].is_null()) {
			impl->cascade_aabb_trees[i] = RD::get_singleton()->storage_buffer_create(FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE);
		}
		if (impl->cascade_brick_maps[i].is_null()) {
			impl->cascade_brick_maps[i] = RD::get_singleton()->storage_buffer_create(FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
		}
	}

	if (impl->fallback_env_map.is_null()) {
		RD::TextureFormat env_format;
		env_format.width = 1;
		env_format.height = 1;
		env_format.depth = 1;
		env_format.array_layers = 6;
		env_format.mipmaps = 1;
		env_format.texture_type = RD::TEXTURE_TYPE_CUBE;
		env_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		env_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		impl->fallback_env_map = RD::get_singleton()->texture_create(env_format, RD::TextureView());
		if (impl->fallback_env_map.is_valid()) {
			RD::get_singleton()->texture_clear(impl->fallback_env_map, Color(0, 0, 0, 1), 0, 1, 0, 6);
		}
	}

	if (impl->fallback_noise.is_null()) {
		RD::TextureFormat noise_format;
		noise_format.width = 128;
		noise_format.height = 128;
		noise_format.depth = 1;
		noise_format.array_layers = 1;
		noise_format.mipmaps = 1;
		noise_format.texture_type = RD::TEXTURE_TYPE_2D;
		noise_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		noise_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
				RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		impl->fallback_noise = RD::get_singleton()->texture_create(noise_format, RD::TextureView());
		if (impl->fallback_noise.is_valid()) {
			Vector<uint8_t> noise_data;
			noise_data.resize(noise_format.width * noise_format.height * 4);
			uint8_t *write_ptr = noise_data.ptrw();
			int offset = 0;
			for (int y = 0; y < (int)noise_format.height; ++y) {
				for (int x = 0; x < (int)noise_format.width; ++x) {
					const float f0 = samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp(x, y, 0, 0);
					const float f1 = samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp(x, y, 0, 1);
					const float f2 = samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp(x, y, 0, 2);
					const float f3 = samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp(x, y, 0, 3);

					write_ptr[offset++] = static_cast<uint8_t>(f0 * 255.0f);
					write_ptr[offset++] = static_cast<uint8_t>(f1 * 255.0f);
					write_ptr[offset++] = static_cast<uint8_t>(f2 * 255.0f);
					write_ptr[offset++] = static_cast<uint8_t>(f3 * 255.0f);
				}
			}
			const Error err = RD::get_singleton()->texture_update(impl->fallback_noise, 0, noise_data);
			if (err != OK) {
				RD::get_singleton()->texture_clear(impl->fallback_noise, Color(0.5, 0.5, 0.5, 1.0), 0, 1, 0, 1);
			}
		}
	}

	if (impl->fallback_prev_lit_output.is_null()) {
		RD::TextureFormat prev_format;
		prev_format.width = 1;
		prev_format.height = 1;
		prev_format.depth = 1;
		prev_format.array_layers = 1;
		prev_format.mipmaps = 1;
		prev_format.texture_type = RD::TEXTURE_TYPE_2D;
		prev_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		prev_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		impl->fallback_prev_lit_output = RD::get_singleton()->texture_create(prev_format, RD::TextureView());
		if (impl->fallback_prev_lit_output.is_valid()) {
			RD::get_singleton()->texture_clear(impl->fallback_prev_lit_output, Color(0, 0, 0, 1), 0, 1, 0, 1);
		}
	}

	if (impl->fallback_storage_texture.is_null()) {
		RD::TextureFormat fallback_format;
		fallback_format.width = 1;
		fallback_format.height = 1;
		fallback_format.depth = 1;
		fallback_format.array_layers = 1;
		fallback_format.mipmaps = 1;
		fallback_format.texture_type = RD::TEXTURE_TYPE_2D;
		fallback_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		fallback_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
				RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		impl->fallback_storage_texture = RD::get_singleton()->texture_create(fallback_format, RD::TextureView());
		if (impl->fallback_storage_texture.is_valid()) {
			RD::get_singleton()->texture_clear(impl->fallback_storage_texture, Color(0, 0, 0, 1), 0, 1, 0, 1);
		}
	}

	impl->initialized = true;
	s_current_impl = impl;
}

void BrixelizerGI::_create_output_textures(const Size2i &p_size) {
	if (impl->output_size == p_size && impl->diffuse_gi_texture.is_valid()) {
		return;
	}

	// Free old textures
	if (impl->diffuse_gi_texture.is_valid()) {
		RD::get_singleton()->free(impl->diffuse_gi_texture);
	}
	if (impl->specular_gi_texture.is_valid()) {
		RD::get_singleton()->free(impl->specular_gi_texture);
	}

	// Create new output textures
	RD::TextureFormat tf;
	tf.width = p_size.x;
	tf.height = p_size.y;
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
					RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	impl->diffuse_gi_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	impl->specular_gi_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
	impl->output_size = p_size;
}

void BrixelizerGI::_create_history_textures(const Size2i &p_size) {
	if (impl->history_size == p_size && impl->history_depth_texture.is_valid() &&
			impl->history_normal_roughness_texture.is_valid() && impl->history_lit_output_texture.is_valid()) {
		return;
	}

	if (impl->history_depth_texture.is_valid()) {
		RD::get_singleton()->free(impl->history_depth_texture);
	}
	if (impl->history_normal_roughness_texture.is_valid()) {
		RD::get_singleton()->free(impl->history_normal_roughness_texture);
	}
	if (impl->history_lit_output_texture.is_valid()) {
		RD::get_singleton()->free(impl->history_lit_output_texture);
	}

	RD::TextureFormat depth_format;
	depth_format.width = p_size.x;
	depth_format.height = p_size.y;
	depth_format.depth = 1;
	depth_format.array_layers = 1;
	depth_format.mipmaps = 1;
	depth_format.texture_type = RD::TEXTURE_TYPE_2D;
	depth_format.format = RD::DATA_FORMAT_R32_SFLOAT;
	depth_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	impl->history_depth_texture = RD::get_singleton()->texture_create(depth_format, RD::TextureView());

	RD::TextureFormat normal_format = depth_format;
	normal_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	impl->history_normal_roughness_texture = RD::get_singleton()->texture_create(normal_format, RD::TextureView());

	RD::TextureFormat lit_format = depth_format;
	lit_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	impl->history_lit_output_texture = RD::get_singleton()->texture_create(lit_format, RD::TextureView());

	impl->history_size = p_size;
	impl->history_valid = false;
	impl->history_lit_valid = false;
}

void BrixelizerGI::_create_roughness_texture(const Size2i &p_size) {
	if (impl->roughness_size == p_size && impl->roughness_texture.is_valid()) {
		return;
	}

	if (impl->roughness_texture.is_valid()) {
		RD::get_singleton()->free_rid(impl->roughness_texture);
	}

	RD::TextureFormat roughness_format;
	roughness_format.width = p_size.x;
	roughness_format.height = p_size.y;
	roughness_format.depth = 1;
	roughness_format.array_layers = 1;
	roughness_format.mipmaps = 1;
	roughness_format.texture_type = RD::TEXTURE_TYPE_2D;
	roughness_format.format = RD::DATA_FORMAT_R8_UNORM;
	roughness_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	impl->roughness_texture = RD::get_singleton()->texture_create(roughness_format, RD::TextureView());
	impl->roughness_size = p_size;
}

void BrixelizerGI::_create_world_normal_texture(const Size2i &p_size) {
	if (impl->world_normal_size == p_size && impl->world_normal_texture.is_valid()) {
		return;
	}

	if (impl->world_normal_texture.is_valid()) {
		RD::get_singleton()->free_rid(impl->world_normal_texture);
	}

	RD::TextureFormat normal_format;
	normal_format.width = p_size.x;
	normal_format.height = p_size.y;
	normal_format.depth = 1;
	normal_format.array_layers = 1;
	normal_format.mipmaps = 1;
	normal_format.texture_type = RD::TEXTURE_TYPE_2D;
	normal_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	normal_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	impl->world_normal_texture = RD::get_singleton()->texture_create(normal_format, RD::TextureView());
	impl->world_normal_size = p_size;
}

bool BrixelizerGI::_decode_roughness(const RID &p_normal_roughness, const Size2i &p_size) {
	if (!impl->decode_roughness_pipeline.is_valid()) {
		return false;
	}
	if (!p_normal_roughness.is_valid() || !impl->roughness_texture.is_valid()) {
		return false;
	}

	RID shader = impl->decode_roughness.version_get_shader(impl->decode_roughness_shader_version, 0);
	if (shader.is_null()) {
		return false;
	}

	Vector<RD::Uniform> uniforms;
	uniforms.resize(2);

	{
		RD::Uniform &u = uniforms.write[0];
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(p_normal_roughness);
	}
	{
		RD::Uniform &u = uniforms.write[1];
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(impl->roughness_texture);
	}

	RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);
	if (uniform_set.is_null()) {
		return false;
	}

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, impl->decode_roughness_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->free_rid(uniform_set);
	return true;
}

bool BrixelizerGI::_decode_normals(const RID &p_normal_roughness, const Size2i &p_size, const Transform3D &p_cam_transform) {
	if (!impl->decode_normal_pipeline.is_valid()) {
		return false;
	}
	if (!p_normal_roughness.is_valid() || !impl->world_normal_texture.is_valid()) {
		return false;
	}

	RID shader = impl->decode_normal.version_get_shader(impl->decode_normal_shader_version, 0);
	if (shader.is_null()) {
		return false;
	}

	Vector<RD::Uniform> uniforms;
	uniforms.resize(2);

	{
		RD::Uniform &u = uniforms.write[0];
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(p_normal_roughness);
	}
	{
		RD::Uniform &u = uniforms.write[1];
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(impl->world_normal_texture);
	}

	RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);
	if (uniform_set.is_null()) {
		return false;
	}

	BrixelizerDecodeNormalPushConstant push_constant = {};
	MaterialStorage::store_transform(p_cam_transform, push_constant.inv_view);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, impl->decode_normal_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(BrixelizerDecodeNormalPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->free_rid(uniform_set);
	return true;
}

void BrixelizerGI::_register_geometry_instances(const PagedArray<RenderGeometryInstance *> &p_instances) {
	if (!impl || !impl->initialized) {
		return;
	}

	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (!mesh_storage) {
		return;
	}

	// Clear buffers from previous frame - only those that weren't reused
	// Track which buffers are in use this frame to avoid freeing them
	HashMap<RID, bool> buffers_in_use_this_frame;

	// Dynamic instances are cleared by Brixelizer after update; just reset list.
	// Note: All dynamic instance IDs are freed by ffxBrixelizerUpdate()
	if (impl->dynamic_instance_ids.size() > 0) {
		impl->dynamic_instance_ids.clear();
	}

	auto get_vertex_format = [](uint64_t p_format, FfxSurfaceFormat &r_format) -> bool {
		if ((p_format & RS::ARRAY_FORMAT_VERTEX) == 0) {
			return false;
		}
		if (p_format & RS::ARRAY_FLAG_USE_2D_VERTICES) {
			return false;
		}
		if (p_format & RS::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
			return false;
		}
		r_format = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
		return true;
	};

	auto create_storage_copy = [](const RID &p_src, uint32_t p_size) -> RID {
		if (!p_src.is_valid() || p_size == 0) {
			return RID();
		}
		RID storage = RD::get_singleton()->storage_buffer_create(p_size);
		if (!storage.is_valid()) {
			return RID();
		}
		RD::get_singleton()->buffer_copy(p_src, storage, 0, 0, p_size);
		return storage;
	};

	// Dynamic instances are cleared by Brixelizer after update; just reset the list.
	if (impl->dynamic_instance_ids.size() > 0) {
		impl->dynamic_instance_ids.clear();
	}

	// Collect new instances to register
	LocalVector<FfxBrixelizerInstanceDescription> instance_descs;
	LocalVector<FfxBrixelizerInstanceID *> instance_id_ptrs;
	LocalVector<FfxBrixelizerInstanceID> instance_ids;

	for (uint32_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstance *gi = p_instances[i];
		if (!gi) {
			continue;
		}

		// Cast to base class to access data
		RenderGeometryInstanceBase *gi_base = static_cast<RenderGeometryInstanceBase *>(gi);
		if (!gi_base->data) {
			continue;
		}

		// Only process mesh instances
		if (gi_base->data->base_type != RS::INSTANCE_MESH) {
			continue;
		}

		RID mesh_rid = gi_base->data->base;
		if (!mesh_rid.is_valid()) {
			continue;
		}

		Transform3D transform = gi->get_transform();
		AABB aabb = gi->get_aabb();

		// Skip if no valid AABB
		if (aabb.size.length_squared() < 0.0001f) {
			continue;
		}

		// Determine if instance is static or dynamic
		// Static geometry should only be re-voxelized when transformed
		// Dynamic geometry is re-voxelized every frame
		// For now, treat everything as dynamic until proper tracking is implemented
		// TODO: Implement proper static geometry tracking via RendererSceneCull instance flags
		bool is_static = false;
		if (!is_static) {
			// Clear dynamic instances at start of frame
			if (impl->dynamic_instance_ids.size() > 0) {
				impl->dynamic_instance_ids.clear();
			}
		}

		// Get surface count for this mesh
		int surface_count = mesh_storage->mesh_get_surface_count(mesh_rid);
		if (surface_count <= 0) {
			continue;
		}

		// Process each surface
		for (int surf_idx = 0; surf_idx < surface_count; surf_idx++) {
			void *surface = mesh_storage->mesh_get_surface(mesh_rid, (uint32_t)surf_idx);
			if (!surface) {
				continue;
			}

			// Only process triangle primitives
			RS::PrimitiveType primitive = mesh_storage->mesh_surface_get_primitive(surface);
			if (primitive != RS::PRIMITIVE_TRIANGLES) {
				continue;
			}

			// Get buffer info
			RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(surface);
			RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(surface);
			uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
			uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
			uint32_t vertex_stride = mesh_storage->mesh_surface_get_vertex_stride(surface);
			uint64_t format = mesh_storage->mesh_surface_get_format(surface);
			FfxSurfaceFormat vertex_format = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;

			// Skip if no valid buffers
			if (!vertex_buffer.is_valid() || vertex_count == 0 || vertex_stride == 0) {
				continue;
			}
			if (!get_vertex_format(format, vertex_format)) {
				continue;
			}

			// Register vertex buffer if not already registered
			if (!impl->registered_vertex_buffers.has(vertex_buffer)) {
				uint32_t vertex_buffer_size = vertex_count * vertex_stride;
				RID vertex_storage = create_storage_copy(vertex_buffer, vertex_buffer_size);
				if (!vertex_storage.is_valid()) {
					continue;
				}

				FfxBrixelizerBufferDescription buffer_desc = {};
				buffer_desc.buffer.resource = (void *)(uintptr_t)vertex_storage.get_id();
				buffer_desc.buffer.description.type = FFX_RESOURCE_TYPE_BUFFER;
				buffer_desc.buffer.description.size = vertex_buffer_size;

				uint32_t ffx_index = 0;
				buffer_desc.outIndex = &ffx_index;

				FfxErrorCode result = ffxBrixelizerRegisterBuffers(&impl->brixelizer_context, &buffer_desc, 1);
				if (result == FFX_OK) {
					BrixelizerGIImpl::RegisteredBuffer reg;
					reg.ffx_index = ffx_index;
					reg.size = vertex_buffer_size;
					reg.storage_rid = vertex_storage;
					impl->registered_vertex_buffers[vertex_buffer] = reg;
					buffers_in_use_this_frame[vertex_buffer] = true; // Mark as used this frame
				} else {
					RD::get_singleton()->free(vertex_storage);
					continue; // Skip this surface if buffer registration failed
				}
			} else {
				// Buffer already registered, mark as used this frame
				buffers_in_use_this_frame[vertex_buffer] = true;
			}

			// Register index buffer if present and not already registered
			uint32_t index_buffer_ffx_idx = UINT32_MAX;
			if (index_buffer.is_valid() && index_count > 0) {
				if (!impl->registered_index_buffers.has(index_buffer)) {
					// Determine index size based on vertex count
					uint32_t index_size = (vertex_count > 65535) ? 4 : 2;
					uint32_t index_buffer_size = index_count * index_size;
					RID index_storage = create_storage_copy(index_buffer, index_buffer_size);
					if (!index_storage.is_valid()) {
						continue;
					}

					FfxBrixelizerBufferDescription buffer_desc = {};
					buffer_desc.buffer.resource = (void *)(uintptr_t)index_storage.get_id();
					buffer_desc.buffer.description.type = FFX_RESOURCE_TYPE_BUFFER;
					buffer_desc.buffer.description.size = index_buffer_size;

					uint32_t ffx_index = 0;
					buffer_desc.outIndex = &ffx_index;

					FfxErrorCode result = ffxBrixelizerRegisterBuffers(&impl->brixelizer_context, &buffer_desc, 1);
					if (result == FFX_OK) {
						BrixelizerGIImpl::RegisteredBuffer reg;
						reg.ffx_index = ffx_index;
						reg.size = index_buffer_size;
						reg.storage_rid = index_storage;
						impl->registered_index_buffers[index_buffer] = reg;
						index_buffer_ffx_idx = ffx_index;
						buffers_in_use_this_frame[index_buffer] = true; // Mark as used this frame
					} else {
						RD::get_singleton()->free(index_storage);
					}
				} else {
					// Buffer already registered, mark as used this frame
					buffers_in_use_this_frame[index_buffer] = true;
					index_buffer_ffx_idx = impl->registered_index_buffers[index_buffer].ffx_index;
				}
			}

			// Get vertex buffer FFX index
			uint32_t vertex_buffer_ffx_idx = impl->registered_vertex_buffers[vertex_buffer].ffx_index;

			// Create instance description
			FfxBrixelizerInstanceDescription inst_desc = {};

			// Set AABB
			AABB world_aabb = transform.xform(aabb);
			inst_desc.aabb.min[0] = world_aabb.position.x;
			inst_desc.aabb.min[1] = world_aabb.position.y;
			inst_desc.aabb.min[2] = world_aabb.position.z;
			inst_desc.aabb.max[0] = world_aabb.position.x + world_aabb.size.x;
			inst_desc.aabb.max[1] = world_aabb.position.y + world_aabb.size.y;
			inst_desc.aabb.max[2] = world_aabb.position.z + world_aabb.size.z;

			// Set transform (row-major 3x4 matrix)
			Basis basis = transform.basis;
			Vector3 origin = transform.origin;
			inst_desc.transform[0] = basis[0][0]; inst_desc.transform[1] = basis[1][0]; inst_desc.transform[2] = basis[2][0]; inst_desc.transform[3] = origin.x;
			inst_desc.transform[4] = basis[0][1]; inst_desc.transform[5] = basis[1][1]; inst_desc.transform[6] = basis[2][1]; inst_desc.transform[7] = origin.y;
			inst_desc.transform[8] = basis[0][2]; inst_desc.transform[9] = basis[1][2]; inst_desc.transform[10] = basis[2][2]; inst_desc.transform[11] = origin.z;

			// Set buffer indices
			inst_desc.vertexBuffer = vertex_buffer_ffx_idx;
			inst_desc.vertexStride = vertex_stride;
			inst_desc.vertexBufferOffset = 0;
			inst_desc.vertexCount = vertex_count;
			inst_desc.vertexFormat = vertex_format;

			if (index_buffer_ffx_idx != UINT32_MAX) {
				inst_desc.indexBuffer = index_buffer_ffx_idx;
				inst_desc.indexFormat = (vertex_count > 65535) ? FFX_INDEX_TYPE_UINT32 : FFX_INDEX_TYPE_UINT16;
				inst_desc.indexBufferOffset = 0;
				inst_desc.triangleCount = index_count / 3;
			} else {
				inst_desc.indexBuffer = UINT32_MAX;
				inst_desc.indexFormat = FFX_INDEX_TYPE_UINT32;
				inst_desc.triangleCount = vertex_count / 3;
			}

			inst_desc.maxCascade = 3; // Use up to cascade 3
			// TODO: Implement proper static geometry tracking
			// For now, treat all instances as dynamic to ensure correctness
			// Static geometry should only be re-voxelized when transformed, not every frame
			inst_desc.flags = FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC;

			// Allocate space for instance ID
			instance_ids.push_back(FfxBrixelizerInstanceID());
			inst_desc.outInstanceID = &instance_ids[instance_ids.size() - 1];

			instance_descs.push_back(inst_desc);
		}
	}

	// Register all instances
	if (instance_descs.size() > 0) {
		FfxErrorCode result = ffxBrixelizerCreateInstances(&impl->brixelizer_context,
			instance_descs.ptr(), instance_descs.size());

		if (result == FFX_OK) {
			// Store instance IDs for cleanup next frame
			for (uint32_t i = 0; i < instance_ids.size(); i++) {
				impl->dynamic_instance_ids.push_back(instance_ids[i]);
			}
		}
	}

	// Free buffers that weren't used this frame to prevent leaks
	for (const KeyValue<RID, BrixelizerGIImpl::RegisteredBuffer> &kv : impl->registered_vertex_buffers) {
		if (!buffers_in_use_this_frame.has(kv.key)) {
			RD::get_singleton()->free(kv.value.storage_rid);
			impl->registered_vertex_buffers.erase(kv.key);
		}
	}
	for (const KeyValue<RID, BrixelizerGIImpl::RegisteredBuffer> &kv : impl->registered_index_buffers) {
		if (!buffers_in_use_this_frame.has(kv.key)) {
			RD::get_singleton()->free(kv.value.storage_rid);
			impl->registered_index_buffers.erase(kv.key);
		}
	}
	buffers_in_use_this_frame.clear();
}

void BrixelizerGI::update(RenderDataRD *p_render_data, const InputTextures &p_input_textures, const PagedArray<RenderGeometryInstance *> *p_instances) {
}

void BrixelizerGI::update(RenderDataRD *p_render_data, const InputTextures &p_input_textures, const PagedArray<RenderGeometryInstance *> *p_instances) {
	if (!impl->initialized) {
		init();
	}

	if (!impl->initialized || !p_render_data) {
		return;
	}

	RenderSceneDataRD *scene_data = p_render_data->scene_data;
	if (!scene_data) {
		return;
	}

	// Get screen size from render buffers
	Size2i screen_size;
	if (p_render_data->render_buffers.is_valid()) {
		screen_size = p_render_data->render_buffers->get_internal_size();
	}
	if (screen_size.x <= 0 || screen_size.y <= 0) {
		return;
	}
	_create_output_textures(screen_size);
	if (!ensure_gi_context(impl, screen_size)) {
		return;
	}
	_create_history_textures(screen_size);
	_create_world_normal_texture(screen_size);
	_create_roughness_texture(screen_size);

	// Store input texture RIDs for FFX resource registration
	RID depth_rid = p_input_textures.depth;
	RID normal_roughness_rid = p_input_textures.normal_roughness;
	RID source_normal_roughness_rid = normal_roughness_rid;
	RID velocity_rid = p_input_textures.velocity;
	RID prev_depth_rid = p_input_textures.prev_depth;
	RID prev_normal_roughness_rid = p_input_textures.prev_normal_roughness;
	RID env_map_rid = p_input_textures.environment_map;
	RID noise_rid = p_input_textures.noise;
	RID prev_lit_output_rid;

	// Validate and ensure fallback textures exist
	if (p_render_data->render_buffers.is_valid()) {
		prev_lit_output_rid = p_render_data->render_buffers->get_back_buffer_texture();
	}
	if (!prev_lit_output_rid.is_valid()) {
		prev_lit_output_rid = impl->fallback_prev_lit_output;
	}

	// Early validation: check if context was properly initialized
	if (!impl->sdf_atlas.is_valid() || !impl->brick_aabbs.is_valid()) {
		ERR_PRINT("BrixelizerGI: Context not fully initialized - missing SDF atlas or brick AABBs");
		return;
	}

	// Ensure fallback env map exists
	if (!env_map_rid.is_valid()) {
		if (!impl->fallback_env_map.is_valid()) {
			ERR_PRINT("BrixelizerGI: Environment map not available and fallback not created - GI may be dark");
		} else {
			RD::get_singleton()->texture_clear(impl->fallback_env_map, get_environment_fallback_color(p_render_data->environment), 0, 1, 0, 6);
		}
		env_map_rid = impl->fallback_env_map;
	}

	// Ensure fallback noise exists
	if (!noise_rid.is_valid()) {
		if (!impl->fallback_noise.is_valid()) {
			ERR_PRINT("BrixelizerGI: Noise texture not available and fallback not created - GI may have artifacts");
		}
		noise_rid = impl->fallback_noise;
	}

	// Get camera transform and projection
	Transform3D cam_transform = scene_data->cam_transform;
	Projection cam_projection = scene_data->cam_projection;
	Vector3 cam_position = cam_transform.origin;
	uint32_t frame_index = impl->frame_index;

	bool camera_jump = false;
	if (frame_index > 0) {
		const float jump_distance = 10.0f; // Heuristic to reset history on large camera jumps.
		camera_jump = scene_data->prev_cam_transform.origin.distance_to(cam_transform.origin) > jump_distance;
	}
	if (camera_jump) {
		impl->history_valid = false;
		impl->history_lit_valid = false;
		prev_depth_rid = depth_rid;
		prev_normal_roughness_rid = normal_roughness_rid;
		prev_lit_output_rid = impl->fallback_prev_lit_output;
	}

	bool using_world_normals = false;
	if (source_normal_roughness_rid.is_valid()) {
		if (_decode_normals(source_normal_roughness_rid, screen_size, cam_transform)) {
			normal_roughness_rid = impl->world_normal_texture;
			using_world_normals = true;
		}
	}
	if (using_world_normals) {
		prev_normal_roughness_rid = normal_roughness_rid;
	}

	if (impl->history_valid && impl->history_depth_texture.is_valid()) {
		prev_depth_rid = impl->history_depth_texture;
	}
	if (impl->history_valid && impl->history_normal_roughness_texture.is_valid()) {
		prev_normal_roughness_rid = impl->history_normal_roughness_texture;
	}
	if (impl->history_lit_valid && impl->history_lit_output_texture.is_valid()) {
		prev_lit_output_rid = impl->history_lit_output_texture;
	}

	if (!prev_depth_rid.is_valid()) {
		prev_depth_rid = depth_rid;
	}
	if (!prev_normal_roughness_rid.is_valid()) {
		prev_normal_roughness_rid = normal_roughness_rid;
	}
	if (!prev_lit_output_rid.is_valid()) {
		prev_lit_output_rid = impl->fallback_prev_lit_output;
	}

	RID roughness_rid = source_normal_roughness_rid;
	uint32_t roughness_channel = 3;
	if (source_normal_roughness_rid.is_valid()) {
		if (_decode_roughness(source_normal_roughness_rid, screen_size)) {
			roughness_rid = impl->roughness_texture;
			roughness_channel = 0;
		}
	}

	// Register geometry instances with Brixelizer
	if (p_instances && p_instances->size() > 0) {
		_register_geometry_instances(*p_instances);
	}

	// Set up Brixelizer update
	FfxBrixelizerUpdateDescription update_desc = {};

	// Set SDF center to camera position
	update_desc.sdfCenter[0] = cam_position.x;
	update_desc.sdfCenter[1] = cam_position.y;
	update_desc.sdfCenter[2] = cam_position.z;

	update_desc.frameIndex = frame_index;
	impl->frame_index++;
	update_desc.maxReferences = 1024 * 1024;
	update_desc.maxBricksPerBake = 1024 * 16;
	update_desc.triangleSwapSize = 1024 * 1024 * 64;
	size_t scratch_size = 0;
	update_desc.outScratchBufferSize = &scratch_size;

	auto setup_ffx_buffer = [&](FfxResource &res, const RID &rid, uint32_t size) {
		if (!rid.is_valid()) {
			return;
		}
		res.resource = (void *)(uintptr_t)rid.get_id();
		res.description.type = FFX_RESOURCE_TYPE_BUFFER;
		res.description.size = size;
	};

	auto setup_ffx_texture = [&](FfxResource &res, const RID &rid, FfxSurfaceFormat format, FfxResourceType type) {
		if (!rid.is_valid()) {
			return;
		}
		RD::TextureFormat tf = RD::get_singleton()->texture_get_format(rid);
		res.resource = (void *)(uintptr_t)rid.get_id();
		res.description.type = type;
		res.description.width = tf.width;
		res.description.height = tf.height;
		res.description.depth = MAX(tf.depth, 1u);
		res.description.mipCount = MAX(tf.mipmaps, 1u);
		res.description.format = format;
	};

	if (!impl->sdf_atlas.is_valid() || !impl->brick_aabbs.is_valid()) {
		ERR_PRINT("BrixelizerGI: SDF atlas or brick AABBs not initialized - check init()");
		return;
	}
	for (uint32_t i = 0; i < impl->cascade_count; i++) {
		if (!impl->cascade_aabb_trees[i].is_valid() || !impl->cascade_brick_maps[i].is_valid()) {
			ERR_PRINT(vformat("BrixelizerGI: Cascade %d resources not initialized - check init()", i));
			return;
		}
	}

	setup_ffx_texture(update_desc.resources.sdfAtlas, impl->sdf_atlas, FFX_SURFACE_FORMAT_R8_UNORM, FFX_RESOURCE_TYPE_TEXTURE3D);
	setup_ffx_buffer(update_desc.resources.brickAABBs, impl->brick_aabbs, FFX_BRIXELIZER_BRICK_AABBS_SIZE);
	for (uint32_t i = 0; i < impl->cascade_count; i++) {
		setup_ffx_buffer(update_desc.resources.cascadeResources[i].aabbTree, impl->cascade_aabb_trees[i], FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE);
		setup_ffx_buffer(update_desc.resources.cascadeResources[i].brickMap, impl->cascade_brick_maps[i], FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
	}

	// Bake the update (will be empty without registered instances)
	FfxBrixelizerBakedUpdateDescription baked_desc = {};
	FfxErrorCode result = ffxBrixelizerBakeUpdate(&impl->brixelizer_context, &update_desc, &baked_desc);
	if (result != FFX_OK) {
		ERR_PRINT("Failed to bake Brixelizer update: " + itos(result));
		return;
	}

	// Create scratch buffer for update
	FfxResource scratch_resource = {};
	if (scratch_size > 0) {
		RID scratch_buffer = RD::get_singleton()->storage_buffer_create(uint32_t(scratch_size));
		scratch_resource.resource = (void *)(uintptr_t)scratch_buffer.get_id();
		scratch_resource.description.type = FFX_RESOURCE_TYPE_BUFFER;
		scratch_resource.description.size = uint32_t(scratch_size);

		result = ffxBrixelizerUpdate(&impl->brixelizer_context, &baked_desc, scratch_resource, nullptr);
		RD::get_singleton()->free(scratch_buffer);
		if (result != FFX_OK) {
			ERR_PRINT("Failed to update Brixelizer context: " + itos(result));
			return;
		}
	}

	// Set up GI dispatch
	FfxBrixelizerGIDispatchDescription gi_desc = {};

	Transform3D view_transform = cam_transform.affine_inverse();
	MaterialStorage::store_transform(view_transform, gi_desc.view);

	Transform3D prev_cam_transform = (frame_index == 0) ? cam_transform : scene_data->prev_cam_transform;
	Projection prev_cam_projection = (frame_index == 0) ? cam_projection : scene_data->prev_cam_projection;

	Transform3D prev_view_transform = prev_cam_transform.affine_inverse();
	MaterialStorage::store_transform(prev_view_transform, gi_desc.prevView);

	MaterialStorage::store_camera(cam_projection, gi_desc.projection);
	MaterialStorage::store_camera(prev_cam_projection, gi_desc.prevProjection);

	gi_desc.cameraPosition[0] = cam_position.x;
	gi_desc.cameraPosition[1] = cam_position.y;
	gi_desc.cameraPosition[2] = cam_position.z;

	gi_desc.startCascade = 0;
	gi_desc.endCascade = impl->cascade_count > 0 ? impl->cascade_count - 1 : 0;
	gi_desc.rayPushoff = 0.25f;
	gi_desc.sdfSolveEps = 0.5f;
	gi_desc.specularRayPushoff = 0.25f;
	gi_desc.specularSDFSolveEps = 0.5f;
	gi_desc.tMin = 0.0f;
	gi_desc.tMax = 10000.0f;

	gi_desc.normalsUnpackMul = 2.0f;
	gi_desc.normalsUnpackAdd = -1.0f;
	gi_desc.isRoughnessPerceptual = true;
	gi_desc.roughnessChannel = roughness_channel;
	gi_desc.roughnessThreshold = 0.9f;
	gi_desc.environmentMapIntensity = 0.1f;
	gi_desc.motionVectorScale.x = 1.0f;
	gi_desc.motionVectorScale.y = 1.0f;

	FfxSurfaceFormat prev_lit_format = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	if (prev_lit_output_rid.is_valid()) {
		const RD::TextureFormat tf = RD::get_singleton()->texture_get_format(prev_lit_output_rid);
		const FfxSurfaceFormat mapped = rd_format_to_ffx_surface_format(tf.format);
		if (mapped != FFX_SURFACE_FORMAT_UNKNOWN) {
			prev_lit_format = mapped;
		}
	}

	FfxSurfaceFormat env_map_format = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	if (env_map_rid.is_valid()) {
		const RD::TextureFormat tf = RD::get_singleton()->texture_get_format(env_map_rid);
		const FfxSurfaceFormat mapped = rd_format_to_ffx_surface_format(tf.format);
		if (mapped != FFX_SURFACE_FORMAT_UNKNOWN) {
			env_map_format = mapped;
		}
	}

	FfxSurfaceFormat roughness_format = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
	if (roughness_rid == impl->roughness_texture) {
		roughness_format = FFX_SURFACE_FORMAT_R8_UNORM;
	}

	// Register input textures
	setup_ffx_texture(gi_desc.depth, depth_rid, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.historyDepth, prev_depth_rid, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.normal, normal_roughness_rid, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.historyNormal, prev_normal_roughness_rid, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.roughness, roughness_rid, roughness_format, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.motionVectors, velocity_rid, FFX_SURFACE_FORMAT_R16G16_FLOAT, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.environmentMap, env_map_rid, env_map_format, FFX_RESOURCE_TYPE_TEXTURE_CUBE);
	setup_ffx_texture(gi_desc.noiseTexture, noise_rid, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.prevLitOutput, prev_lit_output_rid, prev_lit_format, FFX_RESOURCE_TYPE_TEXTURE2D);

	// Register output textures
	setup_ffx_texture(gi_desc.outputDiffuseGI, impl->diffuse_gi_texture, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_RESOURCE_TYPE_TEXTURE2D);
	setup_ffx_texture(gi_desc.outputSpecularGI, impl->specular_gi_texture, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_RESOURCE_TYPE_TEXTURE2D);

	// Register Brixelizer resources
	setup_ffx_texture(gi_desc.sdfAtlas, impl->sdf_atlas, FFX_SURFACE_FORMAT_R8_UNORM, FFX_RESOURCE_TYPE_TEXTURE3D);
	setup_ffx_buffer(gi_desc.bricksAABBs, impl->brick_aabbs, FFX_BRIXELIZER_BRICK_AABBS_SIZE);
	for (uint32_t i = 0; i < impl->cascade_count; i++) {
		setup_ffx_buffer(gi_desc.cascadeAABBTrees[i], impl->cascade_aabb_trees[i], FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE);
		setup_ffx_buffer(gi_desc.cascadeBrickMaps[i], impl->cascade_brick_maps[i], FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
	}

	// Get raw context for GI
	FfxBrixelizerRawContext *raw_context = nullptr;
	ffxBrixelizerGetRawContext(&impl->brixelizer_context, &raw_context);
	gi_desc.brixelizerContext = raw_context;

	// Dispatch GI
	result = ffxBrixelizerGIContextDispatch(&impl->brixelizer_gi_context, &gi_desc, nullptr);
	if (result != FFX_OK) {
		ERR_PRINT("Failed to dispatch BrixelizerGI: " + itos(result));
		return;
	}

	CopyEffects *copy_effects = CopyEffects::get_singleton();
	if (copy_effects && depth_rid.is_valid() && normal_roughness_rid.is_valid() &&
			impl->history_depth_texture.is_valid() && impl->history_normal_roughness_texture.is_valid()) {
		Rect2i rect(0, 0, screen_size.x, screen_size.y);
		copy_effects->copy_depth_to_rect(depth_rid, impl->history_depth_texture, rect);
		copy_effects->copy_to_rect(normal_roughness_rid, impl->history_normal_roughness_texture, rect);
		impl->history_valid = true;
	}

	// Store for next frame
	impl->prev_cam_transform = cam_transform;
	impl->prev_cam_projection = cam_projection;
}

void BrixelizerGI::composite_output(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (!impl || !impl->initialized || p_render_buffers.is_null()) {
		return;
	}

	// Check if we have valid output textures
	if (!impl->diffuse_gi_texture.is_valid() || !impl->specular_gi_texture.is_valid()) {
		return;
	}

	CopyEffects *copy_effects = CopyEffects::get_singleton();
	if (!copy_effects) {
		return;
	}

	Size2i internal_size = p_render_buffers->get_internal_size();
	if (internal_size.x <= 0 || internal_size.y <= 0) {
		return;
	}

	// Ensure GI buffers exist
	if (!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_AMBIENT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, internal_size);
		p_render_buffers->create_texture(RB_SCOPE_GI, RB_TEX_REFLECTION, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, internal_size);
	}

	// Get target GI buffers
	RID ambient_texture = p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
	RID reflection_texture = p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION);

	if (!ambient_texture.is_valid() || !reflection_texture.is_valid()) {
		return;
	}

	RD::get_singleton()->draw_command_begin_label("BrixelizerGI Composite");

	// Copy BrixelizerGI diffuse to ambient buffer
	copy_effects->copy_to_rect(impl->diffuse_gi_texture, ambient_texture, Rect2(0, 0, internal_size.x, internal_size.y));

	// Copy BrixelizerGI specular to reflection buffer
	copy_effects->copy_to_rect(impl->specular_gi_texture, reflection_texture, Rect2(0, 0, internal_size.x, internal_size.y));

	RD::get_singleton()->draw_command_end_label();
}

void BrixelizerGI::store_prev_lit_output(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (!impl || !impl->initialized || p_render_buffers.is_null()) {
		return;
	}

	Size2i internal_size = p_render_buffers->get_internal_size();
	if (internal_size.x <= 0 || internal_size.y <= 0) {
		return;
	}

	RID source = p_render_buffers->get_internal_texture();
	if (!source.is_valid()) {
		return;
	}

	_create_history_textures(internal_size);
	if (!impl->history_lit_output_texture.is_valid()) {
		return;
	}

	CopyEffects *copy_effects = CopyEffects::get_singleton();
	if (!copy_effects) {
		return;
	}

	copy_effects->copy_to_rect(source, impl->history_lit_output_texture, Rect2(0, 0, internal_size.x, internal_size.y));
	impl->history_lit_valid = true;
}

RID BrixelizerGI::get_diffuse_gi_texture() const {
	return impl ? impl->diffuse_gi_texture : RID();
}

RID BrixelizerGI::get_specular_gi_texture() const {
	return impl ? impl->specular_gi_texture : RID();
}

bool BrixelizerGI::is_initialized() const {
	return impl && impl->initialized;
}

void BrixelizerGI::free() {
	if (!impl || !impl->initialized) {
		return;
	}

	if (impl->gi_initialized) {
		ffxBrixelizerGIContextDestroy(&impl->brixelizer_gi_context);
		impl->gi_initialized = false;
	}
	ffxBrixelizerContextDestroy(&impl->brixelizer_context);

	// Free pipelines
	for (int i = 0; i < FFX_BRIXELIZER_PASS_COUNT; i++) {
		if (impl->brixelizer_pipelines[i].pipeline_rid.is_valid()) {
			RD::get_singleton()->free(impl->brixelizer_pipelines[i].pipeline_rid);
		}
		if (impl->brixelizer_pipelines[i].static_uniform_set.is_valid()) {
			RD::get_singleton()->free(impl->brixelizer_pipelines[i].static_uniform_set);
		}
	}
	for (int i = 0; i < FFX_BRIXELIZER_GI_PASS_COUNT; i++) {
		if (impl->gi_pipelines[i].pipeline_rid.is_valid()) {
			RD::get_singleton()->free(impl->gi_pipelines[i].pipeline_rid);
		}
		if (impl->gi_pipelines[i].static_uniform_set.is_valid()) {
			RD::get_singleton()->free(impl->gi_pipelines[i].static_uniform_set);
		}
	}
	if (impl->decode_normal_pipeline.is_valid()) {
		RD::get_singleton()->free(impl->decode_normal_pipeline);
	}
	if (impl->decode_roughness_pipeline.is_valid()) {
		RD::get_singleton()->free(impl->decode_roughness_pipeline);
	}

	// Free shader versions
	for (int i = 0; i < FFX_BRIXELIZER_PASS_COUNT; i++) {
		if (impl->brixelizer_shaders[i] && impl->brixelizer_shader_versions[i].is_valid()) {
			impl->brixelizer_shaders[i]->version_free(impl->brixelizer_shader_versions[i]);
		}
	}
	for (int i = 0; i < FFX_BRIXELIZER_GI_PASS_COUNT; i++) {
		if (impl->gi_shaders[i] && impl->gi_shader_versions[i].is_valid()) {
			impl->gi_shaders[i]->version_free(impl->gi_shader_versions[i]);
		}
	}
	if (impl->decode_normal_shader_version.is_valid()) {
		impl->decode_normal.version_free(impl->decode_normal_shader_version);
	}
	if (impl->decode_roughness_shader_version.is_valid()) {
		impl->decode_roughness.version_free(impl->decode_roughness_shader_version);
	}

	// Free UBO ring buffers
	for (int i = 0; i < BRIXELIZER_UBO_RING_BUFFER_SIZE; i++) {
		if (impl->ubo_ring_buffer[i].is_valid()) {
			RD::get_singleton()->free(impl->ubo_ring_buffer[i]);
		}
	}

	// Free samplers
	if (impl->point_clamp_sampler.is_valid()) {
		RD::get_singleton()->free(impl->point_clamp_sampler);
	}
	if (impl->linear_clamp_sampler.is_valid()) {
		RD::get_singleton()->free(impl->linear_clamp_sampler);
	}
	if (impl->point_wrap_sampler.is_valid()) {
		RD::get_singleton()->free(impl->point_wrap_sampler);
	}
	if (impl->linear_wrap_sampler.is_valid()) {
		RD::get_singleton()->free(impl->linear_wrap_sampler);
	}

	// Free fallback textures
	if (impl->fallback_env_map.is_valid()) {
		RD::get_singleton()->free(impl->fallback_env_map);
	}
	if (impl->fallback_noise.is_valid()) {
		RD::get_singleton()->free(impl->fallback_noise);
	}
	if (impl->fallback_prev_lit_output.is_valid()) {
		RD::get_singleton()->free(impl->fallback_prev_lit_output);
	}
	if (impl->fallback_storage_texture.is_valid()) {
		RD::get_singleton()->free(impl->fallback_storage_texture);
	}
	if (impl->world_normal_texture.is_valid()) {
		RD::get_singleton()->free(impl->world_normal_texture);
	}
	if (impl->roughness_texture.is_valid()) {
		RD::get_singleton()->free(impl->roughness_texture);
	}

	// Free history textures
	if (impl->history_depth_texture.is_valid()) {
		RD::get_singleton()->free(impl->history_depth_texture);
	}
	if (impl->history_normal_roughness_texture.is_valid()) {
		RD::get_singleton()->free(impl->history_normal_roughness_texture);
	}
	if (impl->history_lit_output_texture.is_valid()) {
		RD::get_singleton()->free(impl->history_lit_output_texture);
	}
	impl->history_valid = false;
	impl->history_lit_valid = false;
	impl->history_size = Size2i();

	// Free Brixelizer resources
	if (impl->sdf_atlas.is_valid()) {
		RD::get_singleton()->free(impl->sdf_atlas);
	}
	if (impl->brick_aabbs.is_valid()) {
		RD::get_singleton()->free(impl->brick_aabbs);
	}
	for (uint32_t i = 0; i < impl->cascade_count; i++) {
		if (impl->cascade_aabb_trees[i].is_valid()) {
			RD::get_singleton()->free(impl->cascade_aabb_trees[i]);
		}
		if (impl->cascade_brick_maps[i].is_valid()) {
			RD::get_singleton()->free(impl->cascade_brick_maps[i]);
		}
	}

	for (const KeyValue<RID, BrixelizerGIImpl::RegisteredBuffer> &kv : impl->registered_vertex_buffers) {
		if (kv.value.storage_rid.is_valid()) {
			RD::get_singleton()->free(kv.value.storage_rid);
		}
	}
	impl->registered_vertex_buffers.clear();

	for (const KeyValue<RID, BrixelizerGIImpl::RegisteredBuffer> &kv : impl->registered_index_buffers) {
		if (kv.value.storage_rid.is_valid()) {
			RD::get_singleton()->free(kv.value.storage_rid);
		}
	}
	impl->registered_index_buffers.clear();

	// Free internal resources
	for (const KeyValue<uint32_t, BrixelizerGIImpl::Resource> &kv : impl->resources) {
		if (!kv.value.is_external && kv.value.rid.is_valid()) {
			RD::get_singleton()->free(kv.value.rid);
		}
	}
	impl->resources.clear();

	impl->initialized = false;
	s_current_impl = nullptr;
}
