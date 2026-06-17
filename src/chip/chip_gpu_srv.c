/*
 * chip_gpu_srv.c -- GPU control-queue I/O server task (Phase 8, Part A).
 *
 * A single server task owns the VirtIO control queue.  Clients submit a GpuReq
 * (with their OWN buffers) to the server's MsgPort and block on their own reply
 * -- not on io_lock -- so P96/desktop-present, warp3d and init no longer
 * serialise on a lock held across the GPU round-trip (the cursor/desktop-freeze
 * fix).  See docs/PHASE8_PLAN.md.
 *
 * STATUS: built but NOT yet wired into chip_do_io/chip_do_io_3sg.  During the
 * transition the legacy direct callers still exist, so the ring is still shared
 * and the server takes a BRIEF io_lock around its ring op.  Once every path
 * routes through the server it becomes the sole ring owner and the lock is
 * dropped (then the wait never holds a lock -> full responsiveness + speed).
 */

#include "chip/chip_state.h"
#include "chip/gpu_srv.h"

extern struct ChipGPUState *g_chip_state;

/* Byte copy into/out of MEMF_SHARED DMA buffers (no memcpy -- newlib). */
static void srv_bcopy(volatile uint8 *d, const volatile uint8 *s, uint32 n)
{
    uint32 i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

/* Execute one request on the control queue (runs in the server task).
 * COPY model: copy the caller's cmd/data into the server-owned DMA buffers,
 * submit, then copy the response back to the caller.  io_lock is held only
 * here (server side) to serialise the shared DMA buffers + ring vs the legacy
 * direct callers -- clients never hold it. */
static void gpu_srv_exec(struct ChipGPUState *gs, struct GpuReq *req)
{
    struct ExecIFace *IExec = gs->IExec;
    struct virtqueue *vq = gs->vqs[VIRTIO_GPU_CTRLQ];
    struct vring_sg sg[3];
    uint32 nout, rsize;
    void *cookie;

    if (!vq || !req->cmd || !req->cmd_size) { req->result = 0; return; }
    rsize = req->resp_size ? req->resp_size
                           : (uint32)sizeof(struct virtio_gpu_ctrl_hdr);

    IExec->MutexObtain(gs->io_lock);

    srv_bcopy((volatile uint8 *)gs->cmd_buf, (const volatile uint8 *)req->cmd,
              req->cmd_size);
    chip_zero(gs->resp_buf, rsize);

    if (req->op == GPUREQ_3SG) {
        srv_bcopy((volatile uint8 *)gs->cmd3d_buf,
                  (const volatile uint8 *)req->data, req->data_size);
        sg[0].addr = gs->cmd_buf_phys;  sg[0].len = req->cmd_size;
        sg[1].addr = gs->cmd3d_phys;    sg[1].len = req->data_size;
        sg[2].addr = gs->resp_buf_phys; sg[2].len = rsize;
        nout = 2;
    } else {
        sg[0].addr = gs->cmd_buf_phys;  sg[0].len = req->cmd_size;
        sg[1].addr = gs->resp_buf_phys; sg[1].len = rsize;
        nout = 1;
    }

    cookie = (void *)IExec->FindTask(NULL);   /* the server task */
    {
        int32 rc = VirtQueue_AddBuf(IExec, vq, sg, nout, 1, cookie);
        if (rc == 0) {
            chip_notify_queue(gs, vq, 0);
            req->result = chip_wait_ctrlq(gs, vq, cookie);
        } else {
            req->result = 0;
        }
    }

    if (req->resp && req->resp_size && req->result)
        srv_bcopy((volatile uint8 *)req->resp,
                  (const volatile uint8 *)gs->resp_buf, req->resp_size);

    IExec->MutexRelease(gs->io_lock);
}

/* Server task entry.  Finds state via the chip global, publishes its port, and
 * services requests in priority order until a GPUREQ_QUIT arrives. */
static void gpu_srv_entry(void)
{
    struct ChipGPUState *gs = g_chip_state;
    struct ExecIFace *IExec = gs ? gs->IExec : NULL;
    struct MsgPort *hi, *lo;
    uint32 sig_hi, sig_lo;
    BOOL run = TRUE;

    if (!gs || !IExec) return;

    hi = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    lo = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!hi || !lo) {
        DCHIP("gpu_srv: port alloc failed");
        if (hi) IExec->FreeSysObject(ASOT_PORT, hi);
        if (lo) IExec->FreeSysObject(ASOT_PORT, lo);
        return;
    }
    sig_hi = 1UL << hi->mp_SigBit;
    sig_lo = 1UL << lo->mp_SigBit;

    gs->gpu_srv_hi = hi;
    gs->gpu_srv_lo = lo;
    __asm__ volatile ("" ::: "memory");
    gs->gpu_srv_running = TRUE;
    DCHIP("gpu_srv: started (hi=%p lo=%p)", hi, lo);

    while (run) {
        struct GpuReq *req;
        IExec->Wait(sig_hi | sig_lo);

        /* Strict priority: fully drain HI (present/cursor) every pass, then do
         * ONE LO (bulk draw) and re-check HI -- so the desktop/cursor present is
         * never starved behind the cow's flood of draws. */
        for (;;) {
            while ((req = (struct GpuReq *)IExec->GetMsg(hi))) {
                if (req->op == GPUREQ_QUIT) { run = FALSE; IExec->ReplyMsg(&req->msg); break; }
                gpu_srv_exec(gs, req);
                IExec->ReplyMsg(&req->msg);
            }
            if (!run) break;
            req = (struct GpuReq *)IExec->GetMsg(lo);
            if (!req) break;                 /* both empty -> back to Wait */
            gpu_srv_exec(gs, req);
            IExec->ReplyMsg(&req->msg);
        }
    }

    gs->gpu_srv_running = FALSE;
    __asm__ volatile ("" ::: "memory");
    gs->gpu_srv_hi = NULL;
    gs->gpu_srv_lo = NULL;
    IExec->FreeSysObject(ASOT_PORT, hi);
    IExec->FreeSysObject(ASOT_PORT, lo);
    DCHIP("gpu_srv: stopped");
}

