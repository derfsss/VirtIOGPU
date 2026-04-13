/*
 * chip_gpu_cmds.c -- VirtIO GPU command wrappers for the chip driver.
 *
 * Provides synchronous GPU commands: GET_DISPLAY_INFO, RESOURCE_CREATE_2D,
 * RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH.
 * All commands use polling (no IRQ) and hold gs->io_lock.
 */

#include "chip/chip_state.h"

/* -----------------------------------------------------------------------
 * chip_do_io -- synchronous VirtIO control queue transaction.
 *
 * Submits cmd_buf (cmd_size bytes) + resp_buf (resp_size bytes) on queue 0,
 * kicks the device, polls for completion (no ISR in chip context), and
 * returns the number of bytes written by device into resp_buf.
 * ----------------------------------------------------------------------- */
/* NOTE: Caller MUST hold gs->io_lock for the entire duration
 * (from cmd_buf fill through chip_do_io return).
 * This prevents the flush task from racing with main-thread GPU commands. */
/* Shared wait-for-response: IRQ + Wait(), fallback to polling + yield.
 * Caller has already done AddBuf + notify.  Returns bytes written, 0 on
 * timeout.  Uses gs->ctrlq_wait_task/mask (one DoIO at a time per queue,
 * protected by the caller's io_lock). */
uint32 chip_wait_ctrlq(struct ChipGPUState *gs, struct virtqueue *vq, void *cookie)
{
    struct ExecIFace *IExec = gs->IExec;
    uint32 bytes_written = 0;
    void  *got_cookie    = NULL;

    gs->hot_wait_entries++;
    HOT_STAGE(gs, "wait_ctrlq:enter");

    int8   sig_bit  = -1;
    uint32 sig_mask = 0;
    if (gs->irq_installed) {
        sig_bit = IExec->AllocSignal(-1);
        if (sig_bit >= 0) {
            sig_mask = 1UL << sig_bit;
            IExec->SetSignal(0, sig_mask);
            /* Publish task+mask without Disable/Enable -- matching
             * pair also removed on the retract side.  Store order is
             * task first (so a mid-publish IRQ sees (valid,0) -- the
             * `task && mask` check in chip_irq.c short-circuits on
             * mask=0 and skips the Signal call).  The "memory" clobber
             * prevents the compiler reordering the two stores across
             * each other or across the SetSignal. */
            gs->ctrlq_wait_task = (struct Task *)cookie;
            __asm__ volatile ("" ::: "memory");
            gs->ctrlq_wait_mask = sig_mask;
            __asm__ volatile ("" ::: "memory");
        }
    }

    if (sig_bit >= 0) {
        /* IRQ path: 50 Wait bursts.  Each Wait blocks only until the
         * IRQ fires (fast on normal load), so total budget is bounded
         * by how many consecutive missed IRQs we tolerate before
         * declaring a timeout.  If an IRQ is lost the corresponding
         * burst will block until something else signals the task
         * (e.g. flush-task activity signal), then GetBuf is re-tried. */
        uint32 wait_attempts = 50;
        while (wait_attempts-- > 0) {
            got_cookie = VirtQueue_GetBuf(IExec, vq, &bytes_written);
            if (got_cookie == cookie) break;
            IExec->Wait(sig_mask);
            IExec->SetSignal(0, sig_mask);
        }
        /* Retract publish mask-first -- no Disable()/Enable() here (hottest
         * codepath, one pair removed for investigation).  Race analysis:
         * a concurrent IRQ reads ctrlq_wait_task and ctrlq_wait_mask as two
         * separate loads and short-circuits if either is zero.  With the
         * store order mask=0 then task=NULL, the IRQ can observe either
         * (valid,valid) pre-clear, (valid,0) mid-clear, or (NULL,0) post-
         * clear -- all safe (the (valid,0) case skips the Signal call).
         * The `"memory"` clobber prevents the compiler from reordering
         * these stores across each other or across FreeSignal. */
        gs->ctrlq_wait_mask = 0;
        __asm__ volatile ("" ::: "memory");
        gs->ctrlq_wait_task = NULL;
        __asm__ volatile ("" ::: "memory");
        IExec->FreeSignal(sig_bit);
    } else {
        /* Polling fallback when IRQ unavailable.  Tight busy-poll, no
         * Forbid/Permit yield (being investigated as a possible source
         * of GPU-completion stalls during drawing blit storms). */
        uint32 timeout = 1000000;
        while (timeout-- > 0) {
            got_cookie = VirtQueue_GetBuf(IExec, vq, &bytes_written);
            if (got_cookie == cookie) break;
        }
    }

    HOT_STAGE(gs, "wait_ctrlq:exit");
    gs->hot_wait_exits++;
    if (got_cookie != cookie) {
        /* Drain stale used-ring entries so they don't cascade-break
         * subsequent calls (the "TransferToHost2D failed x50" pattern
         * seen in v53.88 logs). */
        uint32 drained = 0;
        uint32 drain_bytes;
        while (VirtQueue_GetBuf(IExec, vq, &drain_bytes)) {
            drained++;
            if (drained > 256) break;
        }
        static uint32 timeout_log_count = 0;
        if (timeout_log_count < 10) {
            timeout_log_count++;
            DCHIP("wait_ctrlq: TIMEOUT (drained %lu stale) log %lu/10",
                  (unsigned long)drained, (unsigned long)timeout_log_count);
        }
        return 0;
    }
    return bytes_written;
}

