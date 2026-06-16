/*
 * chip_vram.c -- Per-board-bitmap GPU render-target cache (VRAM emulation).
 *
 * In the BIF_BLITTER regime (v53.165) graphics.library allocates Workbench's
 * off-screen window bitmaps in board_mem (via our AllocCardMem) and composites
 * window content INTO those bitmaps via IGraphics->CompositeTagList -- which we
 * hook.  The legacy HW path rendered every composite into the scanout surface,
 * which is wrong for an off-screen destination (it painted window content at
 * the wrong place on screen).  VRAM emulation fixes this: each off-screen dst
 * board_mem bitmap is mirrored by its own GPU 3D RENDER_TARGET resource
 * (ATTACH_BACKING'd to the bitmap's board_mem pages), the composite renders
 * into THAT resource, and the result is transferred back to board_mem so the
 * existing board_mem -> screen blit/flush path presents it.
 *
 * This is Milestone 0 of the VRAM rearchitecture: correctness first.  It still
 * does a TRANSFER_TO_HOST (sync the current dst pixels up) + draw + a
 * TRANSFER_FROM_HOST (sync the result back) per composite.  Later milestones
 * keep the bitmaps GPU-resident and drop the round-trip transfers.
 *
 * The cache is keyed by Planes[0] (stable while the board_mem block is
 * allocated).  chip_FreeCardMem calls chip_vram_invalidate to drop the entry
 * when graphics.library frees (and may reuse) the block.
 *
 * Pixel format: board bitmaps are 32bpp X8R8G8B8 (== PIPE_FORMAT_B8G8R8X8_UNORM
 * byte order on PPC BE, same as the scanout resource).  Non-32bpp dsts fall
 * back to software.
 */

#include "chip/chip_state.h"
#include "virgl/virgl_cmd.h"
#include <graphics/composite.h>
#include <interfaces/graphics.h>

#define VRAM_MAX_BITMAPS   64
#define VRAM_MAX_DIM       2048   /* skip absurdly large bitmaps -> SW */

struct VramBM {
    APTR    addr;          /* Planes[0] in board_mem (key); NULL = free slot */
    uint32  w, h, stride;  /* bitmap geometry */
    uint32  pipe_fmt;      /* PIPE_FORMAT_* of the resource (alpha vs X) */
    uint32  res_id;        /* GPU 3D render-target resource (0 = none) */
    uint32  surface;       /* render-target surface handle */
    struct DMAEntry *dma_list;
    uint32  dma_count;
    uint32  dma_size;      /* bytes StartDMA'd (== EndDMA size) */
    uint32  last_used;     /* eviction clock */
    uint8   gpu_dirty;     /* host texture newer than board_mem */
    uint8   cpu_dirty;     /* board_mem newer than host texture */
};

static struct VramBM g_vram[VRAM_MAX_BITMAPS];
static uint32 g_vram_clock;

/* Handle allocator -- shares gs->comp_next_handle with chip_composite.c so the
 * 200+ temporary handle space stays unified. */
static uint32 vram_alloc_handle(struct ChipGPUState *gs)
{
    if (gs->comp_next_handle == 0)
        gs->comp_next_handle = 200;
    return gs->comp_next_handle++;
}

static int vram_find(APTR addr)
{
    for (int i = 0; i < VRAM_MAX_BITMAPS; i++)
        if (g_vram[i].addr == addr)
            return i;
    return -1;
}

/* Destroy a slot's GPU resources + DMA mapping. */
static void vram_destroy_slot(struct ChipGPUState *gs, int i)
{
    struct VramBM *e = &g_vram[i];
    if (!e->addr)
        return;

    uint32 ctx_id = gs->virgl_2d_ctx;

    if (e->surface) {
        uint32 cmd_words[16];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 16);
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SURFACE, e->surface);
        virgl_submit(gs, ctx_id, &cbuf);
    }
    if (e->res_id) {
        chip_CTXDetachResource(gs, ctx_id, e->res_id);
        chip_ResourceUnref(gs, e->res_id);
    }
    if (e->dma_list) {
        struct ExecIFace *IExec = gs->IExec;
        IExec->FreeSysObject(ASOT_DMAENTRY, e->dma_list);
        IExec->EndDMA(e->addr, e->dma_size, DMA_ReadFromRAM | DMAF_NoModify);
    }

    DCHIP_V("vram: destroyed slot %ld addr=%p res=%lu",
            (long)i, e->addr, (ULONG)e->res_id);

    e->addr = NULL;
    e->res_id = 0;
    e->surface = 0;
    e->dma_list = NULL;
    e->dma_count = 0;
    e->dma_size = 0;
    e->gpu_dirty = 0;
    e->cpu_dirty = 0;
}

