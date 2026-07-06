#ifndef LIBRARIES_GPU_H
#define LIBRARIES_GPU_H

/*
** gpu.library — public types, tags and constants.
** API v0 — see docs/API_DESIGN.md (source of truth until Phase 1 freeze).
*/

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_PORTS_H
#include <exec/ports.h>          /* struct Message/MsgPort (GpuFenceMsg) */
#endif
#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
#endif

struct BitMap;   /* graphics/gfx.h — forward-declared to keep this header light */

#define GPU_LIBNAME      "gpu.library"
#define GPU_API_VERSION  1              /* IGpu "main" interface version   */

/* ---- Errors (negative int32) ------------------------------------------ */

#define GPUERR_OK          0
#define GPUERR_NOTIMPL    -1
#define GPUERR_NOMEM      -2
#define GPUERR_BADARGS    -3
#define GPUERR_NOBACKEND  -4
#define GPUERR_TIMEOUT    -5
#define GPUERR_LOST       -6
#define GPUERR_BUSY       -7

/* ---- Submission queues ------------------------------------------------- */

#define GPU_QUEUE_PRESENT  0   /* HI priority: presents, cursor, composite  */
#define GPU_QUEUE_RENDER   1   /* LO priority: bulk draws, uploads          */
#define GPU_QUEUE_COUNT    2

/* ---- Buffer usage flags ------------------------------------------------ */

#define GPUBUF_USAGE_GENERAL   0x00000000
#define GPUBUF_USAGE_SCANOUT   0x00000001
#define GPUBUF_USAGE_COMMAND   0x00000002
#define GPUBUF_USAGE_UPLOAD    0x00000004

/* ---- Objects ------------------------------------------------------------
** GpuBuffer is returned by the library; treat all fields as read-only.
** BackendData belongs to the owning backend.
*/

struct GpuBuffer
{
    uint32  Size;
    uint32  Usage;
    int32   BackendId;
    APTR    BackendData;
};

struct GpuDisplay;               /* opaque in v0 (GPU_AcquireDisplayA)     */

/* Fence-retirement notification (GPUTAG_NotifyPort): the library PutMsg()s
** one of these to the given port when the fence retires. The client MUST
** ReplyMsg() it promptly — the library recycles the message; unreturned
** messages stall backend unregistration/expunge. (Appended post-v0-freeze
** together with port-notify delivery; struct is append-only from here.) */
struct GpuFenceMsg
{
    struct Message  Msg;
    int32           Fence;
    int32           Result;      /* GPUERR_OK, or the submit's error      */
};

/* ---- Tags ---------------------------------------------------------------
**
** The ABI is taglist-only: no variadic entry points exist (PPC-only
** policy — no VARARGS68K/linearvarargs). For inline tag lists use the
** GPU_TAGS() helper, plain C99 (compound literal, no calling-convention
** magic):
**
**     IGpu->GPU_GetAttrsA(GPU_TAGS(
**         { GPUATTR_APIVersion, (uint32)&ver }));
**
**     buf = IGpu->GPU_CreateBufferA(4096, GPU_TAGS(
**         { GPUTAG_Usage, GPUBUF_USAGE_UPLOAD }));
**
** TAG_DONE is appended automatically.
*/

#define GPU_TAGS(...) \
    ((const struct TagItem []){ __VA_ARGS__, { TAG_DONE, 0 } })

#define GPU_TAGBASE            (TAG_USER + 0x00475055)  /* 'GPU' */

/* GPU_GetAttrsA — each attr tag's ti_Data is a POINTER to result storage  */
#define GPUATTR_APIVersion     (GPU_TAGBASE + 1)   /* uint32 *             */
#define GPUATTR_BackendCount   (GPU_TAGBASE + 2)   /* uint32 *             */
#define GPUATTR_BackendIndex   (GPU_TAGBASE + 3)   /* uint32 (input)       */
#define GPUATTR_BackendName    (GPU_TAGBASE + 4)   /* CONST_STRPTR *       */

/* Creation / submission tags */
#define GPUTAG_Backend         (GPU_TAGBASE + 32)  /* int32 backend id     */
#define GPUTAG_Usage           (GPU_TAGBASE + 33)  /* GPUBUF_USAGE_*       */
#define GPUTAG_RectX           (GPU_TAGBASE + 34)  /* present dirty rect   */
#define GPUTAG_RectY           (GPU_TAGBASE + 35)
#define GPUTAG_RectW           (GPU_TAGBASE + 36)
#define GPUTAG_RectH           (GPU_TAGBASE + 37)

