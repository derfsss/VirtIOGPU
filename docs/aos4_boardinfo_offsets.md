# AmigaOS 4 PCIGraphics.card BoardInfo extension-area offsets

The first 1418 bytes of `BoardInfo` are documented in
`include/p96/boardinfo.h` and match canonical Picasso96.  Everything
beyond that is private to PCIGraphics.card and has no published header.
This file records what is known about the @1418+ region from
reverse-engineering AOS4 binaries.

## Method

- `powerpc-linux-gnu-objdump -d` over `PCIGraphics.card` v53.18 (50KB
  stripped ELF) and `RadeonRX.chip.debug` (Enhancer Software 2.2,
  ~750KB ELF, code present but symbols stripped).
- Distinct offsets used in `lwz`/`stw`/`lhz`/`sth`/`addi` against
  BoardInfo registers (r28/r30/r31 in observed contexts).
- Cross-checked against canonical layout in
  `P96CardDevelop/PrivateInclude/boardinfo.h`.

## Confirmed offsets

| Offset | Field (canonical name) | Type | Confirmed by |
|---|---|---|---|
| +186 | `Flags` | ULONG | RadeonRX `ori r10,r10,16; stw r10,186(r31)` at 0x1001b70 -- this is `BIF_VBLANKINTERRUPT = 1<<4`, confirming canonical bit numbering |
| +1418..1481 | `ChipData[16]` | 16 ULONGs | canonical, not directly observed |
| +1482..1545 | `CardData[16]` | 16 ULONGs | canonical, not directly observed |
| +1546 | `MemorySpaceBase` | ULONG | RadeonRX `stw r6,1546(r31)` where r6 = `bi->MemoryBase` |
| +1550 | `MemorySpaceSize` | ULONG | RadeonRX `stw r5,1550(r31)` where r5 = `bi->MemorySize` |
| +1554 | `DoubleBufferList` | APTR | by elimination -- canonical layout holds through this region |
| +1558 | `SyncTime.tv_secs` | ULONG | RadeonRX `stw r8,1558(r31)` paired with @1562 |
| +1562 | `SyncTime.tv_micro` | ULONG | RadeonRX `stw r9,1562(r31)` |
| +1566 | **PCI VendorID** (UWORD) -- not `SyncPeriod` | UWORD | PCIGraphics.card 0x1001120: `IPCI->ReadConfigWord(self, 0)` then `sth r3,1566(r28)` -- and never reads it back. By process of elimination this is what graphics.library v54 GBD_PCIVendorID reads. RadeonRX.chip writes a ULONG here too, **which appears to be a real bug in RadeonRX**: it overwrites both Vendor (high half) and Device (low half) on every InitChip. |
| +1568 | **PCI DeviceID** (UWORD) | UWORD | PCIGraphics.card 0x1001134: `IPCI->ReadConfigWord(self, 2)` then `sth r3,1568(r28)`. Pairs with VendorID at +1566. |
| +1604 | `WaitQ` (likely) | MinList (12 bytes) | not directly observed; canonical layout intact through surrounding region; node format is set by rtg.library and is not yet known |
| +1620 | `MouseImageBuffer` | APTR | RadeonRX `addi r9,r23,1620` (loads address -- list/pointer use) |
| +1630 | `PCIDevice*` | APTR | AOS4-specific; PCIGraphics.card injection point used by our `chip_board.c` SetMethod hook |
| +1634 | EDID gate | ULONG | AOS4-specific; gate value graphics.library compares against `BOARDINFO_EDID_GATE_MIN` (1777) |
| +1638 | `MaxPlanarMemory` | ULONG | RadeonRX `lwz r10,1638(r28)` (multiple reads) |
| +1714 | EDID modelist | MinList | AOS4-specific; SM502.chip pattern, used by our `chip_register_edid` |

## Where AOS4 diverges from canonical

The @1620..1635 window has been reorganised in AOS4 PCIGraphics.card.
Canonical Picasso96 puts `MouseImageBuffer` at +1620, `backViewPort` at
+1624, `backBitMap` at +1628, `backExtra` at +1632.  AOS4 keeps
`MouseImageBuffer` at +1620 (confirmed) but reuses the next 12 bytes
for `PCIDevice*` (+1630) and the EDID gate (+1634).  The
`backBitMap`/`backExtra`/`YSplit` slots used by classic-Amiga screen
splits do not exist at canonical positions on AOS4.

