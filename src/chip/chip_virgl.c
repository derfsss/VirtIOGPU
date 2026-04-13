/*
 * chip_virgl.c -- Virgl 3D command stream encoder.
 *
 * Builds Virgl gallium command buffers for submission via chip_Submit3D().
 * Each command = header word + payload words, all GP32-swapped for LE wire
 * format.  The command buffer is a plain uint32 array provided by the caller.
 *
 * Reference: mesa/src/gallium/drivers/virgl/virgl_encode.c
 *            virglrenderer/src/vrend_decode.c
 */

#include "chip/chip_state.h"
#include "virgl/virgl_cmd.h"

/* -----------------------------------------------------------------------
 * Buffer management
 * ----------------------------------------------------------------------- */

void virgl_cmd_init(struct VirglCmdBuf *cbuf, uint32 *buf, uint32 max_dwords)
{
    cbuf->buf        = buf;
    cbuf->dwords     = 0;
    cbuf->max_dwords = max_dwords;
}

void virgl_cmd_reset(struct VirglCmdBuf *cbuf)
{
    cbuf->dwords = 0;
}

/* Submit the accumulated command buffer via chip_Submit3D().
 * Returns TRUE on success.  Resets the command buffer afterwards. */
BOOL virgl_submit(struct ChipGPUState *gs, uint32 ctx_id, struct VirglCmdBuf *cbuf)
{
    if (cbuf->dwords == 0) return TRUE;  /* nothing to submit */

    /* If context is already poisoned, don't bother submitting */
    if (gs->virgl_ctx_error) {
        cbuf->dwords = 0;
        return FALSE;
    }

    BOOL ok = chip_Submit3D(gs, ctx_id, cbuf->buf, cbuf->dwords * 4);
    cbuf->dwords = 0;

    if (!ok) {
        /* Mark context as poisoned -- recovery needed */
        gs->virgl_ctx_error = TRUE;
        DCHIP("virgl_submit: FAILED -- context %lu poisoned, recovery needed",
              (unsigned long)ctx_id);
    }
    return ok;
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* String length (no libc dependency) */
static uint32 virgl_strlen(const char *s)
{
    uint32 n = 0;
    while (s[n]) n++;
    return n;
}

/* Pack 4 text bytes into a LE uint32: b[0] in bits 0-7, b[1] in 8-15, etc.
 * On PPC this produces a CPU-endian uint32 that, after GP32, stores bytes
 * in LE order -- correct for x86 host to read as a char sequence. */
static uint32 virgl_pack_text_word(const char *text, uint32 offset, uint32 len)
{
    uint32 val = 0;
    for (uint32 i = 0; i < 4 && (offset + i) < len; i++)
        val |= ((uint32)(uint8)text[offset + i]) << (i * 8);
    return val;
}

/* -----------------------------------------------------------------------
 * Sub-context commands
 *
 * virglrenderer requires CREATE_SUB_CTX + SET_SUB_CTX as the very first
 * commands after CTX_CREATE.  Without them, all rendering state is NULL.
 * ----------------------------------------------------------------------- */

void virgl_cmd_create_sub_ctx(struct VirglCmdBuf *cbuf, uint32 sub_ctx_id)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_SUB_CTX, 0, 1));
    virgl_emit_dword(cbuf, sub_ctx_id);
}

