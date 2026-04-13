/*
 * virgl_cmd.h -- Virgl 3D command stream encoder for VirtIO GPU.
 *
 * Builds structured Virgl command buffers (arrays of LE uint32 words)
 * for submission via chip_Submit3D().  Every word is GP32-swapped
 * (PPC BE -> LE for x86 QEMU host).
 *
 * Reference: mesa/src/gallium/drivers/virgl/virgl_protocol.h
 *            mesa/src/gallium/drivers/virgl/virgl_encode.c
 */

#ifndef VIRGL_CMD_H
#define VIRGL_CMD_H

#include <exec/types.h>
#include "gpu/virtio_gpu_proto.h"   /* GP32 */

/* Forward declaration -- full definition in chip/chip_state.h */
struct ChipGPUState;

/* -----------------------------------------------------------------------
 * Virgl command IDs (VIRGL_CCMD_*)
 * ----------------------------------------------------------------------- */
#define VIRGL_CCMD_NOP                      0
#define VIRGL_CCMD_CREATE_OBJECT            1
#define VIRGL_CCMD_BIND_OBJECT              2
#define VIRGL_CCMD_DESTROY_OBJECT           3
#define VIRGL_CCMD_SET_VIEWPORT_STATE       4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE    5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS       6
#define VIRGL_CCMD_CLEAR                    7
#define VIRGL_CCMD_DRAW_VBO                 8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE    9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS        10
#define VIRGL_CCMD_SET_INDEX_BUFFER         11
#define VIRGL_CCMD_SET_CONSTANT_BUFFER      12
#define VIRGL_CCMD_SET_STENCIL_REF          13
#define VIRGL_CCMD_SET_BLEND_COLOR          14
#define VIRGL_CCMD_SET_SCISSOR_STATE        15
#define VIRGL_CCMD_BLIT                     16
#define VIRGL_CCMD_RESOURCE_COPY_REGION     17
#define VIRGL_CCMD_BIND_SAMPLER_STATES      18
#define VIRGL_CCMD_BEGIN_QUERY              19
#define VIRGL_CCMD_END_QUERY               20
#define VIRGL_CCMD_GET_QUERY_RESULT         21
#define VIRGL_CCMD_SET_POLYGON_STIPPLE      22
#define VIRGL_CCMD_SET_CLIP_STATE           23
#define VIRGL_CCMD_SET_SAMPLE_MASK          24
#define VIRGL_CCMD_SET_STREAMOUT_TARGETS    25
#define VIRGL_CCMD_SET_RENDER_CONDITION     26
#define VIRGL_CCMD_SET_UNIFORM_BUFFER       27
#define VIRGL_CCMD_SET_SUB_CTX             28
#define VIRGL_CCMD_CREATE_SUB_CTX          29
#define VIRGL_CCMD_DESTROY_SUB_CTX         30
#define VIRGL_CCMD_BIND_SHADER             31

/* -----------------------------------------------------------------------
 * Virgl object types
 * ----------------------------------------------------------------------- */
#define VIRGL_OBJECT_BLEND              1
#define VIRGL_OBJECT_RASTERIZER         2
#define VIRGL_OBJECT_DSA                3
#define VIRGL_OBJECT_SHADER             4
#define VIRGL_OBJECT_VERTEX_ELEMENTS    5
#define VIRGL_OBJECT_SAMPLER_VIEW       6
#define VIRGL_OBJECT_SAMPLER_STATE      7
#define VIRGL_OBJECT_SURFACE            8
#define VIRGL_OBJECT_QUERY              9
#define VIRGL_OBJECT_STREAMOUT_TARGET   10

/* -----------------------------------------------------------------------
 * Gallium PIPE_FORMAT subset (values match mesa/src/gallium/include/pipe/p_format.h)
 * These are the same integer values used by virglrenderer.
 * ----------------------------------------------------------------------- */