/* Public: caller MUST hold gs->io_lock for the entire transaction.
 * Returns bytes written into resp_buf, 0 on timeout / queue error. */
uint32 chip_do_io(struct ChipGPUState *gs, uint32 cmd_size, uint32 resp_size)
{
    struct ExecIFace *IExec = gs->IExec;
    struct virtqueue *vq    = gs->vqs[VIRTIO_GPU_CTRLQ];

    if (!vq) {
        DCHIP("chip_do_io: control queue not initialised");
        return 0;
    }

    struct vring_sg sg[2];
    sg[0].addr = gs->cmd_buf_phys;
    sg[0].len  = cmd_size;
    sg[1].addr = gs->resp_buf_phys;
    sg[1].len  = resp_size;

    void *cookie = (void *)IExec->FindTask(NULL);

    int32 rc = VirtQueue_AddBuf(IExec, vq, sg, 1, 1, cookie);
    if (rc != 0) {
        DCHIP("chip_do_io: VirtQueue_AddBuf failed rc=%ld (queue full?)", (LONG)rc);
        return 0;
    }

    chip_notify_queue(gs, vq, 0);

    uint32 bytes_written = chip_wait_ctrlq(gs, vq, cookie);
    if (bytes_written == 0) {
        DCHIP("chip_do_io: timeout waiting for response");
    }
    return bytes_written;
}

/* -----------------------------------------------------------------------
 * chip_gpu_send -- zero buffers, set cmd type, send, return response type.
 * ----------------------------------------------------------------------- */
/* Public: convenience wrapper -- chip_do_io + extract response type.
 * Returns 0 if response was too short to contain a header. */
uint32 chip_gpu_send(struct ChipGPUState *gs, uint32 cmd_size, uint32 resp_size)
{
    uint32 written = chip_do_io(gs, cmd_size, resp_size);
    if (written < sizeof(struct virtio_gpu_ctrl_hdr)) {
        DCHIP("chip_gpu_send: short response (written=%lu)", written);
        return 0;
    }
    struct virtio_gpu_ctrl_hdr *resp = (struct virtio_gpu_ctrl_hdr *)gs->resp_buf;
    return GP32(resp->type);
}

/* -----------------------------------------------------------------------
 * GPU command helpers
 * ----------------------------------------------------------------------- */
/* All GPU command functions hold io_lock for their entire duration
 * to prevent races between the flush task and main P96 thread. */
BOOL chip_GetDisplayInfo(struct ChipGPUState *gs,
                          uint32 *width_out, uint32 *height_out)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_ctrl_hdr *cmd = (struct virtio_gpu_ctrl_hdr *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_resp_display_info));

    cmd->type = GP32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

    uint32 resp_type = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_resp_display_info));

    if (resp_type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        IExec->MutexRelease(gs->io_lock);
        DCHIP("GET_DISPLAY_INFO failed, resp=0x%lx", resp_type);
        return FALSE;
    }

    struct virtio_gpu_resp_display_info *resp =
        (struct virtio_gpu_resp_display_info *)gs->resp_buf;

    for (uint32 i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        uint32 enabled = GP32(resp->pmodes[i].enabled);
        if (enabled) {
            *width_out  = GP32(resp->pmodes[i].r.width);
            *height_out = GP32(resp->pmodes[i].r.height);
            IExec->MutexRelease(gs->io_lock);
            DCHIP("GET_DISPLAY_INFO: scanout %lu enabled %lux%lu",
                  i, *width_out, *height_out);
            return TRUE;
        }
    }

    IExec->MutexRelease(gs->io_lock);
    DCHIP("GET_DISPLAY_INFO: no enabled scanout");
    return FALSE;
}

