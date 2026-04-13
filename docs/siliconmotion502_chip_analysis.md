# SiliconMotion502.chip — Complete Reverse Engineering Analysis

> **Status (v53.111+):** Reference document.  SM502 is a small (~24 KB)
> AOS4 chip driver, useful as a worked example of the
> FindCard/InitCard protocol and EDID-modelist registration pattern
> (the bi+1714 EDID gate mechanism we use was first observed here).
>
> RadeonRX.chip.debug and graphics.library.kmod (referenced in
> `aos4_boardinfo_offsets.md`) are the better references for live
> BoardInfo behaviour because they exercise more of the extension
> area; SM502 stays useful for chip-driver-protocol questions.



**File:** `siliconmotion502.chip`
**Format:** ELF32 PowerPC big-endian (`elf32-powerpc-amigaos`)
**Version:** `$VER: SiliconMotion502.chip 53.12 (22.7.2023)`
**Analysis tool:** `ppc-amigaos-objdump -D` + `ppc-amigaos-readelf -a`

---

## ELF Layout

```
Section  VirtAddr   Size    Description
.text    0x00000000 0x49D0  All executable code
.rodata  0x000049D0 0x069C  Strings, jump tables, data tables
.sbss    0x0001506C 0x0008  2 global APTR variables (zero-init)
```

Only one exported symbol: `_start` at 0x0, size 8 (FindCard + InitCard entry stubs).

---

## Entry Point Protocol

### _start+0: FindCard
```asm
0:  li  r3, 20    ; return value = 20 (board type ID, NOT 1)
4:  blr
```
**FindCard returns 20**, not 1. The return value is a board identifier passed back to PCIGraphics.card. Its meaning is unknown but must be non-zero to signal "card found".

### _start+8: InitCard
The actual InitCard body begins at offset 8. It is NOT a trivial jump — the entire InitCard body lives here, spanning offsets 0x8 through 0xFC.

---

## InitCard Body — Full Annotated Disassembly (0x8–0xFC)

```
Registers on entry:
  r3 = struct BoardInfo *bi
  r4 = char **toolTypes
  r5 = APTR cardDesc   (PCIGraphics.card descriptor object — 3rd arg!)
```

