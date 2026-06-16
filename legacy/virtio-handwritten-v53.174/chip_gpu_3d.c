/*
 * chip_gpu_3d.c -- VirtIO GPU 3D command wrappers (Phase 6).
 *
 * Provides: capset query, 3D context create/destroy, resource create 3D,
 * transfer to/from host 3D, and SUBMIT_3D for Virgl command streams.
 *
 * All commands use chip_wait_ctrlq (IRQ-driven with polling fallback) and
 * require gs->io_lock held by the caller for the duration of the exchange.
 * SUBMIT_3D payloads are staged in the separate gs->cmd3d_buf so large
 * command streams don't collide with the small control buffer.
 */

#include "chip/chip_state.h"

/* The 2-SG control-queue transaction helpers (chip_do_io / chip_gpu_send)
 * live in chip_gpu_cmds.c and are shared with this file via prototypes
 * in chip_state.h.  Earlier this file maintained its own
 * chip_do_io_locked / chip_send_locked duplicates -- removed in v53.111
 * to eliminate bug-fix drift risk on the hottest GPU code path. */

/* -----------------------------------------------------------------------
 * chip_do_io_3sg -- control queue transaction with 3 scatter-gather entries.
 * Used for SUBMIT_3D: [OUT cmd_hdr] [OUT cmd_data] [IN resp].
 * Caller MUST hold gs->io_lock.
 * ----------------------------------------------------------------------- */
static uint32 chip_do_io_3sg(struct ChipGPUState *gs,
                              uint32 hdr_phys, uint32 hdr_size,
                              uint32 data_phys, uint32 data_size,
                              uint32 resp_size)
{
    struct ExecIFace *IExec = gs->IExec;
    struct virtqueue *vq = gs->vqs[VIRTIO_GPU_CTRLQ];
    if (!vq) {
        DCHIP("chip_do_io_3sg: control queue not initialised");
        return 0;
    }

    struct vring_sg sg[3];
    sg[0].addr = hdr_phys;
    sg[0].len  = hdr_size;
    sg[1].addr = data_phys;
    sg[1].len  = data_size;
    sg[2].addr = gs->resp_buf_phys;
    sg[2].len  = resp_size;

    void *cookie = (void *)IExec->FindTask(NULL);
    int32 rc = VirtQueue_AddBuf(IExec, vq, sg, 2, 1, cookie);
    if (rc != 0) {
        DCHIP("chip_do_io_3sg: VirtQueue_AddBuf failed rc=%ld (hdr=%lu data=%lu)",
              (LONG)rc, hdr_size, data_size);
        return 0;
    }

    chip_notify_queue(gs, vq, 0);

    uint32 bytes_written = chip_wait_ctrlq(gs, vq, cookie);
    if (bytes_written == 0) {
        DCHIP("chip_do_io_3sg: timeout (hdr=%lu data=%lu resp=%lu)",
              hdr_size, data_size, resp_size);
        return 0;
    }
    return bytes_written;
}

/* -----------------------------------------------------------------------
 * GET_CAPSET_INFO -- query metadata for one capset by index.
 * ----------------------------------------------------------------------- */
BOOL chip_GetCapsetInfo(struct ChipGPUState *gs, uint32 index,
                         uint32 *id_out, uint32 *max_ver_out, uint32 *max_size_out)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_get_capset_info *cmd =
        (struct virtio_gpu_get_capset_info *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_resp_capset_info));

    cmd->hdr.type     = GP32(VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    cmd->capset_index = GP32(index);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_resp_capset_info));

    if (rt != VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
        IExec->MutexRelease(gs->io_lock);
        DCHIP("GET_CAPSET_INFO[%lu] failed, resp=0x%lx", index, rt);
        return FALSE;
    }

    struct virtio_gpu_resp_capset_info *resp =
        (struct virtio_gpu_resp_capset_info *)gs->resp_buf;

    *id_out       = GP32(resp->capset_id);
    *max_ver_out  = GP32(resp->capset_max_version);
    *max_size_out = GP32(resp->capset_max_size);

    IExec->MutexRelease(gs->io_lock);

    DCHIP("GET_CAPSET_INFO[%lu]: id=%lu max_ver=%lu max_size=%lu",
          index, *id_out, *max_ver_out, *max_size_out);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * GET_CAPSET -- retrieve actual capset data blob.
 * ----------------------------------------------------------------------- */