void virgl_cmd_set_sub_ctx(struct VirglCmdBuf *cbuf, uint32 sub_ctx_id)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_SET_SUB_CTX, 0, 1));
    virgl_emit_dword(cbuf, sub_ctx_id);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Surface
 *
 * Wraps a 3D resource for use as framebuffer color/depth attachment.
 * Payload: handle, res_handle, format, first_element, last_element = 5 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_surface(struct VirglCmdBuf *cbuf, uint32 handle,
                               uint32 res_handle, uint32 format,
                               uint32 first_element, uint32 last_element)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_SURFACE, 5));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, res_handle);
    virgl_emit_dword(cbuf, format);
    virgl_emit_dword(cbuf, first_element);
    virgl_emit_dword(cbuf, last_element);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Blend
 *
 * Payload: handle, S0 (flags), S1 (logicop_func), 8x per-RT blend = 11 words.
 * Only RT0 is configurable; RT1-7 default to no-blend.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_blend(struct VirglCmdBuf *cbuf, uint32 handle,
                             uint32 s0, uint32 rt0_blend)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_BLEND, 11));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, s0);     /* S0: independent_blend, logicop, etc. */
    virgl_emit_dword(cbuf, 0);      /* S1: logicop_func */
    virgl_emit_dword(cbuf, rt0_blend);  /* per-RT[0] */
    for (uint32 i = 1; i < VIRGL_MAX_COLOR_BUFS; i++)
        virgl_emit_dword(cbuf, 0);      /* per-RT[1..7] */
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: DSA (Depth/Stencil/Alpha)
 *
 * Payload: handle, S0, S1 (front stencil), S2 (back stencil),
 *          alpha_ref (float) = 5 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_dsa(struct VirglCmdBuf *cbuf, uint32 handle,
                           uint32 s0, uint32 s1_front, uint32 s2_back,
                           float alpha_ref)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_DSA, 5));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, s0);
    virgl_emit_dword(cbuf, s1_front);
    virgl_emit_dword(cbuf, s2_back);
    virgl_emit_float(cbuf, alpha_ref);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Rasterizer
 *
 * Payload: handle, S0, point_size, sprite_coord_enable, S3,
 *          line_width, offset_units, offset_scale, offset_clamp = 9 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_rasterizer(struct VirglCmdBuf *cbuf, uint32 handle,
                                  uint32 s0, float point_size,
                                  uint32 sprite_coord_enable, uint32 s3,
                                  float line_width, float offset_units,
                                  float offset_scale, float offset_clamp)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_RASTERIZER, 9));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, s0);
    virgl_emit_float(cbuf, point_size);
    virgl_emit_dword(cbuf, sprite_coord_enable);
    virgl_emit_dword(cbuf, s3);
    virgl_emit_float(cbuf, line_width);
    virgl_emit_float(cbuf, offset_units);
    virgl_emit_float(cbuf, offset_scale);
    virgl_emit_float(cbuf, offset_clamp);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Shader
 *
 * Encodes a TGSI text shader.  The text is packed into LE uint32 words
 * following the shader metadata.
 *
 * Layout:
 *   word[1] = handle
 *   word[2] = shader_type (PIPE_SHADER_VERTEX etc.)
 *   word[3] = (1 << 31) | text_len   (first-packet flag | byte length)
 *   word[4] = num_tokens (0 for text-based shaders)
 *   word[5] = so_count   (0 = no stream output)
 *   word[6..N] = TGSI text packed as LE uint32
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_shader(struct VirglCmdBuf *cbuf, uint32 handle,
                              uint32 shader_type, const char *tgsi_text)
{
    uint32 text_len = virgl_strlen(tgsi_text);
    /* Include NUL terminator in the packed text */
    uint32 text_with_nul = text_len + 1;
    uint32 text_words = (text_with_nul + 3) / 4;
    uint32 payload_len = 5 + text_words;  /* handle + type + offset + tokens + so + text */

    uint32 start_pos = cbuf->dwords;

    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_SHADER, payload_len));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, shader_type);
    /* VIRGL_OBJ_SHADER_OFFSET encoding (virglrenderer 1.1.1):
     *   Bit 31 (VIRGL_OBJ_SHADER_OFFSET_CONT) = CONTINUATION flag.
     *     Bit 31 CLEAR = new shader (first packet).
     *     Bit 31 SET   = continuation of previous long shader.
     *   Low 31 bits:
     *     For new shader:    total byte length of TGSI text (incl NUL).
     *     For continuation:  byte offset into shader text for this packet.
     *
     * Validation: expected_token_count = (offlen + 3) / 4 must be >= pkt_length.
     * pkt_length = text_words = (text_with_nul + 3) / 4.
     * offlen = text_with_nul -> (text_with_nul + 3) / 4 >= text_words. ✓ */
    virgl_emit_dword(cbuf, text_with_nul);  /* new shader: total byte length incl NUL, bit 31 clear */
    /* num_tokens: virglrenderer allocates (num_tokens + 10) tgsi_token slots
     * for tgsi_text_translate().  With 0, only 10 slots -- far too few for
     * any real shader.  Use text_words as a safe estimate (binary TGSI is
     * always smaller than the text representation). */
    virgl_emit_dword(cbuf, text_words);  /* num_tokens estimate for allocation */
    virgl_emit_dword(cbuf, 0);  /* so_count = 0 (no stream output) */

    /* Pack TGSI text bytes into LE uint32 words */
    for (uint32 off = 0; off < text_with_nul; off += 4) {
        uint32 word = virgl_pack_text_word(tgsi_text, off, text_with_nul);
        virgl_emit_dword(cbuf, word);
    }

    /* Debug: dump first 6 words (header + metadata) as stored in buffer (LE) */
    DCHIP("CREATE_SHADER h=%lu type=%lu len=%lu tlen=%lu tw=%lu start=%lu end=%lu",
          handle, shader_type, payload_len, text_len, text_words,
          start_pos, cbuf->dwords);
    DCHIP("  buf[0..5]=%08lx %08lx %08lx %08lx %08lx %08lx",
          cbuf->buf[start_pos+0], cbuf->buf[start_pos+1],
          cbuf->buf[start_pos+2], cbuf->buf[start_pos+3],
          cbuf->buf[start_pos+4], cbuf->buf[start_pos+5]);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Vertex Elements
 *
 * Each element = 4 words: src_offset, instance_divisor, vertex_buffer_index,
 *                          src_format.
 * Payload: handle + num_elements * 4.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_vertex_elements(struct VirglCmdBuf *cbuf, uint32 handle,
                                       uint32 num_elements,
                                       const struct VirglVertexElement *elements)
{
    uint32 payload_len = 1 + num_elements * 4;
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_VERTEX_ELEMENTS,
                                          payload_len));
    virgl_emit_dword(cbuf, handle);
    for (uint32 i = 0; i < num_elements; i++) {
        virgl_emit_dword(cbuf, elements[i].src_offset);
        virgl_emit_dword(cbuf, elements[i].instance_divisor);
        virgl_emit_dword(cbuf, elements[i].vertex_buffer_index);
        virgl_emit_dword(cbuf, elements[i].src_format);
    }
}

