# PCIGraphics.card Reverse Engineering Analysis

> **Status (v53.111+):** Historical artifact from the v53.10 DSI-crash
> investigation trail, plus early reverse-engineering of PCIGraphics.card
> internals.  Useful when triaging similar crashes or untangling
> PCIGraphics.card init order.
>
> **For current BoardInfo offsets, vtable slots, and AOS4-specific
> reorganisations** see `aos4_boardinfo_offsets.md`.  That file is kept
> in sync with the live `include/p96/boardinfo.h` accessor inlines and
> incorporates the verified-via-RadeonRX/graphics.library.kmod findings.
> Anything in this file about BoardInfo extension-area layout has been
> superseded.



## Binary Information
- File: `projects/VirtIOGPU/PCIGraphics.card`
- Version: 53.18 (16.11.2019)
- Format: ELF32 PPC Big-Endian, `-mcrt=newlib`, statically linked
- Entry `_start` at `0x0100050c` = just `blr` (not called by chip loader)

## Memory Layout
- `.text` starts at `0x01000074`
- `.rodata` starts at `0x01002000`
- `.data` starts at `0x010182b4`
- `.sbss` starts at `0x010183ec` (size 0x24 = 36 bytes = 9 words)
- `.bss` starts at `0x01018410` (size 0x190 = 400 bytes)
- `_SDA_BASE_` = `0x010203ec`

## SBSS Global Variable Map
All accessed via `lis rX,258` + signed offset:

| SBSS Offset | Address | Variable Name | Value Set By |
|-------------|---------|---------------|--------------|
| +0x00 | 0x010183ec | g_IGraphics2? (3rd GetInterface) | `stw r3,-31764(r9)` in init |
| +0x04 | 0x010183f0 | g_IExpansion? (2nd GetInterface) | `stw r3,-31760(r26)` in init |
| +0x08 | 0x010183f4 | g_IGraphics? (1st GetInterface) | `stw r3,-31756(r27)` in init |
| +0x0c | 0x010183f8 | **g_IExec** | `stw r5,-31752(r31)` in init |
| +0x10 | 0x010183fc | g_LibBase3? (3rd OpenLibrary) | `stw r3,-31748(r29)` in init |
| +0x14 | 0x01018400 | g_LibBase2? (2nd OpenLibrary) | `stw r3,-31744(r30)` in init |
| +0x18 | 0x01018404 | g_LibBase1? (1st OpenLibrary) | `stw r3,-31740(r27)` in init |
| +0x1c | 0x01018408 | g_ExecLibBase? | `stw r10,-31736(r9)` in init (= IExec->field[16]) |
| +0x20 | 0x0101840c | g_boardDesc? | `stw r25,-31732(r9)` before FindCard call |
| +0x24 | 0x01018410 | BSS start (g_boardSlotArray) | (BSS) |

