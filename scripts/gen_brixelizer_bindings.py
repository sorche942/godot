#!/usr/bin/env python3
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

CALLBACKS = {
    "brixelizer": ROOT / "thirdparty/amd-brixelizer/shaders/ffx_brixelizer_callbacks_glsl.h",
    "gi": ROOT / "thirdparty/amd-brixelizer/shaders/ffx_brixelizergi_callbacks_glsl.h",
}

BRIXELIZER_PASSES = [
    ("FFX_BRIXELIZER_PASS_CONTEXT_CLEAR_COUNTERS", "ffx_brixelizer_context_ops_clear_counters_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_COLLECT_CLEAR_BRICKS", "ffx_brixelizer_context_ops_collect_clear_bricks_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_PREPARE_CLEAR_BRICKS", "ffx_brixelizer_context_ops_prepare_clear_bricks_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_CLEAR_BRICK", "ffx_brixelizer_context_ops_clear_brick_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_COLLECT_DIRTY_BRICKS", "ffx_brixelizer_context_ops_collect_dirty_bricks_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_PREPARE_EIKONAL_ARGS", "ffx_brixelizer_context_ops_prepare_eikonal_args_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_EIKONAL", "ffx_brixelizer_context_ops_eikonal_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_MERGE_CASCADES", "ffx_brixelizer_context_ops_merge_cascades_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_PREPARE_MERGE_BRICKS_ARGS", "ffx_brixelizer_context_ops_prepare_merge_bricks_args_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CONTEXT_MERGE_BRICKS", "ffx_brixelizer_context_ops_merge_bricks_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_CLEAR_BUILD_COUNTERS", "ffx_brixelizer_cascade_ops_clear_build_counters_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_RESET_CASCADE", "ffx_brixelizer_cascade_ops_reset_cascade_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_SCROLL_CASCADE", "ffx_brixelizer_cascade_ops_scroll_cascade_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_CLEAR_REF_COUNTERS", "ffx_brixelizer_cascade_ops_clear_ref_counters_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_CLEAR_JOB_COUNTER", "ffx_brixelizer_cascade_ops_clear_job_counter_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_INVALIDATE_JOB_AREAS", "ffx_brixelizer_cascade_ops_invalidate_job_areas_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_COARSE_CULLING", "ffx_brixelizer_cascade_ops_coarse_culling_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_SCAN_JOBS", "ffx_brixelizer_cascade_ops_scan_jobs_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_VOXELIZE", "ffx_brixelizer_cascade_ops_voxelize_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_SCAN_REFERENCES", "ffx_brixelizer_cascade_ops_scan_references_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_COMPACT_REFERENCES", "ffx_brixelizer_cascade_ops_compact_references_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_CLEAR_BRICK_STORAGE", "ffx_brixelizer_cascade_ops_clear_brick_storage_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_EMIT_SDF", "ffx_brixelizer_cascade_ops_emit_sdf_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_COMPRESS_BRICK", "ffx_brixelizer_cascade_ops_compress_brick_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_INITIALIZE_CASCADE", "ffx_brixelizer_cascade_ops_initialize_cascade_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_MARK_UNINITIALIZED", "ffx_brixelizer_cascade_ops_mark_cascade_uninitialized_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_BUILD_TREE_AABB", "ffx_brixelizer_cascade_ops_build_tree_aabb_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_CASCADE_FREE_CASCADE", "ffx_brixelizer_cascade_ops_free_cascade_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_DEBUG_VISUALIZATION", "ffx_brixelizer_debug_visualization_pass.glsl"),
    ("FFX_BRIXELIZER_PASS_DEBUG_INSTANCE_AABBS", "ffx_brixelizer_debug_draw_instance_aabbs.glsl"),
    ("FFX_BRIXELIZER_PASS_DEBUG_AABB_TREE", "ffx_brixelizer_debug_draw_aabb_tree.glsl"),
]

