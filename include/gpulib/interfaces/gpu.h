#ifndef INTERFACES_GPU_H
#define INTERFACES_GPU_H

/*
** gpu.library — IGpu ("main", version 1) interface layout.
** Slot order is ABI: it must match _main_vectors[] in src/lib/gpu_api.c
** and the table in docs/API_DESIGN.md exactly.
**
** PPC-only, taglist-only ABI (user policy 2026-07-05): there are NO
** variadic entry points — no VARARGS68K/linearvarargs anywhere. Callers
** pass explicit TagItem lists; the GPU_TAGS() helper in libraries/gpu.h
** builds them inline in plain C99.
*/

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_INTERFACES_H
#include <exec/interfaces.h>
#endif
#ifndef LIBRARIES_GPU_H
#include <libraries/gpu.h>
#endif

struct GpuIFace
{
    struct InterfaceData Data;

    uint32             APICALL (*Obtain) (struct GpuIFace *Self);
    uint32             APICALL (*Release)(struct GpuIFace *Self);
    void               APICALL (*Expunge)(struct GpuIFace *Self);   /* private */
    struct Interface * APICALL (*Clone)  (struct GpuIFace *Self);   /* private */

    int32              APICALL (*GPU_GetAttrsA)(struct GpuIFace *Self,
                                    const struct TagItem *tags);

    struct GpuBuffer * APICALL (*GPU_CreateBufferA)(struct GpuIFace *Self,
                                    uint32 size, const struct TagItem *tags);
    void               APICALL (*GPU_DestroyBuffer)(struct GpuIFace *Self,
                                    struct GpuBuffer *buf);
    APTR               APICALL (*GPU_MapBuffer)(struct GpuIFace *Self,
                                    struct GpuBuffer *buf);
    void               APICALL (*GPU_UnmapBuffer)(struct GpuIFace *Self,
                                    struct GpuBuffer *buf);

    struct GpuBuffer * APICALL (*GPU_ImportBitMapA)(struct GpuIFace *Self,
                                    struct BitMap *bm,
                                    const struct TagItem *tags);

    int32              APICALL (*GPU_SubmitA)(struct GpuIFace *Self,
                                    uint32 queue, CONST_APTR payload,
                                    uint32 length,
                                    const struct TagItem *tags);
    int32              APICALL (*GPU_TestFence)(struct GpuIFace *Self,
                                    int32 fence);
    int32              APICALL (*GPU_WaitFence)(struct GpuIFace *Self,
                                    int32 fence, uint32 timeout_us);

    int32              APICALL (*GPU_PresentA)(struct GpuIFace *Self,
                                    struct GpuBuffer *buf,
                                    const struct TagItem *tags);

    struct GpuDisplay *APICALL (*GPU_AcquireDisplayA)(struct GpuIFace *Self,
                                    const struct TagItem *tags);
    void               APICALL (*GPU_ReleaseDisplay)(struct GpuIFace *Self,
                                    struct GpuDisplay *disp);

    int32              APICALL (*GPU_RegisterBackendA)(struct GpuIFace *Self,
                                    const struct GpuBackendInfo *info,
                                    const struct TagItem *tags);
    int32              APICALL (*GPU_UnregisterBackend)(struct GpuIFace *Self,
                                    int32 backendId);

    /* -- appended post-v0-freeze (append-only ABI) ---------------------- */

    /* Driver-side: an async backend (GPUTAG_AsyncFences) reports fence
       retirement from TASK context. Retires every pending seq <= seq.   */
    int32              APICALL (*GPU_FenceRetired)(struct GpuIFace *Self,
                                    int32 backendId, int32 seq);
};

#endif /* INTERFACES_GPU_H */