#define PIPE_FORMAT_NONE                    0
#define PIPE_FORMAT_B8G8R8A8_UNORM          1
#define PIPE_FORMAT_B8G8R8X8_UNORM          2
#define PIPE_FORMAT_A8R8G8B8_UNORM          3
#define PIPE_FORMAT_X8R8G8B8_UNORM          4
#define PIPE_FORMAT_B5G5R5A1_UNORM          5
#define PIPE_FORMAT_B4G4R4A4_UNORM          6
#define PIPE_FORMAT_B5G6R5_UNORM            7
#define PIPE_FORMAT_R10G10B10A2_UNORM       8
#define PIPE_FORMAT_Z16_UNORM               11
#define PIPE_FORMAT_Z32_UNORM               12
#define PIPE_FORMAT_Z32_FLOAT               13
#define PIPE_FORMAT_Z24_UNORM_S8_UINT       14
#define PIPE_FORMAT_Z24X8_UNORM             16
#define PIPE_FORMAT_S8_UINT                 18
#define PIPE_FORMAT_R32_FLOAT               28
#define PIPE_FORMAT_R32G32_FLOAT            29
#define PIPE_FORMAT_R32G32B32_FLOAT         30
#define PIPE_FORMAT_R32G32B32A32_FLOAT      31
#define PIPE_FORMAT_R8_UNORM                64
#define PIPE_FORMAT_R8G8B8A8_UNORM          67
#define PIPE_FORMAT_R8G8B8X8_UNORM          134

/* -----------------------------------------------------------------------
 * Gallium PIPE_TEXTURE targets
 * ----------------------------------------------------------------------- */
#define PIPE_BUFFER                 0
#define PIPE_TEXTURE_1D             1
#define PIPE_TEXTURE_2D             2
#define PIPE_TEXTURE_3D             3
#define PIPE_TEXTURE_CUBE           4
#define PIPE_TEXTURE_RECT           5

/* -----------------------------------------------------------------------
 * Gallium PIPE_BIND flags
 * ----------------------------------------------------------------------- */
#define PIPE_BIND_DEPTH_STENCIL     (1 << 0)
#define PIPE_BIND_RENDER_TARGET     (1 << 1)
#define PIPE_BIND_BLENDABLE         (1 << 2)
#define PIPE_BIND_SAMPLER_VIEW      (1 << 3)
#define PIPE_BIND_VERTEX_BUFFER     (1 << 4)
#define PIPE_BIND_INDEX_BUFFER      (1 << 5)
#define PIPE_BIND_CONSTANT_BUFFER   (1 << 6)
#define PIPE_BIND_DISPLAY_TARGET    (1 << 8)
#define PIPE_BIND_CUSTOM            (1 << 16)
#define PIPE_BIND_SCANOUT           (1 << 14)

/* -----------------------------------------------------------------------
 * Gallium PIPE_SHADER types
 * ----------------------------------------------------------------------- */
#define PIPE_SHADER_VERTEX      0
#define PIPE_SHADER_FRAGMENT    1
#define PIPE_SHADER_GEOMETRY    2

/* -----------------------------------------------------------------------
 * Gallium primitive types
 * ----------------------------------------------------------------------- */
#define PIPE_PRIM_POINTS            0
#define PIPE_PRIM_LINES             1
#define PIPE_PRIM_LINE_LOOP         2
#define PIPE_PRIM_LINE_STRIP        3
#define PIPE_PRIM_TRIANGLES         4
#define PIPE_PRIM_TRIANGLE_STRIP    5
#define PIPE_PRIM_TRIANGLE_FAN      6
#define PIPE_PRIM_QUADS             7
#define PIPE_PRIM_QUAD_STRIP        8
#define PIPE_PRIM_POLYGON           9

/* -----------------------------------------------------------------------
 * Gallium PIPE_CLEAR flags
 * ----------------------------------------------------------------------- */
#define PIPE_CLEAR_DEPTH            (1 << 0)
#define PIPE_CLEAR_STENCIL          (1 << 1)
#define PIPE_CLEAR_COLOR0           (1 << 2)
#define PIPE_CLEAR_COLOR1           (1 << 3)
#define PIPE_CLEAR_COLOR2           (1 << 4)
#define PIPE_CLEAR_COLOR3           (1 << 5)

/* -----------------------------------------------------------------------
 * Gallium blend factors and functions
 * ----------------------------------------------------------------------- */