BOOL chip_gpu_srv_start(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;

    if (gs->gpu_srv_running || gs->gpu_srv_task) return TRUE;

    /* CreateTaskTags (no dos dependency) -- mirrors the flush task's primary
     * creation path; properly inits tc_MemEntry etc. (avoids the DSI seen with
     * manual AllocVec+AddTask).  Priority 5: below the pri-10 flush/present so
     * the present preempts the server between its GPU ops. */
    gs->gpu_srv_task = IExec->CreateTaskTags(
        "virtiogpu.gpusrv", 5,
        (CONST_APTR)gpu_srv_entry, 32768, TAG_DONE);
    if (!gs->gpu_srv_task) { DCHIP("gpu_srv: CreateTaskTags failed"); return FALSE; }

    /* Readiness is observed lazily via gs->gpu_srv_port in chip_gpu_srv_do
     * (callers fall back to the legacy path until the port is published). */
    return TRUE;
}

void chip_gpu_srv_stop(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;
    struct GpuReq req;
    struct MsgPort *reply;

    if (!gs->gpu_srv_running || !gs->gpu_srv_hi) return;

    reply = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!reply) return;

    chip_zero(&req, sizeof(req));
    req.op = GPUREQ_QUIT;
    req.msg.mn_Node.ln_Type = NT_MESSAGE;
    req.msg.mn_Length       = sizeof(req);
    req.msg.mn_ReplyPort    = reply;

    IExec->PutMsg(gs->gpu_srv_hi, &req.msg);   /* QUIT on HI so it's seen promptly */
    IExec->WaitPort(reply);
    IExec->GetMsg(reply);
    IExec->FreeSysObject(ASOT_PORT, reply);
}

uint32 chip_gpu_srv_do(struct ChipGPUState *gs, struct GpuReq *req)
{
    struct ExecIFace *IExec = gs->IExec;
    struct MsgPort *transient = NULL;
    struct MsgPort *dest;
    uint32 r;

    if (!gs->gpu_srv_running) return 0;   /* not ready -> caller uses legacy path */

    if (!req->msg.mn_ReplyPort) {
        transient = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
        if (!transient) return 0;
        req->msg.mn_ReplyPort = transient;
    }
    req->msg.mn_Node.ln_Type = NT_MESSAGE;
    req->msg.mn_Length       = sizeof(*req);

    /* Priority routing: present/cursor (pri > 0) -> HI port; bulk draws -> LO. */
    dest = (req->msg.mn_Node.ln_Pri > 0) ? gs->gpu_srv_hi : gs->gpu_srv_lo;
    IExec->PutMsg(dest, &req->msg);
    IExec->WaitPort(req->msg.mn_ReplyPort);
    IExec->GetMsg(req->msg.mn_ReplyPort);
    r = req->result;

    if (transient) {
        IExec->FreeSysObject(ASOT_PORT, transient);
        req->msg.mn_ReplyPort = NULL;
    }
    return r;
}

uint32 chip_srv_send2(struct ChipGPUState *gs, int pri,
                      const void *cmd, uint32 cmd_size,
                      void *resp, uint32 resp_size)
{
    struct GpuReq req;
    chip_zero(&req, sizeof(req));
    req.op = GPUREQ_2SG;
    req.msg.mn_Node.ln_Pri = (BYTE)pri;
    req.cmd = cmd;   req.cmd_size = cmd_size;
    req.resp = resp; req.resp_size = resp_size;
    if (!chip_gpu_srv_do(gs, &req)) return 0;   /* not running / failed */
    if (resp && resp_size >= sizeof(struct virtio_gpu_ctrl_hdr))
        return GP32(((struct virtio_gpu_ctrl_hdr *)resp)->type);
    return req.result;
}

uint32 chip_srv_submit3d(struct ChipGPUState *gs, int pri,
                         const void *hdr, uint32 hdr_size,
                         const void *data, uint32 data_size,
                         void *resp, uint32 resp_size)
{
    struct GpuReq req;
    chip_zero(&req, sizeof(req));
    req.op = GPUREQ_3SG;
    req.msg.mn_Node.ln_Pri = (BYTE)pri;
    req.cmd = hdr;   req.cmd_size = hdr_size;
    req.data = data; req.data_size = data_size;
    req.resp = resp; req.resp_size = resp_size;
    return chip_gpu_srv_do(gs, &req);   /* nonzero result = ok */
}
