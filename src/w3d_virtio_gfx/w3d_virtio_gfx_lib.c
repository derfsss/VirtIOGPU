/*
 * w3d_virtio_gfx_lib.c -- W3D_VirtIO.library: the Warp3D V5 GFX DRIVER for the
 * VirtIO GPU, loaded by the stock Warp3D.library front-end (53.27) from
 * LIBS:Warp3D/GFXdrivers/.
 *
 * Phase 8 Part B (B-gfx1): PROBE skeleton.  The FE selects a HW driver via the
 * GFX driver's bitmap->driver-id lookup at instance-offset 0x58; the stock
 * W3D_Picasso96 GFXdriver only recognizes a fixed board-name allowlist, so our
 * virtio board yields no HW match -> W3D_NODRIVER(-4) and apps can't create a
 * context.  Our own GFX driver must claim the virtio board and return our HW
 * driver's id from [0x58].
 *
 * This B-gfx1 build is a per-vector LOGGING PROBE: it logs which `main` vectors
 * the FE calls (and their args) so we empirically map the GFX-driver interface,
 * confirm whether the FE even loads/selects OUR GFXdriver (vs stock Picasso96 --
 * the coexistence question), and capture the [0x58] bitmap arg.  Then B-gfx2
 * implements the real [0x58] board-claim + the framebuffer/Z ops.
 *
 * Built -mcrt=newlib -nostartfiles, same discipline as the HW backend.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

struct ExecIFace *IExec = NULL;

/* GFX driver name -- DISTINCT from the HW driver "W3D_VirtIOGPU.library" (exec
 * library names must be unique).  Filename in LIBS:Warp3D/GFXdrivers/ matches. */
static const char gfx_name[]  __attribute__((used)) = "W3D_VirtIO.library";
static const char gfx_idstr[] __attribute__((used)) =
    "$VER: W3D_VirtIO.library 54.1 (17.06.2026)\r\n";

#define DBP(...) do { if (IExec) IExec->DebugPrintF("[W3D_VirtIO.gfx] " __VA_ARGS__); } while (0)

/* ---- library manager interface ("__library") ---- */
static uint32 _mgr_Obtain(struct LibraryManagerInterface *Self)
    { return Self->Data.RefCount++; }
static uint32 _mgr_Release(struct LibraryManagerInterface *Self)
    { return Self->Data.RefCount--; }
static struct Library *_mgr_Open(struct LibraryManagerInterface *Self, uint32 version)
{
    struct Library *lib = Self->Data.LibBase;
    (void)version;
    lib->lib_OpenCnt++;
    lib->lib_Flags &= ~LIBF_DELEXP;
    return lib;
}
static BPTR _mgr_Close(struct LibraryManagerInterface *Self)
{
    struct Library *lib = Self->Data.LibBase;
    if (--lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return (BPTR)1;
    return (BPTR)0;
}
static BPTR _mgr_Expunge(struct LibraryManagerInterface *Self)
    { (void)Self; return (BPTR)0; }

static const APTR _mgr_Vectors[] =
{
    (APTR)_mgr_Obtain, (APTR)_mgr_Release, NULL, NULL,
    (APTR)_mgr_Open, (APTR)_mgr_Close, (APTR)_mgr_Expunge, NULL,
    (APTR)-1
};
static const struct TagItem _mgr_Tags[] =
{
    { MIT_Name,        (Tag)"__library"  },
    { MIT_VectorTable, (Tag)_mgr_Vectors },
    { MIT_Version,     1                 },
    { TAG_DONE,        0                 }
};

/* ---- "main" GFX-driver interface (probe) ---- */
static uint32 _main_Obtain(struct Interface *Self)  { return Self->Data.RefCount++; }
static uint32 _main_Release(struct Interface *Self) { return Self->Data.RefCount--; }

/* Per-vector logging stub.  Instance byte-offset of vector N = 76 + N*4 (same
 * 76-byte InterfaceData header as the HW backend); logging confirms the actual
 * offset the FE calls (e.g. the bitmap lookup at off 0x58). */
#define GFXSTUB(n) static uint32 gfx_s##n(APTR Self,uint32 a,uint32 b,uint32 c,uint32 d){ (void)Self; \
    DBP("vec %ld (off 0x%lx) a=%08lx b=%08lx c=%08lx d=%08lx\n", (long)(n), \
        (unsigned long)(76+(n)*4), (unsigned long)a,(unsigned long)b, \
        (unsigned long)c,(unsigned long)d); return 0; }
