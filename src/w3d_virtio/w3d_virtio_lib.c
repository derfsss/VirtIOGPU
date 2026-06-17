/*
 * w3d_virtio_lib.c -- W3D_VirtIOGPU.library: the Warp3D V5 HARDWARE BACKEND
 * for the VirtIO GPU, loaded by the STOCK Warp3D.library front-end (PINNED
 * version 53.27, 24.05.2016) from LIBS:Warp3D/HWdrivers/.
 *
 * Phase 8 Part B.  This file is the B1 SKELETON: ROM tag + library manager +
 * the "main" interface = struct W3DHWIFace (the backend vtable), whose slot
 * ORDER is the FE 53.27 dispatch order recovered in B0
 * (tmp_re/fe5327_slotmap_draft.md, [[reference-warp3d-driver-model]]).
 *
 * Backend ABI (from B0): the FE reaches us via the W3D_Context: the backend
 * W3DHWIFace* is stored at *(ctx+192); each public op thunks `lwz r9,OFF(iface);
 * bctrl`, so **backend slot = (OFF - 76)/4**.  Slots 0-3 = manager (Obtain,
 * Release, Expunge=NULL, Clone=NULL), real ops at slot >= 4.  CreateContext gets
 * the FE-allocated public W3D_Context and hangs private state off ctx->driver
 * (offset 0); the GFX-driver ops (LockHardware/UnLockHardware/SetDrawRegion/
 * TestMode/GetDrivers...) are NOT ours -- the stock W3D_Picasso96.library
 * GFXdriver handles those.
 *
 * B1 = PROBE skeleton: every op slot is a per-slot logging stub (hw_sN), so
 * loading this under the real FE and running an app reveals which slot the FE
 * actually calls for each W3D op -> empirically pins the ambiguous lifecycle
 * slots (CreateContext/DestroyContext/SetState/tex alloc) that static RE can't
 * cleanly assign.  B2 replaces the stubs with the ported virgl rendering core.
 *
 * Built -mcrt=newlib -nostartfiles with the same newlib discipline as the chip.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

/* ---- globals ---- */
struct ExecIFace *IExec = NULL;

/* Filename-matched name: the FE scans libs:Warp3D/HWdrivers/#?.library and opens
 * by filename, so the romtag name MUST case-match the on-disk file name. */
static const char w3d_name[]  __attribute__((used)) = "W3D_VirtIOGPU.library";
static const char w3d_idstr[] __attribute__((used)) =
    "$VER: W3D_VirtIOGPU.library 54.1 (17.06.2026)\r\n";

#define DBP(...) do { if (IExec) IExec->DebugPrintF("[W3D_VirtIOGPU] " __VA_ARGS__); } while (0)

/* ----------------------------------------------------------------------- */
/* Library manager interface ("__library")                                  */
/* ----------------------------------------------------------------------- */
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
    (APTR)_mgr_Obtain,
    (APTR)_mgr_Release,
    NULL, NULL,
    (APTR)_mgr_Open,
    (APTR)_mgr_Close,
    (APTR)_mgr_Expunge,
    NULL,
    (APTR)-1
};
static const struct TagItem _mgr_Tags[] =
{
    { MIT_Name,        (Tag)"__library"  },
    { MIT_VectorTable, (Tag)_mgr_Vectors },
    { MIT_Version,     1                 },
    { TAG_DONE,        0                 }
};

/* ----------------------------------------------------------------------- */
/* "main" interface == struct W3DHWIFace (the backend vtable)               */
/* Manager slots 0-3, then backend ops at slot >=4.                         */
/* B1 PROBE: every op slot logs its index so a runtime trace pins the map.  */
/* ----------------------------------------------------------------------- */
static uint32 _main_Obtain(struct Interface *Self)  { return Self->Data.RefCount++; }
static uint32 _main_Release(struct Interface *Self) { return Self->Data.RefCount--; }

/* B2 iter-1 probe: log Self + 3 args, and return best-effort values for the
 * registration / Z-self-test slots (from B0/B2 RE) so the FE's Warp3D_Init
 * accepts our backend and proceeds to CreateContext + the draw path.  Per-slot
 * returns are centralized in hw_dispatch() so we can iterate quickly.
 * Reading extra args is harmless on the PPC SysV ABI (regs r4-r6). */
/* B2 iter-2: also log the caller's return address (LR) so each slot call maps to
 * the exact FE code path (registration vs selection vs CreateContext vs a caps
 * probe).  ra - FE_load_base = the fe5327.dis file offset of the call site. */
