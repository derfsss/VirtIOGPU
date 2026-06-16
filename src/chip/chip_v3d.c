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
#include "virgl/virgl_cmd.h"

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
    info->vbuf_size     = 65536;             /* chip vbuf (V2D_VBUF_SIZE) */
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

/* ----------------------------------------------------------------------- */
/* Milestone 2: own render target + overlay compositing                     */
/* ----------------------------------------------------------------------- */
static uint32 v3d_handle(struct ChipGPUState *gs)
{
    if (gs->v3d_next_handle < 200) gs->v3d_next_handle = 200;
    return gs->v3d_next_handle++;
}

static BOOL v3d_AllocRenderTarget(struct V3DIFace *Self, APTR token,
                                  uint32 w, uint32 h,
                                  uint32 *res_out, uint32 *surface_out)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    uint32 res, surf, words[16];
    struct VirglCmdBuf cb;
    (void)Self;

    if (!gs || !res_out || !surface_out || !w || !h) return FALSE;
    if (!gs->virgl_2d_ready || gs->virgl_ctx_error)   return FALSE;

    res = chip_alloc_resource_id(gs);
    /* Match the scanout's format (B8G8R8X8) so the composite BLIT is 1:1. */
    if (!chip_ResourceCreate3D(gs, res, PIPE_TEXTURE_2D,
            PIPE_FORMAT_B8G8R8X8_UNORM,
            PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
            w, h, 1, 1, 0, 0, 0)) {
        DCHIP("v3d: AllocRenderTarget RESOURCE_CREATE_3D failed");
        return FALSE;
    }
    chip_CTXAttachResource(gs, gs->virgl_2d_ctx, res);

    surf = v3d_handle(gs);
    virgl_cmd_init(&cb, words, 16);
    virgl_cmd_create_surface(&cb, surf, res, PIPE_FORMAT_B8G8R8X8_UNORM, 0, 0);
    if (!chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4)) {
        DCHIP("v3d: AllocRenderTarget create_surface FAILED");
        chip_ResourceUnref(gs, res);
        return FALSE;
    }

    *res_out     = res;
    *surface_out = surf;
    DCHIP("v3d: AllocRenderTarget -> res=%lu surf=%lu %lux%lu",
          (unsigned long)res, (unsigned long)surf,
          (unsigned long)w, (unsigned long)h);
    return TRUE;
}

static BOOL v3d_AllocDepthBuffer(struct V3DIFace *Self, APTR token,
                                 uint32 w, uint32 h,
                                 uint32 *res_out, uint32 *surface_out)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    uint32 res, surf, words[16];
    struct VirglCmdBuf cb;
    (void)Self;

    if (!gs || !res_out || !surface_out || !w || !h) return FALSE;
    if (!gs->virgl_2d_ready || gs->virgl_ctx_error)   return FALSE;

    res = chip_alloc_resource_id(gs);
    if (!chip_ResourceCreate3D(gs, res, PIPE_TEXTURE_2D,
            PIPE_FORMAT_Z24X8_UNORM, PIPE_BIND_DEPTH_STENCIL,
            w, h, 1, 1, 0, 0, 0)) {
        DCHIP("v3d: AllocDepthBuffer RESOURCE_CREATE_3D failed");
        return FALSE;
    }
    chip_CTXAttachResource(gs, gs->virgl_2d_ctx, res);

    surf = v3d_handle(gs);
    virgl_cmd_init(&cb, words, 16);
    virgl_cmd_create_surface(&cb, surf, res, PIPE_FORMAT_Z24X8_UNORM, 0, 0);
    if (!chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4)) {
        DCHIP("v3d: AllocDepthBuffer create_surface FAILED");
        chip_ResourceUnref(gs, res);
        return FALSE;
    }
    *res_out     = res;
    *surface_out = surf;
    DCHIP("v3d: AllocDepthBuffer -> res=%lu zsurf=%lu %lux%lu",
          (unsigned long)res, (unsigned long)surf,
          (unsigned long)w, (unsigned long)h);
    return TRUE;
}