/* -----------------------------------------------------------------------
 * BIND_OBJECT -- bind a blend/dsa/rasterizer/vertex_elements/surface object.
 * ----------------------------------------------------------------------- */
void virgl_cmd_bind_object(struct VirglCmdBuf *cbuf, uint32 obj_type,
                            uint32 handle)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT,
                                          obj_type, 1));
    virgl_emit_dword(cbuf, handle);
}

/* -----------------------------------------------------------------------
 * BIND_SHADER -- bind a vertex or fragment shader.
 * Payload: handle, shader_type = 2 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_bind_shader(struct VirglCmdBuf *cbuf, uint32 shader_type,
                            uint32 handle)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_BIND_SHADER, 0, 2));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, shader_type);
}

/* -----------------------------------------------------------------------
 * SET_VIEWPORT_STATE -- set viewport transform for one slot.
 * Payload: start_slot, scale[3], translate[3] = 7 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_set_viewport(struct VirglCmdBuf *cbuf, uint32 start_slot,
                             float scale_x, float scale_y, float scale_z,
                             float trans_x, float trans_y, float trans_z)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    virgl_emit_dword(cbuf, start_slot);
    virgl_emit_float(cbuf, scale_x);
    virgl_emit_float(cbuf, scale_y);
    virgl_emit_float(cbuf, scale_z);
    virgl_emit_float(cbuf, trans_x);
    virgl_emit_float(cbuf, trans_y);
    virgl_emit_float(cbuf, trans_z);
}

/* -----------------------------------------------------------------------
 * SET_FRAMEBUFFER_STATE -- bind color + depth surfaces.
 * Payload: nr_cbufs, zsurf_handle, surf_handles[nr_cbufs].
 * ----------------------------------------------------------------------- */
