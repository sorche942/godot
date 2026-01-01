#define VK_NO_PROTOTYPES    // ← This is the key line
#include "brixelizer_manager.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"

#include "core/templates/local_vector.h"

#ifndef USE_VOLK
#define USE_VOLK
#endif
#include "drivers/vulkan/godot_vulkan.h"

#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <FidelityFX/host/ffx_error.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

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
	const uint64_t surface_format = mesh_storage->mesh_surface_get_format(surface);
	const RS::PrimitiveType primitive = mesh_storage->mesh_surface_get_primitive(surface);
	if (primitive != RS::PRIMITIVE_TRIANGLES) {
#ifdef DEBUG_ENABLED
		WARN_PRINT("Brixelizer: skipping surface " + itos(surface_index) + " (non-triangle primitive=" + itos((int)primitive) + ").");
#endif
		return false;
	}
	if (surface_format & RS::ARRAY_FLAG_USE_2D_VERTICES) {
#ifdef DEBUG_ENABLED
		WARN_PRINT("Brixelizer: skipping surface " + itos(surface_index) + " (2D vertices).");
#endif
		// TODO: Add a path for 2D vertex surfaces if needed for Brixelizer ingestion.
		return false;
	}
	if (surface_format & RS::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
#ifdef DEBUG_ENABLED
		WARN_PRINT("Brixelizer: skipping surface " + itos(surface_index) + " (compressed attributes).");
#endif
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
	};

	FfxInterface backend_interface = {};
	FfxDevice device = nullptr;
	void *scratch_buffer = nullptr;
	size_t scratch_buffer_size = 0;
	size_t max_contexts = 1;
	FfxBrixelizerContextDescription desc = {};
	FfxBrixelizerContext context = {};
	bool context_initialized = false;
	HashMap<RID, BufferEntry> buffer_entries;
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

	static bool logged_ptrs = false;

	// Init the vkDeviceContext from the Ffx sdk.
	VkDeviceContext vk_device_context = {};
	vk_device_context.vkDevice = (VkDevice)RD::get_singleton()->get_driver_resource(
			RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	vk_device_context.vkPhysicalDevice = (VkPhysicalDevice)RD::get_singleton()->get_driver_resource(
			RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);

	if (vk_device_context.vkDevice == VK_NULL_HANDLE || vk_device_context.vkPhysicalDevice == VK_NULL_HANDLE) {
		WARN_PRINT("Brixelizer: Vulkan device handles are not ready yet, skipping init.");
		return FFX_ERROR_NULL_DEVICE;
	}

	// Use the global function pointer initialized by volk.
	if (vkGetDeviceProcAddr == nullptr) {
		WARN_PRINT("Brixelizer: vkGetDeviceProcAddr not initialized (volk not ready), skipping init.");
		return FFX_ERROR_INCOMPLETE_INTERFACE;
	}
#ifdef USE_VOLK
	if (vkEnumerateDeviceExtensionProperties == nullptr) {
		WARN_PRINT("Brixelizer: vkEnumerateDeviceExtensionProperties not initialized (volk not ready), skipping init.");
		return FFX_ERROR_INCOMPLETE_INTERFACE;
	}
#endif
	if (!logged_ptrs) {
		String vk_ptrs = "Brixelizer init pointers: vkGetDeviceProcAddr=0x" +
				String::num_uint64((uint64_t)vkGetDeviceProcAddr, 16) +
				" vkEnumerateDeviceExtensionProperties=0x" +
				String::num_uint64((uint64_t)vkEnumerateDeviceExtensionProperties, 16);
		ERR_PRINT(vk_ptrs);
#ifdef USE_VOLK
		String vk_handles = "Brixelizer handles: vkDevice=0x" +
				String::num_uint64((uint64_t)vk_device_context.vkDevice, 16) +
				" vkPhysicalDevice=0x" +
				String::num_uint64((uint64_t)vk_device_context.vkPhysicalDevice, 16) +
				" volkLoadedInstance=0x" +
				String::num_uint64((uint64_t)volkGetLoadedInstance(), 16) +
				" volkLoadedDevice=0x" +
				String::num_uint64((uint64_t)volkGetLoadedDevice(), 16);
		ERR_PRINT(vk_handles);
#endif
		logged_ptrs = true;
	}
	vk_device_context.vkDeviceProcAddr = vkGetDeviceProcAddr;

	impl->device = ffxGetDeviceVK(&vk_device_context);

#ifdef DEBUG_ENABLED
	ERR_PRINT("Brixelizer: probing vkGetPhysicalDeviceProperties.");
	VkPhysicalDeviceProperties device_props = {};
	vkGetPhysicalDeviceProperties(vk_device_context.vkPhysicalDevice, &device_props);
	ERR_PRINT("Brixelizer: physical device name: " + String::utf8(device_props.deviceName));

	ERR_PRINT("Brixelizer: probing vkEnumerateDeviceExtensionProperties.");
	uint32_t ext_count = 0;
	VkResult ext_res = vkEnumerateDeviceExtensionProperties(vk_device_context.vkPhysicalDevice, nullptr, &ext_count, nullptr);
	ERR_PRINT("Brixelizer: vkEnumerateDeviceExtensionProperties result=" + itos((int)ext_res) + " count=" + itos((int)ext_count));
#endif

	ERR_PRINT("Brixelizer: calling ffxGetScratchMemorySizeVK.");
	impl->scratch_buffer_size = ffxGetScratchMemorySizeVK(vk_device_context.vkPhysicalDevice, impl->max_contexts);
	ERR_PRINT("Brixelizer: scratch size=" + String::num_uint64(impl->scratch_buffer_size, 10));
	impl->scratch_buffer = calloc(impl->scratch_buffer_size, 1);
	if (!impl->scratch_buffer) {
		ERR_PRINT("Brixelizer: scratch buffer allocation failed.");
		return FFX_ERROR_OUT_OF_MEMORY;
	}

	impl->backend_interface = {};
	ERR_PRINT("Brixelizer: calling ffxGetInterfaceVK.");
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

	for (uint32_t i = 0; i < impl->desc.numCascades; ++i) {
    impl->desc.cascadeDescs[i].flags = FFX_BRIXELIZER_CASCADE_STATIC;  // Or DYNAMIC, or both — match your scene needs
    impl->desc.cascadeDescs[i].voxelSize = 0.1f * powf(2.0f, (float)i);  // Example: 0.1, 0.2, 0.4, 0.8, ... up to ~12.8 for cascade 7
    // Finer voxels in lower cascades, coarser in higher
}

	ERR_PRINT("Brixelizer: calling ffxBrixelizerContextCreate.");
	FfxErrorCode error = ffxBrixelizerContextCreate(&impl->desc, &impl->context);
	ERR_PRINT("Brixelizer: ffxBrixelizerContextCreate result=" + itos((int)error));
	impl->context_initialized = (error == FFX_OK);
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

	return error;
}

