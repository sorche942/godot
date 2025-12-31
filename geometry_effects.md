   Proposed Abstractions for Geometry-Dependent Compute Effects

   Overview

   A set of infrastructure improvements to simplify implementing complex compute effects (Brixelizer, RTXGI, etc.)
   that require access to geometry data in Godot's RD backend.

   ──────────────────────────────────────────

   1. Geometry Access Layer (`geometry_producer.h`)

   Problem

   Currently each effect (SDFGI, VoxelGI, Brixelizer) independently:
   •  Iterates RenderGeometryInstance array
   •  Queries MeshStorage for buffers
   •  Validates formats, AABBs
   •  Creates buffer copies
   •  Tracks registration manually

   Proposed Abstraction

   cpp
     // servers/rendering/renderer_rd/geometry_producer.h

     struct GeometrySurfaceData {
         RID vertex_buffer;      // Direct RD buffer access
         RID index_buffer;       // Direct RD buffer access
         uint32_t vertex_count;
         uint32_t index_count;
         uint32_t vertex_stride;
         uint64_t vertex_format;
         AABB local_aabb;
         RS::PrimitiveType primitive;
     };

     struct GeometryInstanceData {
         Transform3D transform;
         AABB world_aabb;
         RID mesh_rid;
         bool is_static;
         Vector<GeometrySurfaceData> surfaces;
     };

     // Interface for effects to request geometry
     class GeometryProducer {
     public:
         virtual void on_geometry_added(GeometryInstanceData &p_inst) {}
         virtual void on_geometry_updated(RID p_mesh) {}
         virtual void on_geometry_removed(RID p_mesh) {}

         // Request surfaces for all visible instances matching criteria
         virtual void query_geometry(
             const PagedArray<RenderGeometryInstance *> &p_instances,
             GeometryFilter p_filter,  // static_only, dynamic_only, visible_layer_mask, etc.
             LocalVector<GeometryInstanceData> &r_output
         ) = 0;
     };

   Benefits:
   •  Single place for format validation
   •  Automatic buffer lifetime tracking
   •  Consistent filtering/pattern matching
   •  Effects declare what they need, system provides it

   ──────────────────────────────────────────

   2. Compute Effect Base Class (`compute_effect.h`)

   Problem

   SDFGI, VoxelGI, Brixelizer all reimplement:
   •  Buffer pool management
   •  Pipeline caching
   •  Shader initialization
   •  Debug visualization hooks
   •  Resource lifecycle

   Proposed Abstraction

   cpp
     // servers/rendering/renderer_rd/compute/compute_effect.h

     class ComputeEffect {
     public:
         virtual ~ComputeEffect() {}

         // Lifecycle
         virtual void init() {}
         virtual void update(const ComputeEffectUpdateParams &p_params) {}
         virtual void free() {}

         // Buffer pool (scratch, temporary resources)
         struct BufferRequest {
             uint32_t size;
             RD::BufferUsageBits usage;
         };
         virtual RID request_buffer(const BufferRequest &p_req);
         virtual void release_frame_buffers();

         // Debug hooks (optional)
         virtual void debug_draw(RD::DrawListID p_draw_list, const DebugDrawParams &p_params) {}
         virtual bool has_debug_visualization() const { return false; }

         // Quality/performance hooks
         virtual void set_quality_level(int p_level) {}
         virtual void get_gpu_time(float &r_prepare_ms, float &r_execute_ms) {}

     protected:
         // Common utilities
         void create_compute_pipeline(RID p_shader, RID &r_pipeline);
         void create_uniform_set(const Vector<RD::Uniform> &p_uniforms, RID p_shader, uint32_t p_set, RID
     &r_uniform_set);
     };

   Benefits:
   •  Shared buffer pool reduces fragmentation
   •  Consistent debug interface
   •  Built-in quality/performance hooks
   •  Reduces boilerplate

   ──────────────────────────────────────────

   3. SDK Integration Layer (`sdk_bridge.h`)

   Problem

   Brixelizer implements ~1000 lines of FFX callback bridge that could be reusable for other FFX effects.

   Proposed Abstraction

   cpp
     // servers/rendering/renderer_rd/sdk/ffx_bridge.h

     template <typename FFXContext, typename FFXDispatchDesc>
     class FFXBridge {
     public:
         // Resource translation helpers
         static FfxResource wrap_rid_as_ffx_resource(RID p_rid, FfxSurfaceFormat p_format);
         static RID unwrap_ffx_resource(const FfxResource &p_res);

         // Backend interface builder (auto-generates callback functions)
         FfxInterface build_backend_interface();

         // Pipeline management with auto-caching
         struct FFXPassSetup {
             RID shader_rid;
             uint32_t pass_id;
             bool needs_vertex_buffers;
         };
         void register_pass(const FFXPassSetup &p_setup);
         void dispatch_pass(uint32_t p_pass_id, const FfxComputeJobDescription &p_job);

         // Automatic resource lifecycle
         void register_external_resource(uint32_t p_slot, RID p_rid);
         void clear_external_resources();
     };

     // For RTXGI or other SDKs, could have similar:
     template <typename RTXContext, typename RTXDispatchDesc>
     class RTXBridge { /* similar pattern */ };

   Benefits:
   •  FFX/RTXGI integrations drop from ~1000 to ~100 lines
   •  Consistent resource handling patterns
   •  Reusable across multiple SDK integrations
   •  Type-safe resource wrapping

   ──────────────────────────────────────────

   4. RenderData Extension Pattern

   Problem

   Effects currently access geometry through RenderDataRD::instances but need additional context (camera, environment
   parameters, render buffers).

   Proposed Enhancement

   cpp
     // servers/rendering/renderer_rd/storage_rd/render_data_rd.h (extend)

     struct ComputeEffectParams {
         // Core data
         const RenderDataRD *render_data;
         Ref<RenderSceneBuffersRD> render_buffers;

         // Camera
         Transform3D camera_transform;
         Projection camera_projection;
         Transform3D prev_camera_transform;
         Projection prev_camera_projection;

         // Scene data
         const PagedArray<RenderGeometryInstance *> *instances;
         Size2i internal_size;
         uint32_t frame_index;

         // Optional: filtered geometry (if effect requested)
         const LocalVector<GeometryInstanceData> *filtered_instances;

         // Environment/configuration
         RID environment;
         bool environment_has_param(const String &p_name) const;
         Variant get_environment_param(const String &p_name) const;
     };

   Benefits:
   •  Single struct captures everything effects need
   •  Extensible without breaking changes
   •  Clear data flow

   ──────────────────────────────────────────

   5. Buffer Pool Manager (`buffer_pool.h`)

   Problem

   Each effect manages scratch buffers independently, causing fragmentation and duplicate code.

   Proposed Abstraction

   cpp
     // servers/rendering/renderer_rd/storage_rd/buffer_pool.h

     class ComputeBufferPool {
     public:
         // Request a buffer (returns cached if size/format matches)
         RID request_buffer(uint32_t p_size, RD::BufferUsageBits p_usage);

         // Mark buffers for reuse (called at end of frame)
         void end_frame();

         // Clear all cached buffers
         void purge();

         // Statistics
         uint64_t get_total_allocated() const;
         uint64_t get_cached_count() const;

     private:
         struct PoolEntry {
             RID buffer;
             uint32_t size;
             uint32_t frame_last_used;
             bool in_use;
         };
         LocalVector<PoolEntry> entries;

         // Reuse strategy: LRU by default
     };

   Benefits:
   •  Automatic buffer reuse across effects
   •  Reduces allocation/deallocation overhead
   •  Centralized fragmentation tracking

   ──────────────────────────────────────────

   6. Quality Settings Registry

   Problem

   Each GI effect invents its own quality enum and parameters. No unified debugging/profiling.

   Proposed System

   cpp
     // servers/rendering/rendering/quality_registry.h

     enum ComputeEffectQuality {
         QUALITY_LOW,
         QUALITY_MEDIUM,
         QUALITY_HIGH,
         QUALITY_ULTRA,
         QUALITY_CUSTOM,
     };

     struct EffectMetrics {
         String effect_name;
         float gpu_prepare_ms = 0.0f;
         float gpu_execute_ms = 0.0f;
         uint32_t buffer_pool_hits = 0;
         uint32_t buffer_pool_misses = 0;
         uint32_t geometry_instances_processed = 0;
     };

     class ComputeQualityRegistry {
     public:
         void register_effect(const StringName &p_effect_id, ComputeEffect *p_effect);
         void set_effect_quality(const StringName &p_effect_id, ComputeEffectQuality p_quality);
         ComputeEffectQuality get_effect_quality(const StringName &p_effect_id) const;

         EffectMetrics get_metrics(const StringName &p_effect_id) const;
         void reset_metrics();

         // Auto-adjust quality based on frame time
         void auto_adjust_quality(const StringName &p_effect_id, float p_target_frame_time_ms);
     };

   Benefits:
   •  Unified quality control across effects
   •  Built-in profiling/metrics
   •  Automatic quality scaling for performance

   ──────────────────────────────────────────

   Implementation Phases

   Phase 1: Foundation (Low Risk)
   1. Add GeometryProducer interface
   2. Add ComputeEffect base class
   3. Add ComputeBufferPool
   4. Write tests for buffer pooling

   Phase 2: Migration (Medium Risk)
   1. Refactor Brixelizer to use ComputeEffect base
   2. Add FFXBridge template
   3. Port Brixelizer to GeometryProducer pattern

   Phase 3: Future-Proofing (Low Risk)
   1. Add ComputeQualityRegistry
   2. Document patterns for new effect authors
   3. Create reference implementation (simple GI effect)

   ──────────────────────────────────────────

   Impact Assessment

   Effect           │ Current LOC │ With Abstractions │ Reduction
   -----------------+-------------+-------------------+----------
   BrixelizerGI     │ ~1800       │ ~400              │ ~78%
   Future RTXGI     │ N/A (new)   │ ~400              │ Baseline
   SDFGI (refactor) │ ~2500       │ ~1800             │ ~28%

   Long-term benefits:
   •  New GI effects: 200-400 lines instead of 1500+
   •  Shared optimizations benefit all effects
   •  Consistent debugging/profiling
   •  Easier maintenance

   ──────────────────────────────────────────

   Recommended Order

   1. ComputeBufferPool - Immediate benefit, lowest risk
   2. ComputeEffect base class - Foundation for everything else
   3. FFXBridge - Specific to Brixelizer, high value
   4. GeometryProducer - Larger refactoring, highest value
   5. QualityRegistry - Nice-to-have, low priority
