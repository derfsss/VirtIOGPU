/*
 * chip_lib.c -- Chip driver library boilerplate and ROM tags.
 *
 * Contains the "virtiogpu.chip" NT_LIBRARY resident (pri -128),
 * the __library manager interface, the "main" interface with
 * chip_FindCard as slot[4], CLT init tags, and the _start entry point.
 *
 * Global variable definitions (g_chip_state, g_IExec_early) live here.
 */

#include "chip/chip_state.h"

/* -----------------------------------------------------------------------
 * ROM tag names
 * ----------------------------------------------------------------------- */
static const char chip_name[]  __attribute__((used)) = "virtiogpu.chip";
static const char chip_idstr[] __attribute__((used)) =
    "$VER: virtiogpu.chip 53.103 (13.04.2026)\r\n";

/* -----------------------------------------------------------------------
 * Global variable definitions
 * ----------------------------------------------------------------------- */
struct ExecIFace *g_IExec_early = NULL;
struct ChipGPUState *g_chip_state = NULL;

/* -----------------------------------------------------------------------
 * No-op interrupt handler -- returns 0 (not handled) so interrupt chain continues.
 * PCIGraphics.card calls AddIntServer(&bi->HardInterrupt) after FindCard returns
 * non-NULL. Without a valid handler, the corrupt node crashes the kernel.
 * ----------------------------------------------------------------------- */
static ULONG chip_irq_nop(void)
{
    return 0; /* not handled */
}

/* -----------------------------------------------------------------------
 * chip_FindCard -- slot[4] of "main" interface (combined FindCard+InitCard
 * per SM502 protocol).
 * ----------------------------------------------------------------------- */
static APTR chip_FindCard(struct Interface *Self,
                           struct BoardInfo *bi,
                           APTR cardDesc)
{
    (void)Self;
    struct ExecBase    *sb    = *(struct ExecBase **)4;
    struct ExecIFace   *IExec = (struct ExecIFace *)sb->MainInterface;

    if (IExec) {
        IExec->DebugPrintF("[virtiogpu.chip] v53.157-cursor-signed-fix chip_FindCard ENTRY bi=%p cardDesc=%p\n",
                           bi, cardDesc);
    }

    if (!bi) {
        if (IExec)
            IExec->DebugPrintF("[virtiogpu.chip] chip_FindCard: NULL bi\n");
        return NULL;
    }

    /* cardDesc is NULL when called via VirtIOGPUBoard SetMethod hook.
     * That's OK -- the PCI device is pre-set in BoardInfo by the hook.
     * Only reject if BOTH cardDesc AND the pre-set PCI device are NULL. */
    if (!cardDesc) {
        struct PCIDevice *presetDev = boardinfo_get_pcidevice(bi);
        if (!presetDev) {
            if (IExec)
                IExec->DebugPrintF("[virtiogpu.chip] chip_FindCard: NULL cardDesc "
                                   "and no pre-set PCI device in BoardInfo\n");
            return NULL;
        }
        if (IExec)
            IExec->DebugPrintF("[virtiogpu.chip] chip_FindCard: SetMethod path, "
                               "PCI device pre-set = %p\n", presetDev);
    }

    /* v53.24 FIX: Fill bi->HardInterrupt BEFORE returning non-NULL.
     * PCIGraphics.card RE confirmed: after slot[4] returns non-NULL, it calls
     * IExec->AddIntServer(intNum, &bi->HardInterrupt) -- adding bi+68 to the
     * kernel interrupt chain. Without valid node fields, the chain is corrupted
     * and the scheduler crashes with DSI DAR=0 at kernel+0x1B99C. */
    bi->HardInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    bi->HardInterrupt.is_Node.ln_Pri  = 0;
    bi->HardInterrupt.is_Node.ln_Name = (STRPTR)"VirtIOGPU";
    bi->HardInterrupt.is_Data         = (APTR)bi;
    bi->HardInterrupt.is_Code         = (void (*)())chip_irq_nop;

    if (IExec) {
        IExec->DebugPrintF("[virtiogpu.chip] HardInterrupt filled: Code=%p Data=%p\n",
                           (APTR)bi->HardInterrupt.is_Code, bi->HardInterrupt.is_Data);
    }

    BOOL ok = chip_InitCard_C(bi, NULL, cardDesc);
    if (IExec)
        IExec->DebugPrintF("[virtiogpu.chip] chip_InitCard_C -> %ld\n", (LONG)ok);

    return ok ? (APTR)TRUE : NULL;
}

/* -----------------------------------------------------------------------
 * Library manager interface
 * ----------------------------------------------------------------------- */
