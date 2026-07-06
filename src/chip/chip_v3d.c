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
    info->sampler        = gs->virgl_2d_sampler;
    info->sampler_linear = gs->virgl_2d_sampler_linear;
    info->fb_width      = gs->fb_width;
    info->fb_height     = gs->fb_height;
    info->caps          = 0;
    info->vs3_handle         = gs->virgl_2d_vs3;
    info->fs_modulate_handle = gs->virgl_2d_fs_modulate;
    info->fs_decal_handle    = gs->virgl_2d_fs_decal;
    info->fs_blend_handle    = gs->virgl_2d_fs_blend;
    info->ve3_handle         = gs->virgl_2d_ve3;

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

static BOOL v3d_CreateTexture(struct V3DIFace *Self, APTR token,
                              uint32 w, uint32 h, APTR data, uint32 src_bpr,
                              uint32 *view_out, uint32 *res_out)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    uint32 res, view, words[32];
    struct VirglCmdBuf cb;
    (void)Self;

    if (!gs || !data || !view_out || !res_out || !w || !h) return FALSE;
    if (!gs->virgl_2d_ready || gs->virgl_ctx_error)         return FALSE;

    res = chip_alloc_resource_id(gs);
    if (!chip_ResourceCreate3D(gs, res, PIPE_TEXTURE_2D,
            PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_BIND_SAMPLER_VIEW,
            w, h, 1, 1, 0, 0, 0)) {
        DCHIP("v3d: CreateTexture RESOURCE_CREATE_3D failed");
        return FALSE;
    }
    chip_CTXAttachResource(gs, gs->virgl_2d_ctx, res);

    /* The shared 32bpp uploader GP32-swaps each pixel (needed for command dwords
     * and the BGRA framebuffer path).  A source RAW pixel [R,G,B,A] (BE word
     * 0xRRGGBBAA) thus lands in the texture as bytes [A,B,G,R] -- fully reversed.
     * Under R8G8B8A8_UNORM the texel decodes as (r=A,g=B,b=G,a=R), so we undo the
     * reversal with a reversed sampler-view swizzle (ALPHA,BLUE,GREEN,RED) ->
     * output (R,G,B,A).  (Without this the cow renders magenta/pink: brown
     * 139,90,43,255 -> 255,43,90.) */
    if (!chip_comp_upload_pixels_32bpp(gs, res, (const uint32 *)data,
                                       src_bpr ? src_bpr : w * 4, 0, 0, w, h)) {
        DCHIP("v3d: CreateTexture pixel upload failed");
        chip_ResourceUnref(gs, res);
        return FALSE;
    }

    view = v3d_handle(gs);
    virgl_cmd_init(&cb, words, 32);
    virgl_cmd_create_sampler_view(&cb, view, res, PIPE_FORMAT_R8G8B8A8_UNORM,
        0, 0, PIPE_SWIZZLE_ALPHA, PIPE_SWIZZLE_BLUE,
        PIPE_SWIZZLE_GREEN, PIPE_SWIZZLE_RED);
    if (!chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4)) {
        DCHIP("v3d: CreateTexture sampler view FAILED");
        chip_ResourceUnref(gs, res);
        return FALSE;
    }

    *view_out = view;
    *res_out  = res;
    DCHIP("v3d: CreateTexture -> res=%lu view=%lu %lux%lu",
          (unsigned long)res, (unsigned long)view,
          (unsigned long)w, (unsigned long)h);
    return TRUE;
}

static void v3d_FreeTexture(struct V3DIFace *Self, APTR token,
                            uint32 res, uint32 view)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    uint32 words[8];
    struct VirglCmdBuf cb;
    (void)Self;
    if (!gs) return;
    if (view && !gs->virgl_ctx_error) {
        virgl_cmd_init(&cb, words, 8);
        virgl_cmd_destroy_object(&cb, VIRGL_OBJECT_SAMPLER_VIEW, view);
        chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4);
    }
    if (res) chip_ResourceUnref(gs, res);
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
        /* Mirror the composited frame into board_mem so ReadPixelArray
         * (screenshots) and, on real hardware, the RTG scanout see the 3D
         * output -- QEMU display already came from the BLIT above. */
        chip_overlay_to_board(gs, gs->v3d_overlay_x, gs->v3d_overlay_y,
                              gs->v3d_overlay_w, gs->v3d_overlay_h);
    } else {
        /* Don't keep retrying a failing composite (and don't let it wedge the
         * desktop further) -- drop the overlay. */
        gs->v3d_overlay_active = FALSE;
        DCHIP("v3d: overlay composite BLIT failed -- overlay disabled");
    }
}