GFXSTUB(2)  GFXSTUB(3)  GFXSTUB(4)  GFXSTUB(5)  GFXSTUB(6)  GFXSTUB(7)
GFXSTUB(8)  GFXSTUB(9)  GFXSTUB(10) GFXSTUB(11) GFXSTUB(12) GFXSTUB(13)
GFXSTUB(14) GFXSTUB(15) GFXSTUB(16) GFXSTUB(17) GFXSTUB(18) GFXSTUB(19)
GFXSTUB(20) GFXSTUB(21) GFXSTUB(22) GFXSTUB(23)

static const APTR _main_Vectors[] __attribute__((used)) =
{
    (APTR)_main_Obtain,   /* vec 0  off 0x4c  Obtain  */
    (APTR)_main_Release,  /* vec 1  off 0x50  Release */
    (APTR)gfx_s2,  (APTR)gfx_s3,  (APTR)gfx_s4,  (APTR)gfx_s5,  (APTR)gfx_s6,
    (APTR)gfx_s7,  (APTR)gfx_s8,  (APTR)gfx_s9,  (APTR)gfx_s10, (APTR)gfx_s11,
    (APTR)gfx_s12, (APTR)gfx_s13, (APTR)gfx_s14, (APTR)gfx_s15, (APTR)gfx_s16,
    (APTR)gfx_s17, (APTR)gfx_s18, (APTR)gfx_s19, (APTR)gfx_s20, (APTR)gfx_s21,
    (APTR)gfx_s22, (APTR)gfx_s23,
    (APTR)-1
};
#define GFX_NUM_VECTORS 24
static const struct TagItem _main_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)"main"        },
    { MIT_VectorTable, (Tag)_main_Vectors },
    { MIT_Version,     1                  },
    { MIT_DataSize,    sizeof(struct Interface) + GFX_NUM_VECTORS * sizeof(APTR) },
    { TAG_DONE,        0                  }
};

static const ULONG _gfx_Interfaces[] __attribute__((used)) =
{
    (ULONG)_mgr_Tags, (ULONG)_main_Tags, 0
};

static struct Library *_gfx_Init(struct Library *libBase, ULONG seglist,
                                 struct Interface *exec)
{
    (void)seglist;
    if (exec) IExec = (struct ExecIFace *)exec;
    libBase->lib_Node.ln_Type = NT_LIBRARY;
    libBase->lib_Node.ln_Name = (char *)gfx_name;
    libBase->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->lib_Version      = 54;
    libBase->lib_Revision     = 1;
    libBase->lib_IdString     = (APTR)gfx_idstr;
    DBP("Init: W3D_VirtIO GFX driver loaded (probe skeleton)\n");
    return libBase;
}

static const struct TagItem _gfx_InitTags[] __attribute__((used)) =
{
    { CLT_DataSize,      sizeof(struct Library) },
    { CLT_Interfaces,    (Tag)_gfx_Interfaces   },
    { CLT_InitFunc,      (Tag)_gfx_Init         },
    { CLT_NoLegacyIFace, TRUE                   },
    { TAG_DONE,          0                      }
};

static const struct Resident gfx_romtag __attribute__((used, section(".rodata"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&gfx_romtag,
    (APTR)(&gfx_romtag + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    54,
    NT_LIBRARY,
    0,
    (CONST_STRPTR)gfx_name,
    (CONST_STRPTR)gfx_idstr,
    (APTR)_gfx_InitTags,
};

__asm__(
    ".section \".text\"\n"
    ".globl _start\n"
    "_start:\n"
    "    li  3, -1\n"
    "    blr\n"
);
