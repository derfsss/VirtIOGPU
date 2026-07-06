/*
 * v3d_iface.h -- shared "v3d" transport interface between virtiogpu.chip
 * (provider) and warp3d.library (consumer).
 *
 * Permissively licensed, like the rest of the chip/project code, so that the
 * GPL warp3d.library may include and call it.  The chip never links any GPL
 * code -- this header only describes a thin C ABI.  The chip remains the sole
 * owner of the virtio-gpu device; "v3d" lets an external renderer drive the
 * chip's already-initialised virgl 3D pipeline.
 *
 * Obtained via:  IExec->GetInterface(chipLib, "v3d", 1, NULL) -> struct V3DIFace *
 */
#ifndef V3D_IFACE_H
#define V3D_IFACE_H

#include <exec/types.h>
#include <exec/interfaces.h>

struct BitMap;

#define V3D_IFACE_NAME     "v3d"
#define V3D_IFACE_VERSION  1

/* Handles into the chip's live virgl 3D pipeline returned by ObtainContext.
 * The vertex layout the chip's vbuf_res + ve_handle expect is
 *   pos[4] + colour[4] interleaved floats, stride 32 bytes.
 * All IDs are virgl object handles valid on ctx_id. */
struct V3DContextInfo {
    APTR   token;          /* opaque; pass back to Submit/Flush/ReleaseContext */
    uint32 ctx_id;         /* virgl 3D context */
    uint32 vbuf_res;       /* shared vertex-buffer resource (PIPE_BUFFER)      */
    uint32 vbuf_size;      /* usable bytes in vbuf_res                          */
    uint32 scanout_res;    /* on-screen 3D resource to flush/present           */
    uint32 vs_handle;      /* position+colour vertex shader                    */
    uint32 fs_handle;      /* per-vertex colour fragment shader                */
    uint32 fs_tex_handle;  /* textured fragment shader                         */
    uint32 ve_handle;      /* vertex elements (pos[4]+colour[4], stride 32)    */
    uint32 sampler;        /* sampler state (nearest)                          */
    uint32 sampler_linear; /* sampler state (linear)                           */
    uint32 fb_width;       /* destination width  (pixels)                      */
    uint32 fb_height;      /* destination height (pixels)                      */
    uint32 caps;           /* reserved feature bits                            */
    /* --- W3D_SetTexEnv combine pipeline (pos+texcoord+colour, stride 48);
     *     any 0 => not available, backend falls back to REPLACE. Append-only. --- */
    uint32 vs3_handle;         /* 3-attr passthrough VS                        */
    uint32 fs_modulate_handle; /* W3D_MODULATE FS (texel * colour)             */
    uint32 fs_decal_handle;    /* W3D_DECAL FS                                 */
    uint32 fs_blend_handle;    /* W3D_BLEND FS (env colour in CONST[0])        */
    uint32 ve3_handle;         /* 3-attr vertex elements (stride 48)           */
    uint32 fs_repfog_handle;   /* REPLACE+fog FS (fog colour in CONST[1])      */
};

struct V3DIFace {
    struct InterfaceData Data;

    uint32 (*Obtain)(struct V3DIFace *Self);
    uint32 (*Release)(struct V3DIFace *Self);
    APTR   (*Expunge)(struct V3DIFace *Self);
    struct Interface *(*Clone)(struct V3DIFace *Self);

    /* Fill *info from the chip's live virgl pipeline for rendering into dest.
     * Returns TRUE if 3D is available (virgl negotiated + pipeline ready). */
    BOOL (*ObtainContext)(struct V3DIFace *Self, struct BitMap *dest,
                          struct V3DContextInfo *info);

    /* Submit a pre-encoded virgl command stream: nwords 32-bit words already
     * GP32-swapped by the virgl_cmd encoders.  Forwarded to SUBMIT_3D on
     * ctx_id under the chip's io_lock. */
    BOOL (*Submit)(struct V3DIFace *Self, APTR token, uint32 ctx_id,
                   const uint32 *words, uint32 nwords);

    /* Present a rectangle of res_id to the display (RESOURCE_FLUSH). */
    BOOL (*Flush)(struct V3DIFace *Self, APTR token, uint32 res_id,
                  uint32 x, uint32 y, uint32 w, uint32 h);

    /* Release a context obtained via ObtainContext (no-op for the shared
     * pipeline in milestone 1). */
    void (*ReleaseContext)(struct V3DIFace *Self, APTR token);

    /* --- Milestone 2: own render target + overlay compositing --- */

    /* Allocate a B8G8R8X8 render-target resource (w x h) in the chip's virgl
     * context, create a surface for it, and return both handles.  The caller
     * (warp3d.library) binds the surface as its framebuffer and renders into
     * the RT.  Returns TRUE on success. */
    BOOL (*AllocRenderTarget)(struct V3DIFace *Self, APTR token,
                              uint32 w, uint32 h,
                              uint32 *res_out, uint32 *surface_out);

    /* Register (enable=TRUE) / unregister (enable=FALSE) an RT resource to be
     * composited onto the scanout each frame: the chip BLITs the full RT
     * (sw x sh) onto the scanout dest rect (x,y,w,h) after every flush.  This
     * makes the 3D output persist over the desktop without flicker. */
    void (*RegisterOverlay)(struct V3DIFace *Self, APTR token,
                            uint32 rt_res, uint32 sw, uint32 sh,
                            uint32 x, uint32 y, uint32 w, uint32 h,
                            BOOL enable);

    /* Free an RT resource (and its surface) previously allocated. */
    void (*FreeRenderTarget)(struct V3DIFace *Self, APTR token,
                             uint32 res, uint32 surface);

    /* Allocate a depth buffer (Z24X8) resource + surface for w x h.  The caller
     * binds *surface_out as the framebuffer's zsurf.  Free with FreeRenderTarget. */
    BOOL (*AllocDepthBuffer)(struct V3DIFace *Self, APTR token,
                             uint32 w, uint32 h,
                             uint32 *res_out, uint32 *surface_out);

    /* Create an R8G8B8A8 texture (w x h), upload the pixel data (src_bpr =
     * source bytes-per-row), and create a sampler view.  Returns the sampler
     * view handle (bind to a fragment sampler slot) and the resource id.
     * Free with FreeTexture. */
    BOOL (*CreateTexture)(struct V3DIFace *Self, APTR token,
                          uint32 w, uint32 h, APTR data, uint32 src_bpr,
                          uint32 *view_out, uint32 *res_out);

    /* Destroy a texture's sampler view + resource. */
    void (*FreeTexture)(struct V3DIFace *Self, APTR token,
                        uint32 res, uint32 view);

    /* --- Milestone 3: present into a windowed app's W3D_CC_BITMAP --- */

    /* Read render target rt_res (sw x sh, B8G8R8X8) back into a CPU bitmap at
     * dst_base (dst_stride bytes/row).  The chip BLITs the RT into an internal
     * readback resource, TRANSFER_FROM_HOST_3D's it to guest memory, then
     * reverse-converts (B8G8R8X8 -> the active RTG format) into dst_base.  This
     * is how a windowed Warp3D app's off-screen bitmap receives the 3D output;
     * the app then blits that bitmap into its window itself.  No overlay /
     * scanout compositing.  Returns TRUE on success. */
    BOOL (*PresentBitmap)(struct V3DIFace *Self, APTR token, uint32 rt_res,
                          uint32 sw, uint32 sh,
                          APTR dst_base, uint32 dst_stride);
};

#endif /* V3D_IFACE_H */
