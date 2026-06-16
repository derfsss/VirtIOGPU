/*
 * compat/linux/types.h -- Linux kernel type compatibility for the verbatim
 * virtio-gpu driver code on AmigaOS 4 (PowerPC, BIG-ENDIAN).
 *
 * Part of the Linux-virtio-gpu compatibility layer: the upstream driver
 * (MIT/BSD) is kept as-is in src/linux/; this header provides the Linux
 * kernel types it expects, mapped onto AmigaOS/exec primitives.
 *
 * ENDIANNESS: AmigaOS4/PPC is big-endian; virtio (modern) wire format is
 * little-endian.  The upstream code does all wire encoding through the
 * cpu_to_leN and leN_to_cpu helpers, so defining those as byteswaps here
 * makes every wire field correct BY CONSTRUCTION on big-endian.
 */
#ifndef _COMPAT_LINUX_TYPES_H
#define _COMPAT_LINUX_TYPES_H

#include <exec/types.h>   /* UBYTE/UWORD/ULONG/... and stdint via SDK */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uint8_t   __u8;
typedef uint16_t  __u16;
typedef uint32_t  __u32;
typedef uint64_t  __u64;
typedef int8_t    __s8;
typedef int16_t   __s16;
typedef int32_t   __s32;
typedef int64_t   __s64;

/* Endian-tagged wire types.  They are plain integers at the ABI level; the
 * __le tag documents that the value is little-endian and must be accessed via
 * the cpu_to_leN and leN_to_cpu helpers below. */
typedef uint16_t  __le16;
typedef uint32_t  __le32;
typedef uint64_t  __le64;
typedef uint16_t  __be16;
typedef uint32_t  __be32;
typedef uint64_t  __be64;

typedef uint32_t  dma_addr_t;   /* guest physical addr (we use 32-bit PCI) */
typedef int32_t   ssize_t_compat;

#ifndef __bitwise
#define __bitwise
#endif
#ifndef __force
#define __force
#endif
#ifndef __iomem
#define __iomem
#endif
#ifndef __user
#define __user
#endif

/* ---- big-endian <-> little-endian wire helpers (PPC is BE) ---- */
static inline u16 __sw16(u16 v) { return __builtin_bswap16(v); }
static inline u32 __sw32(u32 v) { return __builtin_bswap32(v); }
static inline u64 __sw64(u64 v) { return __builtin_bswap64(v); }

#define cpu_to_le16(x)  ((__le16)__sw16((u16)(x)))
#define cpu_to_le32(x)  ((__le32)__sw32((u32)(x)))
#define cpu_to_le64(x)  ((__le64)__sw64((u64)(x)))
#define le16_to_cpu(x)  (__sw16((u16)(x)))
#define le32_to_cpu(x)  (__sw32((u32)(x)))
#define le64_to_cpu(x)  (__sw64((u64)(x)))

/* big-endian helpers are no-ops on a BE host */
#define cpu_to_be16(x)  ((__be16)(u16)(x))
#define cpu_to_be32(x)  ((__be32)(u32)(x))
#define cpu_to_be64(x)  ((__be64)(u64)(x))
#define be16_to_cpu(x)  ((u16)(x))
#define be32_to_cpu(x)  ((u32)(x))
#define be64_to_cpu(x)  ((u64)(x))

#endif /* _COMPAT_LINUX_TYPES_H */