/* Milestone 3: read a render target back into a windowed app's bitmap.
 * No overlay -- the app blits the bitmap into its own window. */
static BOOL v3d_PresentBitmap(struct V3DIFace *Self, APTR token, uint32 rt_res,
                              uint32 sw, uint32 sh,
                              APTR dst_base, uint32 dst_stride)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)token;
    struct ExecIFace *IExec;
    uint32 words[32];
    struct VirglCmdBuf cb;
    struct virtio_gpu_box box;
    uint32 s0;
    (void)Self;

    if (!gs || !dst_base || !sw || !sh) return FALSE;
    if (!gs->virgl_2d_ready || gs->virgl_ctx_error) return FALSE;
    IExec = gs->IExec;

    /* (Re)create the readback resource + DMA buffer when the size changes. */
    if (gs->v3d_rb_res == 0 || gs->v3d_rb_w != sw || gs->v3d_rb_h != sh) {
        APTR mem; uint32 n, res; struct DMAEntry *dma;
        if (gs->v3d_rb_mem) {
            chip_dma_free(IExec, gs->v3d_rb_mem,
                          gs->v3d_rb_w * gs->v3d_rb_h * 4);
            gs->v3d_rb_mem = NULL;
        }
        if (gs->v3d_rb_res) { chip_ResourceUnref(gs, gs->v3d_rb_res); gs->v3d_rb_res = 0; }

        mem = IExec->AllocVecTags(sw * sh * 4,
                AVT_Type, MEMF_SHARED, AVT_Alignment, 4096,
                AVT_Contiguous, TRUE, AVT_ClearWithValue, 0, TAG_END);
        if (!mem) return FALSE;
        n = IExec->StartDMA(mem, sw * sh * 4, DMA_ReadFromRAM);
        if (n == 0) { IExec->FreeVec(mem); return FALSE; }
        dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
                ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
        if (!dma) {
            IExec->EndDMA(mem, sw * sh * 4, DMA_ReadFromRAM | DMAF_NoModify);
            IExec->FreeVec(mem); return FALSE;
        }
        IExec->GetDMAList(mem, sw * sh * 4, DMA_ReadFromRAM, dma);

        res = chip_alloc_resource_id(gs);
        if (!chip_ResourceCreate3D(gs, res, PIPE_TEXTURE_2D,
                PIPE_FORMAT_B8G8R8X8_UNORM,
                PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
                sw, sh, 1, 1, 0, 0, 0) ||
            !chip_ResourceAttachBacking(gs, res, dma, n)) {
            IExec->FreeSysObject(ASOT_DMAENTRY, dma);
            IExec->EndDMA(mem, sw * sh * 4, DMA_ReadFromRAM | DMAF_NoModify);
            IExec->FreeVec(mem);
            return FALSE;
        }
        IExec->FreeSysObject(ASOT_DMAENTRY, dma);   /* StartDMA stays open */
        chip_CTXAttachResource(gs, gs->virgl_2d_ctx, res);
        gs->v3d_rb_res = res; gs->v3d_rb_mem = mem;
        gs->v3d_rb_w = sw;    gs->v3d_rb_h = sh;
        DCHIP("v3d: PresentBitmap readback res=%lu %lux%lu",
              (unsigned long)res, (unsigned long)sw, (unsigned long)sh);
    }

    /* BLIT the warp3d RT into the readback resource (1:1, both B8G8R8X8). */
    s0 = VIRGL_BLIT_S0_MASK(PIPE_MASK_RGBA) |
         VIRGL_BLIT_S0_FILTER(PIPE_TEX_FILTER_NEAREST);
    virgl_cmd_init(&cb, words, 32);
    virgl_cmd_blit(&cb, s0,
        gs->v3d_rb_res, PIPE_FORMAT_B8G8R8X8_UNORM, 0, 0, 0, 0, sw, sh, 1,
        rt_res,         PIPE_FORMAT_B8G8R8X8_UNORM, 0, 0, 0, 0, sw, sh, 1);
    {
        static uint32 dbg = 0;
        if (dbg < 3) { dbg++; DCHIP("v3d: Present blit ok, transferring %lux%lu",
                                    (unsigned long)sw, (unsigned long)sh); }
        if (!chip_Submit3D(gs, gs->virgl_2d_ctx, cb.buf, cb.dwords * 4))
            return FALSE;

        /* Pull the readback resource into guest memory, then reverse-convert it
         * (B8G8R8X8 -> active RTG format) into the app's bitmap. */
        chip_zero(&box, sizeof(box));
        box.w = sw; box.h = sh; box.d = 1;
        if (!chip_TransferFromHost3D(gs, gs->virgl_2d_ctx, gs->v3d_rb_res,
                0, sw * 4, 0, 0, &box))
            return FALSE;
        if (dbg <= 3) DCHIP("v3d: Present transfer ok, converting -> %p stride=%lu",
                            dst_base, (unsigned long)dst_stride);

        chip_b8x8_to_active_fmt(gs, gs->v3d_rb_mem, sw * 4, dst_base, dst_stride, sw, sh);
        if (dbg <= 3) DCHIP("v3d: Present done");
    }
    return TRUE;
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
    (APTR)v3d_CreateTexture,    /* slot[12] */
    (APTR)v3d_FreeTexture,      /* slot[13] */
    (APTR)v3d_PresentBitmap,    /* slot[14] */
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