void virgl_cmd_set_framebuffer_state(struct VirglCmdBuf *cbuf,
                                      uint32 nr_cbufs, uint32 zsurf_handle,
                                      const uint32 *surf_handles)
{
    uint32 payload_len = 2 + nr_cbufs;
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_SET_FRAMEBUFFER_STATE,
                                          0, payload_len));
    virgl_emit_dword(cbuf, nr_cbufs);
    virgl_emit_dword(cbuf, zsurf_handle);
    for (uint32 i = 0; i < nr_cbufs; i++)
        virgl_emit_dword(cbuf, surf_handles[i]);
}

/* -----------------------------------------------------------------------
 * SET_VERTEX_BUFFERS -- bind vertex buffers.
 * Each buffer = 3 words: stride, buffer_offset, res_handle.
 * ----------------------------------------------------------------------- */
void virgl_cmd_set_vertex_buffers(struct VirglCmdBuf *cbuf, uint32 num_buffers,
                                   const struct VirglVertexBuffer *buffers)
{
    uint32 payload_len = num_buffers * 3;
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_SET_VERTEX_BUFFERS,
                                          0, payload_len));
    for (uint32 i = 0; i < num_buffers; i++) {
        virgl_emit_dword(cbuf, buffers[i].stride);
        virgl_emit_dword(cbuf, buffers[i].buffer_offset);
        virgl_emit_dword(cbuf, buffers[i].res_handle);
    }
}

/* -----------------------------------------------------------------------
 * SET_SCISSOR_STATE -- set scissor rect for one slot.
 * Payload: start_slot, (minx | miny<<16), (maxx | maxy<<16) = 3 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_set_scissor_state(struct VirglCmdBuf *cbuf, uint32 start_slot,
                                  uint32 minx, uint32 miny,
                                  uint32 maxx, uint32 maxy)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_SET_SCISSOR_STATE, 0, 3));
    virgl_emit_dword(cbuf, start_slot);
    virgl_emit_dword(cbuf, (minx & 0xFFFF) | (miny << 16));
    virgl_emit_dword(cbuf, (maxx & 0xFFFF) | (maxy << 16));
}

/* -----------------------------------------------------------------------
 * CLEAR -- clear framebuffer attachments.
 *
 * Payload: buffers, color[4] (float), depth (double = 2 words),
 *          stencil = 8 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_clear(struct VirglCmdBuf *cbuf, uint32 buffers,
                      float r, float g, float b, float a,
                      double depth, uint32 stencil)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CLEAR, 0, 8));
    virgl_emit_dword(cbuf, buffers);
    virgl_emit_float(cbuf, r);
    virgl_emit_float(cbuf, g);
    virgl_emit_float(cbuf, b);
    virgl_emit_float(cbuf, a);
    virgl_emit_double(cbuf, depth);
    virgl_emit_dword(cbuf, stencil);
}

/* -----------------------------------------------------------------------
 * DRAW_VBO -- draw primitives.
 * Payload: 12 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_draw_vbo(struct VirglCmdBuf *cbuf,
                         uint32 start, uint32 count, uint32 mode,
                         uint32 indexed, uint32 instance_count,
                         int32 index_bias, uint32 start_instance,
                         uint32 primitive_restart, uint32 restart_index,
                         uint32 min_index, uint32 max_index,
                         uint32 cso_handle)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_DRAW_VBO, 0, 12));
    virgl_emit_dword(cbuf, start);
    virgl_emit_dword(cbuf, count);
    virgl_emit_dword(cbuf, mode);
    virgl_emit_dword(cbuf, indexed);
    virgl_emit_dword(cbuf, instance_count);
    virgl_emit_dword(cbuf, (uint32)index_bias);
    virgl_emit_dword(cbuf, start_instance);
    virgl_emit_dword(cbuf, primitive_restart);
    virgl_emit_dword(cbuf, restart_index);
    virgl_emit_dword(cbuf, min_index);
    virgl_emit_dword(cbuf, max_index);
    virgl_emit_dword(cbuf, cso_handle);
}

/* -----------------------------------------------------------------------
 * RESOURCE_INLINE_WRITE -- upload data directly into a 3D resource.
 *
 * The data payload follows the box coordinates in the command stream.
 * Data is copied as raw bytes packed into LE uint32 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_resource_inline_write(struct VirglCmdBuf *cbuf,
                                      uint32 res_handle, uint32 level,
                                      uint32 usage, uint32 stride,
                                      uint32 layer_stride,
                                      uint32 x, uint32 y, uint32 z,
                                      uint32 w, uint32 h, uint32 d,
                                      const void *data, uint32 data_size)
{
    uint32 data_words = (data_size + 3) / 4;
    uint32 payload_len = 11 + data_words;

    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE,
                                          0, payload_len));
    virgl_emit_dword(cbuf, res_handle);
    virgl_emit_dword(cbuf, level);
    virgl_emit_dword(cbuf, usage);
    virgl_emit_dword(cbuf, stride);
    virgl_emit_dword(cbuf, layer_stride);
    virgl_emit_dword(cbuf, x);
    virgl_emit_dword(cbuf, y);
    virgl_emit_dword(cbuf, z);
    virgl_emit_dword(cbuf, w);
    virgl_emit_dword(cbuf, h);
    virgl_emit_dword(cbuf, d);

    /* Pack data bytes as LE uint32 words.
     * Data content is opaque (vertex data, texture pixels) -- the host
     * interprets it according to the resource format. */
    const uint8 *src = (const uint8 *)data;
    for (uint32 off = 0; off < data_size; off += 4) {
        uint32 val = 0;
        for (uint32 i = 0; i < 4 && (off + i) < data_size; i++)
            val |= ((uint32)src[off + i]) << (i * 8);
        virgl_emit_dword(cbuf, val);
    }
}