BOOL chip_GetCapset(struct ChipGPUState *gs, uint32 capset_id, uint32 capset_version,
                     uint8 *buf_out, uint32 buf_size, uint32 *actual_size_out)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_get_capset *cmd =
        (struct virtio_gpu_get_capset *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_resp_capset));

    cmd->hdr.type       = GP32(VIRTIO_GPU_CMD_GET_CAPSET);
    cmd->capset_id      = GP32(capset_id);
    cmd->capset_version = GP32(capset_version);

    uint32 resp_expect = sizeof(struct virtio_gpu_ctrl_hdr) + buf_size;
    if (resp_expect > 4096) resp_expect = 4096;  /* resp_buf is 4K */

    uint32 rt = chip_gpu_send(gs, sizeof(*cmd), resp_expect);

    if (rt != VIRTIO_GPU_RESP_OK_CAPSET) {
        IExec->MutexRelease(gs->io_lock);
        DCHIP("GET_CAPSET id=%lu ver=%lu failed, resp=0x%lx",
              capset_id, capset_version, rt);
        return FALSE;
    }

    /* Copy capset data from resp_buf (after the header) */
    struct virtio_gpu_resp_capset *resp =
        (struct virtio_gpu_resp_capset *)gs->resp_buf;
    uint32 copy_size = buf_size;
    uint32 avail = resp_expect - sizeof(struct virtio_gpu_ctrl_hdr);
    if (copy_size > avail) copy_size = avail;

    for (uint32 i = 0; i < copy_size; i++)
        buf_out[i] = resp->capset_data[i];

    if (actual_size_out)
        *actual_size_out = copy_size;

    IExec->MutexRelease(gs->io_lock);

    DCHIP("GET_CAPSET id=%lu ver=%lu: got %lu bytes", capset_id, capset_version, copy_size);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_QueryCapsets -- iterate all capsets, find best Virgl capset.
 * Called once during init after DRIVER_OK.
 * ----------------------------------------------------------------------- */
void chip_QueryCapsets(struct ChipGPUState *gs)
{
    gs->virgl_caps_valid = FALSE;
    gs->virgl_capset_id  = 0;
    gs->virgl_capset_ver = 0;

    DCHIP("Querying %lu capsets...", gs->num_capsets);

    for (uint32 i = 0; i < gs->num_capsets; i++) {
        uint32 id = 0, max_ver = 0, max_size = 0;
        if (!chip_GetCapsetInfo(gs, i, &id, &max_ver, &max_size))
            continue;

        /* Prefer VIRGL2 over VIRGL (newer, more stable) */
        if (id == VIRTIO_GPU_CAPSET_VIRGL2 ||
            (id == VIRTIO_GPU_CAPSET_VIRGL && gs->virgl_capset_id == 0)) {
            gs->virgl_capset_id  = id;
            gs->virgl_capset_ver = max_ver;

            /* Fetch the actual capset blob */
            uint32 fetch_size = max_size;
            if (fetch_size > VIRTIO_GPU_MAX_CAPSET_SIZE)
                fetch_size = VIRTIO_GPU_MAX_CAPSET_SIZE;

            uint32 actual = 0;
            if (chip_GetCapset(gs, id, max_ver,
                                gs->virgl_caps, fetch_size, &actual)) {
                gs->virgl_capset_size = actual;
                gs->virgl_caps_valid = TRUE;
                DCHIP("Selected capset %lu (ver %lu, %lu bytes)",
                      id, max_ver, actual);
            }

            /* If we got VIRGL2, stop -- it's the best */
            if (id == VIRTIO_GPU_CAPSET_VIRGL2)
                break;
        }
    }

    if (!gs->virgl_caps_valid) {
        DCHIP("WARNING: No usable Virgl capset found -- 3D disabled");
        gs->has_virgl = FALSE;
    }
}

/* -----------------------------------------------------------------------
 * CTX_CREATE -- create a 3D rendering context.
 * ----------------------------------------------------------------------- */
