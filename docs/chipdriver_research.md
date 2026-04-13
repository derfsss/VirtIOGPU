# Picasso96 Chip Driver Research

> **Status (v53.111+):** Foundational reference -- still accurate for the
> "what is a .chip file" / FindCard / InitCard protocol material.  For
> the **live BoardInfo extension-area offsets** see
> `aos4_boardinfo_offsets.md` (the canonical source-of-truth, kept in
> sync with the live `include/p96/boardinfo.h` accessors).  Anything in
> this file about specific BoardInfo offsets has been superseded by
> that file.



## Reference Binaries

Located in `projects/VirtIOGPU/` (excluded from git via `.gitignore`):

| File | Version | Date | Size |
|------|---------|------|------|
| `RadeonHD.chip` | 0.65 | 12.08.2016 | 1.5 MB |
| `RadeonHD.chip.debug` | 3.7 | 19.11.2019 | 1.7 MB |
| `RadeonRX.chip` | 2.11 | 26.01.2022 | 1.7 MB |
| `RadeonRX.chip.debug` | 2.11 | 26.01.2022 | 1.8 MB |
| `siliconmotion502.chip` | 53.12 | 22.07.2023 | 24 KB |

All are PPC ELF (big-endian), statically linked, GCC/adtools compiled.

---

## What a .chip File Is

A Picasso96 **chip driver** — a plugin loaded by `PCIGraphics.card` that provides
hardware-specific GPU functions to the Picasso96 RTG system.

**Key characteristics:**
- An AmigaOS4 library (NT_LIBRARY, RTF_NATIVE|RTF_AUTOINIT, version 53)
- Must have both `__library` (manager) and `main` interfaces via `CLT_Interfaces`
- `PCIGraphics.card` calls `OpenLibrary`, then `GetInterface("main")`, then `slot[4]`
- `slot[4]` of the main interface is `FindCard` — the hardware probe function
- Compiled with `-mcrt=newlib` (required for AmigaOS4 library infrastructure)

---

## PCIGraphics.card Chip Loading Protocol (confirmed from disassembly)

### Exact call sequence

1. **PCI device table** (hardcoded in PCIGraphics.card rodata):
   - Maps `{VendorID:DeviceID}` → chip filename
   - Includes SM501 (126f:0501) → `"SiliconMotion502.chip"`
   - Does NOT include VirtIO GPU (1af4:1050) — must masquerade as SM502

2. **`IExec->OpenLibrary("SiliconMotion502.chip", 0)`** at method offset [424]:
   - Chip must be a proper AmigaOS4 library (RTF_NATIVE|RTF_AUTOINIT, NT_LIBRARY)
   - Must have CLT_DataSize, CLT_Interfaces, CLT_InitFunc in init tags

3. **`IExec->GetInterface(chip_lib, "main", 1, NULL)`** at method offset [448]:
   - Returns NULL if `__library` manager interface is missing → immediate failure
   - Returns NULL if CLT_Interfaces is wrong type → immediate failure

4. **`chip_interface->slot[4](chip_interface, bi, cardDesc)`**:
   - Offset 76 from start of interface (InterfaceData=60 bytes + 4 slots × 4 bytes)
   - This is `FindCard` — returns non-NULL if hardware found
   - PCIGraphics.card calls `DropInterface` immediately after

5. **`IExec->CloseLibrary(chip_lib)`**

### InterfaceData size calculation

- struct Node: ln_Succ(4)+ln_Pred(4)+ln_Type(1)+ln_Pri(1)+[pad 2]+ln_Name(4) = 16 bytes
- LibBase(4) + RefCount(4) + Version(4) + Flags(4) + CheckSum(4) + PositiveSize(4) +
  NegativeSize(4) + IExecPrivate(4) + EnvironmentVector(4) + Reserved3(4) + Reserved4(4) = 44 bytes
- **Total InterfaceData = 60 bytes**
- Obtain=offset 60, Release=64, Expunge=68, Clone=72, **slot4=76**

### What slot[4] (FindCard) receives

```c
static APTR chip_FindCard(struct Interface *Self,
                           struct BoardInfo *bi,
                           APTR cardDesc)
```

- `bi` — the BoardInfo struct to fill (chip fills vtable, board fields)
- `cardDesc` — PCIGraphics.card internal object (**see warning below**)
- Returns non-NULL = hardware found; NULL = not found

---

## CRITICAL: cardDesc is NOT a flat APTR vtable

Early research assumed `cardDesc` could be accessed as `APTR *vt` with byte
offsets from SM502 disassembly (e.g. `vt[424/4]`, `vt[704/4]`). **This is wrong.**

**Confirmed by boot log crash:**
```
[virtiogpu.chip] calling method[424] (OpenLib) vt[424/4]=0xFFFFFFFF
Instruction pointer: 0xFFFFFFFC
ISI exception: Instruction fetch in non-execute segment
```

`vt[424/4]` = `0xFFFFFFFF` = vtable sentinel. PCIGraphics.card's `cardDesc`
is a C++ object with a much shorter vtable than we assumed.

**Fix:** Do not call any cardDesc methods. Set BoardInfo fields directly
using AmigaOS4 APIs:
- `bi->ExecBase` ← AbsExecBase (address 4)
- `bi->UtilBase` ← `IExec->OpenLibrary("utility.library", 0)`
- `bi->CardBase` ← non-NULL placeholder (SysBase works)

The SM502 chip called these cardDesc methods to initialise SM501-specific
hardware. Our VirtIO chip has no such hardware; we do VirtIO PCI init directly.

---

## Required Interface Structure