#define PIPE_BLEND_ADD                  0
#define PIPE_BLEND_SUBTRACT             1
#define PIPE_BLEND_REVERSE_SUBTRACT     2
#define PIPE_BLEND_MIN                  3
#define PIPE_BLEND_MAX                  4

#define PIPE_BLENDFACTOR_ONE                0x01
#define PIPE_BLENDFACTOR_SRC_COLOR          0x02
#define PIPE_BLENDFACTOR_SRC_ALPHA          0x03
#define PIPE_BLENDFACTOR_DST_ALPHA          0x04
#define PIPE_BLENDFACTOR_DST_COLOR          0x05
#define PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE 0x06
#define PIPE_BLENDFACTOR_CONST_COLOR        0x07
#define PIPE_BLENDFACTOR_CONST_ALPHA        0x08
#define PIPE_BLENDFACTOR_ZERO               0x11
#define PIPE_BLENDFACTOR_INV_SRC_COLOR      0x12
#define PIPE_BLENDFACTOR_INV_SRC_ALPHA      0x13
#define PIPE_BLENDFACTOR_INV_DST_ALPHA      0x14
#define PIPE_BLENDFACTOR_INV_DST_COLOR      0x15
#define PIPE_BLENDFACTOR_INV_CONST_COLOR    0x17
#define PIPE_BLENDFACTOR_INV_CONST_ALPHA    0x18

/* -----------------------------------------------------------------------
 * Gallium polygon / face / depth function constants
 * ----------------------------------------------------------------------- */
#define PIPE_POLYGON_MODE_FILL      0
#define PIPE_POLYGON_MODE_LINE      1
#define PIPE_POLYGON_MODE_POINT     2

#define PIPE_FACE_NONE              0
#define PIPE_FACE_FRONT             1
#define PIPE_FACE_BACK              2
#define PIPE_FACE_FRONT_AND_BACK    3

#define PIPE_FUNC_NEVER     0
#define PIPE_FUNC_LESS      1
#define PIPE_FUNC_EQUAL     2
#define PIPE_FUNC_LEQUAL    3
#define PIPE_FUNC_GREATER   4
#define PIPE_FUNC_NOTEQUAL  5
#define PIPE_FUNC_GEQUAL    6
#define PIPE_FUNC_ALWAYS    7

/* -----------------------------------------------------------------------
 * Blend per-RT state encoding (virgl_protocol.h S2 macros)
 * ----------------------------------------------------------------------- */
#define VIRGL_BLEND_RT_BLEND_ENABLE(x)      ((x) & 0x1)
#define VIRGL_BLEND_RT_RGB_FUNC(x)          (((x) & 0x7)  << 1)
#define VIRGL_BLEND_RT_RGB_SRC_FACTOR(x)    (((x) & 0x1f) << 4)
#define VIRGL_BLEND_RT_RGB_DST_FACTOR(x)    (((x) & 0x1f) << 9)
#define VIRGL_BLEND_RT_ALPHA_FUNC(x)        (((x) & 0x7)  << 14)
#define VIRGL_BLEND_RT_ALPHA_SRC_FACTOR(x)  (((x) & 0x1f) << 17)
#define VIRGL_BLEND_RT_ALPHA_DST_FACTOR(x)  (((x) & 0x1f) << 22)
#define VIRGL_BLEND_RT_COLORMASK(x)         (((x) & 0xf)  << 27)

/* Convenience: opaque blend (no blending, write RGBA) */
#define VIRGL_BLEND_RT_OPAQUE   VIRGL_BLEND_RT_COLORMASK(0xF)

