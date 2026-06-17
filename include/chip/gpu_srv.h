/*
 * gpu_srv.h -- GPU control-queue I/O server (Phase 8, Part A).
 *
 * We are a multi-consumer driver: P96/Workbench (2D desktop present), warp3d
 * (3D), and the cursor all drive the one VirtIO GPU.  Holding io_lock across a
 * GPU round-trip on the hot path serialises them and freezes the cursor/desktop
 * under TCG.  The Amiga-native fix is the device-I/O model: a single server
 * task owns the control queue; clients submit per-request work (own buffers)
 * via a priority-sorted MsgPort and block on their own reply -- not a lock.
 *
 * This header defines the request object + the server lifecycle/submit API.
 * (Part A1 wires chip_do_io / chip_do_io_3sg to route through here; until then
 * the server is built but unused.)
 */
#ifndef CHIP_GPU_SRV_H
#define CHIP_GPU_SRV_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/nodes.h>

struct ChipGPUState;

/* GpuReq.op -- scatter/gather shape of the control-queue transaction. */
#define GPUREQ_2SG   1   /* [OUT cmd] [IN resp]                 (most commands) */
#define GPUREQ_3SG   2   /* [OUT hdr] [OUT data] [IN resp]      (SUBMIT_3D)     */
#define GPUREQ_QUIT  3   /* server shutdown sentinel                            */

/* GpuReq.flags */
#define GPUREQF_ASYNC  0x0001  /* draws/blits: no caller block; reply only
                                * recycles the request/buffer (Part A2) */

/* Priorities for the server's priority-sorted port (msg.mn_Node.ln_Pri):
 * present/cursor must jump ahead of bulk 3D draws. */
#define GPUREQ_PRI_PRESENT   10   /* desktop present / cursor          */
#define GPUREQ_PRI_NORMAL     0   /* misc control commands             */
#define GPUREQ_PRI_DRAW     (-10) /* bulk warp3d draws (SUBMIT_3D)     */

/* A single control-queue transaction.  The caller owns all buffers referenced
 * by the *_phys fields (per-context or per-call) -- the server never touches a
 * shared global cmd_buf/resp_buf, so two clients never clobber each other. */
struct GpuReq {
    struct Message msg;        /* mn_ReplyPort = caller's reply port;
                                * mn_Node.ln_Pri = GPUREQ_PRI_*               */
    uint16  op;                /* GPUREQ_2SG / _3SG / _QUIT                    */
    uint16  flags;             /* GPUREQF_*                                    */
    uint32  hdr_phys,  hdr_size;   /* OUT (cmd header)                         */
    uint32  data_phys, data_size;  /* OUT 2nd entry (3SG only; 0 otherwise)    */
    uint32  resp_phys, resp_size;  /* IN  (response; 0 = none wanted)          */
    uint32  result;            /* bytes written into resp (server-filled);
                                * 0 = timeout/error                            */
};

/* Lifecycle (chip_gpu_srv.c). */
BOOL chip_gpu_srv_start(struct ChipGPUState *gs);
void chip_gpu_srv_stop(struct ChipGPUState *gs);

/* Synchronous submit: PutMsg to the server, block on the reply, return
 * req->result.  If req->msg.mn_ReplyPort is NULL a transient port is used.
 * Caller fills op/flags/*_phys/*_size and (optionally) ln_Pri first. */
uint32 chip_gpu_srv_do(struct ChipGPUState *gs, struct GpuReq *req);

#endif /* CHIP_GPU_SRV_H */