BRIXELIZER_GI_PASSES = [
    ("FFX_BRIXELIZER_GI_PASS_BLUR_X", "ffx_brixelizergi_blur_x.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_BLUR_Y", "ffx_brixelizergi_blur_y.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_CLEAR_CACHE", "ffx_brixelizergi_clear_cache.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_EMIT_IRRADIANCE_CACHE", "ffx_brixelizergi_emit_irradiance_cache.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_EMIT_PRIMARY_RAY_RADIANCE", "ffx_brixelizergi_emit_primary_ray_radiance.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_FILL_SCREEN_PROBES", "ffx_brixelizergi_fill_screen_probes.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_INTERPOLATE_SCREEN_PROBES", "ffx_brixelizergi_interpolate_screen_probes.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_PREPARE_CLEAR_CACHE", "ffx_brixelizergi_prepare_clear_cache.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_PROJECT_SCREEN_PROBES", "ffx_brixelizergi_project_screen_probes.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_PROPAGATE_SH", "ffx_brixelizergi_propagate_sh.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_REPROJECT_GI", "ffx_brixelizergi_reproject_gi.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_REPROJECT_SCREEN_PROBES", "ffx_brixelizergi_reproject_screen_probes.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_SPAWN_SCREEN_PROBES", "ffx_brixelizergi_spawn_screen_probes.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_SPECULAR_PRE_TRACE", "ffx_brixelizergi_specular_pre_trace.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_SPECULAR_TRACE", "ffx_brixelizergi_specular_trace.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_DEBUG_VISUALIZATION", "ffx_brixelizergi_debug_visualization.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_GENERATE_DISOCCLUSION_MASK", "ffx_brixelizergi_generate_disocclusion_mask.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_DOWNSAMPLE", "ffx_brixelizergi_downsample.glsl"),
    ("FFX_BRIXELIZER_GI_PASS_UPSAMPLE", "ffx_brixelizergi_upsample.glsl"),
]

PASS_SHADER_DIR = ROOT / "thirdparty/amd-brixelizer/shaders"

DEFINE_RE = re.compile(r"^#define\s+(BRIXELIZER(?:_GI)?_BIND_[A-Z0-9_]+)\s+(\d+)")
LAYOUT_RE = re.compile(r"binding\s*=\s*([A-Z0-9_]+)")
SET_RE = re.compile(r"set\s*=\s*(\d+)")


def parse_callbacks(path):
    mapping = {}
    pending = None
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        m = LAYOUT_RE.search(line)
        if m and "BRIXELIZER" in line:
            macro = m.group(1)
            set_match = SET_RE.search(line)
            set_index = int(set_match.group(1)) if set_match else 0
            if "uniform sampler" in line:
                btype = "sampler"
                name = line.split()[-1].rstrip(";")
                array_len = 1
                if "[" in name:
                    base, rest = name.split("[", 1)
                    count_str = rest.split("]", 1)[0]
                    array_len = int(count_str) if count_str.isdigit() else 1
                    name = base
                mapping[macro] = (btype, name, array_len, set_index)
                continue
            if "uniform texture" in line:
                btype = "srv_texture"
                name = line.split()[-1].rstrip(";")
                array_len = 1
                if "[" in name:
                    base, rest = name.split("[", 1)
                    count_str = rest.split("]", 1)[0]
                    array_len = int(count_str) if count_str.isdigit() else 1
                    name = base
                mapping[macro] = (btype, name, array_len, set_index)
                continue
            if "uniform image" in line or "uniform uimage" in line:
                btype = "uav_texture"
                name = line.split()[-1].rstrip(";")
                array_len = 1
                if "[" in name:
                    base, rest = name.split("[", 1)
                    count_str = rest.split("]", 1)[0]
                    array_len = int(count_str) if count_str.isdigit() else 1
                    name = base
                mapping[macro] = (btype, name, array_len, set_index)
                continue
            if "uniform" in line and "std140" in line:
                pending = (macro, "cbv", set_index)
                continue
            if "buffer" in line:
                btype = "srv_buffer" if "readonly" in line else "uav_buffer"
                pending = (macro, btype, set_index)
                continue
        if pending and "}" in line:
            macro, btype, set_index = pending
            tail = line.split("}", 1)[-1].strip()
            if not tail:
                pending = (macro, btype, set_index)
                continue
            name = tail.rstrip(";")
            array_len = 1
            if "[" in name:
                base, rest = name.split("[", 1)
                count_str = rest.split("]", 1)[0]
                array_len = int(count_str) if count_str.isdigit() else 1
                name = base
            mapping[macro] = (btype, name, array_len, set_index)
            pending = None
    return mapping


def parse_pass_file(path, macro_map):
    macros = []
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = DEFINE_RE.match(raw.strip())
        if not m:
            continue
        macro = m.group(1)
        binding = int(m.group(2))
        if macro not in macro_map:
            continue
        btype, name, array_len, set_index = macro_map[macro]
        macros.append((macro, btype, name, binding, array_len, set_index))
    return macros