## BSS Global Variable Map (starts at 0x01018410)
| BSS Offset | Contents |
|------------|----------|
| +0x00 | g_maxBoards (board count limit) |
| +0x04 | g_currentBoardSlot (zeroed after CopyMem) |
| +0x08..+0x187 | Copied chip interface (384 bytes from chip's "main" interface) |

## Library Names in .rodata
- `0x0100201c` = `"utility.library"`
- `0x0100202c` = `"expansion.library"`
- `0x01002040` = `"newlib.library"`
- `0x01002050` = `"main"` (chip interface name)
- `0x01002058` = `"pci"` (PCI interface name)
- `0x0100205b` = `"PCIGraphics.card"` (library name)
- `0x01002068` = `"PCIGraphics.card 53.18 (16.11.2019)"` (version string)
- `0x01002098` = `"__library"` (manager interface name)

## PCIGraphics.card Interface Vectors (in .data)
At `0x01018308`:
- `0x01000130` = Obtain (slot[0])
- `0x01000144` = Release (slot[1])
- (2 NULLs for Expunge/Clone)
- `0x0100000c` = ??? (another interface slot)
At `0x01018318`:
- `0x01000c2c` = PCIGraphics.card slot[4] (AddVideoMode-equiv)
- `0x01000dc0` = PCIGraphics.card slot[5] (InitBoard-equiv)
- `0xFFFFFFFF` = sentinel

## Key Function Map
| Address | Function |
|---------|----------|
| `0x01000074` | Obtain (interface manager) |
| `0x01000098` | Release (interface manager) |
| `0x01000130` | Obtain (main interface) |
| `0x01000144` | Release (main interface) |
| `0x01000158` | **Main init function** (`CLT_InitFunc` equivalent) — called by exec RTF machinery |
| `0x01000598` | Called on FindCard success path |
| `0x01000a6c` | Legacy chip loader (for version ≤ 53 path) |
| `0x01000b38` | **Modern chip loader** (OpenLibrary + GetInterface + CopyMem path) |
| `0x01000c2c` | AddVideoMode / board slot registration |
| `0x01000dc0` | Board init / chip vtable traversal |

## Chip Loading Protocol — CONFIRMED from disassembly

### Init function at `0x01000158`
Called by exec RTF machinery with args:
- `r3` = libBase (PCIGraphics.card library base)
- `r4` = boardDesc (PCIGraphics.card's private per-board object)
- `r5` = IExec interface pointer

Sequence:
1. Store `IExec` → `sbss+0x0c`
2. Store `IExec->field[16]` (exec.library base) → `sbss+0x1c`
3. `OpenLibrary("utility.library", 51)` → store → `sbss+0x18`
4. `OpenLibrary("expansion.library", 51)` → store → `sbss+0x14`
5. `OpenLibrary("newlib.library", 0)` → store → `sbss+0x10`
6. `GetInterface(lib1, "main", 1, NULL)` → store → `sbss+0x08`
7. `GetInterface(lib2, "main", 1, NULL)` → store → `sbss+0x04`
8. `GetInterface(lib3, "main", 1, NULL)` → store → `sbss+0x00`
9. Set version marker: `sth r10,22(r28)` (r10=18, r28=boardDesc → writes 18 at boardDesc+22)
10. Store boardDesc → `sbss+0x20`
11. `bl 0x01000b38` — **call modern chip loader**
12. If success (returns 1): jump to `0x010003b8` (success exit)
13. If fail: call `0x01000598`, call `0x01000a6c`, then success exit

### Modern chip loader at `0x01000b38`
Called: no args needed (uses sbss globals).
Returns: 1=success, 0=fail.

1. Load `r10 = g_IExec->field[16]` = exec.library base
2. Load `r9 = exec.lib_Version` (halfword at exec.library+20)
3. **Version check**: if r9 == 51 → jump to 0x1000bf0; if ≤ 51 → fail (0x1000bd0); if > 51 → continue
4. `r9 = g_IExec->method[968/4]` → call with args:
   - r3 = g_IExec (Self)
   - r4 = rodata `"SiliconMotion502.chip"` (chip filename, addr `0x010072000`)
   - r5 = rodata `"main"` (interface name, addr varies)
   - Result in r4 = chip's "main" interface pointer
5. If r4==NULL → fail (0x1000bc8)
6. `r9 = g_IExec->method[124/4]` = **IExec->CopyMem**
7. Call `IExec->CopyMem(chip_interface, BSS+8, 384)` — copies chip interface
8. Store 0 → BSS+4
9. Return 1 (success)

**KEY INSIGHT**: PCIGraphics.card does NOT call `chip_FindCard` (slot[4]) here!
It only COPIES the chip's main interface into BSS. FindCard is called later via
a different mechanism (probably through the copied vtable in `0x1000dc0`).

### `g_IExec->method[968/4]` — the "chip opener" (modern loader `0x1000b38`)
Offset 968 from IExec interface start. Slot 242.
This is a late exec.library method — probably a chip-loading helper that opens the
chip library and returns its "main" interface directly.
Arguments: `(g_IExec, chip_filename, interface_name)` → returns interface ptr or NULL.

**NOTE**: Our current SDK has different slot ordering from 2019 SDK (PCIGraphics.card
was compiled with a 2019 exec.h where these late slots differ). Offset is correct
from the binary; function name identification requires cross-referencing args.

### `g_IExec->method[960/4]` = IExec->**AllocNamedMemory** (legacy loader `0x1000a6c`)
Offset 960 from IExec interface start (2019 exec.h slot 240 = AllocNamedMemory).
**CONFIRMED from args**: PCIGraphics.card calls:
```
IExec->AllocNamedMemory(384, "resident", "pcigraphics.card", NULL)
```
Allocates a 384-byte persistent block in exec's named memory, namespace="resident",
name="pcigraphics.card". This is used to share/persist data across chip reloads.
384 bytes = same size as the chip interface copy in BSS (the vtable copy).

Our current SDK has `AllocNamedMemory` at offset 896 (slot 224) — the 2019 SDK had
it 16 slots later at 960. The slot layout diverged between 2019 and our SDK version.

### `g_IExec->method[936/4]` = IExec->**FindNamedMemory** (likely)
Offset 936 from IExec interface start (2019 exec.h ~slot 234).
Likely `FindNamedMemory(Self, space, name)` — checks if named block already exists
before allocating. Consistent with pattern: check first, alloc if not found.

### `g_IExec->method[124/4]` = IExec->**CopyMem**
InterfaceData=60 bytes. Offset 124: slot `(124-60)/4 = 16` from Obtain.
From `interfaces/exec.h` (alphabetical order):
Obtain(0), Release(1), Expunge(2), Clone(3), AddHead(4), AddMemHandler(5),
AddMemList(6), AddTail(7), AllocAbs(8), Allocate(9), AllocEntry(10), AllocMem(11),
AllocPooled(12), AllocVec(13), AllocVecPooled(14), AvailMem(15), **CopyMem(16)** ← offset 124

### IMMU Interface access ("mmu" string at `0x010082B0`)
PCIGraphics.card contains the string `"mmu"` (at address `0x010082B0` in .rodata)
which it uses to `GetInterface(execBase, "mmu", 1, NULL)` to obtain the MMUIFace.

**MMUIFace layout** (from `interfaces/exec.h`, struct `MMUIFace`):

| Offset | Slot | Function | Signature |
|--------|------|----------|-----------|
| 0 | 0 | Obtain | `(Self)` |
| 4 | 1 | Release | `(Self)` |
| 8 | 2 | Expunge_UNIMPLEMENTED | (APTR) |
| 12 | 3 | Clone_UNIMPLEMENTED | (APTR) |
| **16** | **4** | **MapMemory** | `(Self, virt, phys, length, attrib)` |
| **20** | **5** | **UnmapMemory** | `(Self, virt, length)` |
| **24** | **6** | **RemapMemory** | `(Self, virt, phys, length, attrib)` |
| **28** | **7** | **SetMemoryAttrs** | `(Self, virt, length, attrib)` |
| **32** | **8** | **GetMemoryAttrs** | `(Self, virt, flags)` |
| **36** | **9** | **GetPhysicalAddress** | `(Self, virt) → phys` |

PCIGraphics.card uses IMMU to map the GPU framebuffer physical address into virtual
address space (MapMemory) and to get physical addresses of allocated buffers
(GetPhysicalAddress). This is required for setting up the GPU's scanout base address.

## BoardInfo Field Offsets (CONFIRMED from compiler)
Compiled with `#pragma pack(2)`, `ppc-amigaos-gcc -I sdk/include_h`:

| Field | Offset |
|-------|--------|
| MemoryBase | 0x04 = 4 |
| MemorySize | 0x0c = 12 |
| BoardName | 0x10 = 16 |
| CardBase | 0x34 = 52 |
| ExecBase | 0x3c = 60 |
| BoardLock | 0x70 = 112 |
| ResolutionsList | 0x9e = 158 |
| Flags | 0xba = 186 |
| SpecialFeatures | 0x222 = 546 |
| sizeof(BoardInfo) | 0x58a = 1418 |

These MATCH the debug log:
- `bi->ResolutionsList.mlh_Head=0x6FFA40A2` → bi+0x9e+4 = bi+0xa2 ✓
- `bi->SpecialFeatures.mlh_Head=0x6FFA4226` → bi+0x222+4 = bi+0x226 ✓

## The DSI Crash — CONFIRMED Facts (v53.13/v53.14)

**Crash**: `kernel+0x1B99C` (= `0x0181B99C`), DAR=0x00000000, LR=0x0181BA7C
**Crashed task**: `idle.task` (v53.12) or `UHCI Controller Task Unit 1` (v53.14) — varies by timing
**Registers at crash**: `r3=bi`, `r16=bi+4`, `r22=bi+0x20`
**Kernel IP**: same across all builds — the scheduler's list traversal loop.
This is NOT a crash inside our chip code — the crash happens later, in the kernel scheduler.

### CONFIRMED from v53.13 BL@ diagnostic output

`bi->BoardLock.ss_Link.ln_Succ` (bi+112) = **0x00000000 at chip_InitCard_C ENTRY**.
PCIGraphics.card has NOT called `AddSemaphore(&bi->BoardLock)` yet — it calls it AFTER
chip_FindCard returns. So the BoardLock issue is a red herring.

The `bi[0]` (= `bi->RegisterBase` = offset 0) trajectory:
- **At chip_FindCard ENTRY**: `0x84000000` (GPU PCI MMIO BAR — pre-filled by PCIGraphics.card)
- **All through chip_InitCard_C** (ENTRY, after AllocVec, after DMA, after virtio, after FB, after GPU cmds): `0x84000000` — WE NEVER WRITE IT
- **After bi fill** (v53.13): `0x00000000` — **WE WROTE IT** via `bi->RegisterBase = NULL`

**Root cause of first DSI (v53.12/v53.13)**: We wrote `bi->RegisterBase = NULL` (line
`bi->RegisterBase = NULL` in the bi-fill section). PCIGraphics.card's post-FindCard code
loads from `bi[0]` as a pointer (the PCI BAR address). After we set it to NULL, the
code follows NULL → DAR=0x00000000 → crash.

**v53.14 fix**: Removed `bi->RegisterBase = NULL`. Now `bi[0]` stays `0x84000000`.
After fix: `BL@after bi fill: bi[0]=84000000` — confirmed preserved.

### Stack pointer confirmed OK

`sp=0x02080160` at chip_FindCard entry, bi at `0x6FFA4000`. Stack overflow **ruled out**.

### `bi` contents at chip_FindCard entry (every boot, pre-filled by PCIGraphics.card):
```
bi[0] = RegisterBase = 0x84000000  ← GPU PCI MMIO BAR (BAR4 base)
bi[1] = MemoryBase   = 0x80000000  ← GPU PCI memory BAR (BAR0/1 base — 256MB window)
bi[2] = MemoryIOBase = 0x00000000  ← (zero)
bi[3] = MemorySize   = 0x04000000  ← 64MB (GPU RAM size)
```
PCIGraphics.card pre-fills `bi->RegisterBase` and `bi->MemoryBase` with PCI BAR addresses.
These must be **preserved** (not overwritten) — PCIGraphics.card's post-FindCard code uses them.

### v53.14 crash — still crashing, different task

After preserving `bi->RegisterBase`, the crash still occurs at same kernel IP, same DAR=0,
same r3=bi, but crashed task changed to `UHCI Controller Task Unit 1` (timing difference).
This means **there is a second NULL pointer in bi** that the scheduler follows.

The scheduler is treating `bi` as a list node (`struct Node`). It follows:
1. `bi->RegisterBase` = 0x84000000 (PCI MMIO — NOW PRESERVED ✓)
2. Reads word at 0x84000000 → gets some value → follows next node pointer
3. Eventually gets back to bi or a bi-adjacent node
4. Reads a NULL pointer from bi → DAR=0 → crash

**Likely culprit**: `bi->MemoryBase` (offset 4 = `ln_Pred` of a Node overlay). We
overwrite this with `gs->fb_mem` (e.g. `0x6FB1F000`). PCIGraphics.card pre-filled it
as `0x80000000` (GPU 256MB BAR). If PCIGraphics.card uses `bi->MemoryBase` as a node
link pointer, overwriting it corrupts the list.

**Note on 256MB RAM**: AmigaOS4 gfx cards can access up to 256MB RAM. `bi[1]=0x80000000`
represents the 256MB PCI memory BAR window, not just frame buffer size. Our chip correctly
sets `bi->MemoryBase` to the framebuffer virtual address for Picasso96, but PCIGraphics.card
may need the original BAR value preserved for its own use.

### Next investigation

Try preserving `bi->MemoryBase` as well (do NOT overwrite it with `gs->fb_mem`).
PCIGraphics.card's post-FindCard code uses `bi->MemoryBase` (bi+4) as a pointer.
Picasso96 gets `MemoryBase` from the filled BoardInfo — if PCIGraphics.card overwrites
it again after our init, we may need a different approach.

Also: look at what PCIGraphics.card does **after** chip_FindCard returns — specifically
what list it adds bi to and whether bi is used as a Task node.

## Comparison: SM502 vs VirtIOGPU chip behavior

| Aspect | SM502 53.12 | VirtIOGPU 53.12 |
|--------|-------------|-----------------|
| FindCard return | 20 (integer!) | &chip_found_sentinel |
| InitCard return | bi pointer | bi |
| bi->ln_Succ at FindCard entry | unknown | 0x84000000 (PCI BAR!) |
| vtable fill | Does NOT fill bi->fp[] | Fills all 68 slots |
| InitSemaphore | Never called | Not called (gs->io_lock only) |
| NewMinList | Never called | Not called |
| SpecialFeatures | Not touched | Not touched |

---

## Definitive IExec Interface Offset Table

**Critical**: IExec has `Expunge_UNIMPLEMENTED` and `Clone_UNIMPLEMENTED` as plain `APTR`
(not `APICALL`) at slots 2 and 3. Grepping for `APICALL` misses these → all offsets
computed from APICALL-grep alone are **wrong by 8**. Use this table directly.

Each slot = 4 bytes. `offset = slot × 4`.

| Offset | Slot | Function |
|--------|------|----------|
| 0 | 0 | Obtain |
| 4 | 1 | Release |
| 8 | 2 | Expunge_UNIMPLEMENTED (APTR) |
| 12 | 3 | Clone_UNIMPLEMENTED (APTR) |
| 16 | 4 | AddHead |
| 20 | 5 | AddMemHandler |
| 24 | 6 | AddMemList |
| 28 | 7 | AddTail |
| 32 | 8 | AllocAbs |
| 36 | 9 | Allocate |
| 40 | 10 | AllocEntry |
| 44 | 11 | AllocMem |
| 48 | 12 | AllocPooled |
| 52 | 13 | AllocVec |
| 56 | 14 | AllocVecPooled |
| 60 | 15 | AvailMem |
| 64 | 16 | CopyMem |
| 68 | 17 | CopyMemQuick |
| 72 | 18 | CreatePool |
| 76 | 19 | Deallocate |
| 80 | 20 | DeletePool |
| 84 | 21 | Enqueue |
| 88 | 22 | FindName |
| 92 | 23 | FindIName |
| 96 | 24 | Forbid |
| 100 | 25 | FreeEntry |
| 104 | 26 | FreeMem |
| 108 | 27 | FreePooled |
| 112 | 28 | FreeVec |
| 116 | 29 | FreeVecPooled |
| 120 | 30 | InitData |
| 124 | 31 | InitStruct |
| 128 | 32 | Insert |
| 132 | 33 | MakeInterface |
| 136 | 34 | MakeInterfaceTags |
| 140 | 35 | Permit |
| 144 | 36 | RawDoFmt |
| 148 | 37 | RemHead |
| 152 | 38 | RemMemHandler |
| 156 | 39 | Remove |
| 160 | 40 | RemTail |
| 164 | 41 | TypeOfMem |
| 168 | 42 | InitResident |
| 172 | 43 | InitCode |
| 176 | 44 | SumKickData |
| **180** | **45** | **AddTask** |
| 184 | 46 | AddTaskTags |
| 188 | 47 | Disable |
| 192 | 48 | Enable |
| 196 | 49 | Reschedule |
| 200 | 50 | FindTask |
| 204 | 51 | RemTask |
| 208 | 52 | SetTaskPri |
| 212 | 53 | StackSwap |
| 216 | 54 | AllocSignal |
| 220 | 55 | FreeSignal |
| 224 | 56 | SetExcept |
| 228 | 57 | SetSignal |
| 232 | 58 | Signal |
| 236 | 59 | Wait |
| 240 | 60 | AddPort |
| 244 | 61 | CreatePort |
| 248 | 62 | CreateMsgPort |
| 252 | 63 | DeletePort |
| 256 | 64 | DeleteMsgPort |
| 260 | 65 | FindPort |
| 264 | 66 | GetMsg |
| 268 | 67 | PutMsg |
| 272 | 68 | RemPort |
| 276 | 69 | ReplyMsg |
| 280 | 70 | WaitPort |
| ... | ... | ... |
| 424 | 106 | DeleteLibrary |
| 428 | 107 | SetFunction |
| 448 | 112 | CloseDevice |
| 456 | 114 | DeleteIORequest |
| 472 | 118 | SendIO |
| 484 | 121 | AddResource |
| 548 | 137 | AVL_AddNode |
| 552 | 138 | AVL_FindFirstNode |
| 556 | 139 | AVL_FindLastNode |
| 576 | 144 | AVL_FindPrevNodeByKey |
| 580 | 145 | AVL_RemNodeByAddress |
| 744 | 186 | GetTail |
| 748 | 187 | GetSucc |

**NOTE**: IExec at offset 960+ enters the DebugIFace extension area (same interface struct
in AmigaOS4 exec, continued past slot 239). PCIGraphics.card at `0x1000acc` uses
`960(r10)` where r10 = something other than IExec (likely a PCI device interface).

---

## PCIGraphics.card Exec Calls — CONFIRMED from disassembly

From `lwz rN,offset(rM)` + `mtctr rN` + `bctrl` sequences:

### In board init / `0x01000dc0` onwards:

| PC offset | Offset | Function | Notes |
|-----------|--------|----------|-------|
| `0x10000c8` | 216 | **AllocSignal** | Signal allocation at early init |
| `0x10000e4` | 484 | **AddResource** | Adds something to exec resource list |
| `0x10001a4` | 424 | **DeleteLibrary** | (via r5, different reg) |
| `0x10001ec` | 424 | **DeleteLibrary** | |
| `0x1000218` | 424 | **DeleteLibrary** | |
| `0x1000244`-`0x10002b4` | 448 | **CloseDevice** | Closes a device |
| `0x100031c`-`0x1000340` | 456 | **DeleteIORequest** | Cleanup path |
| `0x1000364`-`0x1000388` | 428 | **SetFunction** | Patches library vectors |
| `0x10003ac` | 484 | **AddResource** | |
| `0x10004c8` | 216 | **AllocSignal** | |
| `0x10004e4` | 484 | **AddResource** | |
| `0x1000528`-`0x10006fc` | 80 | **DeletePool** | Memory pool cleanup |
| `0x1000648` | 168(r9) | method on different object | Not IExec |
| `0x1000718`-`0x1000848` | 80(r14) | **DeletePool** on r14 | |
| `0x1000754` | 124 | **InitStruct** | |
| `0x1000784` | 84 | **Enqueue** | Adds node to list |
| `0x10007b0` | 80 | **DeletePool** | |
| `0x10008cc` | 428 | **SetFunction** | |
| `0x10009dc`-`0x1000a18` | 748 | **GetSucc** | List traversal |
| `0x1000a30` | 428 | **SetFunction** | |
| `0x1000acc` | 960(r10) | ? on r10 (not IExec) | |
| `0x1000e34` | 272 | **RemPort** | |
| `0x1000e5c` | 268 | **PutMsg** | Sends message to port |
| `0x1000ec0` | 472 | **SendIO** | **Async IO!** |
| `0x1000edc` | 228 | **SetSignal** | |
| `0x1000f18` | 424 | **DeleteLibrary** | |
| `0x10011a8` | 744 | **GetTail** | |
| `0x1001238` | 424 | **DeleteLibrary** | |
| `0x1001264` | 448 | **CloseDevice** | |
| `0x1001298` | 456 | **DeleteIORequest** | |
| `0x10012b4` | 428 | **SetFunction** | |
| `0x1001410` | 456 | **DeleteIORequest** | |
| `0x1001428` | 428 | **SetFunction** | |
| `0x1001510` | 76(r15) | on r15 (not IExec) | |
| `0x100154c` | 80 | **DeletePool** | |
| `0x1001710`-`0x1001750` | 128(r31) | **Insert** on r31 | |
| `0x1001734` | 456 | **DeleteIORequest** | |
| `0x1001780` | 936(r10) | ? on r10 | |
| `0x100185c` | 576 | **AVL_FindPrevNodeByKey** | |
| `0x1001894` | 580 | **AVL_RemNodeByAddress** | |
| `0x10018b0` | 124 | **InitStruct** | |
| `0x10018c0` | 576 | **AVL_FindPrevNodeByKey** | |
| `0x10018f8` | 580 | **AVL_RemNodeByAddress** | |
| `0x100194c`-`0x10019e8` | 456 | **DeleteIORequest** | |
| `0x1001a18`-`0x1001a5c` | 76 | **Deallocate** | |
| `0x1001a34`-`0x1001a80` | 80 | **DeletePool** | |

### Key observations:
- **PCIGraphics.card calls `SendIO` (offset 472)** — it sends async IO to a device
- **PCIGraphics.card calls `SetFunction` (offset 428)** — it patches library vectors
- **No `AddTask` (offset 180) calls found** — PCIGraphics.card does NOT create tasks
- **No `NewMinList`, `InitSemaphore` calls** — PCIGraphics.card manages these itself
- **`GetSucc`/`GetTail` (744/748)** — traverses exec lists
- **`Enqueue` (84)** — adds nodes to sorted lists
- **`Insert` (128)** — inserts nodes into lists

### Calls inside `0x1000dc0` (board init/vtable traversal) — detailed

The function at `0x1000dc0` is the board init that calls chip_FindCard and chip_InitCard.
Key exec calls inside it (all via g_IExec at sbss+0x0c unless noted):

| ASM line | Offset | Object | Function | Notes |
|----------|--------|--------|----------|-------|
| `0x1000e34` | 272 | **g_IGraphics** (sbss+0x08) | RemPort | Removes a message port |
| `0x1000e5c` | 268 | **g_IGraphics** | PutMsg | Sends a message |
| `0x1000ec0` | 472 | **g_IExec** | SendIO | Async IO send |
| `0x1000edc` | 228 | **g_IExec** | SetSignal | Clears/sets signal bits |
| `0x1000f18` | 424 | **g_IExec** | DeleteLibrary | Cleanup on fail path |
| `0x1000f48` | 128 | r31 object | Insert | Insert node into list (r31 = bi?) |
| `0x1001064` | 124 | r30 object | (call via r30+124) | Some method on r30 |

**CRITICAL — line `0x1000f48`: `lwz r9,128(r31)` + `bctrl`**
r31 at this point = `bi` (loaded from `boardDesc+1630` at line 0x1000f3c: `lwz r31,1630(r28)`).
Offset 128 of bi = `bi->Insert` (function pointer in vtable)? No — vtable starts at bi+274.
Offset 128 of bi is in the fixed fields area (see BoardInfo layout: bi+112=BoardLock,
bi+128 = inside BoardLock.ss_Owner area).

Actually `lwz r9,128(r31)` where r31=bi would load bi+128. In BoardInfo:
- bi+112 = BoardLock (SignalSemaphore, 46 bytes)
- bi+128 = BoardLock+16 = `ss_Owner` (APTR, task that owns the semaphore)

So this is loading `bi->BoardLock.ss_Owner` as a function pointer and calling it — which
makes no sense unless r31 is NOT bi. More likely r31 is a different object (e.g. a
graphics board descriptor) with a vtable at offset 0, where offset 128 = vtable slot 32.

**r31 source**: `lwz r31,1630(r28)` where r28=boardDesc=cardDesc=0x0226A028.
`boardDesc+1630` = some pointer stored there by PCIGraphics.card at runtime.

---

## DSI Crash — Revised Analysis (v53.10)

**Crash dump (v53.10 boot)**:
```
Crashed task: idle.task (0x6FFA6540)
IP: kernel+0x1B99C (= 0x0181B99C)
DAR: 0x00000000
r3  = 0x6FF9C000   ← node being traversed by scheduler
r17 = 0x6FF9C004   ← = r3+4 (ln_Pred)
r22 = 0x6FF9C020   ← = r3+32
r28 = 0x6FFA8000
LR  = 0x0181BA7C
```

**The crashing node is `0x6FF9C000`**, NOT `bi=0x6FFA4000`.

- `bi = 0x6FFA4000`
- `idle.task struct = 0x6FFA6540`
- `0x6FF9C000 = bi - 0x8000` (32KB below bi)

### What is `0x6FF9C000`?
The kernel scheduler traverses exec's `TaskReady` or `TaskWait` list. A node at
`0x6FF9C000` has `ln_Succ = 0x00000000`. The scheduler loads `ln_Succ` → follows
0 → DSI on next dereference.

The node at `0x6FF9C000` is NOT bi. Something else put a corrupt/uninitialized node
there into the exec task list.

### What happens after chip_FindCard returns (v53.12 confirmed)

PCIGraphics.card adds `bi` to some exec list (task/wait/semaphore) after chip_FindCard
returns. The crash is **not** caused by the return value of chip_FindCard (changing from
`bi` to `&chip_found_sentinel` made no difference).

The crashing node is `bi` itself (`r3=bi`). `bi->ln_Succ = 0` at crash time (zeroed
by our chip_InitCard_C). Scheduler follows NULL → DSI.

### What zeroes bi->ln_Succ?

At chip_FindCard entry: `bi->ln_Succ = 0x84000000` (GPU PCI BAR pre-filled by PCIGraphics.card).
At chip_FindCard return: `bi->ln_Succ = 0x00000000`.

Somewhere inside `chip_InitCard_C` this gets zeroed. Candidates:
- Our writes to bi fields (we write `bi->MemoryBase = gs->fb_mem` at offset 4 = ln_Pred,
  NOT offset 0 = ln_Succ — so not a direct write)
- The VirtIO GPU init DMA operations (cmd_buf allocated at phys 0x000A0000, far from bi)
- AllocVecTags for gs (clears gs not bi)
- **CopyMem somewhere** — unlikely

Most likely: PCIGraphics.card's board init code (`0x1000dc0`) reads the FindCard return
value `lwz r5,0(retval)`, and if non-zero, loops. The sentinel has `[0]=0` so it exits.
But PCIGraphics.card may also be writing through the return value or through bi directly
in the post-FindCard sequence. It calls `AllocSignal`, `AddResource`, `SetFunction`,
`SendIO`, `GetSucc`/`GetTail`/`Enqueue`/`Insert` after FindCard.

**Enqueue(bi)** or **Insert(bi)** would add bi to exec's ready/wait task list!
PCIGraphics.card's `Enqueue(84)` and `Insert(128)` calls (confirmed in call table)
are the prime suspects for adding bi as a node.

---

## ROOT CAUSE — DEFINITIVELY SOLVED (v53.24, March 2026)

### The Bug

After `chip_FindCard` (slot[4]) returns non-NULL, PCIGraphics.card calls:

```
pci_obj = bi[1630];                           // PCI device object
intNum  = pci_obj->MapInterrupt(pci_obj);     // get IRQ number
IExec->AddIntServer(intNum, &bi->HardInterrupt);  // bi+68
```

This adds `bi->HardInterrupt` (a `struct Interrupt` at bi offset 68) to the kernel's
interrupt server chain. If `HardInterrupt` fields are uninitialized (all zeros), the
corrupt node corrupts the interrupt chain. When any interrupt fires, the kernel walks
the chain, dereferences NULL pointers → **DSI at kernel+0x1B99C, DAR=0x00000000**.

### PCIGraphics.card post-FindCard disassembly (0x10011C0)

```asm
; POST-SUCCESS PATH — executes when slot[4] returns non-NULL
0x10011c0: lwz r9, 184(r31)          ; pci_obj->MapInterrupt method
0x10011c4: mr  r3, r31               ; r3 = pci_obj
0x10011c8: lwz r31, -31752(r26)      ; r31 = g_IExec (RELOADED from sbss)
0x10011d0: lwz r29, 556(r31)         ; r29 = IExec->AddIntServer (2019 SDK offset)
0x10011d4: bctrl                      ; intNum = pci_obj->MapInterrupt(pci_obj)
0x10011d8: addi r5, r28, 68          ; r5 = &bi->HardInterrupt (bi+68)
0x10011e0: mr   r4, r3               ; r4 = intNum
0x10011e4: mr   r3, r31              ; r3 = IExec
0x10011e8: bctrl                      ; IExec->AddIntServer(intNum, &bi->HardInterrupt)
```

**IExec offset 556**: In the 2019 SDK used by PCIGraphics.card 53.18, this is
`AddIntServer`. Our current SDK has it at offset 496 (the exec interface grew by
~60 bytes between versions for added semaphore/library functions).

### The Fix (v53.24)

Fill `bi->HardInterrupt` with valid `struct Interrupt` fields BEFORE returning
non-NULL from FindCard:

```c
bi->HardInterrupt.is_Node.ln_Type = NT_INTERRUPT;
bi->HardInterrupt.is_Node.ln_Pri  = 0;
bi->HardInterrupt.is_Node.ln_Name = (STRPTR)"VirtIOGPU";
bi->HardInterrupt.is_Data         = (APTR)bi;
bi->HardInterrupt.is_Code         = (void (*)())chip_irq_nop;
```

Where `chip_irq_nop` returns 0 (not handled) so the interrupt chain continues
to other handlers.

### Isolation test results (complete history)

| Version | bi writes | VirtIO init | FindCard returns | Device reset | HardInterrupt | Crash? |
|---------|-----------|-------------|-----------------|-------------|---------------|--------|
| v53.18 | YES | YES | sentinel ptr | NO | not filled | YES |
| v53.19 | NO | YES | sentinel ptr | NO | not filled | YES |
| v53.20 | NO | NO | NULL | NO | n/a | **NO** |
| v53.21 | YES | YES | integer 1 | NO | not filled | YES |
| v53.22 | YES | YES | integer 1 | YES | not filled | YES |
| v53.23 | NO | NO | integer 1 | NO | not filled | YES |
| v53.24 | YES | YES | integer 1 | YES | **filled** | **NO** |
| v53.25 | YES | YES | integer 1 | NO | filled | **NO** |

v53.23 was the definitive test: returning 1 with ZERO bi fills and ZERO VirtIO init
still crashed. v53.24 added HardInterrupt fill → no crash. QED.

### Previous red herrings (now understood)

- **bi->RegisterBase overwrite** (v53.12-v53.13): Was a real bug too, but fixing it
  alone didn't resolve the crash because AddIntServer corruption was the primary cause.
- **VirtIO device interrupt theory** (v53.22): Reset device after init → still crashed.
  The interrupts weren't from VirtIO — they were from the shared PCI interrupt line
  hitting our corrupt HardInterrupt node in the kernel ISR chain.
- **Return value format** (v53.18-v53.21): Sentinel pointer vs integer 1 — irrelevant.
  Any non-NULL return triggers the AddIntServer path.

### Why the real SM502 doesn't crash

The real SM502.chip fills `bi->HardInterrupt.is_Code` with its actual ISR handler
at offset 0xAC0+N. The SM502 disassembly shows it writes dozens of bi fields including
the interrupt handler.

---

## Build Command (VirtIOGPU chip)
```sh
wsl sh -c "docker run --rm -v /mnt/w/Code/amiga/antigravity:/src -w /src/projects/VirtIOGPU walkero/amigagccondocker:os4-gcc11 make clean && docker run --rm -v /mnt/w/Code/amiga/antigravity:/src -w /src/projects/VirtIOGPU walkero/amigagccondocker:os4-gcc11 make -j$(nproc) all"
```
Then deploy via kickstart zip update script.