/* Convenience: standard alpha blend (SrcAlpha, 1-SrcAlpha) */
#define VIRGL_BLEND_RT_ALPHA \
    (VIRGL_BLEND_RT_BLEND_ENABLE(1) | \
     VIRGL_BLEND_RT_RGB_FUNC(PIPE_BLEND_ADD) | \
     VIRGL_BLEND_RT_RGB_SRC_FACTOR(PIPE_BLENDFACTOR_SRC_ALPHA) | \
     VIRGL_BLEND_RT_RGB_DST_FACTOR(PIPE_BLENDFACTOR_INV_SRC_ALPHA) | \
     VIRGL_BLEND_RT_ALPHA_FUNC(PIPE_BLEND_ADD) | \
     VIRGL_BLEND_RT_ALPHA_SRC_FACTOR(PIPE_BLENDFACTOR_ONE) | \
     VIRGL_BLEND_RT_ALPHA_DST_FACTOR(PIPE_BLENDFACTOR_INV_SRC_ALPHA) | \
     VIRGL_BLEND_RT_COLORMASK(0xF))

/* -----------------------------------------------------------------------
 * DSA S0 encoding
 * ----------------------------------------------------------------------- */
#define VIRGL_DSA_S0_DEPTH_ENABLE(x)    ((x) & 0x1)
#define VIRGL_DSA_S0_DEPTH_WRITEMASK(x) (((x) & 0x1) << 1)
#define VIRGL_DSA_S0_DEPTH_FUNC(x)      (((x) & 0x7) << 2)
#define VIRGL_DSA_S0_ALPHA_ENABLE(x)    (((x) & 0x1) << 8)
#define VIRGL_DSA_S0_ALPHA_FUNC(x)      (((x) & 0x7) << 9)

/* -----------------------------------------------------------------------
 * Rasterizer S0/S3 encoding -- matches virglrenderer vrend_decode.c layout.
 *
 * S0 bit layout:
 *   0     flatshade
 *   1     depth_clip_near (and far)
 *   2     clip_halfz
 *   3     rasterizer_discard          <- DO NOT confuse with scissor!
 *   4     flatshade_first
 *   5     light_twoside
 *   6     sprite_coord_mode
 *   7     point_quad_rasterization
 *   8-9   cull_face (2 bits)
 *   10-11 fill_front (2 bits)
 *   12-13 fill_back (2 bits)
 *   14    scissor
 *   15    front_ccw
 *   25    multisample
 *   31    force_persample_interp
 *
 * S3 = line_stipple_pattern[0:15] | line_stipple_factor[16:23] |
 *      clip_plane_enable[24:31]
 * ----------------------------------------------------------------------- */
#define VIRGL_RS_S0_FLATSHADE(x)        ((x) & 0x1)
#define VIRGL_RS_S0_DEPTH_CLIP(x)       (((x) & 0x1) << 1)
#define VIRGL_RS_S0_CLIP_HALFZ(x)       (((x) & 0x1) << 2)
#define VIRGL_RS_S0_RAST_DISCARD(x)     (((x) & 0x1) << 3)
#define VIRGL_RS_S0_CULL_FACE(x)        (((x) & 0x3) << 8)
#define VIRGL_RS_S0_FILL_FRONT(x)       (((x) & 0x3) << 10)
#define VIRGL_RS_S0_FILL_BACK(x)        (((x) & 0x3) << 12)
#define VIRGL_RS_S0_SCISSOR(x)          (((x) & 0x1) << 14)
#define VIRGL_RS_S0_MULTISAMPLE(x)      (((x) & 0x1) << 25)
#define VIRGL_RS_S0_FORCE_PERSAMPLE(x)  (((x) & 0x1) << 31)

/* -----------------------------------------------------------------------
 * Blit S0 encoding
 * ----------------------------------------------------------------------- */
#define VIRGL_BLIT_S0_MASK(x)           ((x) & 0xff)
#define VIRGL_BLIT_S0_FILTER(x)         (((x) & 0x3) << 8)
#define VIRGL_BLIT_S0_SCISSOR_EN(x)     (((x) & 0x1) << 10)

#define VIRGL_BLIT_S1_DST_LEVEL(x)      ((x) & 0xff)
#define VIRGL_BLIT_S1_DST_FORMAT(x)     (((x) & 0xffff) << 8)

/* PIPE_MASK for blit */
#define PIPE_MASK_R     (1 << 0)
#define PIPE_MASK_G     (1 << 1)
#define PIPE_MASK_B     (1 << 2)
#define PIPE_MASK_A     (1 << 3)
#define PIPE_MASK_RGBA  (PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_B | PIPE_MASK_A)
#define PIPE_MASK_RGBZ  (PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_B)  /* no alpha -- for X8 formats */
#define PIPE_MASK_Z     (1 << 4)
#define PIPE_MASK_S     (1 << 5)

