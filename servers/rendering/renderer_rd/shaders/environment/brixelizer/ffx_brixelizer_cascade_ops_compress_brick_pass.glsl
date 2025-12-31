#[compute]

#version 450

#VERSION_DEFINES
#define FFX_GPU
#define FFX_GLSL 1

#include "thirdparty/amd-brixelizer/shaders/ffx_brixelizer_cascade_ops_compress_brick_pass.glsl"
