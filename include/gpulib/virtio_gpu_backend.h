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
#define VGB_OP_V3DCALL    6  /* generic v3d method call: arg = VGB_V3D_*,
                                args in the VgbV3DCall block. Pointers in
                                the args are read/written DURING the
                                (synchronous) submit — valid because the
                                caller blocks until completion            */

/* VGB_OP_V3DCALL method selectors (mirror the chip's v3d transport).
** Arg packing (a[0] = token, then the v3d method's args in order;
** pointer args passed as uint32):                                       */
#define VGB_V3D_OBTAIN     0  /* a[0]=(struct V3DContextInfo *)out        */
#define VGB_V3D_SUBMIT     1  /* a[0]=tok a[1]=ctx a[2]=words a[3]=nwords */
#define VGB_V3D_FLUSH      2  /* a[0]=tok a[1]=res a[2..5]=x,y,w,h        */
#define VGB_V3D_RELEASE    3  /* a[0]=tok                                 */
#define VGB_V3D_ALLOC_RT   4  /* a[0]=tok a[1]=w a[2]=h a[3]=&res a[4]=&surf */
#define VGB_V3D_OVERLAY    5  /* a[0]=tok a[1]=rt a[2]=sw a[3]=sh
                                 a[4..7]=x,y,w,h a[8]=enable              */
#define VGB_V3D_FREE_RT    6  /* a[0]=tok a[1]=res a[2]=surface           */
#define VGB_V3D_ALLOC_Z    7  /* a[0]=tok a[1]=w a[2]=h a[3]=&res a[4]=&surf */
#define VGB_V3D_CREATE_TEX 8  /* a[0]=tok a[1]=w a[2]=h a[3]=data a[4]=bpr
                                 a[5]=&view a[6]=&res                     */
#define VGB_V3D_FREE_TEX   9  /* a[0]=tok a[1]=res a[2]=view              */
#define VGB_V3D_PRESENT_BM 10 /* a[0]=tok a[1]=rt a[2]=sw a[3]=sh
                                 a[4]=dst_base a[5]=dst_stride            */

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

struct VgbV3DCall
{
    struct VgbCmd hdr;    /* op = VGB_OP_V3DCALL, arg = VGB_V3D_*         */
    uint32 a[10];
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
