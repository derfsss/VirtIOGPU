/*
** gpu_vtest — exercises the chip's gpu.library backend end-to-end
** (Phase 3.2a): find the "virtio-gpu" backend, buffer round trip, NOP
** submit + fence, FLUSH submit, Present. Exit code authoritative.
**
** Build: newlib CLI tool (see Makefile GPUVTEST target).
*/

#include <proto/exec.h>
#include <libraries/gpu.h>
#include <interfaces/gpu.h>
#include <gpulib/virtio_gpu_backend.h>

#include <stdio.h>

int main(void)
{
    struct Library *GpuBase;
    struct GpuIFace *IGpu;
    int32 vid = -1;
    uint32 count = 0;
    int rc = 10;

    GpuBase = IExec->OpenLibrary(GPU_LIBNAME, 53);
    IGpu = GpuBase ? (struct GpuIFace *)
           IExec->GetInterface(GpuBase, "main", 1, NULL) : NULL;
    if (IGpu == NULL)
    {
        printf("gpu_vtest: cannot open gpu.library\n");
        return 20;
    }

    /* find the virtio-gpu backend */
    {
        struct TagItem ct[] = { { GPUATTR_BackendCount, 0 }, { TAG_DONE, 0 } };
        uint32 i;
        ct[0].ti_Data = (uint32)&count;
        IGpu->GPU_GetAttrsA(ct);
        for (i = 0; i < 4; i++)
        {
            CONST_STRPTR name = NULL;
            struct TagItem nt[] =
            {
                { GPUATTR_BackendIndex, i },
                { GPUATTR_BackendName,  0 },
                { TAG_DONE,             0 }
            };
            nt[1].ti_Data = (uint32)&name;
            IGpu->GPU_GetAttrsA(nt);
            if (name != NULL && name[0] == 'v' && name[1] == 'i')
            {
                vid = (int32)i;
                break;
            }
        }
    }
    printf("gpu_vtest: %lu backend(s), virtio-gpu id=%ld\n",
           (unsigned long)count, (long)vid);

    if (vid >= 0)
    {
        struct GpuBuffer *buf = IGpu->GPU_CreateBufferA(sizeof(struct VgbCmd),
            GPU_TAGS({ GPUTAG_Backend, (uint32)vid }));
        if (buf != NULL)
        {
            struct VgbCmd *cmd = (struct VgbCmd *)IGpu->GPU_MapBuffer(buf);
            int32 f1 = -1, f2 = -1, t1 = -99, pr = -99;

            if (cmd != NULL)
            {
                cmd->op  = VGB_OP_NOP;
                cmd->arg = 0;
                f1 = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, cmd,
                                       sizeof(*cmd),
                                       GPU_TAGS({ GPUTAG_Backend,
                                                  (uint32)vid }));
                t1 = IGpu->GPU_TestFence(f1);

                cmd->op = VGB_OP_FLUSH;
                f2 = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, cmd,
                                       sizeof(*cmd),
                                       GPU_TAGS({ GPUTAG_Backend,
                                                  (uint32)vid }));
                pr = IGpu->GPU_PresentA(buf, NULL);
                IGpu->GPU_UnmapBuffer(buf);
            }
            printf("gpu_vtest: NOP fence=%ld test=%ld | FLUSH fence=%ld "
                   "present=%ld\n", (long)f1, (long)t1, (long)f2, (long)pr);

            if (f1 > 0 && t1 == 1 && f2 > f1 && pr == GPUERR_OK)
                rc = 0;

            /* virgl path: needs gl=on device + virtiogpu_virgl2d=1 */
            if (rc == 0 && cmd != NULL)
            {
                struct VgbCtxInfo ctx;
                int32 fc;

                cmd = (struct VgbCmd *)IGpu->GPU_MapBuffer(buf);
                cmd->op  = VGB_OP_GETCTX;
                cmd->arg = (uint32)&ctx;
                fc = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, cmd, sizeof(*cmd),
                                       GPU_TAGS({ GPUTAG_Backend,
                                                  (uint32)vid }));
                if (fc == GPUERR_NOTIMPL)
                {
                    printf("gpu_vtest: virgl NOT available (2D profile) -- "
                           "3D ops skipped\n");
                }
                else if (fc > 0)
                {
                    struct VgbFlushRect fr;
                    int32 ft, ff;

                    printf("gpu_vtest: virgl ctx=%lu scanout=%lu %lux%lu\n",
                           (unsigned long)ctx.ctx_id,
                           (unsigned long)ctx.scanout_res,
                           (unsigned long)ctx.fb_width,
                           (unsigned long)ctx.fb_height);

                    cmd->op  = VGB_OP_TRITEST;
                    cmd->arg = 0;
                    ft = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, cmd,
                                           sizeof(*cmd),
                                           GPU_TAGS({ GPUTAG_Backend,
                                                      (uint32)vid }));

                    fr.hdr.op  = VGB_OP_FLUSHRECT;
                    fr.hdr.arg = ctx.scanout_res;
                    fr.x = 0; fr.y = 0;
                    fr.w = ctx.fb_width; fr.h = ctx.fb_height;
                    ff = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, &fr,
                                           sizeof(fr),
                                           GPU_TAGS({ GPUTAG_Backend,
                                                      (uint32)vid }));

                    printf("gpu_vtest: TRITEST fence=%ld FLUSHRECT "
                           "fence=%ld\n", (long)ft, (long)ff);
                    if (ft <= 0 || ff <= 0)
                        rc = 10;
                }
                else
                {
                    printf("gpu_vtest: GETCTX failed (%ld)\n", (long)fc);
                    rc = 10;
                }
                IGpu->GPU_UnmapBuffer(buf);
            }

            IGpu->GPU_DestroyBuffer(buf);

            if (rc == 0)
            {
                printf("gpu_vtest: ALL PASS\n");
                IExec->DebugPrintF("[gpu_vtest] ALL PASS\n");
            }
        }
        else
        {
            printf("gpu_vtest: CreateBuffer on virtio backend failed\n");
        }
    }
    if (rc != 0)
        IExec->DebugPrintF("[gpu_vtest] FAIL\n");

    IExec->DropInterface((struct Interface *)IGpu);
    IExec->CloseLibrary(GpuBase);
    return rc;
}
