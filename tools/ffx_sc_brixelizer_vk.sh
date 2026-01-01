#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FFX_SC="${FFX_SC:-$ROOT_DIR/thirdparty/FidelityFX/sdk/tools/ffx_shader_compiler/bin/FidelityFX_SC}"
GLSLANG="${GLSLANG:-$ROOT_DIR/vulkan_sdk/x86_64/bin/glslangValidator}"

GPU_DIR="$ROOT_DIR/thirdparty/FidelityFX/sdk/include/FidelityFX/gpu"
OUT_DIR="$ROOT_DIR/thirdparty/FidelityFX/sdk/src/backends/vk/shaders/ffx_sc_out"
SHADERS_DIR="$ROOT_DIR/thirdparty/FidelityFX/sdk/src/backends/vk/shaders/brixelizer"

BASE_ARGS=(-reflection -deps=gcc -DFFX_GPU=1)
API_ARGS=(-compiler=glslang -glslangexe="$GLSLANG" -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1)
INCLUDE_ARGS=(-I"$GPU_DIR" -I"$GPU_DIR/brixelizer")

mkdir -p "$OUT_DIR"

for shader in "$SHADERS_DIR"/*.glsl; do
	base="$(basename "$shader" .glsl)"
	"$FFX_SC" "${BASE_ARGS[@]}" "${API_ARGS[@]}" -name="$base" -DFFX_HALF=0 "${INCLUDE_ARGS[@]}" -output="$OUT_DIR" "$shader"
	"$FFX_SC" "${BASE_ARGS[@]}" "${API_ARGS[@]}" -name="${base}_wave64" -DFFX_HALF=0 "${INCLUDE_ARGS[@]}" -output="$OUT_DIR" "$shader"
	"$FFX_SC" "${BASE_ARGS[@]}" "${API_ARGS[@]}" -name="${base}_16bit" -DFFX_HALF=1 "${INCLUDE_ARGS[@]}" -output="$OUT_DIR" "$shader"
	"$FFX_SC" "${BASE_ARGS[@]}" "${API_ARGS[@]}" -name="${base}_wave64_16bit" -DFFX_HALF=1 "${INCLUDE_ARGS[@]}" -output="$OUT_DIR" "$shader"
done
