#pragma once

#include <cstdint>

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"


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

	// The keys are the instance_rids;
	HashMap<RID, BrixelizerInstanceInfo> brixelizer_instances;

	BrixelizerManager();
	~BrixelizerManager();

	int initContext();
	int destroyContext();

	void addInstance(RID instance_rid, RID base_rid, const Transform3D &xform, const AABB &world_aabb, bool dynamic);
	void deleteInstance(RID instance_rid);
	void updateInstance(RID instance_rid, const Transform3D &xform, const AABB &world_aabb);
};

inline BrixelizerManager &brixelizerManager() {
	static BrixelizerManager instance;
	return instance;
}
