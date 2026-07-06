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
#include <utility/hooks.h>

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

        case VGB_OP_GETCTX:
        {
            struct VgbCtxInfo *out = (struct VgbCtxInfo *)cmd->arg;
            if (out == NULL)
                return GPUERR_BADARGS;
            if (!gs->virgl_2d_ready)
                return GPUERR_NOTIMPL;
            out->ctx_id         = gs->virgl_2d_ctx;
            out->vbuf_res       = gs->virgl_2d_vbuf_res;
            out->vbuf_size      = 65536;
            out->scanout_res    = gs->resource_id;
            out->vs_handle      = gs->virgl_2d_vs;
            out->fs_handle      = gs->virgl_2d_fs;
            out->fs_tex_handle  = gs->virgl_2d_fs_tex;
            out->ve_handle      = gs->virgl_2d_ve;
            out->sampler        = gs->virgl_2d_sampler;
            out->sampler_linear = gs->virgl_2d_sampler_linear;
            out->fb_width       = gs->fb_width;
            out->fb_height      = gs->fb_height;
            break;
        }

        case VGB_OP_SUBMIT3D:
        {
            const uint32 *words = (const uint32 *)(cmd + 1);
            uint32 nbytes = length - sizeof(*cmd);
            if (nbytes == 0 || gs->virgl_ctx_error)
                return GPUERR_BADARGS;
            if (!gs->virgl_2d_ready)
                return GPUERR_NOTIMPL;
            if (!chip_Submit3D(gs, cmd->arg, (void *)words, nbytes))
                return GPUERR_LOST;
            break;
        }

        case VGB_OP_FLUSHRECT:
        {
            const struct VgbFlushRect *fr = (const struct VgbFlushRect *)cmd;
            if (length < sizeof(*fr))
                return GPUERR_BADARGS;
            if (!gs->virgl_2d_ready)
                return GPUERR_NOTIMPL;
            if (!chip_ResourceFlush(gs, fr->hdr.arg, fr->x, fr->y,
                                    fr->w, fr->h))
                return GPUERR_LOST;
            break;
        }

        case VGB_OP_TRITEST:
            if (!gs->virgl_2d_ready)
                return GPUERR_NOTIMPL;
            if (!chip_virgl_draw_test_triangle(gs))
                return GPUERR_LOST;
            break;

        case VGB_OP_V3DCALL:
        {
            struct VgbV3DCall *vc = (struct VgbV3DCall *)cmd;
            if (length < sizeof(*vc))
                return GPUERR_BADARGS;
            if (!chip_v3d_backend_call(gs, cmd->arg, vc->a))
                return GPUERR_LOST;
            break;
        }

        default:
            return GPUERR_NOTIMPL;
    }

    vgb_fenceSeq = (vgb_fenceSeq + 1) & 0x00FFFFFF;

    /* Async-fence contract (Phase 3.3): the chip transport completes
       synchronously inside the op (chip_do_io waits the ring), so the
       fence is retired RIGHT HERE -- report it. Fire-and-forget into the
       server's own HI port (we run in the server task; no wait, no
       deadlock). When the chip gains true IRQ-deferred completions, only
       this call site moves to the completion path. */
    if (vgb_IGpu != NULL && vgb_backendId >= 0)
        vgb_IGpu->GPU_FenceRetired(vgb_backendId, (int32)vgb_fenceSeq);

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

/* ---- exclusive display hand-off hook (Phase 6) --------------------------
 * ACQUIRE: park the flush task's desktop presentation and hand the caller
 * the scanout geometry. RELEASE: resume + force a full-frame refresh so
 * the desktop reappears. Runs in the acquiring client's task context.  */

static uint32 vgb_display_hook_entry(struct Hook *hook, APTR object,
                                     APTR message)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)hook->h_Data;
    struct GpuDisplayMsg *msg = (struct GpuDisplayMsg *)message;
    (void)object;

    switch (msg->Op)
    {
        case GPUDISP_ACQUIRE:
            gs->gpub_display_parked = TRUE;
            msg->Width       = gs->fb_width;
            msg->Height      = gs->fb_height;
            msg->PixelFormat = 0;   /* scanout is B8G8R8X8; RGBFTYPE n/a */
            msg->BackendData = (APTR)gs->resource_id;
            DCHIP("gpu_backend: display ACQUIRED (%lux%lu res=%lu) -- "
                  "desktop presentation parked",
                  (unsigned long)gs->fb_width,
                  (unsigned long)gs->fb_height,
                  (unsigned long)gs->resource_id);
            return 0;

        case GPUDISP_RELEASE:
            gs->gpub_display_parked = FALSE;
            chip_flush_signal_activity(gs);   /* full refresh next frame */
            DCHIP("gpu_backend: display RELEASED -- desktop restored");
            return 0;

        default:
            return 1;
    }
}

static struct Hook vgb_display_hook;

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

    vgb_display_hook.h_Entry = (HOOKFUNC)vgb_display_hook_entry;
    vgb_display_hook.h_Data  = gs;

    vgb_backendId = vgb_IGpu->GPU_RegisterBackendA(&info, GPU_TAGS(
                        { GPUTAG_AsyncFences, TRUE },
                        { GPUTAG_DisplayHook, (uint32)&vgb_display_hook }));
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