/* PIPE_TEX_FILTER for blit and sampler */
#define PIPE_TEX_FILTER_NEAREST     0
#define PIPE_TEX_FILTER_LINEAR      1

/* PIPE_TEX_MIPFILTER for sampler */
#define PIPE_TEX_MIPFILTER_NEAREST  0
#define PIPE_TEX_MIPFILTER_LINEAR   1
#define PIPE_TEX_MIPFILTER_NONE     2

/* PIPE_TEX_WRAP for sampler */
#define PIPE_TEX_WRAP_REPEAT            0
#define PIPE_TEX_WRAP_CLAMP             1
#define PIPE_TEX_WRAP_CLAMP_TO_EDGE     2
#define PIPE_TEX_WRAP_CLAMP_TO_BORDER   3
#define PIPE_TEX_WRAP_MIRROR_REPEAT     4

/* PIPE_SWIZZLE for sampler view */
#define PIPE_SWIZZLE_RED    0
#define PIPE_SWIZZLE_GREEN  1
#define PIPE_SWIZZLE_BLUE   2
#define PIPE_SWIZZLE_ALPHA  3
#define PIPE_SWIZZLE_ZERO   4
#define PIPE_SWIZZLE_ONE    5

/* -----------------------------------------------------------------------
 * Sampler state S0 encoding (virgl_protocol.h)
 * ----------------------------------------------------------------------- */
#define VIRGL_SAMPLER_S0_WRAP_S(x)          ((x) & 0x7)
#define VIRGL_SAMPLER_S0_WRAP_T(x)          (((x) & 0x7) << 3)
#define VIRGL_SAMPLER_S0_WRAP_R(x)          (((x) & 0x7) << 6)
#define VIRGL_SAMPLER_S0_MIN_IMG_FILTER(x)  (((x) & 0x3) << 9)
#define VIRGL_SAMPLER_S0_MIN_MIP_FILTER(x)  (((x) & 0x3) << 11)
#define VIRGL_SAMPLER_S0_MAG_IMG_FILTER(x)  (((x) & 0x3) << 13)
#define VIRGL_SAMPLER_S0_COMPARE_MODE(x)    (((x) & 0x1) << 15)
#define VIRGL_SAMPLER_S0_COMPARE_FUNC(x)    (((x) & 0x7) << 16)
#define VIRGL_SAMPLER_S0_SEAMLESS(x)        (((x) & 0x1) << 19)

/* Max color buffers (matches virglrenderer VIRGL_MAX_COLOR_BUFS) */
#define VIRGL_MAX_COLOR_BUFS    8

/* -----------------------------------------------------------------------
 * VirglCmdBuf -- command buffer builder.
 *
 * Wraps a caller-provided uint32 array. Commands are appended via
 * virgl_emit_dword, then the entire buffer is submitted via
 * virgl_submit() -> chip_Submit3D().
 * ----------------------------------------------------------------------- */
struct VirglCmdBuf {
    uint32 *buf;            /* pointer to uint32 array */
    uint32  dwords;         /* current write position (in uint32 words) */
    uint32  max_dwords;     /* capacity (in uint32 words) */
};

/* -----------------------------------------------------------------------
 * Command header macro.
 *
 * Virgl command header = cmd_id | (object_type << 8) | (payload_len << 16)
 * payload_len = number of uint32 words AFTER the header word.
 * ----------------------------------------------------------------------- */
#define VIRGL_CMD_HDR(cmd, obj, length) \
    ((uint32)(cmd) | ((uint32)(obj) << 8) | ((uint32)(length) << 16))

/* -----------------------------------------------------------------------
 * Inline emit helpers
 * ----------------------------------------------------------------------- */

/* Emit a uint32 value, GP32-swapping it from CPU (BE) to LE wire format. */
static inline void virgl_emit_dword(struct VirglCmdBuf *cbuf, uint32 val)
{
    if (cbuf->dwords < cbuf->max_dwords)
        cbuf->buf[cbuf->dwords++] = GP32(val);
}

