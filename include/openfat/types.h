/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed-width scalar types for host tests, 16-bit DOS builds (OpenWatcom)
 * and 32/64-bit kernels. */

#ifndef OPENFAT_TYPES_H
#define OPENFAT_TYPES_H

/* OpenWatcom's 16-bit DOS targets use 8/16/32-bit char/short/long. */
#ifdef __WATCOMC__
typedef unsigned char  ofat_u8;
typedef signed char    ofat_s8;
typedef unsigned short ofat_u16;
typedef signed short   ofat_s16;
typedef unsigned long  ofat_u32;
typedef signed long    ofat_s32;
typedef unsigned long long ofat_u64; /* OpenWatcom has it in every model */
typedef signed long long   ofat_s64;
#else
#include <stdint.h>
typedef uint8_t  ofat_u8;
typedef int8_t   ofat_s8;
typedef uint16_t ofat_u16;
typedef int16_t  ofat_s16;
typedef uint32_t ofat_u32;
typedef int32_t  ofat_s32;
typedef uint64_t ofat_u64; /* also under -std=c90: stdint.h declares it */
typedef int64_t  ofat_s64;
#endif

#endif