def emit_pass_bindings(pass_list, macro_map, label):
    arrays = []
    bindings_refs = []
    for enum_name, fname in pass_list:
        path = PASS_SHADER_DIR / fname
        entries = parse_pass_file(path, macro_map)
        array_name = f"{label}_{fname.replace('.', '_')}"
        arrays.append((array_name, entries))
        bindings_refs.append((enum_name, array_name, len(entries)))
    return arrays, bindings_refs


def main():
    brix_map = parse_callbacks(CALLBACKS["brixelizer"])
    gi_map = parse_callbacks(CALLBACKS["gi"])

    brix_arrays, brix_refs = emit_pass_bindings(BRIXELIZER_PASSES, brix_map, "brixelizer")
    gi_arrays, gi_refs = emit_pass_bindings(BRIXELIZER_GI_PASSES, gi_map, "brixelizer_gi")

    out_path = ROOT / "servers/rendering/renderer_rd/environment/brixelizer_bindings.gen.h"

    lines = []
    lines.append("// This file is generated by scripts/gen_brixelizer_bindings.py.\n")
    lines.append("#pragma once\n")
    lines.append("#include \"thirdparty/amd-brixelizer/ffx_brixelizer_raw.h\"\n")
    lines.append("#include \"thirdparty/amd-brixelizer/ffx_brixelizergi.h\"\n\n")
    lines.append("enum BrixelizerBindingType : uint8_t {\n")
    lines.append("\tBRIXELIZER_BINDING_CBV = 0,\n")
    lines.append("\tBRIXELIZER_BINDING_SRV_TEXTURE = 1,\n")
    lines.append("\tBRIXELIZER_BINDING_UAV_TEXTURE = 2,\n")
    lines.append("\tBRIXELIZER_BINDING_SRV_BUFFER = 3,\n")
    lines.append("\tBRIXELIZER_BINDING_UAV_BUFFER = 4,\n")
    lines.append("\tBRIXELIZER_BINDING_SAMPLER = 5,\n")
    lines.append("};\n\n")
    lines.append("struct BrixelizerBindingDesc {\n")
    lines.append("\tconst char *name;\n")
    lines.append("\tuint32_t binding;\n")
    lines.append("\tuint32_t array_size;\n")
    lines.append("\tuint8_t type;\n")
    lines.append("\tuint8_t set;\n")
    lines.append("};\n\n")
    lines.append("struct BrixelizerPassBindings {\n")
    lines.append("\tconst BrixelizerBindingDesc *bindings;\n")
    lines.append("\tuint32_t count;\n")
    lines.append("};\n\n")

    def btype_to_enum(btype):
        return {
            "cbv": "BRIXELIZER_BINDING_CBV",
            "srv_texture": "BRIXELIZER_BINDING_SRV_TEXTURE",
            "uav_texture": "BRIXELIZER_BINDING_UAV_TEXTURE",
            "srv_buffer": "BRIXELIZER_BINDING_SRV_BUFFER",
            "uav_buffer": "BRIXELIZER_BINDING_UAV_BUFFER",
            "sampler": "BRIXELIZER_BINDING_SAMPLER",
        }[btype]

    for array_name, entries in brix_arrays:
        lines.append(f"static const BrixelizerBindingDesc {array_name}[] = {{\n")
        for macro, btype, name, binding, array_len, set_index in entries:
            lines.append(f"\t{{\"{name}\", {binding}, {array_len}, {btype_to_enum(btype)}, {set_index}}},\n")
        lines.append("};\n\n")

    for array_name, entries in gi_arrays:
        lines.append(f"static const BrixelizerBindingDesc {array_name}[] = {{\n")
        for macro, btype, name, binding, array_len, set_index in entries:
            lines.append(f"\t{{\"{name}\", {binding}, {array_len}, {btype_to_enum(btype)}, {set_index}}},\n")
        lines.append("};\n\n")

    lines.append("static const BrixelizerPassBindings brixelizer_pass_bindings[FFX_BRIXELIZER_PASS_COUNT] = {\n")
    for enum_name, array_name, count in brix_refs:
        lines.append(f"\t{{ {array_name}, {count} }}, // {enum_name}\n")
    lines.append("};\n\n")

    lines.append("static const BrixelizerPassBindings brixelizer_gi_pass_bindings[FFX_BRIXELIZER_GI_PASS_COUNT] = {\n")
    for enum_name, array_name, count in gi_refs:
        lines.append(f"\t{{ {array_name}, {count} }}, // {enum_name}\n")
    lines.append("};\n")

    out_path.write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