/* Find-or-create the GPU render target for a board_mem bitmap.  Returns the
 * slot, or NULL if the bitmap can't be handled (caller -> SW fallback). */
static struct VramBM *vram_get(struct ChipGPUState *gs,
                                APTR addr, uint32 w, uint32 h, uint32 stride,
                                uint32 pipe_fmt)
{
    if (!addr || w == 0 || h == 0 || stride < w * 4)
        return NULL;
    if (w > VRAM_MAX_DIM || h > VRAM_MAX_DIM)
        return NULL;

    int i = vram_find(addr);
    if (i >= 0) {
        struct VramBM *e = &g_vram[i];
        if (e->w == w && e->h == h && e->stride == stride &&
            e->pipe_fmt == pipe_fmt) {
            e->last_used = ++g_vram_clock;
            return e;            /* cache hit */
        }
        /* Geometry/format changed under the same address -- rebuild. */
        vram_destroy_slot(gs, i);
    }

    /* Pick a free slot, else evict the least-recently-used. */
    int slot = -1;
    uint32 oldest = 0xFFFFFFFFu;
    for (int k = 0; k < VRAM_MAX_BITMAPS; k++) {
        if (!g_vram[k].addr) { slot = k; break; }
        if (g_vram[k].last_used < oldest) { oldest = g_vram[k].last_used; slot = k; }
    }
    if (slot < 0)
        return NULL;
    if (g_vram[slot].addr)
        vram_destroy_slot(gs, slot);

    struct ExecIFace *IExec = gs->IExec;
    uint32 ctx_id = gs->virgl_2d_ctx;
    uint32 size   = h * stride;

    /* board_mem CPU writes must be visible to the host over DMA (same policy
     * as fb_mem / the zero-copy scanout region). */
    chip_immu_set_writethrough(IExec, addr, size);

    uint32 dma_count = IExec->StartDMA(addr, size, DMA_ReadFromRAM);
    if (dma_count == 0) {
        DCHIP("vram: StartDMA failed addr=%p size=%lu", addr, (ULONG)size);
        return NULL;
    }
    struct DMAEntry *dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, dma_count, TAG_DONE);
    if (!dma) {
        DCHIP("vram: DMA list alloc failed (%lu entries)", (ULONG)dma_count);
        IExec->EndDMA(addr, size, DMA_ReadFromRAM | DMAF_NoModify);
        return NULL;
    }
    IExec->GetDMAList(addr, size, DMA_ReadFromRAM, dma);

    uint32 res_id = chip_alloc_resource_id(gs);
    if (!chip_ResourceCreate3D(gs, res_id,
            PIPE_TEXTURE_2D, pipe_fmt,
            PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
            w, h, 1, 1, 0, 0, 0)) {
        DCHIP("vram: CREATE_3D failed %lux%lu", (ULONG)w, (ULONG)h);
        goto fail_dma;
    }
    if (!chip_ResourceAttachBacking(gs, res_id, dma, dma_count)) {
        DCHIP("vram: ATTACH_BACKING failed res=%lu", (ULONG)res_id);
        chip_ResourceUnref(gs, res_id);
        goto fail_dma;
    }
    chip_CTXAttachResource(gs, ctx_id, res_id);

    uint32 surface = vram_alloc_handle(gs);
    {
        uint32 cmd_words[16];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 16);
        virgl_cmd_create_surface(&cbuf, surface, res_id, pipe_fmt, 0, 0);
        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("vram: create_surface failed res=%lu", (ULONG)res_id);
            chip_CTXDetachResource(gs, ctx_id, res_id);
            chip_ResourceUnref(gs, res_id);
            goto fail_dma;
        }
    }

    struct VramBM *e = &g_vram[slot];
    e->addr      = addr;
    e->w         = w;
    e->h         = h;
    e->stride    = stride;
    e->pipe_fmt  = pipe_fmt;
    e->res_id    = res_id;
    e->surface   = surface;
    e->dma_list  = dma;
    e->dma_count = dma_count;
    e->dma_size  = size;
    e->last_used = ++g_vram_clock;
    e->gpu_dirty = 0;
    e->cpu_dirty = 1;   /* board_mem holds the current content initially */

    DCHIP("vram: NEW slot %ld addr=%p %lux%lu stride=%lu res=%lu surf=%lu",
          (long)slot, addr, (ULONG)w, (ULONG)h, (ULONG)stride,
          (ULONG)res_id, (ULONG)surface);
    return e;

fail_dma:
    IExec->FreeSysObject(ASOT_DMAENTRY, dma);
    IExec->EndDMA(addr, size, DMA_ReadFromRAM | DMAF_NoModify);
    return NULL;
}

