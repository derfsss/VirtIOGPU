/*
 * chip_board.c -- VirtIOGPUBoard resident and PCIGraphics.card hooks.
 *
 * Contains the VirtIOGPUBoard NT_RESOURCE resident (pri 65) that runs
 * BEFORE graphics.library (pri 63).  It scans PCI for VirtIO GPU
 * (1AF4:1050), then hooks PCIGraphics.card's FindCard and InitBoard
 * methods via SetMethod so our PCI device gets injected when
 * graphics.library asks PCIGraphics.card to enumerate boards.
 *
 * This is the same approach RadeonRX.chip uses (RXCardPatch).
 */

#include "chip/chip_state.h"

/* -----------------------------------------------------------------------
 * ROM tag names
 * ----------------------------------------------------------------------- */
static const char board_name[]  __attribute__((used)) = "VirtIOGPUBoard";
static const char board_idstr[] __attribute__((used)) =
    "$VER: VirtIOGPUBoard 1.1 (08.03.2026)\r\n";

/* -----------------------------------------------------------------------
 * Named memory structure -- persists PCI device pointer between the
 * board_init resident and the SetMethod hook callbacks.
 * ----------------------------------------------------------------------- */
struct VirtIOGPUPatch {
    struct PCIDevice *pciDev;       /* VirtIO GPU PCI device */
    APTR              orig_FindCard;  /* saved original FindCard method */
    APTR              orig_InitBoard; /* saved original InitBoard method */
};

/* Global pointer -- set during board_init, used by hook functions */
static struct VirtIOGPUPatch *g_patch = NULL;

/* Forward declaration */
static APTR board_init(void);

/* -----------------------------------------------------------------------
 * board_romtag -- VirtIOGPUBoard resident.
 * ----------------------------------------------------------------------- */