static void v3d_RegisterOverlay(struct V3DIFace *Self, APTR token,
                                uint32 rt_res, uint32 sw, uint32 sh,
                                uint32 x, uint32 y, uint32 w, uint32 h,
                                BOOL enable)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    (void)Self;
    if (!gs) return;

    if (enable) {
        gs->v3d_overlay_res = rt_res;
        gs->v3d_overlay_sw  = sw;  gs->v3d_overlay_sh = sh;
        gs->v3d_overlay_x   = x;   gs->v3d_overlay_y  = y;
        gs->v3d_overlay_w   = w;   gs->v3d_overlay_h  = h;
        gs->v3d_overlay_active = TRUE;
        DCHIP("v3d: overlay ON res=%lu %lux%lu -> (%lu,%lu %lux%lu)",
              (unsigned long)rt_res, (unsigned long)sw, (unsigned long)sh,
              (unsigned long)x, (unsigned long)y,
              (unsigned long)w, (unsigned long)h);
    } else {
        gs->v3d_overlay_active = FALSE;
        DCHIP("v3d: overlay OFF");
    }
}

static void v3d_FreeRenderTarget(struct V3DIFace *Self, APTR token,
                                 uint32 res, uint32 surface)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    uint32 words[8];
    struct VirglCmdBuf cb;
    (void)Self;
    if (!gs) return;

    if (gs->v3d_overlay_res == res)
        gs->v3d_overlay_active = FALSE;

    if (surface && !gs->virgl_ctx_error) {
        virgl_cmd_init(&cb, words, 8);
        virgl_cmd_destroy_object(&cb, VIRGL_OBJECT_SURFACE, surface);
        chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4);
    }
    if (res)
        chip_ResourceUnref(gs, res);
}

/* Composite the registered RT onto the scanout -- called by the flush task
 * each frame after flush_all so the 3D output persists over the desktop. */
void chip_v3d_composite_overlay(struct ChipGPUState *gs)
{
    uint32 words[32];
    struct VirglCmdBuf cb;
    uint32 s0;

    if (!gs || !gs->v3d_overlay_active) return;
    if (!gs->virgl_2d_ready || gs->virgl_ctx_error) return;

    s0 = VIRGL_BLIT_S0_MASK(PIPE_MASK_RGBA) |
         VIRGL_BLIT_S0_FILTER(PIPE_TEX_FILTER_NEAREST);

    virgl_cmd_init(&cb, words, 32);
    virgl_cmd_blit(&cb, s0,
        /* dst = scanout; res, format, level, box=x,y,z,w,h,d */
        gs->resource_id, PIPE_FORMAT_B8G8R8X8_UNORM, 0,
        gs->v3d_overlay_x, gs->v3d_overlay_y, 0,
        gs->v3d_overlay_w, gs->v3d_overlay_h, 1,
        /* src = warp3d RT (full) */
        gs->v3d_overlay_res, PIPE_FORMAT_B8G8R8X8_UNORM, 0,
        0, 0, 0,
        gs->v3d_overlay_sw, gs->v3d_overlay_sh, 1);

    if (chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4)) {
        chip_ResourceFlush(gs, gs->resource_id,
                           gs->v3d_overlay_x, gs->v3d_overlay_y,
                           gs->v3d_overlay_w, gs->v3d_overlay_h);
    } else {
        /* Don't keep retrying a failing composite (and don't let it wedge the
         * desktop further) -- drop the overlay. */
        gs->v3d_overlay_active = FALSE;
        DCHIP("v3d: overlay composite BLIT failed -- overlay disabled");
    }
}

/* Vector table -- order MUST match struct V3DIFace in v3d_iface.h. */
const APTR _chip_v3d_Vectors[] __attribute__((used)) =
{
    (APTR)v3d_Obtain,        /* slot[0] */
    (APTR)v3d_Release,       /* slot[1] */
    NULL, NULL,              /* slot[2..3] -- Expunge / Clone */
    (APTR)v3d_ObtainContext,    /* slot[4] */
    (APTR)v3d_Submit,           /* slot[5] */
    (APTR)v3d_Flush,            /* slot[6] */
    (APTR)v3d_ReleaseContext,   /* slot[7] */
    (APTR)v3d_AllocRenderTarget,/* slot[8] */
    (APTR)v3d_RegisterOverlay,  /* slot[9] */
    (APTR)v3d_FreeRenderTarget, /* slot[10] */
    (APTR)v3d_AllocDepthBuffer, /* slot[11] */
    (APTR)-1                    /* sentinel */
};
const struct TagItem _chip_v3d_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)V3D_IFACE_NAME       },
    { MIT_VectorTable, (Tag)_chip_v3d_Vectors    },
    { MIT_Version,     V3D_IFACE_VERSION         },
    { MIT_DataSize,    sizeof(struct V3DIFace)   },
    { TAG_DONE,        0                         }
};
