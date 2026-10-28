#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
int snprintf(char* output, size_t capacity, const char* format, ...);
#ifdef __cplusplus
}
#endif
