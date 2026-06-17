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
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/dos.h>

extern struct ChipGPUState *g_chip_state;

/* Execute one request on the control queue (runs in the server task). */
static void gpu_srv_exec(struct ChipGPUState *gs, struct GpuReq *req)
{
    struct ExecIFace *IExec = gs->IExec;
    struct virtqueue *vq = gs->vqs[VIRTIO_GPU_CTRLQ];
    struct vring_sg sg[3];
    uint32 nout;
    void *cookie;

    if (!vq) { req->result = 0; return; }

    if (req->op == GPUREQ_3SG) {
        sg[0].addr = req->hdr_phys;  sg[0].len = req->hdr_size;
        sg[1].addr = req->data_phys; sg[1].len = req->data_size;
        sg[2].addr = req->resp_phys; sg[2].len = req->resp_size;
        nout = 2;
    } else {
        sg[0].addr = req->hdr_phys;  sg[0].len = req->hdr_size;
        sg[1].addr = req->resp_phys; sg[1].len = req->resp_size;
        nout = 1;
    }

    cookie = (void *)IExec->FindTask(NULL);   /* the server task */

    /* Transition lock: the ring is still shared with legacy direct callers.
     * Held only around the ring op here (the win vs. the old model is that
     * CLIENTS no longer hold io_lock at all -- they wait on their reply port). */
    IExec->MutexObtain(gs->io_lock);
    {
        int32 rc = VirtQueue_AddBuf(IExec, vq, sg, nout, 1, cookie);
        if (rc == 0) {
            chip_notify_queue(gs, vq, 0);
            req->result = chip_wait_ctrlq(gs, vq, cookie);
        } else {
            req->result = 0;
        }
    }
    IExec->MutexRelease(gs->io_lock);
}

/* Server task entry.  Finds state via the chip global, publishes its port, and
 * services requests in priority order until a GPUREQ_QUIT arrives. */
static void gpu_srv_entry(void)
{
    struct ChipGPUState *gs = g_chip_state;
    struct ExecIFace *IExec = gs ? gs->IExec : NULL;
    struct MsgPort *port;
    BOOL run = TRUE;

    if (!gs || !IExec) return;

    port = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) { DCHIP("gpu_srv: port alloc failed"); return; }

    gs->gpu_srv_port = port;
    __asm__ volatile ("" ::: "memory");
    gs->gpu_srv_running = TRUE;
    DCHIP("gpu_srv: started (port=%p)", port);

    while (run) {
        struct GpuReq *req;
        IExec->WaitPort(port);
        while ((req = (struct GpuReq *)IExec->GetMsg(port))) {
            if (req->op == GPUREQ_QUIT) {
                run = FALSE;
                IExec->ReplyMsg(&req->msg);
                break;
            }
            gpu_srv_exec(gs, req);
            IExec->ReplyMsg(&req->msg);
        }
    }

    gs->gpu_srv_running = FALSE;
    __asm__ volatile ("" ::: "memory");
    gs->gpu_srv_port = NULL;
    IExec->FreeSysObject(ASOT_PORT, port);
    DCHIP("gpu_srv: stopped");
}

BOOL chip_gpu_srv_start(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;

    if (gs->gpu_srv_running) return TRUE;

    if (!gs->IDOS) {
        if (!gs->DOSBase)
            gs->DOSBase = IExec->OpenLibrary("dos.library", 54);
        if (gs->DOSBase)
            gs->IDOS = (struct DOSIFace *)
                IExec->GetInterface(gs->DOSBase, "main", 1, NULL);
    }
    if (!gs->IDOS) { DCHIP("gpu_srv: no dos.library"); return FALSE; }

    gs->gpu_srv_proc = gs->IDOS->CreateNewProcTags(
        NP_Entry,     (ULONG)gpu_srv_entry,
        NP_Name,      (ULONG)"virtiogpu.gpusrv",
        NP_StackSize, 32768,
        NP_Priority,  5,
        NP_Child,     TRUE,
        TAG_DONE);
    if (!gs->gpu_srv_proc) { DCHIP("gpu_srv: CreateNewProc failed"); return FALSE; }

    /* Readiness is observed lazily via gs->gpu_srv_port in chip_gpu_srv_do
     * (callers fall back to the legacy path until the port is published). */
    return TRUE;
}

void chip_gpu_srv_stop(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;
    struct GpuReq req;
    struct MsgPort *reply;

    if (!gs->gpu_srv_running || !gs->gpu_srv_port) return;

    reply = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!reply) return;

    chip_zero(&req, sizeof(req));
    req.op = GPUREQ_QUIT;
    req.msg.mn_Node.ln_Type = NT_MESSAGE;
    req.msg.mn_Length       = sizeof(req);
    req.msg.mn_ReplyPort    = reply;

    IExec->PutMsg(gs->gpu_srv_port, &req.msg);
    IExec->WaitPort(reply);
    IExec->GetMsg(reply);
    IExec->FreeSysObject(ASOT_PORT, reply);
}

uint32 chip_gpu_srv_do(struct ChipGPUState *gs, struct GpuReq *req)
{
    struct ExecIFace *IExec = gs->IExec;
    struct MsgPort *transient = NULL;
    uint32 r;

    if (!gs->gpu_srv_port) return 0;   /* not ready -> caller uses legacy path */

    if (!req->msg.mn_ReplyPort) {
        transient = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
        if (!transient) return 0;
        req->msg.mn_ReplyPort = transient;
    }
    req->msg.mn_Node.ln_Type = NT_MESSAGE;
    req->msg.mn_Length       = sizeof(*req);

    IExec->PutMsg(gs->gpu_srv_port, &req->msg);
    IExec->WaitPort(req->msg.mn_ReplyPort);
    IExec->GetMsg(req->msg.mn_ReplyPort);
    r = req->result;

    if (transient) {
        IExec->FreeSysObject(ASOT_PORT, transient);
        req->msg.mn_ReplyPort = NULL;
    }
    return r;
}