/* Upload a board_mem sub-rect into the host texture (board_mem -> host). */
static void vram_sync_to_host(struct ChipGPUState *gs, struct VramBM *e,
                               uint32 x, uint32 y, uint32 w, uint32 h)
{
    if (!e->cpu_dirty)
        return;
    struct virtio_gpu_box box = {0};
    box.x = x; box.y = y; box.z = 0;
    box.w = w; box.h = h; box.d = 1;
    uint64 offset = (uint64)((uint32)y * e->stride + (uint32)x * 4);
    chip_TransferToHost3D(gs, gs->virgl_2d_ctx, e->res_id,
                          0, e->stride, 0, offset, &box);
    e->cpu_dirty = 0;
}

/* Download a host-texture sub-rect back into board_mem (host -> board_mem). */
static void vram_sync_from_host(struct ChipGPUState *gs, struct VramBM *e,
                                 uint32 x, uint32 y, uint32 w, uint32 h)
{
    if (!e->gpu_dirty)
        return;
    struct virtio_gpu_box box = {0};
    box.x = x; box.y = y; box.z = 0;
    box.w = w; box.h = h; box.d = 1;
    uint64 offset = (uint64)((uint32)y * e->stride + (uint32)x * 4);
    chip_TransferFromHost3D(gs, gs->virgl_2d_ctx, e->res_id,
                            0, e->stride, 0, offset, &box);
    e->gpu_dirty = 0;
}

/* Bind the off-screen target as the render target (framebuffer + viewport +
 * scissor), draw the composite into it, then restore the scanout target. */
