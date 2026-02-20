#include <rex/runtime/guest/function.h>
#include "generated/rb2_init.h"

int vsprintf_s_entry(char* buffer, size_t size, const char* format, va_list args) {
    return vsprintf_s(buffer, size, format, args);
}

int sprintf_s_entry(char* buffer, size_t size, const char* format, va_list args) {
    return sprintf_s(buffer, size, format, args);
}

int printf_entry(const char* format, va_list args) {
    return printf(format, args);
}

// Memory functions
GUEST_FUNCTION_HOOK(sub_8244e7d0, memset)
GUEST_FUNCTION_HOOK(sub_8244e2a0, memcpy)
GUEST_FUNCTION_HOOK(sub_8244eeb0, memmove)

// String functions
GUEST_FUNCTION_HOOK(sub_8244cea0, strncmp)
GUEST_FUNCTION_HOOK(sub_8244d5f8, strncpy)
GUEST_FUNCTION_HOOK(sub_8244FAE0, (int(*)(char*, size_t, const char*, va_list))vsprintf_s_entry)
GUEST_FUNCTION_HOOK(sub_8244fb88, (errno_t(*)(char*, rsize_t, const char*))strcpy_s)
GUEST_FUNCTION_HOOK(sub_8244fc40, wcslen)
GUEST_FUNCTION_HOOK(sub_8244fc68, bsearch)
GUEST_FUNCTION_HOOK(sub_8244fd70, tolower)
GUEST_FUNCTION_HOOK(sub_824500c0, strtol)
GUEST_FUNCTION_HOOK(sub_824500c8, strtoul)
GUEST_FUNCTION_HOOK(sub_824500d0, (errno_t(*)(char*, size_t, const char*, size_t))strncpy_s)
GUEST_FUNCTION_HOOK(sub_82450218, (errno_t(*)(char*, size_t, const char*, size_t))strncat_s)
GUEST_FUNCTION_HOOK(sub_82450380, (errno_t(*)(char*, size_t, const char*))strcat_s)
GUEST_FUNCTION_HOOK(sub_8244cca0, (int(*)(const char*, va_list))printf_entry)
GUEST_FUNCTION_HOOK(sub_8244cdc0, (int(*)(char*, size_t, const char*, va_list))sprintf_s_entry)

// Math functions
GUEST_FUNCTION_HOOK(sub_8244d5f0, atof)
GUEST_FUNCTION_HOOK(sub_8244d520, atoi)