```c
/* Manager interface (__library) — required for GetInterface to work */
static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    NULL, NULL,
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    NULL,
    (APTR)-1
};
static const struct TagItem _manager_Tags[] = {
    { MIT_Name,        (Tag)"__library"      },
    { MIT_VectorTable, (Tag)_manager_Vectors },
    { MIT_Version,     1                     },
    { TAG_DONE,        0                     }
};

/* Main interface — slot[4] = FindCard */
static const APTR _chip_main_Vectors[] = {
    (APTR)_main_Obtain,
    (APTR)_main_Release,
    NULL, NULL,
    (APTR)chip_FindCard,   /* slot[4] ← PCIGraphics.card calls this */
    (APTR)-1
};
static const struct TagItem _chip_main_Tags[] = {
    { MIT_Name,        (Tag)"main"               },
    { MIT_VectorTable, (Tag)_chip_main_Vectors   },
    { MIT_Version,     1                         },
    { TAG_DONE,        0                         }
};

/* CLT_Interfaces array — MUST be ULONG[], not CONST_APTR[] */
static const ULONG _chip_Interfaces[] = {
    (ULONG)_manager_Tags,
    (ULONG)_chip_main_Tags,
    0
};
```

### CLT_* init tags

```c
static const struct TagItem _chip_InitTags[] = {
    { CLT_DataSize,      sizeof(struct Library) },
    { CLT_Interfaces,    (Tag)_chip_Interfaces  },
    { CLT_InitFunc,      (Tag)_chip_lib_Init    },
    { CLT_NoLegacyIFace, TRUE                   },
    { TAG_DONE,          0                      }
};
```

### _chip_lib_Init must capture IExec

```c
static struct Library *_chip_lib_Init(struct Library *libBase,
                                      ULONG seglist,
                                      struct Interface *exec)
{
    (void)seglist;
    if (exec)
        g_IExec_early = (struct ExecIFace *)exec;  /* save for early debug */
    libBase->lib_Node.ln_Type = NT_LIBRARY;
    libBase->lib_Node.ln_Name = (char *)chip_name;
    libBase->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->lib_Version      = 53;
    libBase->lib_Revision     = 1;
    libBase->lib_IdString     = (APTR)chip_idstr;
    return libBase;
}
```

The `exec` parameter is the IExec interface pointer passed by AmigaOS4
RTF_AUTOINIT machinery. Must be saved before it can be used for debug prints.

---

## Makefile Requirements

```makefile
CHIP_CFLAGS  = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns -DDEBUG \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__
CHIP_LDFLAGS = -mcrt=newlib -nostartfiles
```

`-mcrt=newlib` is required — without it, AmigaOS4 library/interface infrastructure
(CLT_* processing, TOC pointer, GetInterface machinery) does not initialise correctly.

---

## Kicklayout Deployment

### Masquerade strategy

The chip's internal name (`lib_Node.ln_Name`) must be `"SiliconMotion502.chip"`.
It is placed in the Kickstart zip as `Kickstart/siliconmotion502.chip`.

```
# Kicklayout order (critical):
MODULE Kickstart/virtiogpu.device     ← before PCIGraphics.card if using separate device
MODULE Kickstart/PCIGraphics.card
...
MODULE Kickstart/siliconmotion502.chip  ← our chip, masquerading as SM502
;MODULE Kickstart/virtiogpu.chip        ← disabled (old name)
```

### Load order matters

PCIGraphics.card is a resident; it initialises when exec processes residents.
If `virtiogpu.device` is loaded after `PCIGraphics.card`, it will not be in
exec's device list when `chip_FindCard` calls `OpenDevice("virtiogpu.device")`.

### Current approach: embed VirtIO init in chip

Rather than depending on `virtiogpu.device` (timing issues), the chip now
performs VirtIO GPU PCI init directly. `OpenDevice` is not needed.

---

## boardinfo.h (BoardInfo vtable layout)

Reconstructed from binary analysis of `siliconmotion502.chip`, `RadeonHD.chip.debug`,
and `RadeonRX.chip.debug`. See `include/p96/boardinfo.h`.

Key fields set by chip driver:
- `bi->BoardName` = `"VirtIOGPU"`
- `bi->BoardType` = `BT_uaegfx`
- `bi->MemoryBase` = framebuffer virtual address
- `bi->MemorySize` = `stride * height`
- `bi->CardBase` = non-NULL (required by PCIGraphics.card)
- `bi->ExecBase` = SysBase
- `bi->UtilBase` = utility.library base
- `bi->RGBFormats` = `RGBFF_B8G8R8A8 | RGBFF_A8R8G8B8`
- `bi->Flags` |= `BIF_HARDWARESPRITE | BIF_GRANTDIRECTACCESS | BIF_INDISPLAYCHAIN`

MAXMODES = 5 (index 1 and 2 hold valid width/height; others = 0).

---

## SM502 Chip Analysis Notes (for historical reference)

From disassembly of `siliconmotion502.chip` 53.12:

- `_start+0` = `FindCard` body (returns 20, not 1)
- `_start+8` = `InitCard` body (full code, not a jump)
- SM502 does NOT fill bi vtable (fp[0..67]) — PCIGraphics.card fills these from
  chip's rodata descriptors
- `bi->ChipBase` (+56) = cardDesc
- `bi->ExecBase` (+60) = SysBase
- `bi->UtilBase` (+64) = IUtility
- `bi->CardBase` (+52) = video_mem_object->field[20]
- offset +48 = VBIName[28..31] — SM502-private temporary storage

SM502's `chip_open_library` (offset 0x4170) calls:
- cardDesc method[304] → hardware probe (SM501-specific)
- cardDesc method[704] → open video memory interface (SM501-specific)
- cardDesc method[504] → capability check
- cardDesc method[448] → get IUtility

None of these are appropriate for a virtual GPU. We skip all cardDesc callbacks.
