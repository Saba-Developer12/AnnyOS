#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void* memcpy(void* destination, const void* source, size_t count);
void* memmove(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);
int memcmp(const void* left, const void* right, size_t count);
size_t strlen(const char* text);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, size_t count);
char* strchr(const char* text, int character);
char* strncpy(char* destination, const char* source, size_t count);
#ifdef __cplusplus
}
#endif
