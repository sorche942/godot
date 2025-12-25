// DLSS Quality Mode Mapping
// Maps Godot's scaling (0.1-2.0) to DLSS quality presets

#ifndef DLSS_QUALITY_MAPPING_H
#define DLSS_QUALITY_MAPPING_H

#include "servers/rendering/renderer_rd/effects/dlss.h"

// Map Godot scale to DLSS quality mode
// Godot scale: 0.1-2.0 (where 1.0 = native resolution)
// Returns: Nearest DLSS quality mode
inline DLSSQuality map_scale_to_dlss_quality(float p_scale) {
	// DLSS quality modes with their approximate scale factors:
	// DLAA: 1.0x
	// Ultra Quality: 1.3x (0.77 scale factor)
	// Quality: 1.5x (0.67 scale factor)
	// Balanced: 1.7x (0.58 scale factor)
	// Performance: 2.0x (0.5 scale factor)
	// Ultra Performance: 3.0x (0.33 scale factor)

	if (p_scale >= 0.95f) {
		return DLSSQuality::DLAA;  // 0.95-1.05 → DLAA
	} else if (p_scale >= 0.80f) {
		return DLSSQuality::ULTRA_QUALITY;  // 0.80-0.95 → Ultra Quality
	} else if (p_scale >= 0.65f) {
		return DLSSQuality::QUALITY;  // 0.65-0.80 → Quality
	} else if (p_scale >= 0.55f) {
		return DLSSQuality::BALANCED;  // 0.55-0.65 → Balanced
	} else if (p_scale >= 0.40f) {
		return DLSSQuality::PERFORMANCE;  // 0.40-0.55 → Performance
	} else {
		return DLSSQuality::ULTRA_PERFORMANCE;  // 0.1-0.40 → Ultra Performance
	}
}

// Get display name for DLSS quality mode
inline const char *get_dlss_quality_name(DLSSQuality p_quality) {
	switch (p_quality) {
		case DLSSQuality::DLAA: return "DLAA";
		case DLSSQuality::ULTRA_QUALITY: return "Ultra Quality";
		case DLSSQuality::QUALITY: return "Quality";
		case DLSSQuality::BALANCED: return "Balanced";
		case DLSSQuality::PERFORMANCE: return "Performance";
		case DLSSQuality::ULTRA_PERFORMANCE: return "Ultra Performance";
		default: return "Unknown";
	}
}

#endif // DLSS_QUALITY_MAPPING_H