uint32 chip_vram_composite(struct ChipGPUState *gs,
                            uint32 op,
                            struct BitMap *Source,
                            const void *src_data, uint32 src_bpr,
                            uint32 src_format,
                            int32 src_x, int32 src_y, int32 src_w, int32 src_h,
                            struct BitMap *Destination,
                            int32 dst_x, int32 dst_y, int32 dst_w, int32 dst_h,
                            uint32 flags, uint32 color0)
{
    if (!gs->virgl_2d_ready || !gs->virgl_shaders_ok || !Destination)
        return COMPERR_SoftwareFallback;

    /* Destination geometry from the BitMap (SDK-validated fields). */
    APTR   dst_addr   = Destination->Planes[0];
    uint32 dst_stride = (uint32)Destination->BytesPerRow;
    uint32 bm_w = 0, bm_h = 0, bm_fmt = RGBFB_A8R8G8B8;
    if (gs->IGraphics) {
        bm_w   = (uint32)gs->IGraphics->GetBitMapAttr(Destination, BMA_WIDTH);
        bm_h   = (uint32)gs->IGraphics->GetBitMapAttr(Destination, BMA_HEIGHT);
        bm_fmt = (uint32)gs->IGraphics->GetBitMapAttr(Destination, BMA_PIXELFORMAT);
    }
    if (chip_format_bpp((RGBFTYPE)bm_fmt) != 4)
        return COMPERR_SoftwareFallback;   /* 32bpp dst only */

    /* Choose the resource format from the operation, not just the bitmap:
     *  - COMPFLAG_IgnoreDestAlpha (the common case for the opaque on-screen
     *    composition buffer) means "treat the destination as opaque" -- use a
     *    no-alpha target (B8G8R8X8) so dst.a reads 1.0 and the result is
     *    opaque.  Storing the real (often 0) dst alpha here made the buffer
     *    partly transparent and the desktop bled through blue.
     *  - Only when IgnoreDestAlpha is CLEAR on a true A8R8G8B8 bitmap do we
     *    preserve alpha (B8G8R8A8) so genuine semi-transparent menus/shadows
     *    accumulate alpha correctly.
     * BYTE ORDER (critical): this resource is DMA-backed by board_mem
     * DIRECTLY -- no byte-reverse.  The scanout/flush path byte-reverses
     * board_mem -> fb_mem before feeding a B8G8R8X8 (enum 2) resource, so the
     * "B-first" formats expect REVERSED bytes [B,G,R,X].  board_mem is native
     * PPC-BE A8R8G8B8 = bytes [A,R,G,B], so we must use the NATIVE-order
     * formats A8R8G8B8 (enum 3) / X8R8G8B8 (enum 4) -- exactly what item-1
     * zero-copy proved correct for a directly-board_mem-backed resource.
     * Using B8G8R8X8/A8 here scrambled the channels (desktop bled through
     * blue). */
    BOOL ignore_dst_alpha = (flags & COMPFLAG_IgnoreDestAlpha) != 0;
    uint32 pipe_fmt = (bm_fmt == RGBFB_A8R8G8B8 && !ignore_dst_alpha)
                        ? PIPE_FORMAT_A8R8G8B8_UNORM
                        : PIPE_FORMAT_X8R8G8B8_UNORM;

    /* Clamp the dst rect to the bitmap so transfers/draw stay in bounds. */
    if (dst_x < 0 || dst_y < 0 || dst_w <= 0 || dst_h <= 0)
        return COMPERR_SoftwareFallback;
    if ((uint32)dst_x >= bm_w || (uint32)dst_y >= bm_h)
        return COMPERR_SoftwareFallback;
    uint32 tw = (uint32)dst_w, th = (uint32)dst_h;
    if ((uint32)dst_x + tw > bm_w) tw = bm_w - (uint32)dst_x;
    if ((uint32)dst_y + th > bm_h) th = bm_h - (uint32)dst_y;
    if (tw == 0 || th == 0)
        return COMPERR_SoftwareFallback;

    struct VramBM *e = vram_get(gs, dst_addr, bm_w, bm_h, dst_stride, pipe_fmt);
    if (!e) {
        static volatile UBYTE warn_full = 0;
        if (!warn_full) { warn_full = 1;
            DCHIP("vram: get failed (cache full / unsupported) -- SW fallback (once)"); }
        return COMPERR_SoftwareFallback;
    }

    /* 1. Bring the current dst pixels up to the host texture (so the blend
     *    composites over the real destination content).  board_mem is always
     *    authoritative: between two composites to the same bitmap the CPU
     *    (graphics.library text/fills) may have drawn into it directly, so we
     *    re-upload every time (M0 correctness; M5 will track CPU writes and
     *    keep bitmaps GPU-resident to drop this). */
    e->cpu_dirty = 1;
    vram_sync_to_host(gs, e, (uint32)dst_x, (uint32)dst_y, tw, th);

    /* 2. Bind this target as the render target. */
    uint32 ctx_id = gs->virgl_2d_ctx;
    {
        uint32 cmd_words[32];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 32);
        uint32 surf[1] = { e->surface };
        virgl_cmd_set_framebuffer_state(&cbuf, 1, 0, surf);
        virgl_cmd_set_viewport(&cbuf, 0,
            (float)e->w * 0.5f, (float)e->h * 0.5f, 0.5f,
            (float)e->w * 0.5f, (float)e->h * 0.5f, 0.5f);
        virgl_cmd_set_scissor_state(&cbuf, 0, 0, 0, e->w, e->h);
        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("vram: bind RT failed res=%lu", (ULONG)e->res_id);
            return COMPERR_Generic;
        }
    }

    /* 3. Composite into the bound off-screen target (NDC uses comp_rt_*). */
    gs->comp_rt_active = TRUE;
    gs->comp_rt_w = e->w;
    gs->comp_rt_h = e->h;

    uint32 result = chip_virgl_composite(gs, op, Source,
                                          src_data, src_bpr, src_format,
                                          src_x, src_y, src_w, src_h,
                                          dst_x, dst_y, dst_w, dst_h,
                                          flags, color0);

    gs->comp_rt_active = FALSE;

    /* 4. Restore the scanout render target (framebuffer + viewport + scissor). */
    {
        uint32 cmd_words[32];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 32);
        uint32 surf[1] = { gs->virgl_2d_surface };
        virgl_cmd_set_framebuffer_state(&cbuf, 1, 0, surf);
        virgl_cmd_set_viewport(&cbuf, 0,
            (float)gs->virgl_2d_res_w * 0.5f, (float)gs->virgl_2d_res_h * 0.5f, 0.5f,
            (float)gs->virgl_2d_res_w * 0.5f, (float)gs->virgl_2d_res_h * 0.5f, 0.5f);
        virgl_cmd_set_scissor_state(&cbuf, 0, 0, 0,
            gs->virgl_2d_res_w, gs->virgl_2d_res_h);
        virgl_submit(gs, ctx_id, &cbuf);
    }

    if (result != COMPERR_Success)
        return result;

    /* 5. Transfer the composited region back to board_mem so the existing
     *    board_mem -> screen blit + flush present it. */
    e->gpu_dirty = 1;
    vram_sync_from_host(gs, e, (uint32)dst_x, (uint32)dst_y, tw, th);
    e->cpu_dirty = 0;   /* host and board_mem now match for this rect */

    return COMPERR_Success;
}

void chip_vram_invalidate(struct ChipGPUState *gs, APTR addr)
{
    if (!gs || !addr)
        return;
    int i = vram_find(addr);
    if (i >= 0)
        vram_destroy_slot(gs, i);
}

void chip_vram_teardown(struct ChipGPUState *gs)
{
    if (!gs)
        return;
    for (int i = 0; i < VRAM_MAX_BITMAPS; i++)
        if (g_vram[i].addr)
            vram_destroy_slot(gs, i);
}
