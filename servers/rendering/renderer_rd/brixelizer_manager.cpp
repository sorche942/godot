#define VK_NO_PROTOTYPES    // Vulkan headers declare function pointers only.
#include "brixelizer_manager.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"

#include "core/templates/local_vector.h"
#include "core/math/color.h"

#ifndef USE_VOLK
#define USE_VOLK
#endif
#include "drivers/vulkan/godot_vulkan.h"
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#endif

#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <FidelityFX/host/ffx_error.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/gpu/brixelizer/ffx_brixelizer_host_gpu_shared.h>

#include <cstdlib>

namespace {

// Considerations:
// - Only triangle surfaces are submitted; non-triangle primitives are skipped.
// - 2D vertex surfaces and compressed attribute surfaces are skipped until we add a
//   position-only buffer path (Brixelizer expects float3 or half4 positions).
// - Vertex stride is derived from the interleaved vertex buffer; we assume positions
//   are the first 3 floats in that layout.
// - Skinned/animated meshes currently use the base mesh vertex buffer (no skinning output).
// - MultiMesh/Particles are not expanded into per-instance submissions yet.
// - LOD index buffers are not used; we always submit the base surface index buffer.
struct BrixelizerSurfaceData {
	AABB world_aabb;
	uint32_t vertex_count = 0;
	uint32_t vertex_stride = 0;
	uint32_t vertex_buffer_size = 0;
	RID vertex_buffer;
	uint32_t index_count = 0;
	uint32_t index_stride = 0;
	uint32_t index_buffer_size = 0;
	RID index_buffer;
};

static bool brixelizer_get_surface_data(RendererRD::MeshStorage *mesh_storage, void *surface, uint32_t surface_index, const Transform3D &xform, const AABB &world_aabb, BrixelizerSurfaceData &r_data) {
	(void)surface_index;
	const uint64_t surface_format = mesh_storage->mesh_surface_get_format(surface);
	const RS::PrimitiveType primitive = mesh_storage->mesh_surface_get_primitive(surface);
	if (primitive != RS::PRIMITIVE_TRIANGLES) {
		return false;
	}
	if (surface_format & RS::ARRAY_FLAG_USE_2D_VERTICES) {
		// TODO: Add a path for 2D vertex surfaces if needed for Brixelizer ingestion.
		return false;
	}
	if (surface_format & RS::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
		// TODO: Consider decompressing or providing a position-only buffer for compressed surfaces.
		return false;
	}

	AABB local_aabb = mesh_storage->mesh_surface_get_aabb(surface);
	r_data.world_aabb = xform.xform(local_aabb);
	if (!r_data.world_aabb.is_finite()) {
		r_data.world_aabb = world_aabb;
	}

	r_data.vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
	r_data.vertex_buffer_size = mesh_storage->mesh_surface_get_vertex_buffer_size(surface);
	r_data.vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(surface);
	if (r_data.vertex_count == 0 || r_data.vertex_buffer_size == 0 || !r_data.vertex_buffer.is_valid()) {
		return false;
	}

	r_data.index_count = mesh_storage->mesh_surface_get_index_count(surface);
	r_data.index_buffer_size = mesh_storage->mesh_surface_get_index_buffer_size(surface);
	r_data.index_buffer = mesh_storage->mesh_surface_get_index_buffer(surface);
	if (r_data.index_count == 0 || r_data.index_buffer_size == 0 || !r_data.index_buffer.is_valid()) {
		return false;
	}

	r_data.vertex_stride = r_data.vertex_buffer_size / r_data.vertex_count;
	r_data.index_stride = r_data.index_buffer_size / r_data.index_count;
	if (r_data.vertex_stride < sizeof(float) * 3) {
		return false;
	}
	if (r_data.index_stride != 2 && r_data.index_stride != 4) {
		return false;
	}
	if (r_data.vertex_buffer_size < r_data.vertex_stride * r_data.vertex_count) {
		return false;
	}
	if (r_data.index_buffer_size < r_data.index_stride * r_data.index_count) {
		return false;
	}

	return true;
}

static void brixelizer_fill_instance_desc(FfxBrixelizerInstanceDescription &r_desc, const BrixelizerSurfaceData &surface_data, const Transform3D &xform, uint32_t vertex_buffer_index, uint32_t index_buffer_index, bool dynamic, FfxBrixelizerInstanceID *out_id) {
	r_desc.maxCascade = 4;
	r_desc.aabb.min[0] = surface_data.world_aabb.position.x;
	r_desc.aabb.min[1] = surface_data.world_aabb.position.y;
	r_desc.aabb.min[2] = surface_data.world_aabb.position.z;
	r_desc.aabb.max[0] = surface_data.world_aabb.position.x + surface_data.world_aabb.size.x;
	r_desc.aabb.max[1] = surface_data.world_aabb.position.y + surface_data.world_aabb.size.y;
	r_desc.aabb.max[2] = surface_data.world_aabb.position.z + surface_data.world_aabb.size.z;

	// Row-major 3x4: rows from basis columns, translation in the 4th column.
	r_desc.transform[0] = xform.basis[0].x;
	r_desc.transform[1] = xform.basis[1].x;
	r_desc.transform[2] = xform.basis[2].x;
	r_desc.transform[3] = xform.origin.x;
	r_desc.transform[4] = xform.basis[0].y;
	r_desc.transform[5] = xform.basis[1].y;
	r_desc.transform[6] = xform.basis[2].y;
	r_desc.transform[7] = xform.origin.y;
	r_desc.transform[8] = xform.basis[0].z;
	r_desc.transform[9] = xform.basis[1].z;
	r_desc.transform[10] = xform.basis[2].z;
	r_desc.transform[11] = xform.origin.z;

	r_desc.indexFormat = (surface_data.index_stride == 2) ? FFX_INDEX_TYPE_UINT16 : FFX_INDEX_TYPE_UINT32;
	r_desc.indexBuffer = index_buffer_index;
	r_desc.indexBufferOffset = 0;
	r_desc.triangleCount = surface_data.index_count / 3;

	r_desc.vertexBuffer = vertex_buffer_index;
	r_desc.vertexStride = surface_data.vertex_stride;
	r_desc.vertexBufferOffset = 0;
	r_desc.vertexCount = surface_data.vertex_count;
	r_desc.vertexFormat = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;

	r_desc.flags = dynamic ? FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC : FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
	r_desc.outInstanceID = out_id;
}

} // namespace

