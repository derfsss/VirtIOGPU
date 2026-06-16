/*
 * chip_perf.c -- Phase 7 performance experiments.
 *
 * Three optional fast paths, each gated by an ENV: var read once at the
 * first SetSwitch (chip_apply_perf_env).  Default build behaviour is
 * unchanged: when no var is set the driver runs the proven convert +
 * full-frame flush path.  See docs/PHASE7_PERF_PLAN.md.
 *
 *   Item 1  zero-copy 32bpp scanout   ENV:virtiogpu_zerocopy
 *   Item 2  exact dirty-rect tracking ENV:virtiogpu_dirtyrect
 *   Item 3  GPU large-fill accel      ENV:virtiogpu_gpuaccel  (in chip_blit.c)
 *
 * chip_apply_perf_env, chip_mark_dirty and chip_dirty_take cover items
 * 2/3; chip_zc_* implement item 1.
 */

#include "chip/chip_state.h"
#include <dos/dos.h>
#include <proto/dos.h>

/* -----------------------------------------------------------------------
 * chip_apply_perf_env -- read the ENV: toggles once and latch them.
 *
 * Deferred to first SetSwitch (like the null-vtable check) because
 * dos.library / ENV: are not available during chip_InitCard_C.  dos is
 * already opened by chip_apply_null_vtable_overrides which runs just
 * before this in chip_SetSwitch.
 * ----------------------------------------------------------------------- */
void chip_apply_perf_env(struct ChipGPUState *gs, struct BoardInfo *bi)
{
    struct ExecIFace *IExec = gs->IExec;

    if (!gs->DOSBase) {
        gs->DOSBase = IExec->OpenLibrary("dos.library", 54);
        if (gs->DOSBase && !gs->IDOS)
            gs->IDOS = (struct DOSIFace *)
                IExec->GetInterface(gs->DOSBase, "main", 1, NULL);
    }
    if (!gs->IDOS) {
        DCHIP("perf-env: dos.library unavailable -- all experiments OFF");
        return;
    }

    char buf[8];
    gs->zc_enabled       = (gs->IDOS->GetVar("virtiogpu_zerocopy",
                              buf, sizeof(buf), 0) >= 0);
    gs->dirty_enabled    = (gs->IDOS->GetVar("virtiogpu_dirtyrect",
                              buf, sizeof(buf), 0) >= 0);
    gs->gpuaccel_enabled = (gs->IDOS->GetVar("virtiogpu_gpuaccel",
                              buf, sizeof(buf), 0) >= 0);
    gs->virgl2d_enabled  = (gs->IDOS->GetVar("virtiogpu_virgl2d",
                              buf, sizeof(buf), 0) >= 0);

    if (gs->gpuaccel_min_area == 0)
        gs->gpuaccel_min_area = 64 * 64;   /* default large-rect threshold */

    DCHIP("perf-env: zerocopy=%s dirtyrect=%s gpuaccel=%s virgl2d=%s (min_area=%lu)",
          gs->zc_enabled ? "ON" : "off",
          gs->dirty_enabled ? "ON" : "off",
          gs->gpuaccel_enabled ? "ON" : "off",
          gs->virgl2d_enabled ? "ON" : "off",
          (ULONG)gs->gpuaccel_min_area);

    /* Virgl 2D / HW-composite bring-up.  Deferred here (not InitCard) so it
     * can be ENV-gated.  Brings up the 3D scanout context + pipeline, then
     * installs the CompositeTagList hook and advertises DIPF_IS_HWCOMPOSITE
     * so AOS4 routes window compositing through us.  Requires the host to
     * have negotiated VIRGL (gl=on).  Default off -> none of this runs and
     * the driver behaves exactly as before. */
    if (gs->virgl2d_enabled && gs->has_virgl && !gs->virgl_2d_ready) {
        DCHIP("perf-env: virgl2d -- bringing up Virgl 2D + composite hook");
        if (chip_virgl_init_2d(gs)) {
            chip_comp_install_hook(gs);
            chip_comp_set_dipf_flags(gs);
            DCHIP("perf-env: Virgl 2D ACTIVE (res=%lu) + composite hook installed",
                  (ULONG)gs->virgl_2d_resource);
        } else {
            DCHIP("perf-env: Virgl 2D init FAILED -- staying on convert path");
        }
    } else if (gs->virgl2d_enabled && !gs->has_virgl) {
        DCHIP("perf-env: virgl2d requested but VIRGL not negotiated (gl=off) -- ignored");
    }

    /* Phase 6 "first triangle" milestone (ENV:virtiogpu_tritest): draw an RGB
     * 3D triangle via virgl DRAW_VBO each frame from the flush task.  Requires
     * the virgl 3D pipeline (virgl2d) to be up.  Proves the virgl 3D path
     * renders real geometry on the host GL -- the substrate for Warp3D/MiniGL. */
    if (gs->virgl_2d_ready &&
        gs->IDOS->GetVar("virtiogpu_tritest", buf, sizeof(buf), 0) >= 0) {
        gs->virgl_test_quad = 3;
        DCHIP("perf-env: tritest ON -- RGB 3D triangle each frame (Phase 6)");
    }

    /* Item 2 needs every screen write routed through the vtable, so the
     * direct-access grant must be withdrawn.  graphics.library re-reads
     * bi->Flags when a screen is (re)opened, so this takes effect on the
     * next SetGC/SetPanning even though we are past InitCard. */
    if (gs->dirty_enabled && bi) {
        if (bi->Flags & BIF_GRANTDIRECTACCESS) {
            bi->Flags &= ~(ULONG)BIF_GRANTDIRECTACCESS;
            DCHIP("perf-env: cleared BIF_GRANTDIRECTACCESS for dirty-rect "
                  "tracking (Flags=0x%lx)", bi->Flags);
        }
    }
}