```asm
 8:  mflr    r0
 c:  stwu    r1,-32(r1)         ; allocate 32-byte stack frame
10:  stw     r30,24(r1)
14:  mr      r30,r5             ; r30 = cardDesc (save)
18:  stw     r29,20(r1)
1c:  mr      r29,r4             ; r29 = toolTypes (save)
20:  lis     r4,0
24:  stw     r0,36(r1)          ; save LR
28:  addi    r4,r4,19192        ; r4 = 0x4AF8 = rodata+0x128 (utility.library string)
                                ;   Note: 0x49D0 + 0x128 = 0x4AF8 → "utility.library"
2c:  lwz     r0,16(r5)          ; r0 = cardDesc->field16 (library node name or similar)
30:  stw     r31,28(r1)
34:  mr      r31,r3             ; r31 = bi (save)
38:  stw     r0,44(r3)          ; bi+44 = cardDesc->field16
                                ;   offset 44 in BoardInfo = inside SoftInterrupt
                                ;   (HardInterrupt=68 bytes @68, SoftInterrupt @90, len=22 bytes)
                                ;   Actually offset 44 is inside HardInterrupt.is_Data
                                ;   HardInterrupt @68: is_Node(14)+is_Code(4)+is_Data(4) → is_Data@86?
                                ;   Wait: bi is packed. offset 44 = unknown (investigate further)

; === Call cardDesc->method[424] with (cardDesc, 0, 0) ===
3c:  mr      r3,r30             ; r3 = cardDesc
40:  lwz     r0,424(r30)        ; r0 = cardDesc vtable[424/4] = method 106
44:  stw     r5,56(r31)         ; bi->ChipBase = cardDesc   ← KEY: STORE FIRST!
48:  li      r5,0               ; arg3 = 0
4c:  mtctr   r0
50:  bctrl                      ; call cardDesc->method[424](cardDesc, r4=string, r5=0)
                                ; Returns: APTR to allocated video memory (or 0 on fail)
                                ; r4 was set to 0x4AF8 ("utility.library" string) at 0x28
                                ;   → This call OPENS utility.library via the card descriptor!
                                ;   cardDesc->method[424] = OpenLibrary via card

54:  lis     r5,0
58:  li      r6,1
5c:  mr      r0,r3              ; r0 = result (UtilityBase or vram pointer)
60:  mr      r4,r3              ; r4 = result
64:  cmpwi   cr7,r0,0           ; check if result == 0
68:  addi    r5,r5,19048        ; r5 = 0x4A68 = rodata+0x98 → "vga" or similar display string
                                ; Actually 0x49D0+0x98 = 0x4A68 → see rodata at 0x4A60: "J2.5..."
                                ; Hmm, need to check more carefully what 0x4A68 contains
6c:  stw     r0,48(r31)         ; bi->MemoryBase = result (utility.library base)
                                ;   Wait — if this is utility.library, storing it in MemoryBase
                                ;   is suspicious. Let's re-examine...
                                ;   ACTUALLY: cardDesc->method[424] with string arg =
                                ;   it passes the string to some function that interprets it.
                                ;   The string at 0x4AF8 = 0x49D0+0x128:
                                ;   rodata at 0x4AF0: "utility.library\0"  → YES, utility.library
                                ;   So method[424] is GetInterface("utility.library", "main", 1) or
                                ;   OpenLibrary("utility.library", 0)?
70:  li      r7,0
74:  mr      r3,r30             ; r3 = cardDesc again
78:  beq     cr7,c8             ; if result==0, skip to cleanup path

; === Call cardDesc->method[448] with (cardDesc) ===
7c:  lwz     r0,448(r30)        ; r0 = cardDesc->method[448]
80:  mtctr   r0
84:  bctrl                      ; call cardDesc->method[448](cardDesc)
                                ; Returns: ExecBase / SysBase pointer
88:  mr      r0,r3              ; r0 = SysBase
8c:  mr      r3,r31             ; r3 = bi
90:  cmpwi   cr7,r0,0           ; check if SysBase == 0
94:  stw     r0,60(r31)         ; bi->ExecBase = SysBase    ← CONFIRMED
98:  beq     cr7,b4             ; if ExecBase==0, goto error cleanup at b4

; === SUCCESS PATH: store toolTypes, set VBIName field, call vtable fill ===
9c:  li      r0,12
a0:  stw     r29,4(r31)         ; bi->RegisterBase = toolTypes (offset 4)
a4:  sth     r0,30(r31)         ; bi->VBIName[10] = 12 (halfword at offset 30)
a8:  bl      4170               ; call chip_open_library(bi)
                                ; This function opens utility.library interface and
                                ; allocates video memory — see analysis below
ac:  cmpwi   cr7,r3,0
b0:  bne     cr7,e0             ; if returned non-zero, jump to success epilogue
                                ;   NOTE: bne means "branch if NOT equal to zero"
                                ;   So if chip_open_library returns NONZERO → success!
                                ;   Returns 0 on failure, 1 on success

; === FAILURE PATH (chip_open_library failed or ExecBase==0): ===
b4:  lwz     r0,428(r30)        ; r0 = cardDesc->method[428]
b8:  mr      r3,r30             ; r3 = cardDesc
bc:  lwz     r4,48(r31)         ; r4 = bi->MemoryBase (utility.library base we got earlier)
c0:  mtctr   r0
c4:  bctrl                      ; call cardDesc->method[428](cardDesc, utilityBase)
                                ;   method[428] = CloseLibrary / cleanup of utility.library

; === ALWAYS CALLED: cardDesc->method[484] = final teardown/register ===
c8:  mr      r3,r30             ; r3 = cardDesc
cc:  lwz     r30,484(r30)       ; r30 = cardDesc->method[484] (function pointer, not value!)
d0:  mr      r4,r31             ; r4 = bi
d4:  li      r31,0              ; r31 = 0 (will be return value on failure)
d8:  mtctr   r30
dc:  bctrl                      ; call cardDesc->method[484](cardDesc, bi)
                                ;   On FAILURE path: this is a "failed init" notification
                                ;   On SUCCESS path: this was skipped (went to e0)

; === EPILOGUE ===
e0:  lwz     r0,36(r1)
e4:  mr      r3,r31             ; return r31 (BoardInfo* on success, 0 on failure)
e8:  lwz     r29,20(r1)
ec:  lwz     r30,24(r1)
f0:  mtlr    r0
f4:  lwz     r31,28(r1)
f8:  addi    r1,r1,32
fc:  blr
```

### Critical Reinterpretation

Re-examining what `cardDesc->method[424]` actually does:

At offset 0x20-0x28: `lis r4,0; addi r4,r4,0x4AF8` loads the address of `"utility.library"` string (rodata+0x128).

At offset 0x48: `li r5,0`

So the call at 0x50 is: `cardDesc->method[424](cardDesc, "utility.library", 0)`

This is almost certainly: **Open a library by name via the PCIGraphics.card service**. The card descriptor provides a `CardOpenLibrary(desc, name, version)` method at offset 424 that returns the library base.

The result is stored in `bi->MemoryBase` (offset 48). This is WRONG for a normal driver — `MemoryBase` should be video RAM. But wait: looking at the rodata more carefully:

```
rodata+0x128 = 0x49D0+0x128 = 0x4AF8
rodata hex at 0x4AF0: "utility.library\0"
```

YES — `cardDesc->method[424]` is called with `"utility.library"` as argument. This opens utility.library and stores the base in `bi->MemoryBase`... temporarily? Or is `bi->MemoryBase` actually `bi->UtilBase` at a different offset than we think?

**CHECK**: BoardInfo offset 48 = `MemoryBase` per our boardinfo.h (offset 48 = 4th ULONG field: RegisterBase@0+4, MemoryBase@4+4... wait).

Per boardinfo.h with `#pragma pack(2)`:
```
+0   RegisterBase  APTR   (4 bytes)
+4   MemoryBase    APTR   (4 bytes)  ← offset 4, not 48!
+8   MemoryIOBase  APTR
+12  MemorySize    ULONG
+16  BoardName     STRPTR
```

So offset 48 in BoardInfo = **??? Let's recalculate from the struct layout.**

With 2-byte alignment packing:
```
+0   RegisterBase (4)
+4   MemoryBase   (4)
+8   MemoryIOBase (4)
+12  MemorySize   (4)
+16  BoardName    (4)  = STRPTR
+20  VBIName[32]  (32 bytes) = 32 chars
+52  CardBase     (4)
+56  ChipBase     (4)   ← confirmed: stw r5,56(r31)
+60  ExecBase     (4)   ← confirmed: stw r0,60(r31)
+64  UtilBase     (4)
+68  HardInterrupt (struct = 14+4+4 = 22 bytes)
+90  SoftInterrupt
```