/* -----------------------------------------------------------------------
 * chip_v3d_backend_call -- gpu.library backend dispatcher (Phase 3.2b-2).
 *
 * Routes VGB_OP_V3DCALL methods onto the same static v3d implementations
 * the "v3d" interface exports, so warp3d/W3D render through gpu.library
 * with identical semantics. Self is unused by every v3d method (all take
 * token = gs), so NULL is safe. Runs in the gpu.library server task; the
 * IGpu caller is blocked for the duration, so pointer args in a[] remain
 * valid. Returns 1 on success, 0 on failure.
 * ----------------------------------------------------------------------- */
#include <gpulib/virtio_gpu_backend.h>

int32 chip_v3d_backend_call(struct ChipGPUState *gs, uint32 method,
                            uint32 *a)
{
    APTR tok = (APTR)gs;   /* token is always the chip state */

    switch (method)
    {
        case VGB_V3D_OBTAIN:
            return v3d_ObtainContext(NULL, NULL,
                       (struct V3DContextInfo *)a[0]) ? 1 : 0;
        case VGB_V3D_SUBMIT:
            return v3d_Submit(NULL, tok, a[1],
                       (const uint32 *)a[2], a[3]) ? 1 : 0;
        case VGB_V3D_FLUSH:
            return v3d_Flush(NULL, tok, a[1], a[2], a[3], a[4], a[5]) ? 1 : 0;
        case VGB_V3D_RELEASE:
            v3d_ReleaseContext(NULL, tok);
            return 1;
        case VGB_V3D_ALLOC_RT:
            return v3d_AllocRenderTarget(NULL, tok, a[1], a[2],
                       (uint32 *)a[3], (uint32 *)a[4]) ? 1 : 0;
        case VGB_V3D_OVERLAY:
            v3d_RegisterOverlay(NULL, tok, a[1], a[2], a[3],
                                a[4], a[5], a[6], a[7], (BOOL)a[8]);
            return 1;
        case VGB_V3D_FREE_RT:
            v3d_FreeRenderTarget(NULL, tok, a[1], a[2]);
            return 1;
        case VGB_V3D_ALLOC_Z:
            return v3d_AllocDepthBuffer(NULL, tok, a[1], a[2],
                       (uint32 *)a[3], (uint32 *)a[4]) ? 1 : 0;
        case VGB_V3D_CREATE_TEX:
            return v3d_CreateTexture(NULL, tok, a[1], a[2],
                       (APTR)a[3], a[4],
                       (uint32 *)a[5], (uint32 *)a[6]) ? 1 : 0;
        case VGB_V3D_FREE_TEX:
            v3d_FreeTexture(NULL, tok, a[1], a[2]);
            return 1;
        case VGB_V3D_PRESENT_BM:
            return v3d_PresentBitmap(NULL, tok, a[1], a[2], a[3],
                       (APTR)a[4], a[5]) ? 1 : 0;
        default:
            return 0;
    }
}