BOOL chip_ResourceCreate2D(struct ChipGPUState *gs,
                             uint32 resource_id,
                             uint32 format,
                             uint32 width, uint32 height)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_resource_create_2d *cmd =
        (struct virtio_gpu_resource_create_2d *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    cmd->resource_id = GP32(resource_id);
    cmd->format      = GP32(format);
    cmd->width       = GP32(width);
    cmd->height      = GP32(height);

    uint32 rt = chip_gpu_send(gs, sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("RESOURCE_CREATE_2D failed, resp=0x%lx", rt);
        return FALSE;
    }
    DCHIP_V("RESOURCE_CREATE_2D id=%lu %lux%lu fmt=%lu OK", resource_id, width, height, format);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_ResourceUnref -- Destroy a GPU resource.
 *
 * Sends VIRTIO_GPU_CMD_RESOURCE_UNREF to release a previously created
 * resource (2D or 3D).  The resource must have been detached from any
 * 3D context first via chip_CTXDetachResource().
 * ----------------------------------------------------------------------- */
BOOL chip_ResourceUnref(struct ChipGPUState *gs, uint32 resource_id)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_resource_unref *cmd =
        (struct virtio_gpu_resource_unref *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_RESOURCE_UNREF);
    cmd->resource_id = GP32(resource_id);

    uint32 rt = chip_gpu_send(gs, sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("RESOURCE_UNREF failed for id=%lu, resp=0x%lx",
              (unsigned long)resource_id, rt);
        return FALSE;
    }
    DCHIP_V("RESOURCE_UNREF id=%lu OK", (unsigned long)resource_id);
    return TRUE;
}

BOOL chip_ResourceAttachBacking(struct ChipGPUState *gs,
                                  uint32 resource_id,
                                  struct DMAEntry *dma_list,
                                  uint32 dma_count)
{
    struct ExecIFace *IExec = gs->IExec;

    struct virtio_gpu_resource_attach_backing *cmd =
        (struct virtio_gpu_resource_attach_backing *)gs->cmd_buf;
    struct virtio_gpu_mem_entry *entries = (struct virtio_gpu_mem_entry *)(cmd + 1);

    uint32 max_entries = (4096 - sizeof(*cmd)) / sizeof(struct virtio_gpu_mem_entry);
    if (dma_count > max_entries) dma_count = max_entries;

    uint32 cmd_size = sizeof(*cmd) + sizeof(struct virtio_gpu_mem_entry) * dma_count;

    IExec->MutexObtain(gs->io_lock);

    chip_zero(gs->cmd_buf,  cmd_size);
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    cmd->resource_id = GP32(resource_id);
    cmd->nr_entries  = GP32(dma_count);

    for (uint32 i = 0; i < dma_count; i++) {
        entries[i].addr    = GP64((uint64)(uint32)dma_list[i].PhysicalAddress);
        entries[i].length  = GP32(dma_list[i].BlockLength);
        entries[i].padding = 0;
    }

    uint32 rt = chip_gpu_send(gs, cmd_size, sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    DCHIP_V("RESOURCE_ATTACH_BACKING id=%lu nr_entries=%lu", resource_id, dma_count);
    DCHIP_V("  DMA[0] phys=0x%08lx len=%lu",
            dma_count > 0 ? (uint32)dma_list[0].PhysicalAddress : 0,
            dma_count > 0 ? dma_list[0].BlockLength : 0);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("RESOURCE_ATTACH_BACKING failed, resp=0x%lx", rt);
        return FALSE;
    }
    DCHIP_V("RESOURCE_ATTACH_BACKING OK");
    return TRUE;
}

BOOL chip_SetScanout(struct ChipGPUState *gs,
                       uint32 scanout_id, uint32 resource_id,
                       uint32 x, uint32 y, uint32 width, uint32 height)
{
    struct ExecIFace *IExec = gs->IExec;

    /* Only log when rect or size actually changes (per-scanout) -- skipping
     * the per-frame double-buffer swap noise.  Resource ID swaps front<->back
     * so we ignore that field for the change detection. */
    if (scanout_id < 4) {
        if (gs->scanout_log_x[scanout_id] != x ||
            gs->scanout_log_y[scanout_id] != y ||
            gs->scanout_log_w[scanout_id] != width ||
            gs->scanout_log_h[scanout_id] != height)
        {
            DCHIP("SET_SCANOUT scanout=%lu res=%lu rect=(%lu,%lu,%lu,%lu) active=%lux%lu",
                  (ULONG)scanout_id, (ULONG)resource_id,
                  (ULONG)x, (ULONG)y, (ULONG)width, (ULONG)height,
                  (ULONG)gs->active_width, (ULONG)gs->active_height);
            gs->scanout_log_x[scanout_id] = x;
            gs->scanout_log_y[scanout_id] = y;
            gs->scanout_log_w[scanout_id] = width;
            gs->scanout_log_h[scanout_id] = height;
        }
    }

    /* SET_SCANOUT can timeout during mode switches when QEMU's main loop is
     * busy (e.g. processing SDL resize events).  Retry up to 3 times with a
     * brief busy-wait between attempts to let QEMU catch up. */
    for (uint32 attempt = 0; attempt < 3; attempt++) {
        IExec->MutexObtain(gs->io_lock);

        struct virtio_gpu_set_scanout *cmd =
            (struct virtio_gpu_set_scanout *)gs->cmd_buf;
        chip_zero(gs->cmd_buf,  sizeof(*cmd));
        chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

        cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_SET_SCANOUT);
        cmd->r.x         = GP32(x);
        cmd->r.y         = GP32(y);
        cmd->r.width     = GP32(width);
        cmd->r.height    = GP32(height);
        cmd->scanout_id  = GP32(scanout_id);
        cmd->resource_id = GP32(resource_id);

        uint32 rt = chip_gpu_send(gs, sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
        IExec->MutexRelease(gs->io_lock);

        if (rt == VIRTIO_GPU_RESP_OK_NODATA) {
            DCHIP_V("SET_SCANOUT scanout=%lu res=%lu OK", scanout_id, resource_id);
            return TRUE;
        }

        /* Timeout or error -- short memory-read delay before retry.
         * No Forbid/Permit (being phased out of the GPU hot path);
         * a volatile accumulator loop keeps the CPU busy long enough
         * to let the GPU drain its response queue. */
        if (attempt < 2) {
            DCHIP("SET_SCANOUT timeout (attempt %lu/3), retrying...", attempt + 1);
            volatile uint32 delay_acc = 0;
            for (uint32 d = 0; d < 100000; d++) delay_acc += d;
            (void)delay_acc;
        }
    }

    DCHIP("SET_SCANOUT failed after 3 attempts");
    return FALSE;
}