**offset 48 = between MemorySize and CardBase?**

```
+0   RegisterBase  (4) → 0-3
+4   MemoryBase    (4) → 4-7
+8   MemoryIOBase  (4) → 8-11
+12  MemorySize    (4) → 12-15
+16  BoardName     (4) → 16-19
+20  VBIName[32]   (32)→ 20-51
+52  CardBase      (4) → 52-55
+56  ChipBase      (4) → 56-59
+60  ExecBase      (4) → 60-63
+64  UtilBase      (4) → 64-67
```

So **offset 48 = VBIName[28..31]** (bytes 28-31 within VBIName).

And the code stores the utility.library base into `VBIName[28]` (offset 48) ← this looks like a mistake or an overloaded field. This is the WinUAE/OS4 pattern of using "reserved" struct fields for chip-private data.

**offset 44 = VBIName[24..27]**: `stw r0,44(r3)` stores `cardDesc->field16` here.

So the SM502 chip driver is using `VBIName[20..31]` (the last 12 bytes of VBIName) as private storage for:
- `VBIName+24` (bi+44): some value from cardDesc->field[16]
- `VBIName+28` (bi+48): utility.library base address (temporarily?)
- `VBIName+30` (bi+30... wait, that's bi+30 = VBIName[10]): halfword 12

Then `bi->ExecBase` (offset 60) = SysBase from cardDesc->method[448].

**Revised mapping of what InitCard stores at each offset:**

| bi offset | Field (our struct)      | Value stored                                        |
|-----------|-------------------------|-----------------------------------------------------|
| +4        | RegisterBase            | toolTypes (char **)                                 |
| +30       | VBIName[10] (halfword)  | 12 (literal)                                        |
| +38 or 44 | VBIName[18..19]?        | cardDesc->field[16] (some board descriptor value)   |
| +48       | VBIName[28..31]         | utility.library base (temporary / chip-private)     |
| +52       | CardBase                | (not set in InitCard directly)                      |
| +56       | ChipBase                | cardDesc (APTR to card descriptor object)           |
| +60       | ExecBase                | SysBase from cardDesc->method[448]                  |
| +64       | UtilBase                | Set later in chip_open_library (0x4170)             |

---

## cardDesc Object — Method Table

The `cardDesc` is a C++ style object with function pointers at fixed word offsets. From disassembly:

| Offset | Method Signature (inferred)                              | Purpose                                        |
|--------|----------------------------------------------------------|------------------------------------------------|
| +16    | `APTR field` (not a function, a value)                   | Some board name/identifier (stored at bi+44)   |
| +304   | `void f(cardDesc)`                                       | Hardware init / reset (called in chip_open_library) |
| +424   | `APTR f(cardDesc, const char *name, ULONG ver)`          | Open library by name (returns library base)    |
| +428   | `void f(cardDesc, APTR libBase)`                         | Close library (cleanup on failure)             |
| +448   | `APTR f(cardDesc)`                                       | Get SysBase/ExecBase                           |
| +456   | `void f(cardDesc)`                                       | Unknown (called if ExecBase != NULL, on close) |
| +484   | `void f(cardDesc, struct BoardInfo *bi)`                 | Notify card of init failure (teardown)         |
| +504   | `BOOL f(cardDesc)`                                       | Check something (returns 0/1)                  |
| +508   | `void f(cardDesc, APTR)`                                 | Cleanup (called in chip_close_library)         |
| +704   | `APTR f(cardDesc, params...)`                            | Open display/video memory interface            |

**Method[424] with "utility.library" string:**
This is the PCIGraphics.card's wrapper for `IExec->OpenLibrary()`. The chip driver never calls exec directly — it always goes through the cardDesc interface.

**Method[448]:**
Returns SysBase. The chip uses this to avoid depending on AbsExecBase (address 4).

**Method[484]:**
Called on the **failure path** — not after success. This notifies PCIGraphics.card that chip init failed, so it can clean up its own state.

---

## chip_open_library (0x4170) — Full Analysis

This is the function called at offset 0xa8. Despite being named "vtable fill" in early analysis, it actually **opens utility.library and the video memory interface**. It does NOT fill the BoardInfo function pointer table.

```
Entry: r3 = struct BoardInfo *bi

4170: mflr    r0
4174: stwu    r1,-48(r1)          ; 48-byte stack frame
4178: li      r4,0
417c: li      r5,0
4180: stw     r31,44(r1)
4184: stw     r29,36(r1)
4188: lis     r29,1               ; r29 = 0x00010000 base for sbss access
418c: stw     r0,52(r1)           ; save LR
4190: lwz     r31,56(r3)          ; r31 = bi->ChipBase = cardDesc
4194: stw     r30,40(r1)
4198: mr      r30,r3              ; r30 = bi

; Call cardDesc->method[304](cardDesc)  — hardware reset/probe
419c: lwz     r0,304(r31)         ; r0 = cardDesc->method[304]
41a0: mr      r3,r31              ; r3 = cardDesc
41a4: mtctr   r0
41a8: bctrl                       ; call cardDesc->method[304](cardDesc)
                                  ; This is the hardware detection/init step

41ac: lis     r0,-32768           ; 0x80000000 | 0x0A = some constant 0x8000000A
41b0: lis     r10,-32768          ; 0x8000000B
41b4: mr      r11,r3              ; r11 = result from method[304] (display object? handle?)
41b8: lis     r9,1                ; r9 = for sbss access (0x00010000)
41bc: cmpwi   cr7,r11,0           ; if result == 0, fail
41c0: ori     r0,r0,10            ; r0 = 0x8000000A (pixel format constant?)
41c4: stw     r11,20592(r9)       ; sbss+4 = r11 (display handle, saved globally)
                                  ;   sbss[4] = 0x1506C+4 = 0x15070 → stored globally
41c8: ori     r10,r10,11          ; r10 = 0x8000000B
41cc: mr      r3,r31              ; r3 = cardDesc
41d0: li      r4,0
41d4: beq     cr7,424c            ; if method[304] returned 0, goto FAIL

; Call cardDesc->method[704](cardDesc, params on stack)
41d8: stw     r0,8(r1)            ; stack arg: 0x8000000A (pixel format?)
41dc: li      r0,40               ; 40 = 0x28
41e0: stw     r0,12(r1)           ; stack arg: 40
41e4: stw     r10,16(r1)          ; stack arg: 0x8000000B
41e8: stw     r11,20(r1)          ; stack arg: display_handle (from method[304])
41ec: lwz     r0,704(r31)         ; r0 = cardDesc->method[704]
41f0: mtctr   r0
41f4: bctrl                       ; call cardDesc->method[704](cardDesc, 0, ?, 0x8000000A, 40, 0x8000000B, handle)
                                  ; Returns: APTR to video memory / display interface

41f8: lis     r4,0
41fc: li      r5,2
4200: mr      r0,r3               ; r0 = result
4204: mr      r6,r3               ; r6 = result
4208: cmpwi   cr7,r0,0            ; check if == 0
420c: addi    r4,r4,20564         ; r4 = 0x5054 = sbss area address (for GetInterface call?)
                                  ;   Actually 0x49D0+0x5054-0x49D0 = 0x5054? No...
                                  ;   0x49D0 base + relative 20564-0x49D0? Let's recompute:
                                  ;   lis r4,0; addi r4,r4,20564 → r4 = 20564 = 0x5054
                                  ;   But that's in the sbss region (0x1506C)? No, it's .rodata?
                                  ;   The reloc table shows R_PPC_ADDR16_LO for .sbss+0 at 0x4212
                                  ;   So r4 = address of sbss+0 = 0x1506C (the stored value)
4210: stw     r0,20588(r29)       ; sbss+0 = r0 (video mem interface stored globally)
                                  ;   sbss[0] = 0x1506C → stored globally
4214: mr      r3,r31              ; r3 = cardDesc
4218: li      r7,0
421c: beq     cr7,424c            ; if method[704] returned 0, goto FAIL

; Call cardDesc->method[504](cardDesc)  — check capability
4220: lwz     r0,504(r31)         ; r0 = cardDesc->method[504]
4224: mtctr   r0
4228: bctrl                       ; call cardDesc->method[504](cardDesc)
                                  ; Returns: BOOL (non-zero = OK)
422c: lis     r5,0
4230: li      r6,1
4234: extsb   r3,r3               ; sign-extend byte result
4238: addi    r5,r5,20580         ; r5 = sbss+0 address
423c: cmpwi   cr7,r3,0            ; if result == 0, fail
4240: li      r7,0
4244: mr      r3,r31              ; r3 = cardDesc
4248: beq     cr7,4270            ; if method[504] returned 0, goto SUCCESS PART 2
                                  ;   NOTE: beq 4270 means "skip the GetInterface call"?
                                  ;   Wait — beq means "branch if result==0" = FAIL
                                  ;   So if method[504] returns 0: go to 4270
                                  ;   But 4270 is the success path (stw r4,52(r30))!
                                  ;   This is confusing — method[504] returns 0 = normal/OK

; FAIL path:
424c: li      r0,0
4250: lwz     r29,36(r1)
4254: mr      r3,r0               ; return 0 = FAIL
4258: lwz     r0,52(r1)
425c: lwz     r30,40(r1)
4260: lwz     r31,44(r1)
4264: mtlr    r0
4268: addi    r1,r1,48
426c: blr

; SUCCESS PATH (method[504] returned non-zero):
; Get interface from the stored video memory object
4270: lwz     r9,20588(r29)       ; r9 = sbss[0] = video_mem_object from method[704]
4274: lwz     r31,448(r31)        ; r31 = cardDesc->method[448] (GetInterface/GetExecBase)
                                  ;   Wait — here we call method[448] as a FUNCTION on cardDesc
                                  ;   but r31 was cardDesc at this point...
                                  ;   Actually: r31 = cardDesc (from 4190). So:
                                  ;   lwz r31,448(r31) loads cardDesc->method[448] fp into r31
                                  ;   Then mtctr r31 + bctrl = tail call
4278: lwz     r4,20(r9)           ; r4 = video_mem_object->field[20]
427c: mtctr   r31                 ; mtctr = setup indirect call
4280: stw     r4,52(r30)          ; bi->CardBase = video_mem_object->field[20]  (offset 52)
4284: bctrl                       ; call cardDesc->method[448](cardDesc) - last call
                                  ;   but wait: r3 = cardDesc (from 4244), r31 = method[448] fp
                                  ;   So calling: method[448](cardDesc)
                                  ;   Returns: utility.library interface (IUtility)
4288: li      r0,1
428c: lwz     r29,36(r1)
4290: stw     r3,64(r30)          ; bi->UtilBase = IUtility (offset 64)
4294: mr      r3,r0               ; return 1 = SUCCESS
4298: lwz     r0,52(r1)
429c: lwz     r30,40(r1)
42a0: lwz     r31,44(r1)
42a4: mtlr    r0
42a8: addi    r1,r1,48
42ac: blr
```

### chip_open_library Summary

`chip_open_library(bi)`:
1. Loads `bi->ChipBase` = cardDesc
2. Calls `cardDesc->method[304](cardDesc)` → hardware probe → returns a **display_handle**
3. If display_handle == 0: return 0 (fail)
4. Saves display_handle to **global sbss[4]**
5. Calls `cardDesc->method[704](cardDesc, 0, ?, 0x8000000A, 40, 0x8000000B, display_handle)` → returns **video_mem_object**
6. If video_mem_object == 0: return 0 (fail)
7. Saves video_mem_object to **global sbss[0]**
8. Calls `cardDesc->method[504](cardDesc)` → bool probe/verify
9. Stores `video_mem_object->field[20]` into `bi->CardBase` (offset 52)
10. Calls `cardDesc->method[448](cardDesc)` → returns IUtility interface
11. Stores IUtility into `bi->UtilBase` (offset 64)
12. Returns 1 (success)

---

## chip_close_library (0x42b0) — Analysis

Called during FreeCard / cleanup:

```
42b0: (entry, r3 = bi)
42c8: lwz     r4,64(r3)           ; r4 = bi->UtilBase (IUtility)
42cc: lwz     r31,56(r3)          ; r31 = bi->ChipBase = cardDesc
42d0: cmpwi   cr7,r4,0
42dc: mr      r30,r3              ; r30 = bi
42e0: beq     cr7,42f0            ; if IUtility == NULL, skip Drop
42e4: lwz     r0,456(r31)         ; r0 = cardDesc->method[456]
42e8: mtctr   r0
42ec: bctrl                       ; call cardDesc->method[456](cardDesc)
                                  ;   method[456] = DropInterface(IUtility) or similar

42f0: lwz     r0,52(r30)          ; r0 = bi->CardBase
42f8: cmpwi   cr7,r0,0
4300: lwz     r0,508(r31)         ; r0 = cardDesc->method[508]
4304: lwz     r4,20588(r29)       ; r4 = sbss[0] = video_mem_object
4308: mtctr   r0
430c: bctrl                       ; call cardDesc->method[508](cardDesc, video_mem_object)
                                  ;   method[508] = close video memory interface

4314: lwz     r5,20588(r29)       ; r5 = sbss[0]
4320: cmpwi   cr7,r5,0
4328: lwz     r0,708(r31)         ; r0 = cardDesc->method[708]
432c: mtctr   r0
4330: bctrl                       ; call cardDesc->method[708](cardDesc) if sbss[0] != 0

433c: lwz     r0,20592(r9)        ; r0 = sbss[4] = display_handle
4344: cmpwi   cr7,r0,0
434c: lwz     r31,312(r31)        ; r31 = cardDesc->method[312]
4350: mtctr   r31
4354: bctrl                       ; call cardDesc->method[312](cardDesc) if display_handle != 0
```

---

## InitCard — What Does NOT Happen

The SM502 InitCard does **NOT**:
- Write `BoardType`, `PaletteChipType`, or `GraphicsControllerType`
- Write `MaxHorValue`, `MaxVerValue`, `MaxHorResolution`, `MaxVerResolution`
- Write `PixelClockCount`
- Write `BitsPerCannon`
- Write `RGBFormats`
- Write `MaxMemorySize`, `MaxChunkSize`, `MemoryClock`
- Fill the function pointer vtable (fp[0]..fp[67])

These fields are filled in a **different function** — the mode-set and vtable fill happens in the vtable functions at `0x3d58` / `0x3ef4` (the large function that uses a 208-byte stack and 18 saved registers, called from the display mode-set path).

The function at `0x42b0` is what's called at `0x198` in the **reference count down / uninit** path (offset 0x120–0x204), not from InitCard itself.

---

## Reference Counter at bi+40 (halfword)

```
; AddRef (offset 0x100):
100: lwz     r3,16(r3)           ; r3 = bi = *(r3+16)  [some indirection]
104: lhz     r9,40(r3)           ; r9 = bi->halfword_at_40 (refcount?)
108: lbz     r0,22(r3)           ; r0 = bi->byte_at_22 (flags byte)
10c: addi    r9,r9,1             ; r9++
110: rlwinm  r0,r0,0,29,27       ; r0 &= ~(1<<3) = clear bit 3 of flags
114: sth     r9,40(r3)           ; store incremented refcount
118: stb     r0,22(r3)           ; store flags
11c: blr

; Release (offset 0x120):
; Decrements refcount at bi+40, checks flags byte at bi+22
; When refcount reaches 0 AND bit 3 of flags is set: call close/uninit
```

**bi+40 = reference count (UWORD)**
**bi+22 = flags byte, bit 3 = "pending release"**

These are part of the PCIGraphics.card object reference system, not directly related to Picasso96's BoardInfo fields.

---

## BoardInfo Struct Offsets — Cross-Referenced Against SM502

| Offset | SM502 Access            | Field (from boardinfo.h)    | Notes                           |
|--------|-------------------------|-----------------------------|---------------------------------|
| +4     | stw r29,4(r31)          | RegisterBase                | stores toolTypes                |
| +22    | lbz/stb                 | VBIName[2]                  | flags byte (bit3=pending close) |
| +30    | sth r0,30(r31)          | VBIName[10]                 | stores 12 (halfword)            |
| +38    | lwz r0,16(r5) → stw 44 | VBIName[18..21]             | cardDesc->field[16] stored here |
| +40    | lhz/sth                 | VBIName[20..21]             | reference counter (UWORD)       |
| +48    | stw r0,48(r31)          | VBIName[28..31]             | utility.library base (private)  |
| +52    | stw r4,52(r30)          | CardBase                    | video_mem_object->field[20]     |
| +56    | stw r5,56(r31)          | ChipBase                    | cardDesc                        |
| +60    | stw r0,60(r31)          | ExecBase                    | SysBase                         |
| +64    | stw r3,64(r30)          | UtilBase                    | IUtility interface              |

---

## The Vtable Fill — Where Does It Happen?

The `fp[0]..fp[67]` vtable pointers are **pre-filled by PCIGraphics.card** before calling InitCard, based on the rodata function descriptor tables.

Evidence from rodata (after relocation):

At rodata 0x4A90–0x4A9C: function pointers (after reloc) pointing to:
- `0x404` (SetGC?)
- `0x418` (another handler)
- `0xAC0` (InitCard entry)

At rodata 0x4AD0–0x4AF0: more function pointers:
- `0x23C`, `0x250` (AddRef/Release)
- `0x100`, `0x208`
- `0x120`, `0x000`

At rodata 0x4C48–0x4C94: **display mode function table** (5 entries):
```
+0x4C48: 0x2B8  → rodata strings for mode names
+0x4C4C: 0x2AE
+0x4C50: 0x2A4
+0x4C54: 0x29A
+0x4C58: 0x290
+0x4C66: 0x1848  (function)
+0x4C70: 0x1840  (function)
+0x4C7A: 0x1838  (function)
+0x4C84: 0x17FC  (function)
+0x4C8E: 0x17BC  (function)
```

The **`__library` descriptor** at rodata 0x4AB0 contains the ELF "function descriptor" (tocptr, envptr) used by AmigaOS to call the chip via the `pcigfx_card_desc` interface. This is how PCIGraphics.card discovers what functions the chip exports.

The vtable (BoardInfo fp[]) is actually filled by PCIGraphics.card itself using the function descriptor table from rodata. The chip does not call `bi->FillRect = ...` directly. Instead, the chip's rodata contains a table of function pointers that PCIGraphics.card reads and installs into the BoardInfo vtable.

---

## InitCard Return Value

InitCard returns `bi` (the BoardInfo pointer) on success, or `NULL` (0) on failure.

**NOT a BOOL!** — The return is `r31` which is either `bi` (non-zero) or `0` (set at `d4: li r31,0` on failure path).

This contradicts our current chip code which returns `TRUE` (1). We must return the `bi` pointer itself.

---

## Complete cardDesc Method Table (confirmed offsets)

| Offset | Called in           | Purpose                                            |
|--------|---------------------|----------------------------------------------------|
| +16    | read in InitCard    | Some board descriptor value (data, not function)   |
| +304   | chip_open_library   | Hardware detect/probe → returns display_handle     |
| +312   | chip_close_library  | Close display_handle                               |
| +424   | InitCard            | OpenLibrary("utility.library", 0)                  |
| +428   | InitCard fail path  | CloseLibrary(utilbase)                             |
| +448   | InitCard + open_lib | GetInterface or GetExecBase → returns SysBase/IFace|
| +456   | chip_close_library  | DropInterface(IUtility)                            |
| +484   | InitCard fail path  | Notify card of failed init                         |
| +504   | chip_open_library   | Verify/check (returns BOOL, 0=normal/OK)           |
| +508   | chip_close_library  | Close video_mem_object                             |
| +704   | chip_open_library   | Open video memory / display interface              |
| +708   | chip_close_library  | Additional cleanup                                 |

---

## String Table (rodata)

| Offset from rodata base | String                          | Used for                   |
|-------------------------|---------------------------------|----------------------------|
| +0x00  (0x49D0)         | `"SiliconMotion502.chip"`        | Board name                 |
| +0x1E  (0x49EE)         | `"$VER: SiliconMotion502.chip 53.12 (22.7.2023)\r\n"` | Version string |
| +0x98  (0x4A68)         | (contains a reloc ptr?)         | Some string ref            |
| +0x128 (0x4AF8)         | `"utility.library"`              | Passed to method[424]      |
| +0x164 (0x4B34)         | `"CURRENT_POWER_CLOCK %08lx\n"` | Debug printf               |
| +0x180 (0x4B50)         | `"SYSTEM_CTRL PRE %08lx\n"`     | Debug printf               |
| +0x198 (0x4B68)         | `"SYSTEM_CTRL POST %08lx\n"`    | Debug printf               |
| +0x1B0 (0x4B80)         | `"MISC_CTRL PRE %08lx\n"`       | Debug printf               |
| +0x1C8 (0x4B98)         | `"MISC_CTRL POST %08lx\n"`      | Debug printf               |
| +0x1E0 (0x4BB0)         | `"MISC_TIMING %08lx\n"`         | Debug printf               |
| +0x1F4 (0x4BC4)         | `"***...***\n"` (separator)     | Debug printf               |
| +0x684 (0x5054)         | `"timer.device"`                | Opened in chip_open_library|
| +0x694 (0x5064)         | `"main"`                        | Interface name             |

---

## Global Variables (.sbss at 0x1506C)

```
sbss+0 (0x1506C): APTR video_mem_object   (from cardDesc->method[704])
sbss+4 (0x15070): APTR display_handle     (from cardDesc->method[304])
```

These are chip-global singletons. Only one display can be open at a time per chip instance.

---

## Key Corrections for VirtIOGPU Driver

### 1. InitCard return value MUST be BoardInfo*, not TRUE/FALSE

```c
// WRONG (current):
return TRUE;

// CORRECT (from disassembly):
return (BOOL)(ULONG)bi;   // return bi pointer cast to BOOL (non-zero = success)
// Or more accurately, the raw asm returns r31 = bi or 0
// Since BOOL is just a LONG, returning (BOOL)bi works on PPC
```

Actually looking again at the epilogue:
```
e4: mr r3,r31    ; return r31 = bi (success) or 0 (failure, set at d4)
```

The type signature says BOOL but the value returned is the bi pointer. Since bi is non-zero for any valid board, this works as a BOOL. Our `return TRUE` (which returns 1) is technically wrong — we should `return (LONG)bi` or the caller may check the actual pointer value.

**Actually**: PCIGraphics.card likely only checks zero vs non-zero. `return TRUE` (= 1) is probably fine. But to be safe, match the pattern: `return (BOOL)(ULONG)bi`.

### 2. chip_open_library must be called

The function at 0x4170 is critical — it opens utility.library and video memory. Without this, `bi->UtilBase` is NULL and `bi->CardBase` is unset. PCIGraphics.card may check these after InitCard returns.

For VirtIOGPU: we don't need utility.library or the SM502's display hardware init, but we MUST properly respond to the cardDesc callbacks.

### 3. cardDesc->method[424] opens utility.library

Our driver calls `cardDesc->method[448]` to get SysBase. That is correct. But we don't call `cardDesc->method[424]("utility.library", 0)` to open utility.library. We probably should, for `bi->UtilBase`.

### 4. bi->UtilBase (offset 64) must be set

The SM502 stores IUtility at bi+64. PCIGraphics.card may dereference this.

### 5. The vtable fp[] is filled by PCIGraphics.card from rodata descriptors

The SM502 does NOT have `bi->SetGC = chip_SetGC;` style code. Instead, PCIGraphics.card reads the chip ELF's rodata function table and installs the function pointers. Our virtiogpu.chip's explicit vtable fill may be redundant or conflicting.

---

## What PCIGraphics.card Expects

Based on the SM502 analysis, the chip ELF must:

1. Export `_start` with FindCard at +0 (8 bytes) and InitCard at +8
2. Have a rodata function descriptor table that PCIGraphics.card reads
3. InitCard must:
   - Store cardDesc in bi->ChipBase (+56)
   - Call cardDesc->method[424]("utility.library", 0) → store base in bi+48 (private)
   - Call cardDesc->method[448](cardDesc) → SysBase → store in bi->ExecBase (+60)
   - Store toolTypes in bi->RegisterBase (+4)
   - Store 12 (halfword) at bi+30
   - Call chip_open_library(bi) → fills bi->CardBase (+52) and bi->UtilBase (+64)
   - Return bi (non-zero) on success, NULL on failure
4. cardDesc->method[484](cardDesc, bi) is called by PCIGraphics.card after InitCard returns, NOT by the chip

The "cannot configure" error is most likely because:
- **bi->UtilBase is NULL** (we never set it)
- **bi->CardBase is unset** (we never call method[304] or method[704])
- **InitCard returns wrong value** (TRUE=1 instead of bi pointer)
- **PCIGraphics.card checks bi->UtilBase != NULL before accepting the chip**

---

## Function at 0xAC0 — Hardware Mode Set (NOT InitCard)

The function at 0xAC0 is the hardware mode-set function (registered as a chip callback for SetGC/InitDisplay). It is called by PCIGraphics.card when a screen mode is actually applied, long after InitCard returns. It writes:

- `bi+200` (RGBFormats) = 18
- `bi+178` (GraphicsControllerType) = 11
- `bi+174` (PaletteChipType) = 14
- `bi+86` (BoardName or field near it) = pointer (612)
- `bi+186` (Flags) |= 3
- MaxHorValue[]/MaxVerValue[]/MaxHorResolution[]/MaxVerResolution[] — mode geometry
- BitsPerCannon = 8
- Hardware registers via direct MMIO (SM502-specific)

This means these fields are **NOT set during InitCard** — they are set when the first mode is configured. PCIGraphics.card does not require these to be valid when InitCard returns.

---

## Rodata Function Descriptor Table — How PCIGraphics.card Finds Chip Functions

At rodata 0x4A90 (raw, before relocation):
```
00004a90  00000404 00000418 00000000  → .text+0x404, .text+0x418, 0
00004aa0  00000ac0 ffffffff ...       → .text+0xAC0 (hardware mode set), -1 (sentinel)
```

At rodata 0x4AD0 (raw):
```
00004ad0  0000023c 00000250 ...       → .text+0x23C (AddRef), .text+0x250 (Release)
00004ae0  00000100 00000208 ...       → .text+0x100, .text+0x208
00004af0  00000120 00000000 ...       → .text+0x120 (uninit), 0
```

PCIGraphics.card reads the chip ELF's rodata at these known offsets to get the function pointers. These are used as callbacks. The chip's `_start+8` (InitCard) is at `.text+0x8` and is found via the ELF entry point.

The `__library` descriptor at rodata 0x4AB0 and `main` at 0x4A6E are AmigaOS 4 ELF function descriptors containing (code_ptr, toc_ptr) pairs — this is how the OS calls PPC functions in the chip ELF.

**The vtable (bi->fp[0]..fp[67]) is installed by PCIGraphics.card from these descriptors, not by the chip driver itself.**

---

## Minimum Correct InitCard for VirtIOGPU

Based on full analysis, the minimum correct InitCard that matches the SM502 protocol:

```c
BOOL chip_InitCard_C(struct BoardInfo *bi, char **toolTypes, APTR cardDesc)
{
    // 1. Store cardDesc
    bi->ChipBase = cardDesc;

    // 2. Open utility.library via cardDesc (stores base internally — SM502 uses bi+48)
    //    For VirtIOGPU: we don't need utility, but call to satisfy PCIGraphics.card
    typedef APTR (*OpenLib_t)(APTR desc, const char *name, ULONG ver);
    APTR *cd = (APTR *)cardDesc;
    APTR utilBase = ((OpenLib_t)cd[424/4])(cardDesc, "utility.library", 0);
    // Store it privately (SM502 uses bi offset 48 = VBIName[28])
    // We store it somewhere safe; PCIGraphics.card won't look here
    ((ULONG *)bi)[48/4] = (ULONG)utilBase;   // bi->VBIName[28..31]

    // 3. Get SysBase via cardDesc
    typedef APTR (*GetExec_t)(APTR desc);
    struct ExecBase *SysBase = (struct ExecBase *)((GetExec_t)cd[448/4])(cardDesc);
    bi->ExecBase = (struct Library *)SysBase;

    // 4. Get IUtility via cardDesc->method[448] again (same method, returns IUtility after open?)
    //    OR: method[448] IS "GetInterface" and returns IUtility when utility is already open
    //    For now: use SysBase->UtilityBase and GetInterface manually
    //    Actually: SM502 calls method[448] in chip_open_library to get IUtility:
    //      ldw r31,448(r31) then bctrl  → so method[448](cardDesc) = GetInterface(utilbase, "main", 1)
    //    Let's try: bi->UtilBase = method[448](cardDesc) called AFTER utility.library is open

    // 5. Store toolTypes
    bi->RegisterBase = (APTR)toolTypes;

    // 6. Store 12 at VBIName[10] (offset 30)
    ((UWORD *)bi)[30/2] = 12;

    // 7. Open/init hardware via cardDesc methods (chip_open_library equivalent):
    //    - cardDesc->method[304](cardDesc) → display_handle
    //    - cardDesc->method[704](cardDesc, ...) → video_mem_object
    //    - cardDesc->method[504](cardDesc) → verify
    //    - bi->CardBase = video_mem_object->field[20]
    //    - bi->UtilBase = cardDesc->method[448](cardDesc)  [IUtility]
    typedef APTR (*M304_t)(APTR);
    APTR display_handle = ((M304_t)cd[304/4])(cardDesc);
    if (!display_handle) return FALSE;

    typedef APTR (*M704_t)(APTR, ULONG, ULONG, ULONG, ULONG, ULONG, APTR);
    APTR video_obj = ((M704_t)cd[704/4])(cardDesc, 0, 0, 0x8000000A, 40, 0x8000000B, display_handle);
    if (!video_obj) return FALSE;

    bi->CardBase = (struct Library *)(((APTR *)video_obj)[20/4]);  // video_obj->field[20]

    APTR iUtility = ((GetExec_t)cd[448/4])(cardDesc);
    bi->UtilBase = (struct Library *)iUtility;

    // 8. Return bi (not TRUE) — PCIGraphics.card checks for non-zero
    return (BOOL)(ULONG)bi;
}
```

**Note**: This is speculative for VirtIOGPU. The SM502 hardware init (method[304], method[704]) is SM502-specific. For VirtIOGPU, PCIGraphics.card may not have a valid display_handle for a virtual device. The key requirements are:
- `bi->ChipBase` = cardDesc
- `bi->ExecBase` = SysBase (from method[448])
- `bi->UtilBase` ≠ NULL (PCIGraphics.card likely checks this)
- `bi->CardBase` set to something valid
- Return non-zero (bi pointer)

---

## Summary: Why "Cannot Configure" Persists

The root issue is that PCIGraphics.card's protocol goes beyond just filling the BoardInfo struct. It requires the chip driver to call back into cardDesc to open subsystems, and PCIGraphics.card validates those subsystems are open before accepting the chip. Our driver ignores all cardDesc callbacks except method[448] and returns wrong fields.

The fix requires implementing a `chip_open_library` equivalent that calls `cardDesc->method[304]` and `cardDesc->method[704]`, even if these return dummy handles for a virtual GPU.