static const struct Resident board_romtag __attribute__((used, section(".rodata"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&board_romtag,
    (APTR)(&board_romtag + 1),
    RTF_NATIVE | RTF_COLDSTART,
    1,
    NT_RESOURCE,
    65,
    (CONST_STRPTR)board_name,
    (CONST_STRPTR)board_idstr,
    (APTR)board_init,
};

/* -----------------------------------------------------------------------
 * hook_FindCard -- SetMethod replacement for PCIGraphics.card offset 76.
 *
 * PCIGraphics.card's FindCard scans its hardcoded PCI device table.
 * Our hook runs first: if the BoardInfo doesn't already have a PCI device,
 * inject our VirtIO GPU device at bi+1630, then call the original.
 *
 * The original FindCard signature (from PCIGraphics.card disassembly):
 *   BOOL FindCard(struct Interface *Self, struct BoardInfo *bi)
 * ----------------------------------------------------------------------- */
static BOOL hook_FindCard(struct Interface *Self, struct BoardInfo *bi)
{
    typedef BOOL (*FindCardFunc)(struct Interface *, struct BoardInfo *);
    FindCardFunc origFunc = (FindCardFunc)g_patch->orig_FindCard;

    /* First call the original -- let PCIGraphics.card do its normal scan */
    BOOL result = origFunc(Self, bi);

    DBOARD_V("hook_FindCard: original returned %ld", (LONG)result);

    /* Inject our VirtIO GPU PCI device into the BoardInfo.  PCIGraphics.card
     * reads this field from its private extended area during InitBoard to
     * locate the PCI device passed to the .chip's FindCard vector (slot[4]).
     *
     * Unconditional injection -- even if the original FindCard found some
     * other device, we want our virtio-gpu-pci to be the one processed.
     *
     * Uses the boardinfo_set_pcidevice() accessor so the magic offset
     * (1630) is documented in one place (p96/boardinfo.h). */
    if (g_patch && g_patch->pciDev) {
        boardinfo_set_pcidevice(bi, g_patch->pciDev);
        DBOARD_V("hook_FindCard: injected pciDev=%p into BoardInfo", g_patch->pciDev);

        /* BoardName is displayed in screen-mode preferences and the mode
         * database.  This is a well-documented BoardInfo field (offset 16)
         * so we use the struct member directly. */
        bi->BoardName = (STRPTR)"VirtIOGPU";

        result = TRUE;
    }

    return result;
}

/* -----------------------------------------------------------------------
 * hook_InitBoard -- SetMethod replacement for PCIGraphics.card offset 80.
 *
 * After FindCard succeeds, PCIGraphics.card calls InitBoard to load the
 * .chip driver and call HardwareInit.  Our hook checks if the PCI device
 * at bi+1630 is our VirtIO GPU -- if so, we redirect to virtiogpu.chip.
 *
 * The original InitBoard signature:
 *   BOOL InitBoard(struct Interface *Self, struct BoardInfo *bi)
 * ----------------------------------------------------------------------- */
static BOOL hook_InitBoard(struct Interface *Self, struct BoardInfo *bi)
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;
    struct ExecIFace *IExec  = (struct ExecIFace *)SysBase->MainInterface;

    DBOARD_V("hook_InitBoard: bi=%p", bi);

    /* Check whether this BoardInfo has our VirtIO GPU as its PCI device --
     * the hook_FindCard injection above should have set it. */
    struct PCIDevice *pciDev = boardinfo_get_pcidevice(bi);

    if (pciDev && g_patch && pciDev == g_patch->pciDev) {
        DBOARD("hook_InitBoard: loading virtiogpu.chip");

        /* virtiogpu.chip is a pri -128 resident in the same binary -- it hasn't
         * been initialized yet at this point in boot.  Force-init it. */
        struct Library *chipLib = IExec->OpenLibrary("virtiogpu.chip", 0);
        if (!chipLib) {
            DBOARD_V("hook_InitBoard: OpenLibrary failed, trying FindResident");
            struct Resident *chipRes = IExec->FindResident("virtiogpu.chip");
            if (chipRes) {
                DBOARD_V("hook_InitBoard: FindResident(virtiogpu.chip) = %p", chipRes);
                IExec->InitResident(chipRes, 0);
                chipLib = IExec->OpenLibrary("virtiogpu.chip", 0);
            } else {
                DBOARD("hook_InitBoard: FindResident(virtiogpu.chip) FAILED");
            }
        }
        if (!chipLib) {
            DBOARD("hook_InitBoard: FAILED to open virtiogpu.chip");
            return FALSE;
        }

        /* Set bi->CardBase and bi->ChipBase BEFORE calling HardwareInit.
         * graphics.library dereferences these via ->lib_Node.ln_Name
         * (offset 10) to display the card/chip driver names.
         * In normal PCIGraphics.card flow, InitBoard sets these automatically.
         * Since we bypass that flow, we must set them ourselves.
         * CardBase = PCIGraphics.card library, ChipBase = chip library. */
        bi->CardBase = (APTR)Self->Data.LibBase;
        bi->ChipBase = (APTR)chipLib;
        DBOARD_V("hook_InitBoard: CardBase=%p ChipBase=%p", bi->CardBase, chipLib);

        struct Interface *chipIFace = IExec->GetInterface(chipLib, "main", 1, NULL);
        if (!chipIFace) {
            DBOARD("hook_InitBoard: FAILED to get chip interface");
            bi->ChipBase = NULL;
            IExec->CloseLibrary(chipLib);
            return FALSE;
        }

        /* Call HardwareInit = first user method of the "main" interface.
         * InterfaceData = 60 bytes, then Obtain/Release/Expunge/Clone = 16 bytes.
         * First user method at offset 76 = array index 19.
         * Signature: BOOL HardwareInit(struct Interface *Self,
         *                              struct BoardInfo *bi,
         *                              APTR cardDesc)
         * cardDesc = NULL since we're bypassing PCIGraphics.card's loader. */
        typedef BOOL (*HardwareInitFunc)(struct Interface *, struct BoardInfo *, APTR);
        APTR *vtable = (APTR *)chipIFace;
        HardwareInitFunc hwInit = (HardwareInitFunc)vtable[76 / 4];

        DBOARD_V("hook_InitBoard: calling HardwareInit (slot[4]=%p)", hwInit);
        BOOL ok = hwInit(chipIFace, bi, NULL);
        DBOARD("hook_InitBoard: HardwareInit returned %ld", (LONG)ok);

        IExec->DropInterface(chipIFace);
        /* Keep chipLib open -- chip needs to stay loaded */

        return ok;
    }

    /* Not our board -- call original InitBoard */
    DBOARD_V("hook_InitBoard: not our board, calling original");
    typedef BOOL (*InitBoardFunc)(struct Interface *, struct BoardInfo *);
    InitBoardFunc origFunc = (InitBoardFunc)g_patch->orig_InitBoard;
    return origFunc(Self, bi);
}

/* -----------------------------------------------------------------------
 * board_init -- VirtIOGPUBoard resident init function.
 *
 * Runs at pri 65 (before graphics.library at 63).
 * 1. Scan PCI for VirtIO GPU (1AF4:1050)
 * 2. Store PCI device in AllocNamedMemory for persistence
 * 3. Open PCIGraphics.card, get its "main" interface
 * 4. Hook FindCard (offset 76) and InitBoard (offset 80) via SetMethod
 * ----------------------------------------------------------------------- */
static APTR board_init(void)
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;
    struct ExecIFace *IExec  = (struct ExecIFace *)SysBase->MainInterface;
    g_IExec_early = IExec;

    DCHIP("=== VirtIOGPUBoard init (pri 65, SetMethod approach) ===");

    /* --- Step 1: Open expansion.library + PCI interface --- */
    struct Library *ExpBase = IExec->OpenLibrary("expansion.library", 50);
    if (!ExpBase) {
        DCHIP("Board: failed to open expansion.library");
        return NULL;
    }
    struct PCIIFace *IPCI = (struct PCIIFace *)
        IExec->GetInterface(ExpBase, "pci", 1, NULL);
    if (!IPCI) {
        DCHIP("Board: failed to get PCI interface");
        IExec->CloseLibrary(ExpBase);
        return NULL;
    }

    /* --- Step 2: Find VirtIO GPU PCI device 1AF4:1050 --- */
    struct PCIDevice *pciDev = IPCI->FindDeviceTags(
        FDT_VendorID, 0x1AF4,
        FDT_DeviceID, 0x1050,
        TAG_DONE);

    if (!pciDev) {
        DCHIP("Board: VirtIO GPU (1AF4:1050) not found -- no-op");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpBase);
        return NULL;
    }
    DCHIP_V("Board: VirtIO GPU found: pciDev=%p", pciDev);

    /* --- Step 3: Allocate named memory for patch data --- */
    g_patch = (struct VirtIOGPUPatch *)IExec->AllocNamedMemoryTags(
        sizeof(struct VirtIOGPUPatch),
        "resident", "VirtIOGPU",
        TAG_DONE);
    if (!g_patch) {
        /* Fallback: try regular allocation (named memory might not be available) */
        g_patch = (struct VirtIOGPUPatch *)IExec->AllocVecTags(
            sizeof(struct VirtIOGPUPatch),
            AVT_Type, MEMF_SHARED,
            AVT_ClearWithValue, 0,
            TAG_DONE);
    }
    if (!g_patch) {
        DCHIP("Board: failed to allocate patch data");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpBase);
        return NULL;
    }
    /* Zero the struct */
    g_patch->pciDev         = NULL;
    g_patch->orig_FindCard  = NULL;
    g_patch->orig_InitBoard = NULL;

    g_patch->pciDev = pciDev;
    DCHIP_V("Board: patch data at %p, pciDev=%p", g_patch, pciDev);

    /* --- Step 4: Force-init PCIGraphics.card via FindResident + InitResident ---
     * PCIGraphics.card is normally initialized INSIDE graphics.library (pri 63).
     * At pri 65 it hasn't been initialized yet, so OpenLibrary fails.
     * We must force it to initialize early so we can hook its methods.
     * This is what RadeonRX's RXCardPatch does. */
    struct Resident *pcigfx_res = IExec->FindResident("PCIGraphics.card");
    if (!pcigfx_res) {
        DCHIP("Board: FindResident(PCIGraphics.card) failed -- not in kickstart?");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpBase);
        return NULL;
    }
    DCHIP_V("Board: FindResident(PCIGraphics.card) = %p", pcigfx_res);

    /* Force-initialize it -- this creates the library and its interfaces */
    IExec->InitResident(pcigfx_res, 0);

    /* Now OpenLibrary should work */
    struct Library *pcigfxLib = IExec->OpenLibrary("PCIGraphics.card", 0);
    if (!pcigfxLib) {
        DCHIP("Board: OpenLibrary(PCIGraphics.card) STILL failed after InitResident");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpBase);
        return NULL;
    }
    DCHIP_V("Board: PCIGraphics.card opened: %p", pcigfxLib);

    struct Interface *pcigfx_iface = IExec->GetInterface(pcigfxLib, "main", 1, NULL);
    if (!pcigfx_iface) {
        DCHIP("Board: failed to get PCIGraphics.card 'main' interface");
        IExec->CloseLibrary(pcigfxLib);
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpBase);
        return NULL;
    }
    DCHIP_V("Board: PCIGraphics.card interface: %p", pcigfx_iface);

    /* --- Step 5: Save original methods and install hooks --- */

    /* Read current method pointers at offsets 76 and 80 */
    APTR *iface_vtable = (APTR *)pcigfx_iface;
    g_patch->orig_FindCard  = iface_vtable[76 / 4];
    g_patch->orig_InitBoard = iface_vtable[80 / 4];
    DCHIP_V("Board: saved orig FindCard=%p InitBoard=%p",
            g_patch->orig_FindCard, g_patch->orig_InitBoard);

    /* Install our hooks via SetMethod */
    IExec->SetMethod(pcigfx_iface, 76, (APTR)hook_FindCard);
    IExec->SetMethod(pcigfx_iface, 80, (APTR)hook_InitBoard);
    DCHIP_V("Board: SetMethod hooks installed at offsets 76 and 80");

    /* Drop interface but keep library open -- hooks must remain active */
    IExec->DropInterface(pcigfx_iface);
    /* Don't close pcigfxLib -- hooks reference its interface */

    /* Keep expansion.library and IPCI open -- pciDev must remain valid */

    DCHIP("=== VirtIOGPUBoard init SUCCESS -- hooks installed ===");
    return (APTR)1;
}