BOOL chip_CTXCreate(struct ChipGPUState *gs, uint32 ctx_id,
                     const char *debug_name, uint32 context_init)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_ctx_create *cmd =
        (struct virtio_gpu_ctx_create *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type   = GP32(VIRTIO_GPU_CMD_CTX_CREATE);
    cmd->hdr.ctx_id = GP32(ctx_id);

    /* Copy debug name */
    uint32 nlen = 0;
    if (debug_name) {
        while (nlen < 63 && debug_name[nlen]) {
            cmd->debug_name[nlen] = debug_name[nlen];
            nlen++;
        }
        cmd->debug_name[nlen] = '\0';
    }
    cmd->nlen         = GP32(nlen);
    cmd->context_init = GP32(context_init);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("CTX_CREATE ctx=%lu failed, resp=0x%lx", ctx_id, rt);
        return FALSE;
    }
    DCHIP("CTX_CREATE ctx=%lu name='%s' init=%lu OK", ctx_id,
          debug_name ? debug_name : "", context_init);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * CTX_DESTROY -- destroy a 3D rendering context.
 * ----------------------------------------------------------------------- */
void chip_CTXDestroy(struct ChipGPUState *gs, uint32 ctx_id)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_ctx_destroy *cmd =
        (struct virtio_gpu_ctx_destroy *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type   = GP32(VIRTIO_GPU_CMD_CTX_DESTROY);
    cmd->hdr.ctx_id = GP32(ctx_id);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    DCHIP("CTX_DESTROY ctx=%lu resp=0x%lx", ctx_id, rt);
}

/* -----------------------------------------------------------------------
 * CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE
 * ----------------------------------------------------------------------- */
BOOL chip_CTXAttachResource(struct ChipGPUState *gs, uint32 ctx_id, uint32 resource_id)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_ctx_resource *cmd =
        (struct virtio_gpu_ctx_resource *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE);
    cmd->hdr.ctx_id  = GP32(ctx_id);
    cmd->resource_id = GP32(resource_id);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("CTX_ATTACH_RESOURCE ctx=%lu res=%lu failed, resp=0x%lx",
              ctx_id, resource_id, rt);
        return FALSE;
    }
    return TRUE;
}

BOOL chip_CTXDetachResource(struct ChipGPUState *gs, uint32 ctx_id, uint32 resource_id)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_ctx_resource *cmd =
        (struct virtio_gpu_ctx_resource *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE);
    cmd->hdr.ctx_id  = GP32(ctx_id);
    cmd->resource_id = GP32(resource_id);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("CTX_DETACH_RESOURCE ctx=%lu res=%lu failed, resp=0x%lx",
              ctx_id, resource_id, rt);
        return FALSE;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * RESOURCE_CREATE_3D -- create a 3D-capable resource.
 * ----------------------------------------------------------------------- */
BOOL chip_ResourceCreate3D(struct ChipGPUState *gs, uint32 resource_id,
                             uint32 target, uint32 format, uint32 bind,
                             uint32 width, uint32 height, uint32 depth,
                             uint32 array_size, uint32 last_level,
                             uint32 nr_samples, uint32 flags)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_resource_create_3d *cmd =
        (struct virtio_gpu_resource_create_3d *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
    cmd->resource_id = GP32(resource_id);
    cmd->target      = GP32(target);
    cmd->format      = GP32(format);
    cmd->bind        = GP32(bind);
    cmd->width       = GP32(width);
    cmd->height      = GP32(height);
    cmd->depth       = GP32(depth);
    cmd->array_size  = GP32(array_size);
    cmd->last_level  = GP32(last_level);
    cmd->nr_samples  = GP32(nr_samples);
    cmd->flags       = GP32(flags);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("RESOURCE_CREATE_3D id=%lu failed, resp=0x%lx", resource_id, rt);
        return FALSE;
    }
    DCHIP("RESOURCE_CREATE_3D id=%lu %lux%lu target=%lu fmt=%lu OK",
          resource_id, width, height, target, format);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * TRANSFER_TO_HOST_3D -- copy guest memory to 3D resource on host.
 * ----------------------------------------------------------------------- */