static uint32 _manager_Obtain(struct LibraryManagerInterface *Self)
    { return Self->Data.RefCount++; }
static uint32 _manager_Release(struct LibraryManagerInterface *Self)
    { return Self->Data.RefCount--; }
static struct Library *_manager_Open(struct LibraryManagerInterface *Self, uint32 version)
{
    struct Library *lib = Self->Data.LibBase;
    lib->lib_OpenCnt++;
    lib->lib_Flags &= ~LIBF_DELEXP;
    return lib;
}
static BPTR _manager_Close(struct LibraryManagerInterface *Self)
{
    struct Library *lib = Self->Data.LibBase;
    if (--lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return (BPTR)1;
    return (BPTR)0;
}
static BPTR _manager_Expunge(struct LibraryManagerInterface *Self)
    { (void)Self; return (BPTR)0; }

static const APTR _manager_Vectors[] =
{
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    NULL, NULL,
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    NULL,
    (APTR)-1
};
static const struct TagItem _manager_Tags[] =
{
    { MIT_Name,        (Tag)"__library"      },
    { MIT_VectorTable, (Tag)_manager_Vectors },
    { MIT_Version,     1                     },
    { TAG_DONE,        0                     }
};

/* -----------------------------------------------------------------------
 * "main" interface -- chip_FindCard is slot[4]
 * ----------------------------------------------------------------------- */
static ULONG _main_Obtain(struct Interface *Self)  { return Self->Data.RefCount++; }
static ULONG _main_Release(struct Interface *Self) { return Self->Data.RefCount--; }

static const APTR _chip_main_Vectors[] __attribute__((used)) =
{
    (APTR)_main_Obtain,    /* slot[0] */
    (APTR)_main_Release,   /* slot[1] */
    NULL, NULL,            /* slot[2..3] -- Expunge/Clone */
    (APTR)chip_FindCard,   /* slot[4] -- combined FindCard+InitCard (SM502 protocol) */
    (APTR)-1               /* sentinel -- SM502 RE confirmed 0xFFFFFFFF, not NULL */
};
static const struct TagItem _chip_main_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)"main"               },
    { MIT_VectorTable, (Tag)_chip_main_Vectors   },
    { MIT_Version,     1                         },
    { TAG_DONE,        0                         }
};
static const ULONG _chip_Interfaces[] __attribute__((used)) =
{
    (ULONG)_manager_Tags,
    (ULONG)_chip_main_Tags,
    0
};

/* -----------------------------------------------------------------------
 * Library init function
 * ----------------------------------------------------------------------- */
static struct Library *_chip_lib_Init(struct Library *libBase,
                                      ULONG seglist,
                                      struct Interface *exec)
{
    (void)seglist;
    if (exec)
        g_IExec_early = (struct ExecIFace *)exec;
    libBase->lib_Node.ln_Type = NT_LIBRARY;
    libBase->lib_Node.ln_Name = (char *)chip_name;
    libBase->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->lib_Version      = 53;
    libBase->lib_Revision     = 10;
    libBase->lib_IdString     = (APTR)chip_idstr;
    return libBase;
}

static const struct TagItem _chip_InitTags[] __attribute__((used)) =
{
    { CLT_DataSize,      sizeof(struct Library)  },
    { CLT_Interfaces,    (Tag)_chip_Interfaces   },
    { CLT_InitFunc,      (Tag)_chip_lib_Init     },
    { CLT_NoLegacyIFace, TRUE                    },
    { TAG_DONE,          0                       }
};

/* -----------------------------------------------------------------------
 * chip_romtag -- the chip driver library resident (loaded on demand).
 * ----------------------------------------------------------------------- */
static const struct Resident chip_romtag __attribute__((used, section(".rodata"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&chip_romtag,
    (APTR)(&chip_romtag + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    53,
    NT_LIBRARY,
    -128,
    (CONST_STRPTR)chip_name,
    (CONST_STRPTR)chip_idstr,
    (APTR)_chip_InitTags,
};

/* -----------------------------------------------------------------------
 * _start -- PCIGraphics.card calls _start+0 as FindCard (legacy stub).
 * Our real work is in chip_FindCard (the AmigaOS4 interface path).
 * ----------------------------------------------------------------------- */
__asm__(
    ".section \".text\"\n"
    ".globl _start\n"
    "_start:\n"
    "    li  3, 1\n"              /* FindCard: return 1 */
    "    blr\n"
    "    b   chip_InitCard_C\n"   /* InitCard: jump to C (legacy unused) */
);