/* -----------------------------------------------------------------------
 * BLIT -- GPU-accelerated copy/scale/format-convert between resources.
 *
 * This is the primary mechanism for 2D blitting via the 3D pipeline.
 * Payload: 17 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_blit(struct VirglCmdBuf *cbuf,
                     uint32 s0_flags,
                     uint32 dst_res, uint32 dst_format, uint32 dst_level,
                     uint32 dst_x1, uint32 dst_y1, uint32 dst_z1,
                     uint32 dst_x2, uint32 dst_y2, uint32 dst_z2,
                     uint32 src_res, uint32 src_format, uint32 src_level,
                     uint32 src_x1, uint32 src_y1, uint32 src_z1,
                     uint32 src_x2, uint32 src_y2, uint32 src_z2)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_BLIT, 0, 17));
    virgl_emit_dword(cbuf, s0_flags);

    /* Destination */
    virgl_emit_dword(cbuf, VIRGL_BLIT_S1_DST_LEVEL(dst_level) |
                           VIRGL_BLIT_S1_DST_FORMAT(dst_format));
    virgl_emit_dword(cbuf, dst_res);
    virgl_emit_dword(cbuf, dst_x1);
    virgl_emit_dword(cbuf, dst_y1);
    virgl_emit_dword(cbuf, dst_z1);
    virgl_emit_dword(cbuf, dst_x2);
    virgl_emit_dword(cbuf, dst_y2);
    virgl_emit_dword(cbuf, dst_z2);

    /* Source */
    virgl_emit_dword(cbuf, VIRGL_BLIT_S1_DST_LEVEL(src_level) |
                           VIRGL_BLIT_S1_DST_FORMAT(src_format));
    virgl_emit_dword(cbuf, src_res);
    virgl_emit_dword(cbuf, src_x1);
    virgl_emit_dword(cbuf, src_y1);
    virgl_emit_dword(cbuf, src_z1);
    virgl_emit_dword(cbuf, src_x2);
    virgl_emit_dword(cbuf, src_y2);
    virgl_emit_dword(cbuf, src_z2);
}

/* -----------------------------------------------------------------------
 * DESTROY_OBJECT -- destroy a previously created object by type + handle.
 * ----------------------------------------------------------------------- */
void virgl_cmd_destroy_object(struct VirglCmdBuf *cbuf, uint32 obj_type,
                               uint32 handle)
{
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_DESTROY_OBJECT,
                                          obj_type, 1));
    virgl_emit_dword(cbuf, handle);
}

/* =======================================================================
 * Convenience functions -- create default pipeline objects
 * ======================================================================= */

