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

#define VGB_OP_NOP    0   /* transport proof: fence only                 */
#define VGB_OP_FLUSH  1   /* wake the chip flush task: present a frame   */

struct VgbCmd
{
    uint32 op;
    uint32 arg;           /* op-specific; 0 for NOP/FLUSH                */
};

#endif /* GPULIB_VIRTIO_GPU_BACKEND_H */
