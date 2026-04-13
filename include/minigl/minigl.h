/*
 * minigl.h -- MiniGL public API header for AmigaOS 4.
 *
 * Provides a minimal subset of OpenGL 1.x functions backed by Virgl
 * commands submitted to the VirtIO GPU driver.
 *
 * This is NOT a full OpenGL implementation -- only the most common
 * functions used by AmigaOS MiniGL applications are provided.
 */

#ifndef MINIGL_H
#define MINIGL_H

#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>

/* GL type definitions */
typedef uint32  GLenum;
typedef uint8   GLboolean;
typedef uint32  GLbitfield;
typedef int8    GLbyte;
typedef int16   GLshort;
typedef int32   GLint;
typedef int32   GLsizei;
typedef uint8   GLubyte;
typedef uint16  GLushort;
typedef uint32  GLuint;
typedef float   GLfloat;
typedef float   GLclampf;
typedef double  GLdouble;
typedef double  GLclampd;
typedef void    GLvoid;

/* Boolean values */
#define GL_FALSE    0
#define GL_TRUE     1

/* Error codes */
#define GL_NO_ERROR             0
#define GL_INVALID_ENUM         0x0500
#define GL_INVALID_VALUE        0x0501
#define GL_INVALID_OPERATION    0x0502
#define GL_STACK_OVERFLOW       0x0503
#define GL_STACK_UNDERFLOW      0x0504
#define GL_OUT_OF_MEMORY        0x0505

/* String queries */
#define GL_VENDOR       0x1F00
#define GL_RENDERER     0x1F01
#define GL_VERSION      0x1F02
#define GL_EXTENSIONS   0x1F03

/* Primitive types */
#define GL_POINTS           0x0000
#define GL_LINES            0x0001
#define GL_LINE_LOOP        0x0002
#define GL_LINE_STRIP       0x0003
#define GL_TRIANGLES        0x0004
#define GL_TRIANGLE_STRIP   0x0005
#define GL_TRIANGLE_FAN     0x0006
#define GL_QUADS            0x0007
#define GL_QUAD_STRIP       0x0008
#define GL_POLYGON          0x0009

/* Buffer clear bits */
#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_STENCIL_BUFFER_BIT   0x00000400
#define GL_COLOR_BUFFER_BIT     0x00004000

/* Enable/Disable caps */
#define GL_DEPTH_TEST       0x0B71
#define GL_BLEND            0x0BE2
#define GL_CULL_FACE        0x0B44
#define GL_TEXTURE_2D       0x0DE1
#define GL_SCISSOR_TEST     0x0C11
#define GL_ALPHA_TEST       0x0BC0

/* Blend functions */
#define GL_ZERO                     0
#define GL_ONE                      1
#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_SRC_ALPHA_SATURATE       0x0308

/* Depth functions */
#define GL_NEVER        0x0200
#define GL_LESS         0x0201
#define GL_EQUAL        0x0202
#define GL_LEQUAL       0x0203
#define GL_GREATER      0x0204
#define GL_NOTEQUAL     0x0205
#define GL_GEQUAL       0x0206
#define GL_ALWAYS       0x0207

/* Matrix modes */
#define GL_MODELVIEW    0x1700
#define GL_PROJECTION   0x1701
#define GL_TEXTURE      0x1702

/* Shade model */
#define GL_FLAT         0x1D00
#define GL_SMOOTH       0x1D01

/* Texture parameters */
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_REPEAT               0x2901
#define GL_CLAMP                0x2900
#define GL_CLAMP_TO_EDGE        0x812F

/* Pixel formats */
#define GL_ALPHA            0x1906
#define GL_RGB              0x1907
#define GL_RGBA             0x1908
#define GL_LUMINANCE        0x1909
#define GL_LUMINANCE_ALPHA  0x190A
#define GL_UNSIGNED_BYTE    0x1401
#define GL_FLOAT            0x1406

/* Get parameters */
#define GL_MAX_TEXTURE_SIZE     0x0D33
#define GL_VIEWPORT             0x0BA2

/* MiniGL context (opaque) */
struct MiniGLContext;
typedef struct MiniGLContext *MGLContext;

/* -----------------------------------------------------------------------
 * MiniGL-specific context management (not standard GL)
 * ----------------------------------------------------------------------- */

/* Create a MiniGL context bound to a RastPort.
 * Returns NULL on failure. */
MGLContext mglCreateContext(struct RastPort *rp, uint32 width, uint32 height);

/* Destroy a MiniGL context. */
void mglDeleteContext(MGLContext ctx);

/* Make a context current for GL calls. */
void mglMakeCurrent(MGLContext ctx);

/* Swap front/back buffers (present rendered frame). */
void mglSwapBuffers(MGLContext ctx);

/* Lock/unlock context for rendering. */
void mglLockDisplay(MGLContext ctx);
void mglUnlockDisplay(MGLContext ctx);

/* -----------------------------------------------------------------------
 * GL 1.x function declarations
 * ----------------------------------------------------------------------- */

/* State */
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glShadeModel(GLenum mode);
void glCullFace(GLenum mode);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);

/* Clear */
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClear(GLbitfield mask);

/* Viewport / Matrix */
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble zNear, GLdouble zFar);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble zNear, GLdouble zFar);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);

/* Immediate mode */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glTexCoord2f(GLfloat s, GLfloat t);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);

/* Textures */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);

/* Queries */
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetFloatv(GLenum pname, GLfloat *params);

/* Flush */
void glFlush(void);
void glFinish(void);

#endif /* MINIGL_H */