struct BrixelizerManager::Impl {
	struct BufferEntry {
		uint32_t index = 0;
		uint32_t refcount = 0;
		RID storage_copy;
	};

	FfxInterface backend_interface = {};
	FfxDevice device = nullptr;
	void *scratch_buffer = nullptr;
	size_t scratch_buffer_size = 0;
	RID update_scratch_buffer;
	size_t update_scratch_size = 0;
	FfxResource update_scratch_resource = {};
	uint32_t max_references = 0;
	uint32_t triangle_swap_size = 0;
	uint32_t max_bricks_per_bake = 0;
	FfxBrixelizerPopulateDebugAABBsFlags debug_flags = FFX_BRIXELIZER_POPULATE_AABBS_NONE;
	bool use_debug_desc = false;
	FfxBrixelizerDebugVisualizationDescription debug_desc = {};
	size_t max_contexts = 1;
	FfxBrixelizerContextDescription desc = {};
	FfxBrixelizerContext context = {};
	bool context_initialized = false;
	HashMap<RID, BufferEntry> buffer_entries;
	FfxBrixelizerResources resources = {};
	RID sdf_atlas;
	RID brick_aabbs;
	Vector<RID> cascade_aabb_trees;
	Vector<RID> cascade_brick_maps;
	bool sdf_atlas_in_general = false;
};

BrixelizerManager::BrixelizerManager() {
	impl = memnew(Impl);
}

BrixelizerManager::~BrixelizerManager() {
	destroyContext();
	memdelete(impl);
	impl = nullptr;
}

