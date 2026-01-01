// Minimal stub to satisfy the Vulkan backend when frame generation swapchain
// support is not compiled in.

#ifndef USE_VOLK
#define USE_VOLK
#endif
#include "drivers/vulkan/godot_vulkan.h"

#include "thirdparty/FidelityFX/sdk/include/FidelityFX/host/backends/vk/ffx_vk.h"
#include "thirdparty/FidelityFX/sdk/include/FidelityFX/host/ffx_error.h"

extern "C" FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(const FfxFrameGenerationConfig *config) {
	(void)config;
	return FFX_ERROR_INVALID_ARGUMENT;
}
