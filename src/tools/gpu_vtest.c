/*
** gpu_vtest — exercises the chip's gpu.library backend end-to-end
** (Phase 3.2a): find the "virtio-gpu" backend, buffer round trip, NOP
** submit + fence, FLUSH submit, Present. Exit code authoritative.
**
** Build: newlib CLI tool (see Makefile GPUVTEST target).
*/

#include <proto/exec.h>
#include <proto/graphics.h>
#include <graphics/gfx.h>
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

            /* async-fence notification: the virtio backend registers with
               GPUTAG_AsyncFences, so this signal is delivered via the
               GPU_FenceRetired path -- Wait() proves deferred delivery. */
            if (rc == 0 && cmd != NULL)
            {
                int8 sn = IExec->AllocSignal(-1);
                if (sn != -1)
                {
                    uint32 mask = 1u << sn;
                    int32 fn;
                    IExec->SetSignal(0, mask);
                    cmd->op = VGB_OP_NOP;
                    fn = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, cmd,
                             sizeof(*cmd),
                             GPU_TAGS({ GPUTAG_Backend, (uint32)vid },
                                      { GPUTAG_NotifySignal, mask }));
                    if (fn > 0)
                    {
                        IExec->Wait(mask);
                        printf("gpu_vtest: async notify fence=%ld "
                               "DELIVERED\n", (long)fn);
                    }
                    else
                    {
                        printf("gpu_vtest: async notify submit failed "
                               "(%ld)\n", (long)fn);
                        rc = 10;
                    }
                    IExec->FreeSignal(sn);
                }
            }

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

                    /* Phase 5b: composite mechanic through the public API
                       -- import a P96 bitmap, feed its locked base/stride
                       to the backend's texture upload, free. This is the
                       exact bitmap->GPU-texture path the composite Tier 1
                       hook uses. */
                    {
                        struct BitMap *bm = IGraphics->AllocBitMapTags(
                            32, 32, 32,
                            BMATags_PixelFormat, PIXF_A8R8G8B8,
                            BMATags_Clear,       TRUE,
                            TAG_DONE);
                        if (bm != NULL)
                        {
                            uint32 bpr = 0, iw = 0, ih = 0;
                            struct GpuBuffer *ib = IGpu->GPU_ImportBitMapA(
                                bm, GPU_TAGS(
                                { GPUTAG_OutBytesPerRow, (uint32)&bpr },
                                { GPUTAG_OutWidth,       (uint32)&iw },
                                { GPUTAG_OutHeight,      (uint32)&ih }));
                            if (ib != NULL)
                            {
                                uint32 *px = (uint32 *)
                                    IGpu->GPU_MapBuffer(ib);
                                uint32 view = 0, res = 0;
                                struct VgbV3DCall vc;
                                int32 fr2;
                                uint32 yy, xx;

                                for (yy = 0; yy < ih; yy++)
                                    for (xx = 0; xx < iw; xx++)
                                        px[yy * (bpr / 4) + xx] =
                                            0xFF000000 | (xx * 8 << 16) |
                                            (yy * 8);

                                vc.a[0] = 0;
                                vc.a[1] = iw; vc.a[2] = ih;
                                vc.a[3] = (uint32)px; vc.a[4] = bpr;
                                vc.a[5] = (uint32)&view;
                                vc.a[6] = (uint32)&res;
                                vc.hdr.op  = VGB_OP_V3DCALL;
                                vc.hdr.arg = VGB_V3D_CREATE_TEX;
                                fr2 = IGpu->GPU_SubmitA(GPU_QUEUE_RENDER,
                                        &vc, sizeof(vc),
                                        GPU_TAGS({ GPUTAG_Backend,
                                                   (uint32)vid }));
                                printf("gpu_vtest: import->texture "
                                       "fence=%ld view=%lu res=%lu\n",
                                       (long)fr2, (unsigned long)view,
                                       (unsigned long)res);
                                if (fr2 <= 0 || view == 0 || res == 0)
                                    rc = 10;
                                else
                                {
                                    vc.hdr.arg = VGB_V3D_FREE_TEX;
                                    vc.a[1] = res; vc.a[2] = view;
                                    IGpu->GPU_SubmitA(GPU_QUEUE_RENDER,
                                        &vc, sizeof(vc),
                                        GPU_TAGS({ GPUTAG_Backend,
                                                   (uint32)vid }));
                                }
                                IGpu->GPU_DestroyBuffer(ib);
                            }
                            else
                            {
                                printf("gpu_vtest: ImportBitMapA "
                                       "failed\n");
                                rc = 10;
                            }
                            IGraphics->FreeBitMap(bm);
                        }
                    }
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