int BrixelizerManager::initContext() {
	if (!impl || impl->context_initialized) {
		return FFX_OK;
	}

	// Init the vkDeviceContext from the Ffx sdk.
	VkDeviceContext vk_device_context = {};
	vk_device_context.vkDevice = (VkDevice)RD::get_singleton()->get_driver_resource(
			RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	vk_device_context.vkPhysicalDevice = (VkPhysicalDevice)RD::get_singleton()->get_driver_resource(
			RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);

	if (vk_device_context.vkDevice == VK_NULL_HANDLE || vk_device_context.vkPhysicalDevice == VK_NULL_HANDLE) {
		return FFX_ERROR_NULL_DEVICE;
	}

	// Use the global function pointer initialized by volk.
	if (vkGetDeviceProcAddr == nullptr) {
		return FFX_ERROR_INCOMPLETE_INTERFACE;
	}
#ifdef USE_VOLK
	if (vkEnumerateDeviceExtensionProperties == nullptr) {
		return FFX_ERROR_INCOMPLETE_INTERFACE;
	}
#endif
	vk_device_context.vkDeviceProcAddr = vkGetDeviceProcAddr;

	impl->device = ffxGetDeviceVK(&vk_device_context);

	impl->scratch_buffer_size = ffxGetScratchMemorySizeVK(vk_device_context.vkPhysicalDevice, impl->max_contexts);
	impl->scratch_buffer = calloc(impl->scratch_buffer_size, 1);
	if (!impl->scratch_buffer) {
		return FFX_ERROR_OUT_OF_MEMORY;
	}

	impl->backend_interface = {};
	ffxGetInterfaceVK(
			&impl->backend_interface,
			impl->device,
			impl->scratch_buffer,
			impl->scratch_buffer_size,
			impl->max_contexts);

	impl->desc.backendInterface = impl->backend_interface;
	impl->desc.sdfCenter[0] = 0.0f;
	impl->desc.sdfCenter[1] = 0.0f;
	impl->desc.sdfCenter[2] = 0.0f;
	impl->desc.numCascades = 8;
	impl->desc.flags = (FfxBrixelizerContextFlags)0;
	// Tuning note: these budgets are conservative placeholders and should be revisited per scene scale.
	impl->max_references = 1000000;
	impl->triangle_swap_size = 1000000;
	impl->max_bricks_per_bake = 100000;
	impl->debug_flags = FFX_BRIXELIZER_POPULATE_AABBS_NONE;
	impl->use_debug_desc = false;
	impl->debug_desc = {};

	for (uint32_t i = 0; i < impl->desc.numCascades; ++i) {
		impl->desc.cascadeDescs[i].flags = FFX_BRIXELIZER_CASCADE_DYNAMIC;
		impl->desc.cascadeDescs[i].voxelSize = 0.1f * powf(2.0f, (float)i);
	}

	FfxErrorCode error = ffxBrixelizerContextCreate(&impl->desc, &impl->context);
	impl->context_initialized = (error == FFX_OK);
	if (!impl->context_initialized) {
		return error;
	}

	if (!impl->sdf_atlas.is_valid()) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8_UNORM;
		tf.width = 512;
		tf.height = 512;
		tf.depth = 512;
		tf.texture_type = RD::TEXTURE_TYPE_3D;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

		RD::TextureView tv;
		impl->sdf_atlas = RD::get_singleton()->texture_create(tf, tv);
		if (impl->sdf_atlas.is_valid()) {
			// Ensure a defined layout before any FFX access.
			RD::get_singleton()->texture_clear(impl->sdf_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
			impl->sdf_atlas_in_general = false;
		}
	}
	ERR_FAIL_COND_V(!impl->sdf_atlas.is_valid(), FFX_ERROR_OUT_OF_MEMORY);

	if (!impl->brick_aabbs.is_valid()) {
		const uint32_t brick_count = FFX_BRIXELIZER_CASCADE_RESOLUTION * FFX_BRIXELIZER_CASCADE_RESOLUTION * FFX_BRIXELIZER_CASCADE_RESOLUTION;
		impl->brick_aabbs = RD::get_singleton()->storage_buffer_create(brick_count * sizeof(uint32_t));
	}
	ERR_FAIL_COND_V(!impl->brick_aabbs.is_valid(), FFX_ERROR_OUT_OF_MEMORY);

	// Add debug name to identify buffer in validation layer output.
	RD::get_singleton()->set_resource_name(impl->brick_aabbs, "Brixelizer_BrickAABBs");

	impl->cascade_aabb_trees.resize(impl->desc.numCascades);
	impl->cascade_brick_maps.resize(impl->desc.numCascades);
	for (uint32_t i = 0; i < impl->desc.numCascades; ++i) {
		if (!impl->cascade_aabb_trees[i].is_valid()) {
			impl->cascade_aabb_trees.write[i] = RD::get_singleton()->storage_buffer_create(FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE);
		}
		if (!impl->cascade_brick_maps[i].is_valid()) {
			impl->cascade_brick_maps.write[i] = RD::get_singleton()->storage_buffer_create(FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
		}
		ERR_FAIL_COND_V(!impl->cascade_aabb_trees[i].is_valid(), FFX_ERROR_OUT_OF_MEMORY);
		ERR_FAIL_COND_V(!impl->cascade_brick_maps[i].is_valid(), FFX_ERROR_OUT_OF_MEMORY);

		// Add debug names to identify buffers in validation layer output.
		RD::get_singleton()->set_resource_name(impl->cascade_aabb_trees[i], vformat("Brixelizer_CascadeAABBTree_%d", i));
		RD::get_singleton()->set_resource_name(impl->cascade_brick_maps[i], vformat("Brixelizer_CascadeBrickMap_%d", i));
	}

	{
		FfxResourceDescription desc = {};
		desc.type = FFX_RESOURCE_TYPE_TEXTURE3D;
		desc.format = FFX_SURFACE_FORMAT_R8_UNORM;
		desc.width = 512;
		desc.height = 512;
		desc.depth = 512;
		desc.mipCount = 1;
		desc.usage = FFX_RESOURCE_USAGE_UAV;
		desc.flags = FFX_RESOURCE_FLAGS_NONE;

		uint64_t handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, impl->sdf_atlas);
		impl->resources.sdfAtlas = ffxGetResourceVK((void *)handle, desc, L"BrixelizerSdfAtlas", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	{
		const uint32_t brick_count = FFX_BRIXELIZER_CASCADE_RESOLUTION * FFX_BRIXELIZER_CASCADE_RESOLUTION * FFX_BRIXELIZER_CASCADE_RESOLUTION;
		FfxResourceDescription desc = {};
		desc.type = FFX_RESOURCE_TYPE_BUFFER;
		desc.size = brick_count * sizeof(uint32_t);
		desc.stride = sizeof(uint32_t);
		desc.format = FFX_SURFACE_FORMAT_UNKNOWN;
		desc.usage = FFX_RESOURCE_USAGE_UAV;
		desc.flags = FFX_RESOURCE_FLAGS_NONE;

		uint64_t handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, impl->brick_aabbs);
		impl->resources.brickAABBs = ffxGetResourceVK((void *)handle, desc, L"BrixelizerBrickAABBs", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	for (uint32_t i = 0; i < impl->desc.numCascades; ++i) {
		FfxResourceDescription tree_desc = {};
		tree_desc.type = FFX_RESOURCE_TYPE_BUFFER;
		tree_desc.size = FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE;
		tree_desc.stride = FFX_BRIXELIZER_CASCADE_AABB_TREE_STRIDE;
		tree_desc.format = FFX_SURFACE_FORMAT_UNKNOWN;
		tree_desc.usage = FFX_RESOURCE_USAGE_UAV;
		tree_desc.flags = FFX_RESOURCE_FLAGS_NONE;

		uint64_t tree_handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, impl->cascade_aabb_trees[i]);
		impl->resources.cascadeResources[i].aabbTree = ffxGetResourceVK((void *)tree_handle, tree_desc, L"BrixelizerAabbTree", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

		FfxResourceDescription map_desc = {};
		map_desc.type = FFX_RESOURCE_TYPE_BUFFER;
		map_desc.size = FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE;
		map_desc.stride = FFX_BRIXELIZER_CASCADE_BRICK_MAP_STRIDE;
		map_desc.format = FFX_SURFACE_FORMAT_UNKNOWN;
		map_desc.usage = FFX_RESOURCE_USAGE_UAV;
		map_desc.flags = FFX_RESOURCE_FLAGS_NONE;

		uint64_t map_handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, impl->cascade_brick_maps[i]);
		impl->resources.cascadeResources[i].brickMap = ffxGetResourceVK((void *)map_handle, map_desc, L"BrixelizerBrickMap", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	return error;
}

int BrixelizerManager::destroyContext() {
	if (!impl || !impl->context_initialized) {
		return FFX_OK;
	}

	FfxErrorCode error = ffxBrixelizerContextDestroy(&impl->context);
	impl->context_initialized = false;

	if (impl->scratch_buffer) {
		free(impl->scratch_buffer);
		impl->scratch_buffer = nullptr;
	}
	if (impl->update_scratch_buffer.is_valid()) {
		RD::get_singleton()->free_rid(impl->update_scratch_buffer);
		impl->update_scratch_buffer = RID();
		impl->update_scratch_size = 0;
		impl->update_scratch_resource = {};
	}
	if (impl->sdf_atlas.is_valid()) {
		RD::get_singleton()->free_rid(impl->sdf_atlas);
		impl->sdf_atlas = RID();
		impl->sdf_atlas_in_general = false;
	}
	if (impl->brick_aabbs.is_valid()) {
		RD::get_singleton()->free_rid(impl->brick_aabbs);
		impl->brick_aabbs = RID();
	}
	for (KeyValue<RID, Impl::BufferEntry> &entry : impl->buffer_entries) {
		if (entry.value.storage_copy.is_valid()) {
			RD::get_singleton()->free_rid(entry.value.storage_copy);
			entry.value.storage_copy = RID();
		}
	}
	impl->buffer_entries.clear();
	for (int i = 0; i < impl->cascade_aabb_trees.size(); i++) {
		if (impl->cascade_aabb_trees[i].is_valid()) {
			RD::get_singleton()->free_rid(impl->cascade_aabb_trees[i]);
		}
	}
	for (int i = 0; i < impl->cascade_brick_maps.size(); i++) {
		if (impl->cascade_brick_maps[i].is_valid()) {
			RD::get_singleton()->free_rid(impl->cascade_brick_maps[i]);
		}
	}
	impl->cascade_aabb_trees.clear();
	impl->cascade_brick_maps.clear();
	impl->resources = {};

	return error;
}

void BrixelizerManager::shutdown() {
	if (!impl) {
		return;
	}

#ifdef VULKAN_ENABLED
	{
		VkDevice vk_device = (VkDevice)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
		if (vk_device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(vk_device);
		}
	}
#endif

	if (impl->context_initialized) {
		LocalVector<FfxBrixelizerInstanceID> instance_ids;
		for (KeyValue<RID, BrixelizerInstanceInfo> &entry : brixelizer_instances) {
			for (int i = 0; i < entry.value.surfaces.size(); i++) {
				if (entry.value.surfaces[i].instance_id != BRIXELIZER_INVALID_ID) {
					instance_ids.push_back(entry.value.surfaces[i].instance_id);
				}
			}
		}
		if (!instance_ids.is_empty()) {
			ffxBrixelizerDeleteInstances(&impl->context, instance_ids.ptr(), instance_ids.size());
		}

		for (KeyValue<RID, Impl::BufferEntry> &entry : impl->buffer_entries) {
			uint32_t index = entry.value.index;
			ffxBrixelizerUnregisterBuffers(&impl->context, &index, 1);
			if (entry.value.storage_copy.is_valid()) {
				RD::get_singleton()->free_rid(entry.value.storage_copy);
				entry.value.storage_copy = RID();
			}
		}
	}

	brixelizer_instances.clear();
	pending_updates.clear();
	pending_deletes.clear();
	impl->buffer_entries.clear();

	destroyContext();
}

void BrixelizerManager::addInstance(RID instance_rid, RID base_rid, const Transform3D &xform, const AABB &world_aabb, bool dynamic) {
	if (initContext() != FFX_OK) {
		return;
	}

	pending_updates.erase(instance_rid);
	pending_deletes.erase(instance_rid);

	if (brixelizer_instances.has(instance_rid)) {
		return;
	}

	BrixelizerInstanceInfo instance_info = {};
	instance_info.instance_rid = instance_rid;
	instance_info.base_rid = base_rid;
	instance_info.dynamic = dynamic;

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	if (!base_rid.is_valid() || !mesh_storage->owns_mesh(base_rid)) {
		return;
	}

	uint32_t surface_count = mesh_storage->mesh_get_surface_count(base_rid);
	if (surface_count == 0) {
		return;
	}
	instance_info.surfaces.resize(surface_count);
	bool created_any = false;

	auto register_buffer = [&](RID buffer_rid, uint32_t buffer_size, uint32_t buffer_stride, uint32_t &r_index) -> bool {
		const bool use_storage_buffer_copy = true; // Test path: ensure storage-buffer usage for FFX descriptors.
		if (!buffer_rid.is_valid() || buffer_size == 0 || buffer_stride == 0) {
			return false;
		}

		if (HashMap<RID, Impl::BufferEntry>::Iterator it = impl->buffer_entries.find(buffer_rid)) {
			it->value.refcount++;
			r_index = it->value.index;
			return true;
		}

		RID buffer_for_ffx = buffer_rid;
		if (use_storage_buffer_copy) {
			RID storage_copy = RD::get_singleton()->storage_buffer_create(buffer_size);
			ERR_FAIL_COND_V(!storage_copy.is_valid(), false);
			Error copy_err = RD::get_singleton()->buffer_copy(buffer_rid, storage_copy, 0, 0, buffer_size);
			if (copy_err != OK) {
				RD::get_singleton()->free_rid(storage_copy);
				return false;
			}
			buffer_for_ffx = storage_copy;
		}

		uint64_t handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, buffer_for_ffx);
		if (handle == 0) {
			if (use_storage_buffer_copy && buffer_for_ffx != buffer_rid) {
				RD::get_singleton()->free_rid(buffer_for_ffx);
			}
			return false;
		}

		FfxResourceDescription desc = {};
		desc.type = FFX_RESOURCE_TYPE_BUFFER;
		desc.size = buffer_size;
		desc.stride = buffer_stride;
		desc.format = FFX_SURFACE_FORMAT_UNKNOWN;
		desc.usage = FFX_RESOURCE_USAGE_READ_ONLY;
		desc.flags = FFX_RESOURCE_FLAGS_NONE;

		FfxResource res = ffxGetResourceVK((void *)handle, desc, L"BrixelizerBuffer");

		FfxBrixelizerBufferDescription bdesc = {};
		bdesc.buffer = res;
		bdesc.outIndex = &r_index;

		FfxErrorCode error = ffxBrixelizerRegisterBuffers(&impl->context, &bdesc, 1);
		if (error != FFX_OK) {
			return false;
		}

		Impl::BufferEntry entry;
		entry.index = r_index;
		entry.refcount = 1;
		if (use_storage_buffer_copy) {
			entry.storage_copy = buffer_for_ffx;
		}
		impl->buffer_entries.insert(buffer_rid, entry);
		return true;
	};
	auto unref_buffer = [&](RID buffer_rid) {
		if (!buffer_rid.is_valid()) {
			return;
		}
		HashMap<RID, Impl::BufferEntry>::Iterator it = impl->buffer_entries.find(buffer_rid);
		if (!it) {
			return;
		}
		if (--it->value.refcount == 0) {
			uint32_t index = it->value.index;
			ffxBrixelizerUnregisterBuffers(&impl->context, &index, 1);
			if (it->value.storage_copy.is_valid()) {
				RD::get_singleton()->free_rid(it->value.storage_copy);
				it->value.storage_copy = RID();
			}
			impl->buffer_entries.erase(buffer_rid);
		}
	};

	for (uint32_t surface_index = 0; surface_index < surface_count; surface_index++) {
		void *surface = mesh_storage->mesh_get_surface(base_rid, surface_index);
		ERR_CONTINUE(surface == nullptr);

		BrixelizerSurfaceData surface_data;
		if (!brixelizer_get_surface_data(mesh_storage, surface, surface_index, xform, world_aabb, surface_data)) {
			continue;
		}

		uint32_t vertex_buffer_index = 0;
		uint32_t index_buffer_index = 0;
		if (!register_buffer(surface_data.vertex_buffer, surface_data.vertex_buffer_size, surface_data.vertex_stride, vertex_buffer_index)) {
			continue;
		}
		if (!register_buffer(surface_data.index_buffer, surface_data.index_buffer_size, surface_data.index_stride, index_buffer_index)) {
			unref_buffer(surface_data.vertex_buffer);
			continue;
		}

		FfxBrixelizerInstanceDescription instance_desc = {};
		BrixelizerInstanceInfo::SurfaceInfo &surface_info = instance_info.surfaces.write[surface_index];
		surface_info.instance_id = BRIXELIZER_INVALID_ID;
		brixelizer_fill_instance_desc(instance_desc, surface_data, xform, vertex_buffer_index, index_buffer_index, instance_info.dynamic, &surface_info.instance_id);

		FfxErrorCode create_error = ffxBrixelizerCreateInstances(&impl->context, &instance_desc, 1);
		if (create_error != FFX_OK) {
			unref_buffer(surface_data.vertex_buffer);
			unref_buffer(surface_data.index_buffer);
			continue;
		}

		surface_info.vertex_buffer = surface_data.vertex_buffer;
		surface_info.index_buffer = surface_data.index_buffer;
		created_any = true;
	}

	if (!created_any) {
		return;
	}

	brixelizer_instances.insert(instance_rid, instance_info);
}

void BrixelizerManager::deleteInstance(RID instance_rid) {
	if (!impl || !impl->context_initialized) {
		return;
	}

	if (!brixelizer_instances.has(instance_rid)) {
		return;
	}

	pending_updates.erase(instance_rid);
	pending_deletes.insert(instance_rid);
}

void BrixelizerManager::updateInstance(RID instance_rid, const Transform3D &xform, const AABB &world_aabb) {
	if (!impl || !impl->context_initialized) {
		return;
	}

	if (!brixelizer_instances.has(instance_rid)) {
		return;
	}

	PendingUpdate pending;
	pending.xform = xform;
	pending.world_aabb = world_aabb;
	pending_updates.insert(instance_rid, pending);
	pending_deletes.erase(instance_rid);
}

FfxCommandList BrixelizerManager::getCommandList(VkCommandBuffer *out_vk_cmd_buffer) const {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, nullptr);

#ifdef VULKAN_ENABLED
	RenderingDeviceDriver *driver = rd->get_device_driver();
	if (driver == nullptr || driver->get_api_name() != "Vulkan") {
		return nullptr;
	}

	RDD::CommandBufferID cmd_buffer = rd->get_current_command_buffer();
	if (!cmd_buffer) {
		return nullptr;
	}

	RenderingDeviceDriverVulkan *vk_driver = static_cast<RenderingDeviceDriverVulkan *>(driver);
	VkCommandBuffer vk_cmd_buffer = vk_driver->command_buffer_get_vk(cmd_buffer);
	if (vk_cmd_buffer == VK_NULL_HANDLE) {
		return nullptr;
	}
	if (out_vk_cmd_buffer != nullptr) {
		*out_vk_cmd_buffer = vk_cmd_buffer;
	}

	return ffxGetCommandListVK(vk_cmd_buffer);
#else
	if (out_vk_cmd_buffer != nullptr) {
		*out_vk_cmd_buffer = VK_NULL_HANDLE;
	}
	return nullptr;
#endif
}

FfxErrorCode BrixelizerManager::frameUpdate(const Vector3 &sdf_center, size_t *out_scratch_size, FfxBrixelizerStats *out_stats) {
	if (!impl || !impl->context_initialized) {
		return FFX_ERROR_INVALID_POINTER;
	}

	VkCommandBuffer vk_cmd_buffer = VK_NULL_HANDLE;
	FfxCommandList command_list = getCommandList(&vk_cmd_buffer);
	if (command_list == nullptr) {
		return FFX_ERROR_INVALID_POINTER;
	}

	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, FFX_ERROR_INVALID_POINTER);
	const uint32_t frame_index = (uint32_t)rd->get_frames_drawn();

	if (!pending_deletes.is_empty()) {
		for (const RID &rid : pending_deletes) {
			_deleteInstanceInternal(rid);
		}
		pending_deletes.clear();
	}

	if (!pending_updates.is_empty()) {
		for (KeyValue<RID, PendingUpdate> &entry : pending_updates) {
			_updateInstanceInternal(entry.key, entry.value.xform, entry.value.world_aabb);
		}
		pending_updates.clear();
	}

	FfxBrixelizerUpdateDescription desc = {};
	desc.resources = impl->resources;
	desc.frameIndex = frame_index;
	desc.sdfCenter[0] = sdf_center.x;
	desc.sdfCenter[1] = sdf_center.y;
	desc.sdfCenter[2] = sdf_center.z;
	desc.populateDebugAABBsFlags = impl->debug_flags;
	desc.debugVisualizationDesc = impl->use_debug_desc ? &impl->debug_desc : nullptr;
	desc.maxReferences = impl->max_references;
	desc.triangleSwapSize = impl->triangle_swap_size;
	desc.maxBricksPerBake = impl->max_bricks_per_bake;
	size_t local_scratch_size = 0;
	desc.outScratchBufferSize = out_scratch_size ? out_scratch_size : &local_scratch_size;
	desc.outStats = out_stats;


	FfxBrixelizerBakedUpdateDescription baked_desc = {};
	FfxErrorCode error = ffxBrixelizerBakeUpdate(&impl->context, &desc, &baked_desc);
	if (error != FFX_OK) {
		return error;
	}

	const size_t required_scratch_size = out_scratch_size ? *out_scratch_size : local_scratch_size;
	if (required_scratch_size == 0) {
		return FFX_ERROR_INVALID_POINTER;
	}

	if (!impl->update_scratch_buffer.is_valid() || impl->update_scratch_size < required_scratch_size) {
		if (impl->update_scratch_buffer.is_valid()) {
			RD::get_singleton()->free_rid(impl->update_scratch_buffer);
		}
		impl->update_scratch_buffer = RD::get_singleton()->storage_buffer_create(required_scratch_size);
		impl->update_scratch_size = required_scratch_size;

		// Add debug name to identify buffer in validation layer output.
		RD::get_singleton()->set_resource_name(impl->update_scratch_buffer, "Brixelizer_UpdateScratchBuffer");

		FfxResourceDescription scratch_desc = {};
		scratch_desc.type = FFX_RESOURCE_TYPE_BUFFER;
		scratch_desc.size = required_scratch_size;
		scratch_desc.stride = sizeof(uint32_t);
		scratch_desc.format = FFX_SURFACE_FORMAT_UNKNOWN;
		scratch_desc.usage = FFX_RESOURCE_USAGE_UAV;
		scratch_desc.flags = FFX_RESOURCE_FLAGS_NONE;

		uint64_t handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, impl->update_scratch_buffer);
		impl->update_scratch_resource = ffxGetResourceVK((void *)handle, scratch_desc, L"BrixelizerUpdateScratch", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	}

#ifdef VULKAN_ENABLED
	if (impl->sdf_atlas.is_valid() && !impl->sdf_atlas_in_general && vk_cmd_buffer != VK_NULL_HANDLE) {
		VkImage vk_image = (VkImage)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, impl->sdf_atlas);
		if (vk_image != VK_NULL_HANDLE) {
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = vk_image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(
					vk_cmd_buffer,
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					0,
					0,
					nullptr,
					0,
					nullptr,
					1,
					&barrier);
			impl->sdf_atlas_in_general = true;
		}
	}
#endif
	FfxErrorCode err = ffxBrixelizerUpdate(&impl->context, &baked_desc, impl->update_scratch_resource, command_list);
	return err;
}

void BrixelizerManager::_deleteInstanceInternal(RID instance_rid) {
	HashMap<RID, BrixelizerInstanceInfo>::Iterator it = brixelizer_instances.find(instance_rid);
	if (!it) {
		return;
	}

	const Vector<BrixelizerInstanceInfo::SurfaceInfo> &surfaces = it->value.surfaces;
	LocalVector<FfxBrixelizerInstanceID> valid_ids;
	valid_ids.reserve(surfaces.size());

	auto unref_buffer = [&](RID buffer_rid) {
		if (!buffer_rid.is_valid()) {
			return;
		}
		HashMap<RID, Impl::BufferEntry>::Iterator bit = impl->buffer_entries.find(buffer_rid);
		if (!bit) {
			return;
		}
		if (--bit->value.refcount == 0) {
			uint32_t index = bit->value.index;
			ffxBrixelizerUnregisterBuffers(&impl->context, &index, 1);
			if (bit->value.storage_copy.is_valid()) {
				RD::get_singleton()->free_rid(bit->value.storage_copy);
				bit->value.storage_copy = RID();
			}
			impl->buffer_entries.erase(buffer_rid);
		}
	};

	for (int i = 0; i < surfaces.size(); i++) {
		if (surfaces[i].instance_id != BRIXELIZER_INVALID_ID) {
			valid_ids.push_back(surfaces[i].instance_id);
		}
		unref_buffer(surfaces[i].vertex_buffer);
		unref_buffer(surfaces[i].index_buffer);
	}
	if (!valid_ids.is_empty()) {
		ffxBrixelizerDeleteInstances(&impl->context, valid_ids.ptr(), valid_ids.size());
	}

	brixelizer_instances.erase(instance_rid);
}

void BrixelizerManager::_updateInstanceInternal(RID instance_rid, const Transform3D &xform, const AABB &world_aabb) {
	HashMap<RID, BrixelizerInstanceInfo>::Iterator it = brixelizer_instances.find(instance_rid);
	if (!it) {
		return;
	}

	BrixelizerInstanceInfo &info = it->value;
	if (!info.dynamic) {
		_deleteInstanceInternal(instance_rid);
		addInstance(info.instance_rid, info.base_rid, xform, world_aabb, info.dynamic);
		return;
	}

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);

	uint32_t surface_count = mesh_storage->mesh_get_surface_count(info.base_rid);
	if (surface_count == 0 || surface_count != (uint32_t)info.surfaces.size()) {
		// TODO: Track per-surface instances to handle meshes whose surface counts change at runtime.
		_deleteInstanceInternal(instance_rid);
		addInstance(info.instance_rid, info.base_rid, xform, world_aabb, info.dynamic);
		return;
	}

	auto find_buffer_index = [&](RID buffer_rid, uint32_t &r_index) -> bool {
		if (!buffer_rid.is_valid()) {
			return false;
		}
		if (HashMap<RID, Impl::BufferEntry>::Iterator bit = impl->buffer_entries.find(buffer_rid)) {
			r_index = bit->value.index;
			return true;
		}
		return false;
	};

	for (uint32_t surface_index = 0; surface_index < surface_count; surface_index++) {
		void *surface = mesh_storage->mesh_get_surface(info.base_rid, surface_index);
		ERR_CONTINUE(surface == nullptr);

		BrixelizerSurfaceData surface_data;
		if (!brixelizer_get_surface_data(mesh_storage, surface, surface_index, xform, world_aabb, surface_data)) {
			continue;
		}

		uint32_t vertex_buffer_index = 0;
		uint32_t index_buffer_index = 0;
		if (!find_buffer_index(surface_data.vertex_buffer, vertex_buffer_index) ||
				!find_buffer_index(surface_data.index_buffer, index_buffer_index)) {
			_deleteInstanceInternal(instance_rid);
			addInstance(info.instance_rid, info.base_rid, xform, world_aabb, info.dynamic);
			return;
		}

		FfxBrixelizerInstanceDescription instance_desc = {};
		brixelizer_fill_instance_desc(instance_desc, surface_data, xform, vertex_buffer_index, index_buffer_index, true, nullptr);

		ffxBrixelizerCreateInstances(&impl->context, &instance_desc, 1);
	}
}
