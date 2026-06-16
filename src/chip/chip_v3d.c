/*
 * chip_v3d.c -- "v3d" transport interface implementation.
 *
 * Exposes the chip's already-initialised virgl 3D pipeline to an external
 * renderer (warp3d.library) as a thin transport.  The chip stays the sole
 * owner of the virtio-gpu device; this interface only hands out virgl object
 * handles and forwards pre-encoded command streams.  See include/v3d/v3d_iface.h.
 *
 * Thread safety: chip_Submit3D and chip_ResourceFlush already take the chip's
 * recursive io_lock, so v3d_Submit / v3d_Flush are safe against the periodic
 * flush task without any extra locking here.
 */
#include "chip/chip_state.h"
#include "v3d/v3d_iface.h"

static uint32 v3d_Obtain(struct V3DIFace *Self)  { return Self->Data.RefCount++; }
static uint32 v3d_Release(struct V3DIFace *Self) { return Self->Data.RefCount--; }

static BOOL v3d_ObtainContext(struct V3DIFace *Self, struct BitMap *dest,
                              struct V3DContextInfo *info)
{
    struct ChipGPUState *gs = g_chip_state;
    (void)Self; (void)dest;

    if (!gs || !info)
        return FALSE;
    if (!gs->virgl_2d_ready) {
        DCHIP("v3d: ObtainContext FAILED -- virgl 3D pipeline not ready "
              "(needs gl=on + virtiogpu_virgl2d=1)");
        return FALSE;
    }

    info->token         = (APTR)gs;
    info->ctx_id        = gs->virgl_2d_ctx;
    info->vbuf_res      = gs->virgl_2d_vbuf_res;
    info->vbuf_size     = 256;               /* chip allocates a 256-byte vbuf */
    info->scanout_res   = gs->resource_id;
    info->vs_handle     = gs->virgl_2d_vs;
    info->fs_handle     = gs->virgl_2d_fs;
    info->fs_tex_handle = gs->virgl_2d_fs_tex;
    info->ve_handle     = gs->virgl_2d_ve;
    info->fb_width      = gs->fb_width;
    info->fb_height     = gs->fb_height;
    info->caps          = 0;

    DCHIP("v3d: ObtainContext -> ctx=%lu vbuf=%lu scanout=%lu vs=%lu fs=%lu "
          "ve=%lu %lux%lu",
          (unsigned long)info->ctx_id, (unsigned long)info->vbuf_res,
          (unsigned long)info->scanout_res, (unsigned long)info->vs_handle,
          (unsigned long)info->fs_handle, (unsigned long)info->ve_handle,
          (unsigned long)info->fb_width, (unsigned long)info->fb_height);
    return TRUE;
}

static BOOL v3d_Submit(struct V3DIFace *Self, APTR token, uint32 ctx_id,
                       const uint32 *words, uint32 nwords)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    (void)Self;

    if (!gs || !words || nwords == 0)
        return FALSE;
    if (gs->virgl_ctx_error)
        return FALSE;

    return chip_Submit3D(gs, ctx_id, (void *)words, nwords * 4);
}

static BOOL v3d_Flush(struct V3DIFace *Self, APTR token, uint32 res_id,
                      uint32 x, uint32 y, uint32 w, uint32 h)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    (void)Self;

    if (!gs)
        return FALSE;
    return chip_ResourceFlush(gs, res_id, x, y, w, h);
}

static void v3d_ReleaseContext(struct V3DIFace *Self, APTR token)
{
    (void)Self; (void)token;   /* shared pipeline -- nothing to release (M1) */
}

/* Vector table -- order MUST match struct V3DIFace in v3d_iface.h. */
const APTR _chip_v3d_Vectors[] __attribute__((used)) =
{
    (APTR)v3d_Obtain,        /* slot[0] */
    (APTR)v3d_Release,       /* slot[1] */
    NULL, NULL,              /* slot[2..3] -- Expunge / Clone */
    (APTR)v3d_ObtainContext, /* slot[4] */
    (APTR)v3d_Submit,        /* slot[5] */
    (APTR)v3d_Flush,         /* slot[6] */
    (APTR)v3d_ReleaseContext,/* slot[7] */
    (APTR)-1                 /* sentinel */
};
const struct TagItem _chip_v3d_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)V3D_IFACE_NAME    },
    { MIT_VectorTable, (Tag)_chip_v3d_Vectors },
    { MIT_Version,     V3D_IFACE_VERSION      },
    { TAG_DONE,        0                      }
};
