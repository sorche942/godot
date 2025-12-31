// Stub for missing ffx_object_management.h
// These are utility functions for safe resource/pipeline cleanup

#pragma once

#include "ffx_types.h"
#include "ffx_interface.h"

static inline void ffxSafeReleasePipeline(FfxInterface* backendInterface, FfxPipelineState* pipeline, FfxUInt32 effectContextId) {
    if (pipeline && pipeline->pipeline) {
        backendInterface->fpDestroyPipeline(backendInterface, pipeline, effectContextId);
        memset(pipeline, 0, sizeof(*pipeline));
    }
}

static inline void ffxSafeReleaseResource(FfxInterface* backendInterface, FfxResourceInternal resource, FfxUInt32 effectContextId) {
    if (resource.internalIndex != 0) {
        backendInterface->fpDestroyResource(backendInterface, resource, effectContextId);
    }
}
