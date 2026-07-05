/* -------------------------------------------------------------------------
 * chip_gpu_backend.c -- gpu.library backend registration (Phase 3.2a).
 *
 * Registers this chip as a gpu.library backend ("virtio-gpu") so IGpu
 * clients reach the VirtIO GPU through the public API. Called from the
 * flush task's startup (full task context, safely after chip init --
 * deliberately NOT from the boot-time resident/InitBoard path).
 *
 * 3.2a scope: buffers = shared guest memory; Submit = VgbCmd opcodes
 * (NOP = fence transport proof, FLUSH = wake the flush task to present a
 * frame); Present = FLUSH. Ops are synchronous (fences retire at return),
 * so the backend registers WITHOUT GPUTAG_AsyncFences; the 3.2b ring
 * integration flips to async + IGpu->GPU_FenceRetired from the completion
 * path and carries opaque virgl streams.
 *
 * Chip build rules apply: -nostartfiles newlib, no libc calls, no struct
 * assignments, member-wise everything.
 * ---------------------------------------------------------------------- */

#include "chip/chip_state.h"

#include <libraries/gpu.h>          /* vendored: include/gpulib */
#include <interfaces/gpu.h>
#include <gpulib/virtio_gpu_backend.h>

#include <exec/exectags.h>
#include <exec/memory.h>

static struct Library  *vgb_GpuBase;
static struct GpuIFace *vgb_IGpu;
static int32            vgb_backendId = -1;
static uint32           vgb_fenceSeq;   /* touched only by the backend's
                                           gpu.library server task        */

/* ---- ops (priv = ChipGPUState) ------------------------------------------ */

static struct GpuBuffer *vgb_CreateBuffer(APTR priv, uint32 size,
                                          const struct TagItem *tags)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)priv;
    struct ExecIFace *IExec = gs->IExec;
    struct GpuBuffer *buf;
    (void)tags;

    buf = IExec->AllocVecTags(sizeof(*buf) + size,
                              AVT_Type, MEMF_SHARED,
                              AVT_ClearWithValue, 0,
                              TAG_DONE);
    if (!buf)
        return NULL;
    buf->Size        = size;
    buf->BackendData = buf + 1;
    return buf;
}

static void vgb_DestroyBuffer(APTR priv, struct GpuBuffer *buf)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)priv;
    gs->IExec->FreeVec(buf);
}

static APTR vgb_MapBuffer(APTR priv, struct GpuBuffer *buf)
{
    (void)priv;
    return buf->BackendData;
}

static void vgb_UnmapBuffer(APTR priv, struct GpuBuffer *buf)
{
    (void)priv; (void)buf;
}

static int32 vgb_Submit(APTR priv, uint32 queue, CONST_APTR payload,
                        uint32 length, const struct TagItem *tags)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)priv;
    const struct VgbCmd *cmd = (const struct VgbCmd *)payload;
    (void)queue; (void)tags;

    if (cmd == NULL || length < sizeof(struct VgbCmd))
        return GPUERR_BADARGS;

    switch (cmd->op)
    {
        case VGB_OP_NOP:
            break;
        case VGB_OP_FLUSH:
            chip_flush_signal_activity(gs);
            break;
        default:
            return GPUERR_NOTIMPL;
    }

    vgb_fenceSeq = (vgb_fenceSeq + 1) & 0x00FFFFFF;
    return (int32)vgb_fenceSeq;
}

static int32 vgb_TestFence(APTR priv, int32 fence)
{
    (void)priv;
    return (fence > 0 && (uint32)fence <= vgb_fenceSeq) ? 1 : GPUERR_BADARGS;
}

static int32 vgb_WaitFence(APTR priv, int32 fence, uint32 timeout_us)
{
    (void)timeout_us;
    return (vgb_TestFence(priv, fence) == 1) ? GPUERR_OK : GPUERR_BADARGS;
}

static int32 vgb_Present(APTR priv, struct GpuBuffer *buf,
                         const struct TagItem *tags)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)priv;
    (void)tags;

    if (buf == NULL)
        return GPUERR_BADARGS;
    chip_flush_signal_activity(gs);
    return GPUERR_OK;
}

/* ---- registration (called from the flush task's startup) ---------------- */

void gpu_backend_init(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;
    struct GpuBackendInfo info;

    vgb_GpuBase = IExec->OpenLibrary("gpu.library", 53);
    if (!vgb_GpuBase)
    {
        /* Early boot: the on-demand kickstart-library path isn't up yet.
           Force-init the resident ourselves -- the same pattern
           VirtIOGPUBoard uses for PCIGraphics.card. */
        struct Resident *res = IExec->FindResident("gpu.library");
        if (res)
        {
            IExec->InitResident(res, 0);
            vgb_GpuBase = IExec->OpenLibrary("gpu.library", 53);
        }
    }
    if (!vgb_GpuBase)
    {
        DCHIP("gpu_backend: gpu.library v53 not found -- backend disabled");
        return;
    }
    vgb_IGpu = (struct GpuIFace *)
        IExec->GetInterface(vgb_GpuBase, "main", 1, NULL);
    if (!vgb_IGpu)
    {
        DCHIP("gpu_backend: GetInterface failed -- backend disabled");
        IExec->CloseLibrary(vgb_GpuBase);
        vgb_GpuBase = NULL;
        return;
    }

    info.Version       = GPU_BACKEND_API_VERSION;
    info.Name          = VGB_BACKEND_NAME;
    info.Private       = gs;
    info.CreateBuffer  = vgb_CreateBuffer;
    info.DestroyBuffer = vgb_DestroyBuffer;
    info.MapBuffer     = vgb_MapBuffer;
    info.UnmapBuffer   = vgb_UnmapBuffer;
    info.Submit        = vgb_Submit;
    info.TestFence     = vgb_TestFence;
    info.WaitFence     = vgb_WaitFence;
    info.Present       = vgb_Present;

    vgb_backendId = vgb_IGpu->GPU_RegisterBackendA(&info, NULL);
    if (vgb_backendId < 0)
    {
        DCHIP("gpu_backend: RegisterBackendA failed (%ld)",
              (LONG)vgb_backendId);
        IExec->DropInterface((struct Interface *)vgb_IGpu);
        IExec->CloseLibrary(vgb_GpuBase);
        vgb_IGpu = NULL;
        vgb_GpuBase = NULL;
        return;
    }

    /* Registration holds gpu.library open for the chip's lifetime --
       this also pins the library against expunge (backend state must
       not evaporate; see gpu.library Phase 1 finding). The chip never
       expunges, so there is no teardown path by design. */
    DCHIP("gpu_backend: registered as '%s' id=%ld",
          VGB_BACKEND_NAME, (LONG)vgb_backendId);
}
