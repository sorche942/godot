/**************************************************************************/
/*  ray_tracing_manager_rd.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "ray_tracing_manager_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"

namespace RendererRD {

RayTracingManager::RayTracingManager() {
}

RayTracingManager::~RayTracingManager() {
	free();
}

void RayTracingManager::free() {
	RD *rd = RD::get_singleton();
	if (tlas.is_valid()) {
		rd->free_rid(tlas);
		tlas = RID();
	}
	for (KeyValue<RID, BLASCache> &E : mesh_to_blas) {
		rd->free_rid(E.value.blas);
	}
	mesh_to_blas.clear();
}

void RayTracingManager::update(const PagedArray<RenderGeometryInstance *> &p_instances, RID p_main_compute_list) {
	RD *rd = RD::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();

	Vector<RDD::TLASInstanceInfo> tlas_instances;
	bool blas_built_this_frame = false;

	for (int i = 0; i < (int)p_instances.size(); i++) {
		RenderGeometryInstance *inst = p_instances[i];
		// Only supporting Meshes for now (not MultiMesh or Particles)
		if (inst->type != RenderGeometryInstance::TYPE_MESH) {
			continue;
		}

		RID mesh_rid = inst->mesh;
		if (!mesh_rid.is_valid()) continue;

		// Check/Create BLAS
		if (!mesh_to_blas.has(mesh_rid)) {
			// Create BLAS from mesh surfaces
			uint32_t surface_count;
			const RID *materials = mesh_storage->mesh_get_surface_count_and_materials(mesh_rid, surface_count);
			
			if (surface_count == 0) continue;

			Vector<RDD::BLASGeometryInfo> geometries;
			for (uint32_t s = 0; s < surface_count; s++) {
				void *surface = mesh_storage->mesh_get_surface(mesh_rid, s);
				
				RDD::BLASGeometryInfo geom_info;
				geom_info.vertex_buffer = rd->buffer_get_driver_id(mesh_storage->mesh_surface_get_vertex_buffer(surface));
				geom_info.vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
				// TODO: Get actual stride/format. Assuming packed float3 positions for now.
				geom_info.vertex_format = RDD::DATA_FORMAT_R32G32B32_SFLOAT; 
				geom_info.vertex_stride = 12; 
				
				if (mesh_storage->mesh_surface_get_primitive(surface) == RS::PRIMITIVE_TRIANGLES) {
					RID idx_buf = mesh_storage->mesh_surface_get_index_buffer(surface);
					if (idx_buf.is_valid()) {
						geom_info.index_buffer = rd->buffer_get_driver_id(idx_buf);
						geom_info.index_count = mesh_storage->mesh_surface_get_index_count(surface);
						// TODO: Check index format (16 vs 32 bit).
						geom_info.index_format = RDD::DATA_FORMAT_R32_UINT;
					}
				}
				
				geometries.push_back(geom_info);
			}
			
			RID blas = rd->blas_create(geometries);
			mesh_to_blas[mesh_rid] = { blas };
			rd->compute_list_build_blas(p_main_compute_list, blas);
			blas_built_this_frame = true;
		}

		// Add to TLAS
		RDD::TLASInstanceInfo inst_info;
		inst_info.blas = rd->blas_get_driver_id(mesh_to_blas[mesh_rid].blas);
		inst_info.transform = inst->transform;
		inst_info.instance_id = i;
		inst_info.instance_mask = 0xFF;
		tlas_instances.push_back(inst_info);
	}

	if (blas_built_this_frame) {
		rd->compute_list_add_barrier(p_main_compute_list);
	}

	if (tlas.is_valid()) {
		rd->free_rid(tlas);
	}
	
	if (!tlas_instances.is_empty()) {
		tlas = rd->tlas_create(tlas_instances);
		rd->compute_list_build_tlas(p_main_compute_list, tlas);
	}
}

} // namespace RendererRD