/* -----------------------------------------------------------------------
 * Item 2 -- dirty-rectangle accumulator.
 * ----------------------------------------------------------------------- */
void chip_mark_dirty(struct ChipGPUState *gs, WORD x, WORD y, UWORD w, UWORD h)
{
    if (!gs || !gs->dirty_enabled || !gs->dirty_lock) return;
    if (w == 0 || h == 0) return;

    /* Half-open bbox.  Compute the far edge in LONG to avoid WORD wrap on
     * a rect that runs off the right/bottom; chip_dirty_take clamps to the
     * visible area. */
    LONG x1 = (LONG)x + (LONG)w;
    LONG y1 = (LONG)y + (LONG)h;
    if (x1 > 0x7FFF) x1 = 0x7FFF;
    if (y1 > 0x7FFF) y1 = 0x7FFF;

    gs->IExec->MutexObtain(gs->dirty_lock);
    if (!gs->dirty_valid) {
        gs->dirty_x0 = x;      gs->dirty_y0 = y;
        gs->dirty_x1 = (WORD)x1; gs->dirty_y1 = (WORD)y1;
        gs->dirty_valid = TRUE;
    } else {
        if (x          < gs->dirty_x0) gs->dirty_x0 = x;
        if (y          < gs->dirty_y0) gs->dirty_y0 = y;
        if ((WORD)x1   > gs->dirty_x1) gs->dirty_x1 = (WORD)x1;
        if ((WORD)y1   > gs->dirty_y1) gs->dirty_y1 = (WORD)y1;
    }
    gs->IExec->MutexRelease(gs->dirty_lock);
}

/* Snapshot the pending region and clear it.  Returns the rect to present.
 *  - tracking OFF  -> always the full active rect (unifies the flush path).
 *  - tracking ON, region pending -> the clamped union bbox.
 *  - tracking ON, nothing pending -> FALSE (idle frame: present nothing).
 */
BOOL chip_dirty_take(struct ChipGPUState *gs,
                     WORD *x, WORD *y, UWORD *w, UWORD *h)
{
    uint32 fw = gs->active_width  < gs->fb_width  ? gs->active_width  : gs->fb_width;
    uint32 fh = gs->active_height < gs->fb_height ? gs->active_height : gs->fb_height;
    if (fw == 0 || fh == 0) return FALSE;

    if (!gs->dirty_enabled || !gs->dirty_lock) {
        *x = 0; *y = 0; *w = (UWORD)fw; *h = (UWORD)fh;
        return TRUE;
    }

    gs->IExec->MutexObtain(gs->dirty_lock);
    BOOL valid = gs->dirty_valid;
    LONG x0 = gs->dirty_x0, y0 = gs->dirty_y0;
    LONG x1 = gs->dirty_x1, y1 = gs->dirty_y1;
    gs->dirty_valid = FALSE;
    gs->IExec->MutexRelease(gs->dirty_lock);

    if (!valid) return FALSE;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (LONG)fw) x1 = (LONG)fw;
    if (y1 > (LONG)fh) y1 = (LONG)fh;
    if (x0 >= x1 || y0 >= y1) return FALSE;

    *x = (WORD)x0; *y = (WORD)y0;
    *w = (UWORD)(x1 - x0); *h = (UWORD)(y1 - y0);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Item 1 -- zero-copy 32bpp scanout.
 * ----------------------------------------------------------------------- */