/* Completion notification (GPU_SubmitA / GPU_PresentA).
** GPUTAG_NotifySignal: ti_Data = signal MASK; the SUBMITTING task is
**   Signal()ed with it when the fence retires. Composes with exec Wait()
**   so one task can block on GPU + IDCMP + timer in a single call.
** GPUTAG_NotifyPort: ti_Data = struct MsgPort *; a fence-retirement
**   message is PutMsg()d there (works with PA_SIGNAL and PA_SOFTINT
**   ports). Delivery lands with the Phase 2 I/O server; until then
**   backends that cannot honour it MUST fail the submit with
**   GPUERR_NOTIMPL rather than silently not notifying.               */
#define GPUTAG_NotifySignal    (GPU_TAGBASE + 38)  /* uint32 signal mask   */
#define GPUTAG_NotifyPort      (GPU_TAGBASE + 39)  /* struct MsgPort *     */

/* GPU_ImportBitMapA (appended post-v0-freeze, Phase 5): wraps a P96
** BitMap as a GpuBuffer via graphics v54 LockBitMapTagList. The bitmap
** STAYS LOCKED until GPU_DestroyBuffer (which unlocks it) — keep imports
** SHORT-LIVED: import → use → destroy. Imported buffers have
** BackendId == GPU_BUFFER_IMPORTED and are core-handled: Map/Unmap/
** Destroy are valid; Present/queue ops are not (it is a CPU-side view;
** feed the base/stride to backend ops instead, e.g. texture upload).
** Output tags (each ti_Data = POINTER to receiving storage):           */
#define GPU_BUFFER_IMPORTED    (-1)
#define GPUTAG_OutBytesPerRow  (GPU_TAGBASE + 40)  /* uint32 *           */
#define GPUTAG_OutPixelFormat  (GPU_TAGBASE + 41)  /* uint32 * (RGBFTYPE)*/
#define GPUTAG_OutWidth        (GPU_TAGBASE + 42)  /* uint32 *           */
#define GPUTAG_OutHeight       (GPU_TAGBASE + 43)  /* uint32 *           */
#define GPUTAG_OutOnBoard      (GPU_TAGBASE + 44)  /* uint32 * (BOOL)    */

/* GPU_RegisterBackendA tags.
** GPUTAG_AsyncFences (BOOL, default FALSE): the backend's fences do NOT
** retire at issue; the backend reports retirement from task context via
** GPU_FenceRetired(backendId, seq), and the library defers notification
** delivery until then. Retirement is monotonic: reporting seq N retires
** every pending seq <= N.                                              */
#define GPUTAG_AsyncFences     (GPU_TAGBASE + 64)  /* BOOL                 */

/* ---- Backend registration (driver side) --------------------------------
** Version field FIRST. NULL ops => library returns GPUERR_NOTIMPL for that
** operation. The Private pointer is passed back as `priv` to every op.
** The built-in null backend (id 0) is the executable reference.
*/

#define GPU_BACKEND_API_VERSION  1

struct GpuBackendInfo
{
    uint32        Version;                 /* GPU_BACKEND_API_VERSION      */
    CONST_STRPTR  Name;
    APTR          Private;

    struct GpuBuffer * (*CreateBuffer)(APTR priv, uint32 size,
                                       const struct TagItem *tags);
    void   (*DestroyBuffer)(APTR priv, struct GpuBuffer *buf);
    APTR   (*MapBuffer)    (APTR priv, struct GpuBuffer *buf);
    void   (*UnmapBuffer)  (APTR priv, struct GpuBuffer *buf);

    int32  (*Submit)   (APTR priv, uint32 queue, CONST_APTR payload,
                        uint32 length, const struct TagItem *tags);
    int32  (*TestFence)(APTR priv, int32 fence);
    int32  (*WaitFence)(APTR priv, int32 fence, uint32 timeout_us);

    int32  (*Present)  (APTR priv, struct GpuBuffer *buf,
                        const struct TagItem *tags);
};

#endif /* LIBRARIES_GPU_H */