BOOL chip_TransferToHost2D(struct ChipGPUState *gs,
                             uint32 resource_id,
                             uint32 x, uint32 y,
                             uint32 width, uint32 height,
                             uint64 offset)
{
    /* Blob fast-path: if this is the framebuffer and we created it as
     * BLOB_MEM_GUEST, the host already reads our DMA pages directly --
     * the transfer is a no-op.  Other resources (cursor, virgl 3D, the
     * second 2D back buffer) still go through the real command. */
    if (gs->fb_is_blob && resource_id == gs->fb_resource_id) {
        return TRUE;
    }

    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_transfer_to_host_2d *cmd =
        (struct virtio_gpu_transfer_to_host_2d *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    cmd->r.x         = GP32(x);
    cmd->r.y         = GP32(y);
    cmd->r.width     = GP32(width);
    cmd->r.height    = GP32(height);
    cmd->offset      = GP64(offset);
    cmd->resource_id = GP32(resource_id);

    uint32 rt = chip_gpu_send(gs, sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("TransferToHost2D failed: resp=0x%lx res=%lu rect=(%lu,%lu %lux%lu)",
              rt, resource_id, x, y, width, height);
        return FALSE;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_GetEDID -- issue VIRTIO_GPU_CMD_GET_EDID for a scanout.
 *
 * Stores the EDID blob in gs->edid[] and parses the preferred timing
 * from the first detailed timing descriptor (EDID bytes 54-71) to
 * extract native resolution into gs->edid_native_w/h.
 * ----------------------------------------------------------------------- */
BOOL chip_GetEDID(struct ChipGPUState *gs, uint32 scanout)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_get_edid *cmd =
        (struct virtio_gpu_get_edid *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_resp_edid));

    cmd->hdr.type = GP32(VIRTIO_GPU_CMD_GET_EDID);
    cmd->scanout  = GP32(scanout);

    uint32 resp_type = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_resp_edid));

    if (resp_type != VIRTIO_GPU_RESP_OK_EDID) {
        IExec->MutexRelease(gs->io_lock);
        DCHIP("GET_EDID failed for scanout %lu, resp=0x%lx", scanout, resp_type);
        gs->has_edid = FALSE;
        return FALSE;
    }

    struct virtio_gpu_resp_edid *resp =
        (struct virtio_gpu_resp_edid *)gs->resp_buf;

    gs->edid_size = GP32(resp->size);
    if (gs->edid_size > 1024) gs->edid_size = 1024;

    /* Copy EDID blob -- resp_buf may be reused by next command */
    for (uint32 i = 0; i < gs->edid_size; i++)
        gs->edid[i] = resp->edid[i];

    gs->has_edid = TRUE;
    IExec->MutexRelease(gs->io_lock);

    DCHIP_V("GET_EDID: scanout %lu, size=%lu bytes", scanout, gs->edid_size);

    /* Parse all Detailed Timing Descriptors in the EDID blob.
     *
     * DTD layout (18 bytes, byte offsets relative to descriptor start):
     *   [0-1] pixel_clock in 10 kHz units (0 = not a DTD; could be
     *         monitor name, range limits, serial number, etc.)
     *   [2]   h_active_lo (low 8 bits)
     *   [3]   h_blanking_lo
     *   [4]   h_active_hi:4 (upper nibble) | h_blanking_hi:4
     *   [5]   v_active_lo
     *   [6]   v_blanking_lo
     *   [7]   v_active_hi:4 | v_blanking_hi:4
     *
     * Base block has 4 DTDs at bytes 54, 72, 90, 108.  CEA-861 extension
     * blocks (extension tag 0x02) have a DTD offset at byte 2 from the
     * extension start, and DTDs run from there to byte 125 of the
     * extension (variable count).  We walk both.
     *
     * EDID 1.3+ guarantees the first DTD is the preferred timing, so
     * gs->edid_native_w/h is always edid_dtd_w[0]/edid_dtd_h[0] (when
     * edid_dtd_count > 0). */
    gs->edid_native_w  = 0;
    gs->edid_native_h  = 0;
    gs->edid_dtd_count = 0;

    /* Inline DTD-extraction helper -- adds (w, h) to the list if it's
     * a valid distinct entry.  We deduplicate so the same resolution
     * appearing in multiple DTDs (common when EDID lists the preferred
     * timing in both the base block and a CEA-861 extension) only
     * registers once. */
    #define ADD_DTD(W, H)                                                  \
        do {                                                                \
            if ((W) > 0 && (H) > 0                                          \
                && gs->edid_dtd_count < (uint32)(sizeof(gs->edid_dtd_w) /   \
                                                  sizeof(gs->edid_dtd_w[0]))) { \
                BOOL dup = FALSE;                                           \
                for (uint32 _i = 0; _i < gs->edid_dtd_count; _i++) {        \
                    if (gs->edid_dtd_w[_i] == (W) && gs->edid_dtd_h[_i] == (H)) { \
                        dup = TRUE; break;                                  \
                    }                                                       \
                }                                                           \
                if (!dup) {                                                 \
                    gs->edid_dtd_w[gs->edid_dtd_count] = (uint16)(W);       \
                    gs->edid_dtd_h[gs->edid_dtd_count] = (uint16)(H);       \
                    gs->edid_dtd_count++;                                   \
                }                                                           \
            }                                                               \
        } while (0)

    /* Base block: 4 DTDs at fixed offsets. */
    static const uint32 base_dtd_offsets[4] = { 54, 72, 90, 108 };
    for (uint32 i = 0; i < 4; i++) {
        uint32 off = base_dtd_offsets[i];
        if (off + 8 > gs->edid_size) break;
        const uint8 *dtd = &gs->edid[off];
        uint16 pixel_clock = (uint16)dtd[0] | ((uint16)dtd[1] << 8);
        if (pixel_clock == 0) continue;     /* descriptor is not a DTD */
        uint32 h_active = (uint32)dtd[2] | (((uint32)dtd[4] >> 4) << 8);
        uint32 v_active = (uint32)dtd[5] | (((uint32)dtd[7] >> 4) << 8);
        ADD_DTD(h_active, v_active);
    }

    /* Extension blocks: byte 126 of the base block is the extension
     * count.  Each extension is 128 bytes; CEA-861 (tag 0x02) carries
     * its own DTDs starting at offset (extension_base + dtd_offset)
     * given in byte 2 of the extension, running to byte 125 in 18-byte
     * steps. */
    if (gs->edid_size >= 128) {
        uint32 ext_count = gs->edid[126];
        for (uint32 e = 0; e < ext_count; e++) {
            uint32 ext_base = 128 * (e + 1);
            if (ext_base + 128 > gs->edid_size) break;
            if (gs->edid[ext_base] != 0x02) continue;  /* not CEA-861 */
            uint32 dtd_off = gs->edid[ext_base + 2];   /* relative offset */
            if (dtd_off < 4 || dtd_off > 125) continue;
            for (uint32 off = ext_base + dtd_off;
                 off + 18 <= ext_base + 126;
                 off += 18) {
                const uint8 *dtd = &gs->edid[off];
                uint16 pixel_clock = (uint16)dtd[0] | ((uint16)dtd[1] << 8);
                if (pixel_clock == 0) break;       /* CEA marker terminator */
                uint32 h_active = (uint32)dtd[2] | (((uint32)dtd[4] >> 4) << 8);
                uint32 v_active = (uint32)dtd[5] | (((uint32)dtd[7] >> 4) << 8);
                ADD_DTD(h_active, v_active);
            }
        }
    }
    #undef ADD_DTD

    if (gs->edid_dtd_count > 0) {
        gs->edid_native_w = gs->edid_dtd_w[0];
        gs->edid_native_h = gs->edid_dtd_h[0];
        DCHIP("EDID: %lu DTD(s) parsed; preferred=%lux%lu",
              gs->edid_dtd_count, gs->edid_native_w, gs->edid_native_h);
        for (uint32 i = 1; i < gs->edid_dtd_count; i++) {
            DCHIP_V("  DTD[%lu] %ux%u", i,
                    (unsigned)gs->edid_dtd_w[i], (unsigned)gs->edid_dtd_h[i]);
        }
    } else {
        DCHIP("EDID: no valid DTD timings found");
    }

    return TRUE;
}

