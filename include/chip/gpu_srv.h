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

/* A single control-queue transaction (COPY model).  The caller passes its
 * command in its OWN memory (stack/context buffers); the server copies it into
 * the server-owned DMA cmd/cmd3d buffers, submits, and copies the response back
 * into the caller's resp buffer.  So callers never touch the shared DMA buffers
 * and never hold io_lock across the GPU wait -- they block only on their reply
 * port.  The server (sole hot io_lock holder) prioritises present/cursor over
 * bulk draws (msg.mn_Node.ln_Pri). */
struct GpuReq {
    struct Message msg;        /* mn_ReplyPort = caller's reply port;
                                * mn_Node.ln_Pri = GPUREQ_PRI_*               */
    uint16  op;                /* GPUREQ_2SG / _3SG / _QUIT                    */
    uint16  flags;             /* GPUREQF_*                                    */
    const void *cmd;   uint32 cmd_size;   /* OUT header (-> DMA cmd_buf)       */
    const void *data;  uint32 data_size;  /* OUT data (3SG only; -> cmd3d_buf) */
    void       *resp;  uint32 resp_size;  /* IN (<- DMA resp_buf; 0 = none)    */
    uint32  result;            /* bytes written into resp (0 = timeout/error)  */
};

/* Lifecycle (chip_gpu_srv.c). */
BOOL chip_gpu_srv_start(struct ChipGPUState *gs);
void chip_gpu_srv_stop(struct ChipGPUState *gs);

/* Synchronous submit: PutMsg to the server, block on the reply, return
 * req->result.  If req->msg.mn_ReplyPort is NULL a transient port is used.
 * Caller fills op/flags/cmd/data/resp and (optionally) ln_Pri first.
 * Returns 0 (and does nothing) if the server isn't running -> caller falls
 * back to the legacy direct path. */
uint32 chip_gpu_srv_do(struct ChipGPUState *gs, struct GpuReq *req);

/* Convenience: 2-SG transaction (cmd + resp) via the server, priority `pri`.
 * Returns the response type (GP32 of resp hdr), or 0 on failure / not-running. */
uint32 chip_srv_send2(struct ChipGPUState *gs, int pri,
                      const void *cmd, uint32 cmd_size,
                      void *resp, uint32 resp_size);

/* Convenience: 3-SG SUBMIT_3D (hdr + data + resp) via the server.
 * Returns nonzero on success, 0 on failure / not-running. */
uint32 chip_srv_submit3d(struct ChipGPUState *gs, int pri,
                         const void *hdr, uint32 hdr_size,
                         const void *data, uint32 data_size,
                         void *resp, uint32 resp_size);

#endif /* CHIP_GPU_SRV_H */