/* Emit a float as its IEEE 754 bit pattern, GP32-swapped. */
static inline void virgl_emit_float(struct VirglCmdBuf *cbuf, float val)
{
    union { float f; uint32 u; } conv;
    conv.f = val;
    virgl_emit_dword(cbuf, conv.u);
}

/* Emit a double as two LE uint32 words (low first, then high). */
static inline void virgl_emit_double(struct VirglCmdBuf *cbuf, double val)
{
    union { double d; uint64 u; } conv;
    conv.d = val;
    virgl_emit_dword(cbuf, (uint32)(conv.u & 0xFFFFFFFF));
    virgl_emit_dword(cbuf, (uint32)(conv.u >> 32));
}

/* -----------------------------------------------------------------------
 * Vertex element / vertex buffer descriptors for batch encoding.
 * ----------------------------------------------------------------------- */
struct VirglVertexElement {
    uint32 src_offset;
    uint32 instance_divisor;
    uint32 vertex_buffer_index;
    uint32 src_format;      /* PIPE_FORMAT_* */
};

struct VirglVertexBuffer {
    uint32 stride;
    uint32 buffer_offset;
    uint32 res_handle;
};

/* -----------------------------------------------------------------------
 * Function declarations -- implemented in chip_virgl.c
 * ----------------------------------------------------------------------- */

/* Buffer management */
void virgl_cmd_init(struct VirglCmdBuf *cbuf, uint32 *buf, uint32 max_dwords);
void virgl_cmd_reset(struct VirglCmdBuf *cbuf);
BOOL virgl_submit(struct ChipGPUState *gs, uint32 ctx_id, struct VirglCmdBuf *cbuf);

/* Sub-context (must be first commands in a new 3D context) */
void virgl_cmd_create_sub_ctx(struct VirglCmdBuf *cbuf, uint32 sub_ctx_id);
void virgl_cmd_set_sub_ctx(struct VirglCmdBuf *cbuf, uint32 sub_ctx_id);

/* Object creation */
void virgl_cmd_create_surface(struct VirglCmdBuf *cbuf, uint32 handle,
                               uint32 res_handle, uint32 format,
                               uint32 first_element, uint32 last_element);
void virgl_cmd_create_blend(struct VirglCmdBuf *cbuf, uint32 handle,
                             uint32 s0, uint32 rt0_blend);
void virgl_cmd_create_dsa(struct VirglCmdBuf *cbuf, uint32 handle,
                           uint32 s0, uint32 s1_front, uint32 s2_back,
                           float alpha_ref);
void virgl_cmd_create_rasterizer(struct VirglCmdBuf *cbuf, uint32 handle,
                                  uint32 s0, float point_size,
                                  uint32 sprite_coord_enable, uint32 s3,
                                  float line_width, float offset_units,
                                  float offset_scale, float offset_clamp);
void virgl_cmd_create_shader(struct VirglCmdBuf *cbuf, uint32 handle,
                              uint32 shader_type, const char *tgsi_text);
void virgl_cmd_create_vertex_elements(struct VirglCmdBuf *cbuf, uint32 handle,
                                       uint32 num_elements,
                                       const struct VirglVertexElement *elements);

/* Object binding */
void virgl_cmd_bind_object(struct VirglCmdBuf *cbuf, uint32 obj_type,
                            uint32 handle);
void virgl_cmd_bind_shader(struct VirglCmdBuf *cbuf, uint32 shader_type,
                            uint32 handle);

/* Pipeline state */
void virgl_cmd_set_viewport(struct VirglCmdBuf *cbuf, uint32 start_slot,
                             float scale_x, float scale_y, float scale_z,
                             float trans_x, float trans_y, float trans_z);
void virgl_cmd_set_framebuffer_state(struct VirglCmdBuf *cbuf,
                                      uint32 nr_cbufs, uint32 zsurf_handle,
                                      const uint32 *surf_handles);
void virgl_cmd_set_vertex_buffers(struct VirglCmdBuf *cbuf, uint32 num_buffers,
                                   const struct VirglVertexBuffer *buffers);
