#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void annyos_lwip_assert(const char* message, const char* file, int line);
uint32_t annyos_lwip_rand(void);
#ifdef __cplusplus
}
#endif

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H 1
#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"
#define LWIP_PLATFORM_DIAG(message) do { } while (0)
#define LWIP_PLATFORM_ASSERT(message) annyos_lwip_assert((message), __FILE__, __LINE__)
#define LWIP_RAND() annyos_lwip_rand()