/* Eligible only on the gl=off (pixman) path with a plain, contiguous
 * 32bpp A8R8G8B8 screen bitmap -- no virgl (X8R8G8B8 R/B-swaps on gl=on),
 * no virtual width, no panning offset. */
static BOOL chip_zc_eligible(struct ChipGPUState *gs)
{
    if (!gs->zc_enabled) return FALSE;
    /* Defer to the virgl 3D scanout path only when it's actually active --
     * NOT merely because virgl is available.  With virgl2d off the plain 2D
     * scanout is used, where zero-copy (board_mem directly backing the 2D
     * scanout, native byte order, no board_mem->fb_mem convert) applies. */
    if (gs->virgl2d_enabled) return FALSE;
    if (gs->active_format != RGBFB_A8R8G8B8) return FALSE;
    if (!gs->panning_mem) return FALSE;
    if (gs->pan_xoff != 0 || gs->pan_yoff != 0) return FALSE;
    if (gs->panning_width &&
        gs->panning_width != gs->active_width) return FALSE;
    if (gs->active_width == 0 || gs->active_height == 0) return FALSE;
    return TRUE;
}

/* Release the direct resource + DMA mapping and restore the fb_mem-backed
 * scanout.  Safe to call when nothing is set up. */
void chip_zc_teardown(struct ChipGPUState *gs)
{
    if (!gs) return;
    struct ExecIFace *IExec = gs->IExec;

    if (gs->zc_active || gs->zc_resource) {
        /* Restore the conventional scanout BEFORE freeing the direct
         * resource so a concurrent present never sees a dead resource. */
        uint32 aw = gs->active_width  < gs->fb_width  ? gs->active_width  : gs->fb_width;
        uint32 ah = gs->active_height < gs->fb_height ? gs->active_height : gs->fb_height;
        if (aw && ah)
            chip_SetScanout(gs, 0, gs->fb_resource_id, 0, 0, aw, ah);
        gs->resource_id = gs->fb_resource_id;
    }

    if (gs->zc_resource) {
        chip_ResourceUnref(gs, gs->zc_resource);
        gs->zc_resource = 0;
    }
    if (gs->zc_dma_list) {
        IExec->FreeSysObject(ASOT_DMAENTRY, gs->zc_dma_list);
        gs->zc_dma_list = NULL;
    }
    if (gs->zc_mem) {
        IExec->EndDMA(gs->zc_mem, gs->zc_size, DMA_ReadFromRAM | DMAF_NoModify);
        gs->zc_mem = NULL;
    }
    gs->zc_dma_count = 0;
    gs->zc_size = 0;
    gs->zc_w = gs->zc_h = 0;
    gs->zc_active = FALSE;
}

/* Ensure the direct resource exists, is sized to the active mode, and is
 * backed by the current panning surface.  Tears down + rebuilds when the
 * surface pointer or dimensions change (screen switch / mode change). */
