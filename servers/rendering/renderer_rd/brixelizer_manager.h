#pragma once

#include <cstddef>
#include <cstdint>

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/rid.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

typedef struct FfxBrixelizerStats FfxBrixelizerStats;
typedef int FfxErrorCode;
typedef void *FfxCommandList;
typedef struct VkCommandBuffer_T *VkCommandBuffer;

static constexpr uint32_t BRIXELIZER_INVALID_ID = 0x00ffffffu;

struct BrixelizerInstanceInfo {
	RID instance_rid;
	RID base_rid;
	bool dynamic = false;
	struct SurfaceInfo {
		uint32_t instance_id = BRIXELIZER_INVALID_ID;
		RID vertex_buffer;
		RID index_buffer;
	};
	Vector<SurfaceInfo> surfaces;
};

struct BrixelizerManager {
	struct Impl;
	Impl *impl = nullptr;

	struct PendingUpdate {
		Transform3D xform;
		AABB world_aabb;
	};

	// The keys are the instance_rids;
	HashMap<RID, BrixelizerInstanceInfo> brixelizer_instances;
	HashMap<RID, PendingUpdate> pending_updates;
	HashSet<RID> pending_deletes;

	BrixelizerManager();
	~BrixelizerManager();

	int initContext();
	int destroyContext();

	void addInstance(RID instance_rid, RID base_rid, const Transform3D &xform, const AABB &world_aabb, bool dynamic);
	void deleteInstance(RID instance_rid);
	void updateInstance(RID instance_rid, const Transform3D &xform, const AABB &world_aabb);
	FfxCommandList getCommandList(VkCommandBuffer *out_vk_cmd_buffer = nullptr) const;
	FfxErrorCode frameUpdate(const Vector3 &sdf_center, size_t *out_scratch_size = nullptr, FfxBrixelizerStats *out_stats = nullptr);
	void shutdown();

private:
	void _deleteInstanceInternal(RID instance_rid);
	void _updateInstanceInternal(RID instance_rid, const Transform3D &xform, const AABB &world_aabb);
};

inline BrixelizerManager &brixelizerManager() {
	static BrixelizerManager instance;
	return instance;
}