static uint32 g_dummy_state[16];    /* non-NULL handle ClearDrawRegion returns */
static uint32 hw_dispatch(long slot, uint32 a, uint32 b, uint32 c, uint32 d, uint32 ra)
{
    uint32 ret = 0;
    /* Mirror the working R200 backend's exact slot returns (from Ghidra) so the
     * caller's CreateContext init sequence accepts us. */
    switch (slot) {
    /* The cow checks W3D_AllocZBuffer == W3D_SUCCESS(0) and bails otherwise
     * (CoW3D6.c:1792); same for CreateContext.  These two MUST return 0.  The
     * Z read/clear/identify returns are NOT checked-and-bail. */
    /* slot18 (off 0x94) = backend<->GFXdriver INIT: FE calls it as
     * slot18(W3DGFXBase, W3DGFXversion).  A real HW driver registers its board
     * with the GFXdriver here so the GFXdriver's bitmap->driver lookup
     * (CreateContext primary select via IW3DGFX->[0x58]) maps the app bitmap to
     * it.  A bare return does NOT register -> GFXdriver has no mapping -> the FE
     * returns W3D_NODRIVER(-4).  TODO: replicate R200's slot18 GFX handshake.
     * (return 0 = "HW driver"; ==1 would mark CPU + risk the Warp3D_Init drop.) */
    /* slot19 (off 0x98) = format-support query: FE calls it with (ctx,
     * format_id, destfmt, 0) and the CreateContext format loop requires the
     * return == 5 to treat the format as supported (else W3D_UNSUPPORTEDFMT -18).
     * Return 5 to claim support. */
    case 19: ret = 5;      break;
    /* slot23 (off 0xa8) is the format-support query the cow's CreateContext
     * actually hammers (format ids 0x6f-0x72,0x14-0x17 + destfmt); the loop
     * needs == 5 to treat the format as supported (else -18 UNSUPPORTEDFMT). */
    case 23: ret = 5;      break;
    case 57: ret = 0x48aa; break;   /* identify -> chip magic (informational)     */
    case 61: ret = (uint32)(APTR)g_dummy_state; break; /* ClearDrawRegion -> non-NULL */
    case 4:                         /* AllocZBuffer -> W3D_SUCCESS(0) (cow-checked)*/
    case 22: case 42:               /* ReadZPixel/SetPenMask -> 0                  */
    default: ret = 0; break;
    }
    DBP("slot %ld ra=%08lx a=%08lx b=%08lx c=%08lx d=%08lx -> %08lx\n",
        slot, (unsigned long)ra, (unsigned long)a, (unsigned long)b,
        (unsigned long)c, (unsigned long)d, (unsigned long)ret);
    return ret;
}

/* slot 9 = backend CreateContext(Self, W3D_Context *ctx).  Probe step: log the
 * ctx + return W3D_SUCCESS(0) to confirm the FE reaches CreateContext and
 * proceeds.  Next iteration: alloc private state -> ctx->driver (offset 0) like
 * R200's FUN_00004780.  ctx->driver is left as-is for now. */
static uint32 hw_s9(APTR Self, APTR ctx)
{
    (void)Self;
    DBP("slot 9 CreateContext ctx=%08lx\n", (unsigned long)ctx);
    return 0;                              /* W3D_SUCCESS */
}

#define HWSTUB(n) static uint32 hw_s##n(APTR Self,uint32 a,uint32 b,uint32 c,uint32 d){ (void)Self; return hw_dispatch((n),a,b,c,d,(uint32)(APTR)__builtin_return_address(0)); }
HWSTUB(4)  HWSTUB(5)  HWSTUB(6)  HWSTUB(7)  HWSTUB(8)
HWSTUB(10) HWSTUB(11) HWSTUB(12) HWSTUB(13) HWSTUB(14) HWSTUB(15) HWSTUB(16) HWSTUB(17) HWSTUB(18) HWSTUB(19)
HWSTUB(20) HWSTUB(21) HWSTUB(22) HWSTUB(23) HWSTUB(24) HWSTUB(25) HWSTUB(26) HWSTUB(27) HWSTUB(28) HWSTUB(29)
HWSTUB(30) HWSTUB(31) HWSTUB(32) HWSTUB(33) HWSTUB(34) HWSTUB(35) HWSTUB(36) HWSTUB(37) HWSTUB(38) HWSTUB(39)
HWSTUB(40) HWSTUB(41) HWSTUB(42) HWSTUB(43) HWSTUB(44) HWSTUB(45) HWSTUB(46) HWSTUB(47) HWSTUB(48) HWSTUB(49)
HWSTUB(50) HWSTUB(51) HWSTUB(52) HWSTUB(53) HWSTUB(54) HWSTUB(55) HWSTUB(56) HWSTUB(57) HWSTUB(58) HWSTUB(59)
HWSTUB(60) HWSTUB(61) HWSTUB(62) HWSTUB(63) HWSTUB(64) HWSTUB(65) HWSTUB(66) HWSTUB(67) HWSTUB(68) HWSTUB(69)
HWSTUB(70) HWSTUB(71) HWSTUB(72) HWSTUB(73) HWSTUB(74) HWSTUB(75) HWSTUB(76) HWSTUB(77) HWSTUB(78) HWSTUB(79)
HWSTUB(80) HWSTUB(81) HWSTUB(82) HWSTUB(83) HWSTUB(84) HWSTUB(85) HWSTUB(86) HWSTUB(87)