Beyond +1650 (canonical `SecondaryCLUT[256]`, 768 bytes), RadeonRX
uses +2018, +2020, +2072, +2074, +2132, +2304..2324, +2700..2738 etc.
for chip-private fields -- well inside canonical's `SecondaryCLUT`
range.  The canonical fields that come after `SecondaryCLUT` --
`RGBFormatBack` (+2418), `RGBFormatSprite` (+2422), `spriteExtra`
(+2426), `HostMouseImage` (+2430), `MonitorWidth` (+2434),
`MonitorHeight` (+2436), `DisplayMemory` (+2438) -- cannot be assumed
to live at the canonical offsets in AOS4.

## Verified flag bit numbers (AOS4, from RadeonRX disassembly)

| Bit | Flag |
|---|---|
| 0 | `BIB_HARDWARESPRITE` |
| 4 | `BIB_VBLANKINTERRUPT` (was 25 in earlier reverse engineering -- WRONG) |
| 5 | `BIB_HASSPRITEBUFFER` |
| 15 | `BIB_BLITTER` (RadeonRX `ori r9,r9,32768`) |
| 20 | `BIB_INDISPLAYCHAIN` |
| 22 | `BIB_NOMASKBLITS` |
| 23 | `BIB_NOC2PBLITS` |
| 26 | `BIB_GRANTDIRECTACCESS` |

All others taken from canonical and assumed correct (no contradicting
evidence in AOS4 binaries).

## What we use these offsets for

- `boardinfo_get_pcidevice`/`set_pcidevice` (+1630) -- VirtIO GPU
  injection in `chip_board.c`.
- `boardinfo_edid_gate`/`boardinfo_edid_modelist` (+1634/+1714) --
  EDID registration callback path for graphics.library v54+ mode
  discovery.
- `boardinfo_set_pci_id` (+1566 / +1568) -- VirtIO GPU 0x1AF4 / 0x1050,
  read by graphics.library v54 GBD_PCIVendorID / GBD_PCIProductID.
- `boardinfo_set_sync_time` (+1558 / +1562) -- pseudo-vblank baseline.
- `boardinfo_set_essential_formats` (+1616).
- `boardinfo_set_max_bm_size` (+1642 / +1646).
- `boardinfo_set_memory_space` (+1546/+1550) -- mirror `MemoryBase`/
  `MemorySize` for tools that read the canonical fields.

## What still needs reverse engineering

- AOS4 location (if any) of `MonitorWidth`, `MonitorHeight`. No
  paired-UWORD store at any extension-area offset in
  graphics.library.kmod and no `Monitor*` strings in any chip driver
  -- the field may not exist as a `BoardInfo` member on AOS4 at all.
- `MaxBMWidth/MaxBMHeight` location (canonical +1642/+1646 are list/
  pointer fields in AOS4, not dimensions).
- Field meanings at `bi+1554, +1616, +1626, +1642, +1646, +1710,
  +1726, +1778, +1782` -- all written by graphics.library.kmod but
  the canonical names don't match observed access patterns.
- Whether vtable slots 43..50 (canonical `EnableSoftSprite[Default]`,
  `AllocCardMemAbs`, `SetSplitPosition`, `ReInitMemory`,
  `GetCompatibleDACFormats`, `CoerceMode`, `Reserved3Default`)
  exist as function pointers in AOS4.  graphics.library.kmod
  installs no defaults at any of those offsets -- either AOS4 does
  not use the slots, or it uses them only conditionally on flag
  bits we can't yet identify.
- Meaning of the @2018+ chip-private region in RadeonRX.

## Resolved by graphics.library.kmod disassembly

- **`WaitQ` node format**: not needed by chip drivers.  The canonical
  pattern (`PiccoloSD64.card.asm`) is HardInterrupt -> `Cause(&bi->SoftInterrupt)`
  -> rtg.library's SoftInterrupt drains the queue internally.  Implemented
  in v53.106 by driving `Cause()` from the existing flush task at the
  active mode's refresh rate.
- **vtable slots 51/52, 65..67**: confirmed as function pointers in AOS4
  via `stw` install patterns at `bi+478, 482, 534, 538, 542`.
- **vtable slots 43..50**: `stw` installs from graphics.library.kmod
  not observed.  Layout in this region in AOS4 is uncertain.

## See also

- `chipdriver_research.md` -- earlier PCIGraphics.card analysis.
- `pcigraphics_card_analysis.md` -- DSI crash diagnosis trail.
- `siliconmotion502_chip_analysis.md` -- SM502 chip pattern reference.
- `include/p96/boardinfo.h` -- the live struct definitions and accessors.