/* Default blend: no blending, write all RGBA channels. */
void virgl_setup_default_blend(struct VirglCmdBuf *cbuf, uint32 handle)
{
    virgl_cmd_create_blend(cbuf, handle, 0, VIRGL_BLEND_RT_OPAQUE);
}

/* Alpha blend: SrcAlpha / 1-SrcAlpha, write all channels. */
void virgl_setup_alpha_blend(struct VirglCmdBuf *cbuf, uint32 handle)
{
    virgl_cmd_create_blend(cbuf, handle, 0, VIRGL_BLEND_RT_ALPHA);
}

/* Default DSA: no depth test, no stencil, no alpha test. */
void virgl_setup_default_dsa(struct VirglCmdBuf *cbuf, uint32 handle)
{
    virgl_cmd_create_dsa(cbuf, handle, 0, 0, 0, 0.0f);
}

/* Default rasterizer: fill mode, no culling, depth_clip enabled. */
void virgl_setup_default_rasterizer(struct VirglCmdBuf *cbuf, uint32 handle)
{
    uint32 s0 = VIRGL_RS_S0_DEPTH_CLIP(1) |
                VIRGL_RS_S0_CULL_FACE(PIPE_FACE_NONE) |
                VIRGL_RS_S0_FILL_FRONT(PIPE_POLYGON_MODE_FILL) |
                VIRGL_RS_S0_FILL_BACK(PIPE_POLYGON_MODE_FILL);
    virgl_cmd_create_rasterizer(cbuf, handle,
                                 s0, 1.0f,  /* point_size */
                                 0,         /* sprite_coord_enable */
                                 0,         /* s3: no line stipple/clip planes */
                                 1.0f,      /* line_width */
                                 0.0f, 0.0f, 0.0f);  /* offsets */
}

/* =======================================================================
 * Convenience: Simple TGSI shaders
 * ======================================================================= */

/* Passthrough vertex shader:
 *   IN[0] = position (vec4)
 *   IN[1] = color/texcoord (vec4)
 *   OUT[0] = position
 *   OUT[1] = generic[0] (interpolated to fragment shader)
 */
