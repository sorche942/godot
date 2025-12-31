#pragma once

#include "core/math/transform_3d.h"
#include "core/templates/paged_array.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_geometry_instance.h"

class RenderDataRD;

namespace RendererRD {

// Forward declaration - implementation hidden to avoid FFX type conflicts with FSR2
struct BrixelizerGIImpl;

class BrixelizerGI {
public:
	struct InputTextures {
		RID depth;
		RID normal_roughness;
		RID velocity;
		RID prev_depth;
		RID prev_normal_roughness;
		RID environment_map;
		RID noise;
	};

private:
	BrixelizerGIImpl *impl = nullptr;

	void _init_shaders();
	void _create_output_textures(const Size2i &p_size);
	void _create_history_textures(const Size2i &p_size);
	void _create_world_normal_texture(const Size2i &p_size);
	void _create_roughness_texture(const Size2i &p_size);
	bool _decode_roughness(const RID &p_normal_roughness, const Size2i &p_size);
	bool _decode_normals(const RID &p_normal_roughness, const Size2i &p_size, const Transform3D &p_cam_transform);
	void _register_geometry_instances(const PagedArray<RenderGeometryInstance *> &p_instances);

public:
	void init();
	void update(RenderDataRD *p_render_data, const InputTextures &p_input_textures, const PagedArray<RenderGeometryInstance *> *p_instances = nullptr);
	void composite_output(Ref<RenderSceneBuffersRD> p_render_buffers);
	void store_prev_lit_output(Ref<RenderSceneBuffersRD> p_render_buffers);
	void free();

	RID get_diffuse_gi_texture() const;
	RID get_specular_gi_texture() const;
	bool is_initialized() const;

	BrixelizerGI();
	~BrixelizerGI();
};

}