BOOL chip_ResourceFlush(struct ChipGPUState *gs,
                          uint32 resource_id,
                          uint32 x, uint32 y,
                          uint32 width, uint32 height)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_resource_flush *cmd =
        (struct virtio_gpu_resource_flush *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    cmd->r.x         = GP32(x);
    cmd->r.y         = GP32(y);
    cmd->r.width     = GP32(width);
    cmd->r.height    = GP32(height);
    cmd->resource_id = GP32(resource_id);

    uint32 rt = chip_gpu_send(gs, sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("ResourceFlush failed: resp=0x%lx res=%lu rect=(%lu,%lu %lux%lu)",
              rt, resource_id, x, y, width, height);
        return FALSE;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Cursor commands -- fire-and-forget on queue 1 (cursor queue).
 *
 * Uses a dedicated cursor_cmd_buf + cursor_lock, completely independent
 * from the control queue's cmd_buf + io_lock.  This means mouse moves
 * never block on framebuffer flush (and vice versa).
 *
 * For MOVE_CURSOR we drain any previous used entry then kick without
 * waiting -- the device applies cursor position immediately.
 * For UPDATE_CURSOR we do a brief poll since the device must read the
 * pixel data before we can reuse the buffer.
 * ----------------------------------------------------------------------- */
static void chip_cursor_send(struct ChipGPUState *gs, uint32 cmd_type,
                              uint32 resource_id, uint32 x, uint32 y,
                              uint32 hot_x, uint32 hot_y)
{
    struct ExecIFace *IExec = gs->IExec;
    struct virtqueue *cq = gs->vqs[VIRTIO_GPU_CURSQ];
    if (!cq || !gs->cursor_cmd_buf) return;

    IExec->MutexObtain(gs->cursor_lock);

    /* Drain any previous completion before reusing the buffer */
    uint32 drain_bytes;
    while (VirtQueue_GetBuf(IExec, cq, &drain_bytes))
        ;

    struct virtio_gpu_update_cursor *cmd =
        (struct virtio_gpu_update_cursor *)gs->cursor_cmd_buf;
    chip_zero(gs->cursor_cmd_buf, sizeof(*cmd));

    cmd->hdr.type       = GP32(cmd_type);
    cmd->pos.scanout_id = GP32(0);
    cmd->pos.x          = GP32(x);
    cmd->pos.y          = GP32(y);
    cmd->resource_id    = GP32(resource_id);
    cmd->hot_x          = GP32(hot_x);
    cmd->hot_y          = GP32(hot_y);

    struct vring_sg sg[1];
    sg[0].addr = gs->cursor_cmd_phys;
    sg[0].len  = sizeof(*cmd);

    void *cookie = (void *)IExec->FindTask(NULL);
    int32 rc = VirtQueue_AddBuf(IExec, cq, sg, 1, 0, cookie);
    if (rc != 0) {
        DCHIP("chip_cursor_send: VirtQueue_AddBuf rc=%ld type=0x%lx res=%lu "
              "(cursor command DROPPED -- cursor may be stale)",
              (LONG)rc, (ULONG)cmd_type, (ULONG)resource_id);
        IExec->MutexRelease(gs->cursor_lock);
        return;
    }

    /* Kick cursor queue */
    chip_notify_queue(gs, cq, VIRTIO_GPU_CURSQ);

    /* Wait for device to consume the command before releasing the buffer.
     * Without this, rapid MOVE_CURSOR calls can overwrite cursor_cmd_buf
     * while the GPU is still reading the previous command, causing the
     * cursor to briefly jump to a wrong position. */
    {
        uint32 bytes_written = 0;
        void *got = NULL;
        int8   sig_bit  = -1;
        uint32 sig_mask = 0;

        if (gs->irq_installed) {
            sig_bit = IExec->AllocSignal(-1);
            if (sig_bit >= 0) {
                sig_mask = 1UL << sig_bit;
                IExec->SetSignal(0, sig_mask);
                /* Publish without Disable/Enable -- see chip_wait_ctrlq
                 * for the race analysis.  Store task first, barrier,
                 * then mask, so a mid-publish IRQ sees (valid, 0) and
                 * short-circuits on mask=0. */
                gs->cursorq_wait_task = (struct Task *)cookie;
                __asm__ volatile ("" ::: "memory");
                gs->cursorq_wait_mask = sig_mask;
                __asm__ volatile ("" ::: "memory");
            }
        }

        if (sig_bit >= 0) {
            uint32 wait_attempts = 50;
            while (wait_attempts-- > 0) {
                got = VirtQueue_GetBuf(IExec, cq, &bytes_written);
                if (got == cookie) break;
                IExec->Wait(sig_mask);
                IExec->SetSignal(0, sig_mask);
            }
            /* Retract without Disable/Enable -- mirror the chip_wait_ctrlq
             * pattern.  Store mask=0 first so any mid-retract IRQ sees
             * (valid, 0) and skips the Signal call. */
            gs->cursorq_wait_mask = 0;
            __asm__ volatile ("" ::: "memory");
            gs->cursorq_wait_task = NULL;
            __asm__ volatile ("" ::: "memory");
            IExec->FreeSignal(sig_bit);
        } else {
            /* Polling fallback when IRQ unavailable.  Tight busy-poll,
             * no Forbid/Permit yield (see chip_wait_ctrlq). */
            uint32 timeout = 50000;
            while (timeout-- > 0) {
                got = VirtQueue_GetBuf(IExec, cq, &bytes_written);
                if (got == cookie) break;
            }
        }
        /* Drain any stale responses on timeout to keep the queue healthy. */
        if (got != cookie) {
            uint32 drained = 0;
            uint32 drain_bytes;
            while (VirtQueue_GetBuf(IExec, cq, &drain_bytes)) {
                drained++;
                if (drained > 256) break;
            }
            /* Log the timeout once per session -- a healthy cursor path
             * never times out, so any occurrence is real news.  Drained
             * count helps confirm the queue isn't stuck. */
            static volatile UBYTE warned = 0;
            if (!warned) {
                warned = 1;
                DCHIP("chip_cursor_send: WAIT TIMEOUT type=0x%lx res=%lu "
                      "drained=%lu stale entries (logged once)",
                      (ULONG)cmd_type, (ULONG)resource_id, (ULONG)drained);
            }
        }
    }

    IExec->MutexRelease(gs->cursor_lock);
}

/* -----------------------------------------------------------------------
 * chip_CursorCreate -- allocate 64x64 ARGB cursor resource.
 * ----------------------------------------------------------------------- */
BOOL chip_CursorCreate(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;

    if (gs->cursor_created) return TRUE;

    /* Need cursor queue for hardware cursor */
    if (!gs->vqs[VIRTIO_GPU_CURSQ]) {
        DCHIP("CursorCreate: no cursor queue, hw cursor unavailable");
        return FALSE;
    }

    /* Allocate 64x64 ARGB pixel buffer */
    uint32 cursor_size = 64 * 64 * 4;
    gs->cursor_mem = IExec->AllocVecTags(cursor_size,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      4096,
        AVT_Contiguous,     TRUE,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!gs->cursor_mem) {
        DCHIP("CursorCreate: alloc failed");
        return FALSE;
    }

    /* DMA-map cursor buffer */
    uint32 n = IExec->StartDMA(gs->cursor_mem, cursor_size, DMA_ReadFromRAM);
    if (n == 0) {
        IExec->FreeVec(gs->cursor_mem);
        gs->cursor_mem = NULL;
        DCHIP("CursorCreate: StartDMA failed");
        return FALSE;
    }

    if (n > 1) {
        DCHIP("WARNING: cursor DMA buffer spans %lu entries -- expected contiguous", n);
    }

    struct DMAEntry *dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
    if (!dma) {
        IExec->EndDMA(gs->cursor_mem, cursor_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(gs->cursor_mem);
        gs->cursor_mem = NULL;
        return FALSE;
    }

    IExec->GetDMAList(gs->cursor_mem, cursor_size, DMA_ReadFromRAM, dma);
    gs->cursor_phys = (uint32)dma[0].PhysicalAddress;

    /* Leave cursor pixel buffer at default MEMF_SHARED (implicit CI).
     * Matches the per-DMA-buffer policy used by VirtualSCSIDevice /
     * AmigaNVMeDevice.  CPU writes are rare (only on cursor image
     * change), so the CI write cost is negligible. */
    gs->cursor_resource_id = 2;  /* resource 1 = framebuffer, 2 = cursor */

    /* Create GPU resource */
    if (!chip_ResourceCreate2D(gs, gs->cursor_resource_id,
                                VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                64, 64)) {
        IExec->FreeSysObject(ASOT_DMAENTRY, dma);
        IExec->EndDMA(gs->cursor_mem, cursor_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(gs->cursor_mem);
        gs->cursor_mem = NULL;
        DCHIP("CursorCreate: RESOURCE_CREATE_2D failed");
        return FALSE;
    }

    /* Attach backing memory */
    if (!chip_ResourceAttachBacking(gs, gs->cursor_resource_id, dma, 1)) {
        IExec->FreeSysObject(ASOT_DMAENTRY, dma);
        IExec->EndDMA(gs->cursor_mem, cursor_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(gs->cursor_mem);
        gs->cursor_mem = NULL;
        DCHIP("CursorCreate: ATTACH_BACKING failed");
        return FALSE;
    }

    IExec->FreeSysObject(ASOT_DMAENTRY, dma);
    gs->cursor_created = TRUE;
    DCHIP_V("CursorCreate: resource=%lu 64x64 ARGB OK", gs->cursor_resource_id);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_CursorUpdate -- upload cursor image and set hotspot.
 * Caller must fill gs->cursor_mem with 64x64 ARGB data first.
 * ----------------------------------------------------------------------- */
void chip_CursorUpdate(struct ChipGPUState *gs, uint32 hot_x, uint32 hot_y)
{
    if (!gs->cursor_created) return;

    DCHIP("UPDATE_CURSOR pos=%d,%d hot=%lu,%lu res=%lu active=%lux%lu",
          (int)gs->cursor_x, (int)gs->cursor_y,
          (ULONG)hot_x, (ULONG)hot_y,
          (ULONG)gs->cursor_resource_id,
          (ULONG)gs->active_width, (ULONG)gs->active_height);

    /* Transfer cursor pixels to GPU resource */
    chip_TransferToHost2D(gs, gs->cursor_resource_id,
                           0, 0, 64, 64, 0ULL);

    /* Send UPDATE_CURSOR on cursor queue.
     * Include current position so QEMU doesn't warp cursor to (0,0). */
    chip_cursor_send(gs, VIRTIO_GPU_CMD_UPDATE_CURSOR,
                      gs->cursor_resource_id,
                      (uint32)gs->cursor_x, (uint32)gs->cursor_y,
                      hot_x, hot_y);
    /* Track what we've pushed so the post-SetGC MOVE_CURSOR refresh
     * (in chip_SetSpritePosition) only fires when the position has
     * actually drifted from what the GPU is showing. */
    gs->cursor_last_sent_x = gs->cursor_x;
    gs->cursor_last_sent_y = gs->cursor_y;
}

/* -----------------------------------------------------------------------
 * chip_CursorMove -- move cursor position without changing image.
 * ----------------------------------------------------------------------- */
void chip_CursorMove(struct ChipGPUState *gs, uint32 x, uint32 y)
{
    if (!gs->cursor_created) return;

    gs->cursor_x = (WORD)x;
    gs->cursor_y = (WORD)y;

    chip_cursor_send(gs, VIRTIO_GPU_CMD_MOVE_CURSOR,
                      gs->cursor_resource_id, x, y, 0, 0);
}

/* -----------------------------------------------------------------------
 * chip_CursorHide -- hide cursor by sending UPDATE_CURSOR with resource_id=0.
 * VirtIO GPU spec: resource_id=0 removes the cursor plane from the scanout.
 * ----------------------------------------------------------------------- */
void chip_CursorHide(struct ChipGPUState *gs)
{
    if (!gs->cursor_created) return;

    chip_cursor_send(gs, VIRTIO_GPU_CMD_UPDATE_CURSOR,
                      0, 0, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * chip_handle_display_change -- respond to VirtIO GPU config change event.
 *
 * Called from the flush task when VIRTIO_GPU_EVENT_DISPLAY is detected.
 * Two responsibilities:
 *
 *   1. Don't resize the active scanout.  The scanout dimensions are
 *      controlled by the AmigaOS screen mode, not the host window;
 *      auto-resizing would create a feedback loop (SET_SCANOUT -> QEMU
 *      resizes window -> new event -> SET_SCANOUT -> ...).
 *
 *   2. DO add the new dimensions to bi->ResolutionsList if they aren't
 *      already there.  This way ScreenMode prefs gets the new mode as
 *      a selectable option without the user needing to reboot.
 *      chip_add_mode is a no-op if the resolution is already registered.
 *
 * The slot_idx in chip_add_mode is used to encode a unique DisplayID;
 * we pass a value above the static-mode-table range so the new entry
 * can't collide with any pre-registered mode's ID.
 * ----------------------------------------------------------------------- */
void chip_handle_display_change(struct ChipGPUState *gs)
{
    uint32 new_w = 0, new_h = 0;
    if (!chip_GetDisplayInfo(gs, &new_w, &new_h)) return;

    DCHIP("Display config changed: host %lux%lu (active scanout %lux%lu)",
          new_w, new_h, gs->active_width, gs->active_height);

    /* Pick a DisplayID slot beyond the chip_modes[] table range.  Using
     * 0xC0 + (counter & 0x3F) keeps us inside the safe 0x0005xxxx
     * DisplayID space chip_add_mode constructs while staying well clear
     * of the static-table indices (0..chip_num_modes-1). */
    ULONG slot_idx = 0xC0 + (gs->hotplug_mode_count & 0x3F);
    if (chip_add_mode(gs, (UWORD)new_w, (UWORD)new_h,
                      0, 0, 0, 0, 0, 0,    /* CVT-RB synthesised in chip_add_mode */
                      0, 0, slot_idx)) {
        gs->hotplug_mode_count++;
    }
}