static const APTR _main_Vectors[] __attribute__((used)) =
{
    (APTR)_main_Obtain,   /* slot 0  Obtain  */
    (APTR)_main_Release,  /* slot 1  Release */
    NULL,                 /* slot 2  Expunge */
    NULL,                 /* slot 3  Clone   */
    (APTR)hw_s4,  (APTR)hw_s5,  (APTR)hw_s6,  (APTR)hw_s7,  (APTR)hw_s8,  (APTR)hw_s9,
    (APTR)hw_s10, (APTR)hw_s11, (APTR)hw_s12, (APTR)hw_s13, (APTR)hw_s14, (APTR)hw_s15,
    (APTR)hw_s16, (APTR)hw_s17, (APTR)hw_s18, (APTR)hw_s19, (APTR)hw_s20, (APTR)hw_s21,
    (APTR)hw_s22, (APTR)hw_s23, (APTR)hw_s24, (APTR)hw_s25, (APTR)hw_s26, (APTR)hw_s27,
    (APTR)hw_s28, (APTR)hw_s29, (APTR)hw_s30, (APTR)hw_s31, (APTR)hw_s32, (APTR)hw_s33,
    (APTR)hw_s34, (APTR)hw_s35, (APTR)hw_s36, (APTR)hw_s37, (APTR)hw_s38, (APTR)hw_s39,
    (APTR)hw_s40, (APTR)hw_s41, (APTR)hw_s42, (APTR)hw_s43, (APTR)hw_s44, (APTR)hw_s45,
    (APTR)hw_s46, (APTR)hw_s47, (APTR)hw_s48, (APTR)hw_s49, (APTR)hw_s50, (APTR)hw_s51,
    (APTR)hw_s52, (APTR)hw_s53, (APTR)hw_s54, (APTR)hw_s55, (APTR)hw_s56, (APTR)hw_s57,
    (APTR)hw_s58, (APTR)hw_s59, (APTR)hw_s60, (APTR)hw_s61, (APTR)hw_s62, (APTR)hw_s63,
    (APTR)hw_s64, (APTR)hw_s65, (APTR)hw_s66, (APTR)hw_s67, (APTR)hw_s68, (APTR)hw_s69,
    (APTR)hw_s70, (APTR)hw_s71, (APTR)hw_s72, (APTR)hw_s73, (APTR)hw_s74, (APTR)hw_s75,
    (APTR)hw_s76, (APTR)hw_s77, (APTR)hw_s78, (APTR)hw_s79, (APTR)hw_s80, (APTR)hw_s81,
    (APTR)hw_s82, (APTR)hw_s83, (APTR)hw_s84, (APTR)hw_s85, (APTR)hw_s86, (APTR)hw_s87,
    (APTR)-1              /* sentinel */
};
/* Backend "main" iface.  MIT_DataSize MUST cover the InterfaceData header +
 * all 88 vectors (slots 0..87, last byte off 76+87*4=424) -- without it
 * GetInterface("main",1) in the FE's Warp3D_Init scan fails to instantiate the
 * interface and our driver never enters Drivers[]. */
#define W3DHW_NUM_VECTORS 88
static const struct TagItem _main_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)"main"        },
    { MIT_VectorTable, (Tag)_main_Vectors },
    { MIT_Version,     1                  },
    { MIT_DataSize,    sizeof(struct Interface) + W3DHW_NUM_VECTORS * sizeof(APTR) },
    { TAG_DONE,        0                  }
};

static const ULONG _w3d_Interfaces[] __attribute__((used)) =
{
    (ULONG)_mgr_Tags,
    (ULONG)_main_Tags,
    0
};

/* ----------------------------------------------------------------------- */
/* Library init                                                             */
/* ----------------------------------------------------------------------- */
static struct Library *_w3d_Init(struct Library *libBase, ULONG seglist,
                                 struct Interface *exec)
{
    (void)seglist;
    if (exec)
        IExec = (struct ExecIFace *)exec;
    libBase->lib_Node.ln_Type = NT_LIBRARY;
    libBase->lib_Node.ln_Name = (char *)w3d_name;
    libBase->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->lib_Version      = 54;
    libBase->lib_Revision     = 1;
    libBase->lib_IdString     = (APTR)w3d_idstr;
    DBP("Init: W3D_VirtIOGPU.library backend loaded (probe skeleton)\n");
    return libBase;
}

static const struct TagItem _w3d_InitTags[] __attribute__((used)) =
{
    { CLT_DataSize,      sizeof(struct Library) },
    { CLT_Interfaces,    (Tag)_w3d_Interfaces   },
    { CLT_InitFunc,      (Tag)_w3d_Init         },
    { CLT_NoLegacyIFace, TRUE                   },
    { TAG_DONE,          0                      }
};

static const struct Resident w3d_romtag __attribute__((used, section(".rodata"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&w3d_romtag,
    (APTR)(&w3d_romtag + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    54,
    NT_LIBRARY,
    0,
    (CONST_STRPTR)w3d_name,
    (CONST_STRPTR)w3d_idstr,
    (APTR)_w3d_InitTags,
};

/* Pure AmigaOS4 interface library -- no legacy entry point. */
__asm__(
    ".section \".text\"\n"
    ".globl _start\n"
    "_start:\n"
    "    li  3, -1\n"
    "    blr\n"
);