BOOL chip_TransferToHost3D(struct ChipGPUState *gs, uint32 ctx_id,
                             uint32 resource_id,
                             uint32 level, uint32 stride, uint32 layer_stride,
                             uint64 offset, struct virtio_gpu_box *box)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_transfer_host_3d *cmd =
        (struct virtio_gpu_transfer_host_3d *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
    cmd->hdr.ctx_id  = GP32(ctx_id);
    cmd->resource_id = GP32(resource_id);
    cmd->level       = GP32(level);
    cmd->stride      = GP32(stride);
    cmd->layer_stride = GP32(layer_stride);
    cmd->offset      = GP64(offset);
    cmd->box.x       = GP32(box->x);
    cmd->box.y       = GP32(box->y);
    cmd->box.z       = GP32(box->z);
    cmd->box.w       = GP32(box->w);
    cmd->box.h       = GP32(box->h);
    cmd->box.d       = GP32(box->d);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("TRANSFER_TO_HOST_3D res=%lu failed, resp=0x%lx", resource_id, rt);
        return FALSE;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * TRANSFER_FROM_HOST_3D -- copy 3D resource from host to guest memory.
 * ----------------------------------------------------------------------- */
BOOL chip_TransferFromHost3D(struct ChipGPUState *gs, uint32 ctx_id,
                               uint32 resource_id,
                               uint32 level, uint32 stride, uint32 layer_stride,
                               uint64 offset, struct virtio_gpu_box *box)
{
    struct ExecIFace *IExec = gs->IExec;
    IExec->MutexObtain(gs->io_lock);

    struct virtio_gpu_transfer_host_3d *cmd =
        (struct virtio_gpu_transfer_host_3d *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*cmd));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    cmd->hdr.ctx_id  = GP32(ctx_id);
    cmd->resource_id = GP32(resource_id);
    cmd->level       = GP32(level);
    cmd->stride      = GP32(stride);
    cmd->layer_stride = GP32(layer_stride);
    cmd->offset      = GP64(offset);
    cmd->box.x       = GP32(box->x);
    cmd->box.y       = GP32(box->y);
    cmd->box.z       = GP32(box->z);
    cmd->box.w       = GP32(box->w);
    cmd->box.h       = GP32(box->h);
    cmd->box.d       = GP32(box->d);

    uint32 rt = chip_gpu_send(gs,
        sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("TRANSFER_FROM_HOST_3D res=%lu failed, resp=0x%lx", resource_id, rt);
        return FALSE;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * SUBMIT_3D -- submit Virgl command stream to the GPU.
 *
 * The command data is placed in cmd3d_buf and sent as a separate OUT
 * scatter-gather entry after the header.  This uses the 3-entry SG
 * pattern: [OUT hdr] [OUT data] [IN resp].
 * ----------------------------------------------------------------------- */
BOOL chip_Submit3D(struct ChipGPUState *gs, uint32 ctx_id,
                    void *cmd_data, uint32 cmd_size)
{
    struct ExecIFace *IExec = gs->IExec;

    if (!gs->cmd3d_buf) {
        DCHIP("SUBMIT_3D: no cmd3d_buf -- 3D not initialised");
        return FALSE;
    }
    if (cmd_size == 0) {
        DCHIP("SUBMIT_3D: cmd_size=0 -- empty submission rejected");
        return FALSE;
    }
    if (cmd_size > 64 * 1024) {
        DCHIP("SUBMIT_3D: cmd_size %lu exceeds 64K buffer", cmd_size);
        return FALSE;
    }

    IExec->MutexObtain(gs->io_lock);

    /* Fill header in cmd_buf */
    struct virtio_gpu_cmd_submit *hdr =
        (struct virtio_gpu_cmd_submit *)gs->cmd_buf;
    chip_zero(gs->cmd_buf,  sizeof(*hdr));
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    hdr->hdr.type   = GP32(VIRTIO_GPU_CMD_SUBMIT_3D);
    hdr->hdr.ctx_id = GP32(ctx_id);
    hdr->size       = GP32(cmd_size);

    /* Copy command data into cmd3d_buf */
    volatile uint8 *dst = (volatile uint8 *)gs->cmd3d_buf;
    uint8 *src = (uint8 *)cmd_data;
    for (uint32 i = 0; i < cmd_size; i++)
        dst[i] = src[i];

    /* Send with 3 scatter-gather entries */
    uint32 written = chip_do_io_3sg(gs,
        gs->cmd_buf_phys, sizeof(*hdr),
        gs->cmd3d_phys, cmd_size,
        sizeof(struct virtio_gpu_ctrl_hdr));

    uint32 rt = 0;
    if (written >= sizeof(struct virtio_gpu_ctrl_hdr)) {
        struct virtio_gpu_ctrl_hdr *resp =
            (struct virtio_gpu_ctrl_hdr *)gs->resp_buf;
        rt = GP32(resp->type);
    }

    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("SUBMIT_3D ctx=%lu size=%lu failed, resp=0x%lx", ctx_id, cmd_size, rt);
        return FALSE;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * RESOURCE_CREATE_BLOB -- create a blob resource.
 *
 * Requires VIRTIO_GPU_F_RESOURCE_BLOB feature.  Two flavours:
 *
 *   blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D   (no backing entries):
 *     Pass dma_list=NULL, dma_count=0.  blob_id selects a host-3D
 *     buffer.  Used by Virgl integration paths.
 *
 *   blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST    (with backing entries):
 *     The host imports our DMA-mapped guest pages and reads them
 *     directly on RESOURCE_FLUSH -- no TRANSFER_TO_HOST round-trip
 *     per frame.  This is the framebuffer fast-path.  blob_id is
 *     ignored.  Caller passes the same dma_list/dma_count it would
 *     for chip_ResourceAttachBacking, and the entries are placed
 *     inline after the header, just like ATTACH_BACKING.
 *
 * The cmd_buf is 4 KB, so the entry list is bounded; we clamp on
 * overflow to match chip_ResourceAttachBacking's behaviour.
 * ----------------------------------------------------------------------- */
BOOL chip_ResourceCreateBlob(struct ChipGPUState *gs, uint32 resource_id,
                              uint32 blob_mem, uint32 blob_flags,
                              uint64 blob_id, uint64 size,
                              struct DMAEntry *dma_list, uint32 dma_count)
{
    struct ExecIFace *IExec = gs->IExec;

    if (!gs->has_blob) {
        DCHIP("RESOURCE_CREATE_BLOB: feature not negotiated");
        return FALSE;
    }

    /* Inline mem entries after the header (same layout as ATTACH_BACKING). */
    struct virtio_gpu_resource_create_blob *cmd =
        (struct virtio_gpu_resource_create_blob *)gs->cmd_buf;
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *)(cmd + 1);

    uint32 max_entries = (4096 - sizeof(*cmd)) / sizeof(struct virtio_gpu_mem_entry);
    if (dma_count > max_entries) {
        DCHIP("RESOURCE_CREATE_BLOB: dma_count=%lu clamped to %lu (cmd_buf size)",
              dma_count, max_entries);
        dma_count = max_entries;
    }

    uint32 cmd_size = sizeof(*cmd)
                    + sizeof(struct virtio_gpu_mem_entry) * dma_count;

    IExec->MutexObtain(gs->io_lock);

    chip_zero(gs->cmd_buf,  cmd_size);
    chip_zero(gs->resp_buf, sizeof(struct virtio_gpu_ctrl_hdr));

    cmd->hdr.type    = GP32(VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB);
    cmd->resource_id = GP32(resource_id);
    cmd->blob_mem    = GP32(blob_mem);
    cmd->blob_flags  = GP32(blob_flags);
    cmd->nr_entries  = GP32(dma_count);
    cmd->blob_id     = GP64(blob_id);
    cmd->size        = GP64(size);

    for (uint32 i = 0; i < dma_count; i++) {
        entries[i].addr    = GP64((uint64)(uint32)dma_list[i].PhysicalAddress);
        entries[i].length  = GP32(dma_list[i].BlockLength);
        entries[i].padding = 0;
    }

    uint32 rt = chip_gpu_send(gs, cmd_size,
                                 sizeof(struct virtio_gpu_ctrl_hdr));
    IExec->MutexRelease(gs->io_lock);

    if (rt != VIRTIO_GPU_RESP_OK_NODATA) {
        DCHIP("RESOURCE_CREATE_BLOB id=%lu mem=%lu nr=%lu failed, resp=0x%lx",
              resource_id, blob_mem, dma_count, rt);
        return FALSE;
    }
    DCHIP("RESOURCE_CREATE_BLOB id=%lu mem=%lu flags=%lu size=%llu nr_entries=%lu OK",
          resource_id, blob_mem, blob_flags, (unsigned long long)size, dma_count);
    return TRUE;
}