static const char tgsi_vs_passthrough[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: MOV OUT[1], IN[1]\n"
    "  2: END\n";

/* Solid color fragment shader:
 *   IN[0] = interpolated generic[0] from VS (color)
 *   OUT[0] = output color
 */
static const char tgsi_fs_color[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: END\n";

/* Texture-sampling fragment shader:
 *   IN[0]  = interpolated generic[0] (texcoord from VS)
 *   OUT[0] = output color
 *   SAMP[0] = sampler for source texture
 *   TEX OUT[0], IN[0], SAMP[0], 2D
 *
 * The blend state handles Porter-Duff compositing math --
 * this shader just samples the source texture.
 */
static const char tgsi_fs_texture[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n"
    "  1: END\n";

void virgl_setup_passthrough_vs(struct VirglCmdBuf *cbuf, uint32 handle)
{
    DCHIP("virgl_setup_passthrough_vs: handle=%lu", handle);
    virgl_cmd_create_shader(cbuf, handle, PIPE_SHADER_VERTEX,
                             tgsi_vs_passthrough);
}

void virgl_setup_color_fs(struct VirglCmdBuf *cbuf, uint32 handle)
{
    DCHIP("virgl_setup_color_fs: handle=%lu", handle);
    virgl_cmd_create_shader(cbuf, handle, PIPE_SHADER_FRAGMENT,
                             tgsi_fs_color);
}

void virgl_setup_texture_fs(struct VirglCmdBuf *cbuf, uint32 handle)
{
    DCHIP("virgl_setup_texture_fs: handle=%lu", handle);
    virgl_cmd_create_shader(cbuf, handle, PIPE_SHADER_FRAGMENT,
                             tgsi_fs_texture);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Sampler State
 *
 * Controls texture filtering and wrapping.
 * Payload: handle, S0, lod_bias, min_lod, max_lod, border_color[4] = 9 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_sampler_state(struct VirglCmdBuf *cbuf, uint32 handle,
                                      uint32 s0, float lod_bias,
                                      float min_lod, float max_lod,
                                      float border_r, float border_g,
                                      float border_b, float border_a)
{
    DCHIP("virgl_create_sampler_state: handle=%lu s0=0x%08lx", handle, s0);
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_SAMPLER_STATE, 9));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, s0);
    virgl_emit_float(cbuf, lod_bias);
    virgl_emit_float(cbuf, min_lod);
    virgl_emit_float(cbuf, max_lod);
    virgl_emit_float(cbuf, border_r);
    virgl_emit_float(cbuf, border_g);
    virgl_emit_float(cbuf, border_b);
    virgl_emit_float(cbuf, border_a);
}

/* -----------------------------------------------------------------------
 * CREATE_OBJECT: Sampler View
 *
 * Binds a resource as a texture source for shader sampling.
 * Payload: handle, res_handle, format, S0 (element range),
 *          swizzle word = 5 words.
 * ----------------------------------------------------------------------- */
void virgl_cmd_create_sampler_view(struct VirglCmdBuf *cbuf, uint32 handle,
                                     uint32 res_handle, uint32 format,
                                     uint32 first_element, uint32 last_element,
                                     uint32 swizzle_r, uint32 swizzle_g,
                                     uint32 swizzle_b, uint32 swizzle_a)
{
    /* Payload is 6 dwords (VIRGL_OBJ_SAMPLER_VIEW_SIZE):
     *   [1] handle, [2] res_handle, [3] format,
     *   [4] first_element/layer, [5] last_element/level,
     *   [6] swizzle packed.
     * Note: first_element and last_element are SEPARATE dwords,
     * NOT packed into one word. */
    uint32 swiz = (swizzle_r & 0x7) |
                  ((swizzle_g & 0x7) << 3) |
                  ((swizzle_b & 0x7) << 6) |
                  ((swizzle_a & 0x7) << 9);

    DCHIP("virgl_create_sampler_view: handle=%lu res=%lu fmt=%lu swiz=0x%03lx",
          handle, res_handle, format, swiz);
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT,
                                          VIRGL_OBJECT_SAMPLER_VIEW, 6));
    virgl_emit_dword(cbuf, handle);
    virgl_emit_dword(cbuf, res_handle);
    virgl_emit_dword(cbuf, format);
    virgl_emit_dword(cbuf, first_element);
    virgl_emit_dword(cbuf, last_element);
    virgl_emit_dword(cbuf, swiz);
}

/* -----------------------------------------------------------------------
 * SET_SAMPLER_VIEWS -- bind sampler views to a shader stage.
 * Payload: shader_type, start_slot, view_handles[count].
 * ----------------------------------------------------------------------- */
void virgl_cmd_set_sampler_views(struct VirglCmdBuf *cbuf,
                                   uint32 shader_type, uint32 start_slot,
                                   uint32 count, const uint32 *view_handles)
{
    uint32 payload_len = 2 + count;
    DCHIP("virgl_set_sampler_views: shader=%lu start=%lu count=%lu",
          shader_type, start_slot, count);
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_SET_SAMPLER_VIEWS,
                                          0, payload_len));
    virgl_emit_dword(cbuf, shader_type);
    virgl_emit_dword(cbuf, start_slot);
    for (uint32 i = 0; i < count; i++)
        virgl_emit_dword(cbuf, view_handles[i]);
}

/* -----------------------------------------------------------------------
 * BIND_SAMPLER_STATES -- bind sampler state objects to a shader stage.
 * Payload: shader_type, start_slot, state_handles[count].
 * ----------------------------------------------------------------------- */
void virgl_cmd_bind_sampler_states(struct VirglCmdBuf *cbuf,
                                     uint32 shader_type, uint32 start_slot,
                                     uint32 count, const uint32 *state_handles)
{
    uint32 payload_len = 2 + count;
    DCHIP("virgl_bind_sampler_states: shader=%lu start=%lu count=%lu",
          shader_type, start_slot, count);
    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_BIND_SAMPLER_STATES,
                                          0, payload_len));
    virgl_emit_dword(cbuf, shader_type);
    virgl_emit_dword(cbuf, start_slot);
    for (uint32 i = 0; i < count; i++)
        virgl_emit_dword(cbuf, state_handles[i]);
}