void chip_zc_update(struct ChipGPUState *gs)
{
    if (!gs) return;
    struct ExecIFace *IExec = gs->IExec;

    if (!chip_zc_eligible(gs)) {
        if (gs->zc_active || gs->zc_resource) {
            DCHIP("zc: no longer eligible -- tearing down direct scanout");
            chip_zc_teardown(gs);
        }
        return;
    }

    uint32 w = gs->active_width;
    uint32 h = gs->active_height;

    /* Already current? */
    if (gs->zc_active && gs->zc_mem == gs->panning_mem &&
        gs->zc_w == w && gs->zc_h == h)
        return;

    DCHIP("zc: (re)attach direct scanout mem=%p %lux%lu (was mem=%p %lux%lu)",
          gs->panning_mem, (ULONG)w, (ULONG)h,
          gs->zc_mem, (ULONG)gs->zc_w, (ULONG)gs->zc_h);

    /* Drop the previous direct mapping (keep displaying the old resource
     * until the new SET_SCANOUT below succeeds). */
    uint32 old_res = gs->zc_resource;
    APTR   old_mem = gs->zc_mem;
    uint32 old_size = gs->zc_size;
    struct DMAEntry *old_dma = gs->zc_dma_list;
    gs->zc_resource = 0;
    gs->zc_mem = NULL;
    gs->zc_dma_list = NULL;

    uint32 stride = w * 4;
    uint32 size   = h * stride;

    /* The host reads these guest pages over DMA on TRANSFER_TO_HOST, so
     * make CPU pixel writes visible in physical memory (write-through),
     * same policy as fb_mem. */
    chip_immu_set_writethrough(IExec, gs->panning_mem, size);

    uint32 dma_count = IExec->StartDMA(gs->panning_mem, size, DMA_ReadFromRAM);
    if (dma_count == 0) {
        DCHIP("zc: StartDMA failed -- staying on convert path");
        goto restore_old;
    }
    struct DMAEntry *dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, dma_count, TAG_DONE);
    if (!dma) {
        DCHIP("zc: DMA list alloc failed");
        IExec->EndDMA(gs->panning_mem, size, DMA_ReadFromRAM | DMAF_NoModify);
        goto restore_old;
    }
    IExec->GetDMAList(gs->panning_mem, size, DMA_ReadFromRAM, dma);

    /* StartDMA may re-set the region's cache mode (cache-inhibited/coherent
     * for the device), clobbering the write-through set above -- which halved
     * board_mem write speed in the zc A/B (copyToVRAM 4586->2674 MiB/s).
     * Re-assert WRITE-THROUGH AFTER the DMA mapping: WT is DMA-safe (CPU
     * writes still reach physical RAM, so the device's scanout DMA sees
     * current pixels) and restores fast CPU draws. */
    chip_immu_set_writethrough(IExec, gs->panning_mem, size);

    uint32 res = chip_alloc_resource_id(gs);
    if (!chip_ResourceCreate2D(gs, res, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM, w, h)) {
        DCHIP("zc: CREATE_2D failed");
        goto fail_new;
    }
    if (!chip_ResourceAttachBacking(gs, res, dma, dma_count)) {
        DCHIP("zc: ATTACH_BACKING failed");
        chip_ResourceUnref(gs, res);
        goto fail_new;
    }
    if (!chip_SetScanout(gs, 0, res, 0, 0, w, h)) {
        DCHIP("zc: SET_SCANOUT failed");
        chip_ResourceUnref(gs, res);
        goto fail_new;
    }

    /* New mapping is live -- commit it and release the previous one. */
    gs->zc_resource  = res;
    gs->zc_mem       = gs->panning_mem;
    gs->zc_size      = size;
    gs->zc_dma_list  = dma;
    gs->zc_dma_count = dma_count;
    gs->zc_w = w; gs->zc_h = h;
    gs->zc_active = TRUE;
    gs->resource_id = res;

    if (old_res)  chip_ResourceUnref(gs, old_res);
    if (old_dma)  IExec->FreeSysObject(ASOT_DMAENTRY, old_dma);
    if (old_mem)  IExec->EndDMA(old_mem, old_size, DMA_ReadFromRAM | DMAF_NoModify);

    DCHIP("zc: direct scanout live res=%lu %lux%lu (no convert)",
          (ULONG)res, (ULONG)w, (ULONG)h);
    return;

fail_new:
    IExec->FreeSysObject(ASOT_DMAENTRY, dma);
    IExec->EndDMA(gs->panning_mem, size, DMA_ReadFromRAM | DMAF_NoModify);
restore_old:
    /* Could not build the new mapping -- keep the old one if it was live,
     * otherwise fall back to the convert path. */
    gs->zc_resource = old_res;
    gs->zc_mem      = old_mem;
    gs->zc_size     = old_size;
    gs->zc_dma_list = old_dma;
    if (!old_res) {
        gs->zc_active = FALSE;
        gs->resource_id = gs->fb_resource_id;
    }
}

/* Present a rect via the direct resource: transfer the dirty box + flush,
 * no conversion.  Returns FALSE if zero-copy isn't active (caller uses the
 * convert path). */
BOOL chip_zc_present(struct ChipGPUState *gs,
                     WORD x, WORD y, UWORD w, UWORD h)
{
    if (!gs->zc_active || !gs->zc_resource) return FALSE;
    if (w == 0 || h == 0) return TRUE;   /* nothing to do, but zc owns the frame */

    uint32 stride = gs->zc_w * 4;
    uint64 offset = (uint64)((uint32)y * stride + (uint32)x * 4);

    if (chip_TransferToHost2D(gs, gs->zc_resource, x, y, w, h, offset))
        chip_ResourceFlush(gs, gs->zc_resource, x, y, w, h);

    return TRUE;
}
