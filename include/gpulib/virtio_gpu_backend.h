#ifndef GPULIB_VIRTIO_GPU_BACKEND_H
#define GPULIB_VIRTIO_GPU_BACKEND_H

/*
** VirtIO-GPU gpu.library backend — submit payload protocol (Phase 3.2a).
**
** GPU_SubmitA payloads for the "virtio-gpu" backend start with a uint32
** opcode. This surface is scaffolding: 3.2b replaces/extends it with the
** formalised v3d command transport (opaque virgl streams).
*/

#define VGB_BACKEND_NAME  "virtio-gpu"

#define VGB_OP_NOP        0  /* transport proof: fence only               */
#define VGB_OP_FLUSH      1  /* wake the chip flush task: present a frame */
#define VGB_OP_GETCTX     2  /* arg = (struct VgbCtxInfo *) to fill; needs
                                virgl (gl=on + virtiogpu_virgl2d=1), else
                                the submit fails GPUERR_NOTIMPL           */
#define VGB_OP_SUBMIT3D   3  /* arg = ctx_id; pre-encoded GP32-swapped
                                virgl words follow the VgbCmd header      */
#define VGB_OP_FLUSHRECT  4  /* struct VgbFlushRect: present a rect of a
                                virgl resource (RESOURCE_FLUSH)           */
#define VGB_OP_TRITEST    5  /* draw the chip's proven RGB test triangle
                                over the scanout (visual transport proof) */

struct VgbCmd
{
    uint32 op;
    uint32 arg;           /* op-specific; 0 when unused                  */
};

struct VgbFlushRect
{
    struct VgbCmd hdr;    /* op = VGB_OP_FLUSHRECT, arg = res_id         */
    uint32 x, y, w, h;
};

/* Filled by VGB_OP_GETCTX: live handles into the chip's virgl pipeline
** (mirrors the chip's "v3d" V3DContextInfo; same vertex layout contract:
** pos[4]+colour[4] interleaved floats, stride 32). */
struct VgbCtxInfo
{
    uint32 ctx_id;
    uint32 vbuf_res;
    uint32 vbuf_size;
    uint32 scanout_res;
    uint32 vs_handle;
    uint32 fs_handle;
    uint32 fs_tex_handle;
    uint32 ve_handle;
    uint32 sampler;
    uint32 sampler_linear;
    uint32 fb_width;
    uint32 fb_height;
};

#endif /* GPULIB_VIRTIO_GPU_BACKEND_H */
