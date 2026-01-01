#pragma once

#ifndef USE_VOLK
#define USE_VOLK
#endif
#include "drivers/vulkan/godot_vulkan.h"

#include <cstdarg>
#include <codecvt>
#include <locale>
#include <cstring>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <wchar.h>

#ifndef _countof
template <typename T, size_t N>
constexpr size_t _countof(T const (&)[N]) {
	return N;
}
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) (void)(x)
#endif

inline int wcscpy_s(wchar_t *dest, size_t destsz, const wchar_t *src) {
	if (!dest || !src) {
		return EINVAL;
	}
	size_t len = wcslen(src);
	if (len + 1 > destsz) {
		if (destsz) {
			dest[0] = L'\0';
		}
		return ERANGE;
	}
	wmemcpy(dest, src, len + 1);
	return 0;
}

template <size_t N>
inline int wcscpy_s(wchar_t (&dest)[N], const wchar_t *src) {
	return wcscpy_s(dest, N, src);
}

inline int swprintf_s(wchar_t *dest, size_t size, const wchar_t *format, ...) {
	va_list args;
	va_start(args, format);
	int result = vswprintf(dest, size, format, args);
	va_end(args);
	return result;
}

inline int sprintf_s(char *dest, size_t size, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int result = vsnprintf(dest, size, format, args);
	va_end(args);
	return result;
}

inline int strcpy_s(char *dest, size_t size, const char *src) {
	if (!dest || !src) {
		return EINVAL;
	}
	size_t len = strlen(src);
	if (len + 1 > size) {
		if (size) {
			dest[0] = '\0';
		}
		return ERANGE;
	}
	memcpy(dest, src, len + 1);
	return 0;
}