void BrixelizerManager::addInstance(RID instance_rid, RID base_rid, const Transform3D &xform, const AABB &world_aabb, bool dynamic) {
	if (initContext() != FFX_OK) {
		return;
	}

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
#ifdef DEBUG_ENABLED
		WARN_PRINT("Brixelizer: base RID is not a valid Mesh, skipping instance.");
#endif
		return;
	}

	uint32_t surface_count = mesh_storage->mesh_get_surface_count(base_rid);
	if (surface_count == 0) {
		return;
	}
	instance_info.surfaces.resize(surface_count);
	bool created_any = false;
	uint32_t created_count = 0;

	auto register_buffer = [&](RID buffer_rid, uint32_t buffer_size, uint32_t buffer_stride, uint32_t &r_index) -> bool {
		if (!buffer_rid.is_valid() || buffer_size == 0 || buffer_stride == 0) {
			return false;
		}

		if (HashMap<RID, Impl::BufferEntry>::Iterator it = impl->buffer_entries.find(buffer_rid)) {
			it->value.refcount++;
			r_index = it->value.index;
			return true;
		}

		uint64_t handle = RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_BUFFER, buffer_rid);
		if (handle == 0) {
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
#ifdef DEBUG_ENABLED
		if (create_error != FFX_OK) {
			WARN_PRINT("Brixelizer: failed to create instance for surface " + itos(surface_index) + " (error " + itos((int)create_error) + ").");
		} else {
			WARN_PRINT("Brixelizer: created instance for surface " + itos(surface_index) + " (instance_id=" + itos((int)surface_info.instance_id) +
					", vertices=" + itos((int)surface_data.vertex_count) + ", triangles=" + itos((int)surface_data.index_count / 3) + ").");
		}
#endif
		if (create_error != FFX_OK) {
			unref_buffer(surface_data.vertex_buffer);
			unref_buffer(surface_data.index_buffer);
			continue;
		}

		surface_info.vertex_buffer = surface_data.vertex_buffer;
		surface_info.index_buffer = surface_data.index_buffer;
		created_any = true;
		created_count++;
	}

	if (!created_any) {
#ifdef DEBUG_ENABLED
		String mesh_path = mesh_storage->mesh_get_path(base_rid);
		if (mesh_path.is_empty()) {
			mesh_path = "<unknown>";
		}
		WARN_PRINT("Brixelizer: no valid triangle surfaces for mesh (path=" + mesh_path + ").");
#endif
		return;
	}

	brixelizer_instances.insert(instance_rid, instance_info);
#ifdef DEBUG_ENABLED
	WARN_PRINT("Brixelizer: instance registered for RID (surfaces=" + itos(surface_count) + ", created=" + itos(created_count) + ").");
#endif
}

void BrixelizerManager::deleteInstance(RID instance_rid) {
	if (!impl || !impl->context_initialized) {
		return;
	}

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

void BrixelizerManager::updateInstance(RID instance_rid, const Transform3D &xform, const AABB &world_aabb) {
	if (!impl || !impl->context_initialized) {
		return;
	}

	HashMap<RID, BrixelizerInstanceInfo>::Iterator it = brixelizer_instances.find(instance_rid);
	if (!it) {
		return;
	}

	BrixelizerInstanceInfo &info = it->value;
	if (!info.dynamic) {
		deleteInstance(instance_rid);
		addInstance(info.instance_rid, info.base_rid, xform, world_aabb, info.dynamic);
		return;
	}

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);

	uint32_t surface_count = mesh_storage->mesh_get_surface_count(info.base_rid);
	if (surface_count == 0 || surface_count != (uint32_t)info.surfaces.size()) {
#ifdef DEBUG_ENABLED
		WARN_PRINT("Brixelizer: dynamic instance surface mismatch; falling back to delete+add.");
#endif
		// TODO: Track per-surface instances to handle meshes whose surface counts change at runtime.
		deleteInstance(instance_rid);
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
#ifdef DEBUG_ENABLED
			WARN_PRINT("Brixelizer: buffer index missing; falling back to delete+add.");
#endif
			deleteInstance(instance_rid);
			addInstance(info.instance_rid, info.base_rid, xform, world_aabb, info.dynamic);
			return;
		}

		FfxBrixelizerInstanceDescription instance_desc = {};
		brixelizer_fill_instance_desc(instance_desc, surface_data, xform, vertex_buffer_index, index_buffer_index, true, nullptr);

		ffxBrixelizerCreateInstances(&impl->context, &instance_desc, 1);
	}
}