void virgl_cmd_set_scissor_state(struct VirglCmdBuf *cbuf, uint32 start_slot,
                                  uint32 minx, uint32 miny,
                                  uint32 maxx, uint32 maxy);

/* Drawing */
void virgl_cmd_clear(struct VirglCmdBuf *cbuf, uint32 buffers,
                      float r, float g, float b, float a,
                      double depth, uint32 stencil);
void virgl_cmd_draw_vbo(struct VirglCmdBuf *cbuf,
                         uint32 start, uint32 count, uint32 mode,
                         uint32 indexed, uint32 instance_count,
                         int32 index_bias, uint32 start_instance,
                         uint32 primitive_restart, uint32 restart_index,
                         uint32 min_index, uint32 max_index,
                         uint32 cso_handle);
void virgl_cmd_resource_inline_write(struct VirglCmdBuf *cbuf,
                                      uint32 res_handle, uint32 level,
                                      uint32 usage, uint32 stride,
                                      uint32 layer_stride,
                                      uint32 x, uint32 y, uint32 z,
                                      uint32 w, uint32 h, uint32 d,
                                      const void *data, uint32 data_size);
void virgl_cmd_blit(struct VirglCmdBuf *cbuf,
                     uint32 s0_flags,
                     uint32 dst_res, uint32 dst_format, uint32 dst_level,
                     uint32 dst_x1, uint32 dst_y1, uint32 dst_z1,
                     uint32 dst_x2, uint32 dst_y2, uint32 dst_z2,
                     uint32 src_res, uint32 src_format, uint32 src_level,
                     uint32 src_x1, uint32 src_y1, uint32 src_z1,
                     uint32 src_x2, uint32 src_y2, uint32 src_z2);

/* Object destruction */
void virgl_cmd_destroy_object(struct VirglCmdBuf *cbuf, uint32 obj_type,
                               uint32 handle);

/* Convenience: create default pipeline objects */
void virgl_setup_default_blend(struct VirglCmdBuf *cbuf, uint32 handle);
void virgl_setup_alpha_blend(struct VirglCmdBuf *cbuf, uint32 handle);
void virgl_setup_default_dsa(struct VirglCmdBuf *cbuf, uint32 handle);
void virgl_setup_default_rasterizer(struct VirglCmdBuf *cbuf, uint32 handle);

/* Convenience: create simple TGSI shaders */
void virgl_setup_passthrough_vs(struct VirglCmdBuf *cbuf, uint32 handle);
void virgl_setup_color_fs(struct VirglCmdBuf *cbuf, uint32 handle);
void virgl_setup_texture_fs(struct VirglCmdBuf *cbuf, uint32 handle);

/* Sampler state + view */
void virgl_cmd_create_sampler_state(struct VirglCmdBuf *cbuf, uint32 handle,
                                      uint32 s0, float lod_bias,
                                      float min_lod, float max_lod,
                                      float border_r, float border_g,
                                      float border_b, float border_a);
void virgl_cmd_create_sampler_view(struct VirglCmdBuf *cbuf, uint32 handle,
                                     uint32 res_handle, uint32 format,
                                     uint32 first_element, uint32 last_element,
                                     uint32 swizzle_r, uint32 swizzle_g,
                                     uint32 swizzle_b, uint32 swizzle_a);
void virgl_cmd_set_sampler_views(struct VirglCmdBuf *cbuf,
                                   uint32 shader_type, uint32 start_slot,
                                   uint32 count, const uint32 *view_handles);
void virgl_cmd_bind_sampler_states(struct VirglCmdBuf *cbuf,
                                     uint32 shader_type, uint32 start_slot,
                                     uint32 count, const uint32 *state_handles);

/* Vertex upload -- uploads float data via INLINE_WRITE with GP32 swap.
 * Implemented in chip_virgl_2d.c, used by chip_composite.c too. */
void virgl_upload_vertex_floats(struct VirglCmdBuf *cbuf, uint32 res_handle,
                                  const float *data, uint32 num_floats);

#endif /* VIRGL_CMD_H */
