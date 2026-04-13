/*
 * boardinfo.h -- Picasso96 chip driver internal interface (reconstructed).
 *
 *  SDK BOUNDARY 
 * AmigaOS 4 does NOT publish the full BoardInfo struct or the chip driver
 * vtable layout -- Hyperion treats them as a Picasso96-private ABI.  The
 * SDK's <libraries/Picasso96.h> (V54.16) only exports format enums
 * (RGBFB_xxx / RGBFF_xxx) and peripheral types (struct P96Mode, struct
 * RenderInfo, struct YUVRenderInfo, struct TrueColorInfo).
 *
 * This header fills the gap: it reconstructs BoardInfo, ModeInfo,
 * BitMapExtra, and the 68-entry vtable from multiple open sources so
 * that a chip driver can be written in portable C rather than inline
 * assembly with raw offsets.
 *
 *  RECONSTRUCTION SOURCES 
 *   - WinUAE include/picasso96.h PSSO_BoardInfo_* offset macros
 *   - A2000-gfxcard drivers/gfx_vbcc/rtg.h (MNT open-source driver)
 *   - Binary analysis of siliconmotion502.chip 53.12 (2023)
 *   - Binary analysis of RadeonRX.chip 2.11 (2022)
 *
 *  INVARIANTS (do not change without re-verification) 
 *   MAXMODES = 5 (PLANAR, CHUNKY, HICOLOR, TRUECOLOR, TRUEALPHA)
 *   Vtable base offset = 274 bytes from start of BoardInfo
 *   Vtable size = 68 entries x 4 bytes = 272 bytes
 *   SpecialFeatures MinList starts at 274 + 272 = 546
 *   Documented fields end at offset 1418 (MouseSaveBuffer)
 *   PCIGraphics.card extended private area: 1418+ (see bottom of file)
 *
 *  SDK-PUBLIC TYPES AND CONSTANTS USED 
 * Pulled from <libraries/Picasso96.h>:
 *   RGBFTYPE enum (= enPixelFormat in graphics/gfx.h, same values)
 *   RGBFB_xxx / RGBFF_xxx enum values and bit masks
 *   struct RenderInfo (chip RenderInfo pattern)
 *
 * Pulled from <graphics/xxx.h>:
 *   struct BitMap (chip BitMap operations -- vtable takes this by ref)
 *   BMF_xxx / BMA_xxx -- used by chip_GetBitMapAttr implementation
 *   DIPF_xxx mode property flags -- used for display advertisement
 *   PIXF_xxx -- numerically identical to RGBFB_xxx, preferred for new code
 *
 * Pulled from <graphics/composite.h>:
 *   enPDOperator / COMPTAG_xxx -- used by chip_composite.c hook
 *
 * Pulled from <graphics/board.h> (V54):
 *   GBD_xxx tag IDs -- queried by graphics.library V54 apps via
 *   GetBoardDataTagList; answered from fields we populate in this struct.
 *
 *  RELATIONSHIP WITH PCIGRAPHICS.CARD 
 * PCIGraphics.card allocates a larger buffer than the documented struct
 * size (1418 bytes).  Fields above offset 1418 are private to the card.
 * We expose a small accessor layer at the bottom of this file for the
 * handful of private fields we need to touch (PCIDevice, EDID gate,
 * EDID mode list) -- those values were discovered by disassembly and
 * are stable across PCIGraphics.card v53.x releases.
 */

#ifndef P96_BOARDINFO_H
#define P96_BOARDINFO_H

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <exec/interrupts.h>
#include <libraries/Picasso96.h>

/* Forward declaration -- full struct defined in <expansion/pci.h>.
 * Avoids pulling that header into every chip_*.c transitively. */
struct PCIDevice;

#ifdef __GNUC__
# ifdef __PPC__
#  pragma pack(2)
# endif
#endif

/* -----------------------------------------------------------------------
 * MAXMODES -- number of display mode classes
 * PLANAR=0, CHUNKY=1, HICOLOR=2, TRUECOLOR=3, TRUEALPHA=4
 * ----------------------------------------------------------------------- */
#define MAXMODES  5

/* -----------------------------------------------------------------------
 * Pixel clock / PLL result from ResolvePixelClock / PixelClockDividers
 * ----------------------------------------------------------------------- */
struct P96ClockData {
    UWORD  Divider;
    UWORD  Multiplier;
    ULONG  PixelClock;
};

/* -----------------------------------------------------------------------
 * Screen mode (ModeInfo) -- describes a display timing
 * ----------------------------------------------------------------------- */
struct ModeInfo {
    struct Node  Node;       /* ln_Succ, ln_Pred, ln_Type, ln_Pri, ln_Name */
    UWORD        OpenCount;
    BOOL         Active;     /* BOOL is UWORD on AmigaOS */
    UWORD        Width;
    UWORD        Height;
    UBYTE        Depth;
    UBYTE        Flags;      /* GMF_* */
    UWORD        HorTotal;
    UWORD        HorBlankSize;
    UWORD        HorSyncStart;
    UWORD        HorSyncSize;
    UBYTE        HorSyncSkew;
    UBYTE        HorEnableSkew;
    UWORD        VerTotal;
    UWORD        VerBlankSize;
    UWORD        VerSyncStart;
    UWORD        VerSyncSize;
    UBYTE        clock;       /* clock index */
    UBYTE        clock_div;   /* clock divider */
    ULONG        PixelClock;  /* pixel clock in Hz */
};

/* -----------------------------------------------------------------------
 * Resolution -- groups ModeInfo entries for one WxH across depth classes.
 * Layout reverse-engineered from live AmigaOS 4.1 P96 data (March 2026).
 * Real struct is 80 bytes (NOT 72 as in WinUAE PSSO offsets).
 *
 * Verified against two live entries (640x480 FakeNative + BootVGA):
 *   Entry spacing = 0x50 = 80 bytes.
 *   Node.ln_Name = &P96ID (embedded, offset 14 from struct start).
 *   DisplayID at @42-45 (ULONG), Width/Height at @46-49 (UWORD each).
 *   Modes[5] at @52-71, BoardInfo* at @72-75, Reserved at @76-79.
 * ----------------------------------------------------------------------- */
#define MAXRESOLUTIONNAMELENGTH  22
#define P96ID_LENGTH              6

struct Resolution {
    struct Node  Node;                         /* @0   list linkage (14 bytes) */
    char         P96ID[P96ID_LENGTH];          /* @14  "RTG-0:" board ID      */
    char         Name[MAXRESOLUTIONNAMELENGTH]; /* @20  e.g. "SM502:800x600"  */
    ULONG        DisplayID;                    /* @42  P96 display mode ID    */
    UWORD        Width;                        /* @46                         */
    UWORD        Height;                       /* @48                         */
    UWORD        Flags;                        /* @50                         */
    struct ModeInfo *Modes[MAXMODES];          /* @52  one per depth class    */
    struct BoardInfo *BoardInfo;               /* @72  owner board            */
    ULONG        Reserved;                     /* @76  padding (always 0)     */
};
/* sizeof(Resolution) = 80 */

/* Resolution flags */
#define P96F_FAMILY  0x0001  /* multiple depths share this resolution */

/* ModeInfo flags */
#define GMF_DOUBLESCAN   0x01
#define GMF_INTERLACE    0x02
#define GMF_HPOLARITY    0x04
#define GMF_VPOLARITY    0x08

/* -----------------------------------------------------------------------
 * RenderInfo -- render surface descriptor (passed to blit functions)
 * PSSO_RenderInfo: Memory@0, BytesPerRow@4, pad@6, RGBFormat@8, sizeof=12
 * ----------------------------------------------------------------------- */
struct RenderInfoChip {
    APTR     Memory;        /* Framebuffer base address   */
    UWORD    BytesPerRow;   /* Stride in bytes            */
    WORD     pad;
    RGBFTYPE RGBFormat;     /* Pixel format               */
};

/* -----------------------------------------------------------------------
 * Template -- 1bpp blitter template
 * ----------------------------------------------------------------------- */
struct ChipTemplate {
    APTR   Memory;
    WORD   BytesPerRow;
    UBYTE  XOffset;
    UBYTE  DrawMode;
    ULONG  FgPen;
    ULONG  BgPen;
};

/* -----------------------------------------------------------------------
 * Pattern -- repeating fill pattern
 * ----------------------------------------------------------------------- */
struct ChipPattern {
    APTR   Memory;
    UWORD  XOffset;
    UWORD  YOffset;
    ULONG  FgPen;
    ULONG  BgPen;
    UBYTE  Size;      /* height = 1 << Size, width always 16 */
    UBYTE  DrawMode;
};

/* -----------------------------------------------------------------------
 * ColorIndexMapping -- direct-color CLUT lookup table passed to
 * BlitPlanar2Direct.  Apps with classic palette artwork (8-bit BitMaps)
 * use this to render onto a TrueColor RTG screen: each palette index
 * maps to a 32-bit ARGB value, and ColorMask says which channels matter
 * (typically 0x00FFFFFF for "RGB only, alpha is don't-care").
 *
 * Layout matches Picasso96 P96CardDevelop SDK exactly.
 * ----------------------------------------------------------------------- */
struct ColorIndexMapping {
    ULONG  ColorMask;       /* AND with each output color */
    ULONG  Colors[256];     /* index -> direct ARGB color */
};

/* -----------------------------------------------------------------------
 * Line -- P96 line drawing descriptor (passed to DrawLine as APTR)
 * ----------------------------------------------------------------------- */
struct ChipLine {
    WORD   X;             /* start X */
    WORD   Y;             /* start Y */
    UWORD  Length;        /* line length in pixels */
    WORD   dX;            /* delta X (signed) */
    WORD   dY;            /* delta Y (signed) */
    WORD   sDelta;        /* short axis delta (abs) */
    WORD   lDelta;        /* long axis delta (abs) */
    WORD   twoSDminusLD;  /* 2*sDelta - lDelta (Bresenham error init) */
    UWORD  LinePtrn;      /* 16-bit line pattern (0xFFFF = solid) */
    UWORD  PatternShift;  /* starting bit in pattern */
    ULONG  FgPen;         /* foreground color */
    ULONG  BgPen;         /* background color */
    BOOL   Horizontal;    /* TRUE if X is the long axis */
    UBYTE  DrawMode;      /* JAM1, JAM2, COMPLEMENT */
    UBYTE  Pad;
    UWORD  LinePtrnTotal; /* total pattern bits */
};

/* -----------------------------------------------------------------------
 * BitMapExtra -- augmented bitmap descriptor
 * ----------------------------------------------------------------------- */
struct BitMap;  /* forward declaration */
struct BitMapExtra {
    /* BoardNode (8 bytes) */
    APTR   bn_BoardInfo;
    APTR   bn_HashChain;
    /* rest */
    APTR   bme_Match;
    struct BitMap *bme_BitMap;
    APTR   bme_BoardInfo2;
    APTR   bme_MemChunk;
    struct RenderInfoChip bme_RenderInfo;  /* 12 bytes */
    UWORD  bme_Width;
    UWORD  bme_Height;
    UWORD  bme_Flags;
    UWORD  bme_BaseLevel;
    UWORD  bme_CurrentLevel;
    APTR   bme_CompanionMaster;
};

/* -----------------------------------------------------------------------
 * BoardInfo -- the central chip driver state structure.
 *
 * rtg.library allocates this structure. FindCard/InitCard fill in
 * board parameters and function pointers. The exact field layout must
 * match the on-Amiga ABI (PSSO_BoardInfo_* offsets from WinUAE).
 *
 * With MAXMODES=5 and pack(2):
 *
 * Offset  Field
 * ------  -----
 *      0  RegisterBase
 *      4  MemoryBase
 *      8  MemoryIOBase
 *     12  MemorySize
 *     16  BoardName
 *     20  VBIName[32]
 *     52  CardBase
 *     56  ChipBase
 *     60  ExecBase
 *     64  UtilBase
 *     68  HardInterrupt (22 bytes)
 *     90  SoftInterrupt (22 bytes)
 *    112  BoardLock (46 bytes)
 *    158  ResolutionsList (12 bytes)
 *    170  BoardType
 *    174  PaletteChipType
 *    178  GraphicsControllerType
 *    182  MoniSwitch
 *    184  BitsPerCannon
 *    186  Flags
 *    190  SoftSpriteFlags
 *    192  ChipFlags
 *    194  CardFlags
 *    198  BoardNum
 *    200  RGBFormats (WORD, signed)
 *    202  MaxHorValue[5]       (10 bytes)
 *    212  MaxVerValue[5]       (10 bytes)
 *    222  MaxHorResolution[5]  (10 bytes)
 *    232  MaxVerResolution[5]  (10 bytes)
 *    242  MaxMemorySize
 *    246  MaxChunkSize
 *    250  MemoryClock
 *    254  PixelClockCount[5]   (20 bytes)
 *    274  AllocCardMem  <- vtable begins here (fp[0])
 *    ...  (68 function pointers x 4 = 272 bytes)
 *    546  SpecialFeatures (MinList, 12 bytes)
 *    558  ModeInfo (APTR)
 *    562  RGBFormat (ULONG)
 *    566  XOffset (WORD)
 *    568  YOffset (WORD)
 *    570  Depth (UBYTE)
 *    571  ClearMask (UBYTE)
 *    572  Border (WORD/BOOL)
 *    574  Mask (ULONG)
 *    578  CLUT[256x3]          (768 bytes)
 *   1346  ViewPort (APTR)
 *   1350  VisibleBitMap (APTR)
 *   1354  BitMapExtra (APTR)
 *   1358  BitMapList (MinList, 12 bytes)
 *   1370  MemList (MinList, 12 bytes)
 *   1382  MouseX (WORD)
 *   1384  MouseY (WORD)
 *   1386  MouseWidth (UBYTE)
 *   1387  MouseHeight (UBYTE)
 *   1388  MouseXOffset (UBYTE)
 *   1389  MouseYOffset (UBYTE)
 *   1390  MouseImage (APTR)
 *   1394  MousePens (APTR, 4 bytes)
 *   1398  MouseRect (8 bytes)
 *   1406  MouseChunky (APTR)
 *   1410  MouseRendered (APTR)
 *   1414  MouseSaveBuffer (APTR)
 * ----------------------------------------------------------------------- */
struct BoardInfo {
    /* ---- Fixed hardware addresses (set by FindCard) ---- */
    APTR       RegisterBase;        /* @0   Legacy VGA port registers      */
    APTR       MemoryBase;          /* @4   Linear framebuffer base (virt) */
    APTR       MemoryIOBase;        /* @8   MMIO base (non-VGA regs)       */
    ULONG      MemorySize;          /* @12  Total framebuffer bytes        */

    /* ---- Board identification ---- */
    STRPTR     BoardName;           /* @16  e.g. "VirtIOGPU"              */
    char       VBIName[32];         /* @20  VBlank interrupt name          */

    /* ---- Library bases (filled by rtg.library) ---- */
    APTR       CardBase;            /* @52  card.library base              */
    APTR       ChipBase;            /* @56  chip library base              */
    APTR       ExecBase;            /* @60  exec.library base              */
    APTR       UtilBase;            /* @64  utility.library base           */

    /* ---- Interrupt structures (filled by rtg.library) ---- */
    struct Interrupt HardInterrupt; /* @68  (22 bytes)                     */
    struct Interrupt SoftInterrupt; /* @90  (22 bytes)                     */

    /* ---- Board semaphore (filled by rtg.library) ---- */
    struct SignalSemaphore BoardLock; /* @112 (46 bytes)                   */

    /* ---- Resolution list (managed by rtg.library) ---- */
    struct MinList ResolutionsList; /* @158 (12 bytes)                     */

    /* ---- Board type identification ---- */
    ULONG      BoardType;           /* @170 BT_* enum value                */
    ULONG      PaletteChipType;     /* @174                                */
    ULONG      GraphicsControllerType; /* @178                             */

    /* ---- Monitor/DAC settings ---- */
    UWORD      MoniSwitch;          /* @182                                */
    UWORD      BitsPerCannon;       /* @184 bits per DAC channel           */

    /* ---- Flags ---- */
    ULONG      Flags;               /* @186 BIB_* / BIF_* bit flags       */
    UWORD      SoftSpriteFlags;     /* @190 pixel formats for soft sprite  */
    UWORD      ChipFlags;           /* @192 private chip driver flags      */
    ULONG      CardFlags;           /* @194 private card driver flags      */

    /* ---- Board number and format masks ---- */
    UWORD      BoardNum;            /* @198 assigned board index           */
    WORD       RGBFormats;          /* @200 supported RGBFF_* mask (signed)*/

    /* ---- Mode capability tables (MAXMODES=5 entries each) ---- */
    UWORD      MaxHorValue[MAXMODES];      /* @202 max bitmap width per mode  */
    UWORD      MaxVerValue[MAXMODES];      /* @212 max bitmap height per mode */
    UWORD      MaxHorResolution[MAXMODES]; /* @222 max display width per mode */
    UWORD      MaxVerResolution[MAXMODES]; /* @232 max display height per mode*/

    /* ---- Memory limits ---- */
    ULONG      MaxMemorySize;       /* @242 max allocatable memory         */
    ULONG      MaxChunkSize;        /* @246 max contiguous allocation      */
    ULONG      MemoryClock;         /* @250 memory bus clock in Hz         */

    /* ---- Pixel clock counts per mode ---- */
    ULONG      PixelClockCount[MAXMODES]; /* @254 clocks available per mode*/

    /* ================================================================
     * Function pointer table -- 68 entries, starts at offset 274.
     * Chip driver fills these in InitCard.
     * rtg.library calls through them for all hardware operations.
     * Any unimplemented entry MUST point to a valid no-op stub.
     * ================================================================ */

    /* [0]  AllocCardMem -- allocate board memory
     *      APTR AllocCardMem(struct BoardInfo *bi, ULONG size, BOOL force, BOOL nocache) */
    APTR       (*AllocCardMem)(struct BoardInfo *bi, ULONG size, BOOL force, BOOL nocache);

    /* [1]  FreeCardMem -- free board memory
     *      BOOL FreeCardMem(struct BoardInfo *bi, APTR mem) */
    BOOL       (*FreeCardMem)(struct BoardInfo *bi, APTR mem);

    /* [2]  SetSwitch -- enable/disable RTG output
     *      BOOL SetSwitch(struct BoardInfo *bi, BOOL enabled) */
    BOOL       (*SetSwitch)(struct BoardInfo *bi, BOOL enabled);

    /* [3]  SetColorArray -- load CLUT palette (8-bit mode)
     *      void SetColorArray(struct BoardInfo *bi, UWORD start, UWORD count) */
    void       (*SetColorArray)(struct BoardInfo *bi, UWORD start, UWORD count);

    /* [4]  SetDAC -- set DAC format
     *      void SetDAC(struct BoardInfo *bi, RGBFTYPE format) */
    void       (*SetDAC)(struct BoardInfo *bi, RGBFTYPE format);

    /* [5]  SetGC -- configure graphics context (width/height/depth)
     *      void SetGC(struct BoardInfo *bi, struct ModeInfo *mi, BOOL border) */
    void       (*SetGC)(struct BoardInfo *bi, struct ModeInfo *mi, BOOL border);

    /* [6]  SetPanning -- set visible window offset
     *      void SetPanning(struct BoardInfo *bi, APTR mem, UWORD width,
     *                      WORD xOff, WORD yOff, RGBFTYPE format) */
    void       (*SetPanning)(struct BoardInfo *bi, APTR mem, UWORD width,
                             WORD xOff, WORD yOff, RGBFTYPE format);

    /* [7]  CalculateBytesPerRow
     *      UWORD CalculateBytesPerRow(struct BoardInfo *bi, UWORD width, RGBFTYPE format) */
    UWORD      (*CalculateBytesPerRow)(struct BoardInfo *bi, UWORD width, RGBFTYPE format);

    /* [8]  CalculateMemory -- calculate required framebuffer size
     *      APTR CalculateMemory(struct BoardInfo *bi, ULONG size, RGBFTYPE format) */
    APTR       (*CalculateMemory)(struct BoardInfo *bi, ULONG size, RGBFTYPE format);

    /* [9]  GetCompatibleFormats -- return supported RGBFF_* mask
     *      ULONG GetCompatibleFormats(struct BoardInfo *bi, RGBFTYPE format) */
    ULONG      (*GetCompatibleFormats)(struct BoardInfo *bi, RGBFTYPE format);

    /* [10] SetDisplay -- show/hide display
     *      BOOL SetDisplay(struct BoardInfo *bi, BOOL enabled) */
    BOOL       (*SetDisplay)(struct BoardInfo *bi, BOOL enabled);

    /* [11] ResolvePixelClock -- find best PLL for given clock
     *      ULONG ResolvePixelClock(struct BoardInfo *bi, struct ModeInfo *mi,
     *                              ULONG pixelClock, RGBFTYPE format) */
    ULONG      (*ResolvePixelClock)(struct BoardInfo *bi, struct ModeInfo *mi,
                                   ULONG pixelClock, RGBFTYPE format);

    /* [12] GetPixelClock -- enumerate valid pixel clocks
     *      ULONG GetPixelClock(struct BoardInfo *bi, struct ModeInfo *mi,
     *                          ULONG index, RGBFTYPE format) */
    ULONG      (*GetPixelClock)(struct BoardInfo *bi, struct ModeInfo *mi,
                               ULONG index, RGBFTYPE format);

    /* [13] SetClock -- program PLL to selected clock
     *      void SetClock(struct BoardInfo *bi) */
    void       (*SetClock)(struct BoardInfo *bi);

    /* [14] SetMemoryMode -- configure memory controller
     *      void SetMemoryMode(struct BoardInfo *bi, RGBFTYPE format) */
    void       (*SetMemoryMode)(struct BoardInfo *bi, RGBFTYPE format);

    /* [15] SetWriteMask
     *      void SetWriteMask(struct BoardInfo *bi, UBYTE mask) */
    void       (*SetWriteMask)(struct BoardInfo *bi, UBYTE mask);

    /* [16] SetClearMask
     *      void SetClearMask(struct BoardInfo *bi, UBYTE mask) */
    void       (*SetClearMask)(struct BoardInfo *bi, UBYTE mask);

    /* [17] SetReadPlane
     *      void SetReadPlane(struct BoardInfo *bi, UBYTE plane) */
    void       (*SetReadPlane)(struct BoardInfo *bi, UBYTE plane);

    /* [18] WaitVerticalSync -- wait for VBlank
     *      void WaitVerticalSync(struct BoardInfo *bi, BOOL beam) */
    void       (*WaitVerticalSync)(struct BoardInfo *bi, BOOL beam);

    /* [19] SetInterrupt -- enable/disable VBlank interrupt
     *      BOOL SetInterrupt(struct BoardInfo *bi, BOOL enabled) */
    BOOL       (*SetInterrupt)(struct BoardInfo *bi, BOOL enabled);

    /* [20] WaitBlitter -- wait for 2D blitter idle
     *      void WaitBlitter(struct BoardInfo *bi) */
    void       (*WaitBlitter)(struct BoardInfo *bi);

    /* [21..24] canonical: ScrollPlanar, ScrollPlanarDefault,
     * UpdatePlanar, UpdatePlanarDefault.  For boards that maintain a
     * planar memory aperture; not applicable to a virtual GPU. */
    void       (*_fp21)(void);
    void       (*_fp22)(void);
    void       (*_fp23)(void);
    void       (*_fp24)(void);

    /* [25] BlitPlanar2Chunky -- planar->chunky blit
     *      void BlitPlanar2Chunky(struct BoardInfo *bi, struct BitMap *bm,
     *                             struct RenderInfoChip *ri,
     *                             WORD x, WORD y, WORD dx, WORD dy,
     *                             UWORD w, UWORD h, UBYTE mask, UBYTE layer) */
    void       (*BlitPlanar2Chunky)(struct BoardInfo *bi, struct BitMap *bm,
                                   struct RenderInfoChip *ri,
                                   WORD x, WORD y, WORD dx, WORD dy,
                                   UWORD w, UWORD h, UBYTE mask, UBYTE layer);

    /* [26] BlitPlanar2Chunky fallback */
    void       (*BlitPlanar2ChunkyFB)(struct BoardInfo *bi, struct BitMap *bm,
                                      struct RenderInfoChip *ri,
                                      WORD x, WORD y, WORD dx, WORD dy,
                                      UWORD w, UWORD h, UBYTE mask, UBYTE layer);

    /* [27] FillRect -- fill rectangle with solid colour
     *      void FillRect(struct BoardInfo *bi, struct RenderInfoChip *ri,
     *                    WORD x, WORD y, UWORD w, UWORD h,
     *                    ULONG color, UBYTE mask, RGBFTYPE format) */
    void       (*FillRect)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                           WORD x, WORD y, UWORD w, UWORD h,
                           ULONG color, UBYTE mask, RGBFTYPE format);

    /* [28] FillRect fallback */
    void       (*FillRectFB)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                             WORD x, WORD y, UWORD w, UWORD h,
                             ULONG color, UBYTE mask, RGBFTYPE format);

    /* [29] InvertRect -- invert rectangle pixels
     *      void InvertRect(struct BoardInfo *bi, struct RenderInfoChip *ri,
     *                      WORD x, WORD y, UWORD w, UWORD h,
     *                      UBYTE mask, RGBFTYPE format) */
    void       (*InvertRect)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                             WORD x, WORD y, UWORD w, UWORD h,
                             UBYTE mask, RGBFTYPE format);

    /* [30] InvertRect fallback */
    void       (*InvertRectFB)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                               WORD x, WORD y, UWORD w, UWORD h,
                               UBYTE mask, RGBFTYPE format);

    /* [31] BlitRect -- copy rectangle within same surface
     *      void BlitRect(struct BoardInfo *bi, struct RenderInfoChip *ri,
     *                    WORD x, WORD y, WORD dx, WORD dy,
     *                    UWORD w, UWORD h, UBYTE mask) */
    void       (*BlitRect)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                           WORD x, WORD y, WORD dx, WORD dy,
                           UWORD w, UWORD h, UBYTE mask);

    /* [32] BlitRect fallback */
    void       (*BlitRectFB)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                             WORD x, WORD y, WORD dx, WORD dy,
                             UWORD w, UWORD h, UBYTE mask);

    /* [33] BlitTemplate -- blit 1bpp template
     *      void BlitTemplate(struct BoardInfo *bi, struct RenderInfoChip *ri,
     *                        struct ChipTemplate *tmpl,
     *                        WORD x, WORD y, UWORD w, UWORD h,
     *                        UBYTE mask, RGBFTYPE format) */
    void       (*BlitTemplate)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                               struct ChipTemplate *tmpl,
                               WORD x, WORD y, UWORD w, UWORD h,
                               UBYTE mask, RGBFTYPE format);

    /* [34] BlitTemplate fallback */
    void       (*BlitTemplateFB)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                                 struct ChipTemplate *tmpl,
                                 WORD x, WORD y, UWORD w, UWORD h,
                                 UBYTE mask, RGBFTYPE format);

    /* [35] BlitPattern -- blit repeating pattern
     *      void BlitPattern(struct BoardInfo *bi, struct RenderInfoChip *ri,
     *                       struct ChipPattern *pat,
     *                       WORD x, WORD y, UWORD w, UWORD h,
     *                       UBYTE mask, RGBFTYPE format) */
    void       (*BlitPattern)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                              struct ChipPattern *pat,
                              WORD x, WORD y, UWORD w, UWORD h,
                              UBYTE mask, RGBFTYPE format);

    /* [36] BlitPattern fallback */
    void       (*BlitPatternFB)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                                struct ChipPattern *pat,
                                WORD x, WORD y, UWORD w, UWORD h,
                                UBYTE mask, RGBFTYPE format);

    /* [37] DrawLine -- hardware line draw
     *      void DrawLine(struct BoardInfo *bi, struct RenderInfoChip *ri,
     *                    struct Line *line, UBYTE mask, RGBFTYPE format) */
    void       (*DrawLine)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                           APTR line, UBYTE mask, RGBFTYPE format);

    /* [38] DrawLine fallback */
    void       (*DrawLineFB)(struct BoardInfo *bi, struct RenderInfoChip *ri,
                             APTR line, UBYTE mask, RGBFTYPE format);

    /* [39] BlitRectNoMaskComplete -- blit between two render infos
     *      void BlitRectNoMaskComplete(struct BoardInfo *bi,
     *                                  struct RenderInfoChip *src,
     *                                  struct RenderInfoChip *dst,
     *                                  WORD x, WORD y, WORD dx, WORD dy,
     *                                  UWORD w, UWORD h, UBYTE opcode,
     *                                  RGBFTYPE format) */
    void       (*BlitRectNoMaskComplete)(struct BoardInfo *bi,
                                        struct RenderInfoChip *src,
                                        struct RenderInfoChip *dst,
                                        WORD x, WORD y, WORD dx, WORD dy,
                                        UWORD w, UWORD h, UBYTE opcode,
                                        RGBFTYPE format);

    /* [40] BlitRectNoMaskComplete fallback */
    void       (*BlitRectNoMaskCompleteFB)(struct BoardInfo *bi,
                                           struct RenderInfoChip *src,
                                           struct RenderInfoChip *dst,
                                           WORD x, WORD y, WORD dx, WORD dy,
                                           UWORD w, UWORD h, UBYTE opcode,
                                           RGBFTYPE format);

    /* [41] BlitPlanar2Direct -- planar->direct-color blit with CLUT lookup
     *      void BlitPlanar2Direct(struct BoardInfo *bi, struct BitMap *bm,
     *                             struct RenderInfoChip *ri,
     *                             struct ColorIndexMapping *clut,
     *                             SHORT x, SHORT y, SHORT dx, SHORT dy,
     *                             SHORT w, SHORT h, UBYTE minterm, UBYTE mask)
     * Slot index confirmed against Picasso96 P96CardDevelop boardinfo.h. */
    void       (*BlitPlanar2Direct)(struct BoardInfo *bi, struct BitMap *bm,
                                    struct RenderInfoChip *ri,
                                    struct ColorIndexMapping *clut,
                                    WORD x, WORD y, WORD dx, WORD dy,
                                    WORD w, WORD h, UBYTE minterm, UBYTE mask);

    /* [42] BlitPlanar2Direct fallback */
    void       (*BlitPlanar2DirectFB)(struct BoardInfo *bi, struct BitMap *bm,
                                      struct RenderInfoChip *ri,
                                      struct ColorIndexMapping *clut,
                                      WORD x, WORD y, WORD dx, WORD dy,
                                      WORD w, WORD h, UBYTE minterm, UBYTE mask);

    /* Slots 43-52: layout uncertain in AOS4.  Disassembly of
     * graphics.library.kmod (v53.x) shows it installs default function
     * pointers at slots 41/42 and 51/52 (matching canonical
     * BlitPlanar2Direct[Default] and WriteYUVRect[Default]) but does
     * NOT install anything at slots 43-50.  Either AOS4 doesn't use
     * those canonical slots (EnableSoftSprite[Default],
     * AllocCardMemAbs, SetSplitPosition, ReInitMemory,
     * GetCompatibleDACFormats, CoerceMode, Reserved3Default), or it
     * uses them only when chips advertise the corresponding flags --
     * we can't tell without flag-conditional reverse engineering.
     * Keep all ten as opaque stub_void targets.  Wiring named
     * function pointers here remains risky.
     */
    void       (*_fp43)(void);
    void       (*_fp44)(void);
    void       (*_fp45)(void);
    void       (*_fp46)(void);
    void       (*_fp47)(void);
    void       (*_fp48)(void);
    void       (*_fp49)(void);
    void       (*_fp50)(void);
    void       (*_fp51)(void);
    void       (*_fp52)(void);

    /* [53] GetVSyncState -- query whether in VBlank
     *      BOOL GetVSyncState(struct BoardInfo *bi, BOOL toggle) */
    BOOL       (*GetVSyncState)(struct BoardInfo *bi, BOOL toggle);

    /* [54] GetCurrentY -- return current beam position Y
     *      ULONG GetCurrentY(struct BoardInfo *bi) */
    ULONG      (*GetCurrentY)(struct BoardInfo *bi);

    /* [55] SetDPMSLevel -- DPMS power management
     *      void SetDPMSLevel(struct BoardInfo *bi, ULONG level) */
    void       (*SetDPMSLevel)(struct BoardInfo *bi, ULONG level);

    /* [56] ResetChip -- soft reset
     *      void ResetChip(struct BoardInfo *bi) */
    void       (*ResetChip)(struct BoardInfo *bi);

    /* [57] canonical: GetFeatureAttrs (V45+ SpecialFeature query). */
    void       (*_fp57)(void);

    /* [58] AllocBitMap -- allocate RTG bitmap
     *      APTR AllocBitMap(struct BoardInfo *bi, ...) */
    APTR       (*AllocBitMap)(struct BoardInfo *bi, ULONG width, ULONG height,
                              ULONG depth, RGBFTYPE format, struct BitMap *friend_bm);

    /* [59] FreeBitMap
     *      BOOL FreeBitMap(struct BoardInfo *bi, struct BitMapExtra *bme) */
    BOOL       (*FreeBitMap)(struct BoardInfo *bi, struct BitMapExtra *bme);

    /* [60] GetBitMapAttr
     *      ULONG GetBitMapAttr(struct BoardInfo *bi, struct BitMapExtra *bme, ULONG attr) */
    ULONG      (*GetBitMapAttr)(struct BoardInfo *bi, struct BitMapExtra *bme, ULONG attr);

    /* [61] SetSprite -- enable/disable hardware cursor
     *      BOOL SetSprite(struct BoardInfo *bi, BOOL activate, RGBFTYPE format) */
    BOOL       (*SetSprite)(struct BoardInfo *bi, BOOL activate, RGBFTYPE format);

    /* [62] SetSpritePosition -- move hardware cursor
     *      void SetSpritePosition(struct BoardInfo *bi, UWORD x, UWORD y, RGBFTYPE format) */
    void       (*SetSpritePosition)(struct BoardInfo *bi, UWORD x, UWORD y,
                                   RGBFTYPE format);

    /* [63] SetSpriteImage -- upload cursor pixels
     *      void SetSpriteImage(struct BoardInfo *bi, RGBFTYPE format) */
    void       (*SetSpriteImage)(struct BoardInfo *bi, RGBFTYPE format);

    /* [64] SetSpriteColor -- set indexed sprite palette entry
     *      void SetSpriteColor(struct BoardInfo *bi, UBYTE idx,
     *                          UBYTE r, UBYTE g, UBYTE b, RGBFTYPE format) */
    void       (*SetSpriteColor)(struct BoardInfo *bi, UBYTE idx,
                                UBYTE r, UBYTE g, UBYTE b, RGBFTYPE format);

    /* [65..67] canonical: CreateFeature, SetFeatureAttrs, DeleteFeature
     * (V45+ SpecialFeature lifecycle for FlickerFixer, VideoCapture,
     * VideoWindow, MemoryWindow).  AOS4 graphics.library.kmod installs
     * defaults at all three offsets (verified by stw at bi+534/538/542),
     * so these slots ARE function pointers in AOS4.  Signatures still
     * unverified, so kept as opaque. */
    void       (*_fp65)(void);
    void       (*_fp66)(void);
    void       (*_fp67)(void);

    /* ================================================================
     * Runtime fields -- managed by rtg.library after init.
     * Chip driver should not write to these directly.
     * ================================================================ */

    struct MinList SpecialFeatures;  /* @546 feature extension list        */
    struct ModeInfo *ModeInfo;       /* @558 current active ModeInfo       */
    ULONG      RGBFormat;            /* @562 current active pixel format   */
    WORD       XOffset;              /* @566                               */
    WORD       YOffset;              /* @568                               */
    UBYTE      Depth;                /* @570 current bit depth             */
    UBYTE      ClearMask;            /* @571                               */
    WORD       Border;               /* @572 (BOOL-sized)                  */
    ULONG      Mask;                 /* @574                               */
    UBYTE      CLUT[3 * 256];        /* @578 palette (R,G,B x 256)        */

    APTR       ViewPort;             /* @1346                              */
    APTR       VisibleBitMap;        /* @1350                              */
    APTR       BitMapExtra;          /* @1354                              */
    struct MinList BitMapList;       /* @1358 (12 bytes)                   */
    struct MinList MemList;          /* @1370 (12 bytes)                   */

    /* Sprite/cursor runtime state */
    WORD       MouseX;               /* @1382                              */
    WORD       MouseY;               /* @1384                              */
    UBYTE      MouseWidth;           /* @1386                              */
    UBYTE      MouseHeight;          /* @1387                              */
    UBYTE      MouseXOffset;         /* @1388 hot spot X                   */
    UBYTE      MouseYOffset;         /* @1389 hot spot Y                   */
    APTR       MouseImage;           /* @1390 sprite pixel data            */
    APTR       MousePens;            /* @1394                              */
    WORD       MouseRect[4];         /* @1398 (8 bytes: MinX,MinY,MaxX,MaxY) */
    APTR       MouseChunky;          /* @1406                              */
    APTR       MouseRendered;        /* @1410                              */
    APTR       MouseSaveBuffer;      /* @1414                              */
};

/* -----------------------------------------------------------------------
 * BIF_* / BIB_* flags for BoardInfo.Flags
 * ----------------------------------------------------------------------- */
#define BIB_HARDWARESPRITE      0
#define BIB_NOMEMORYMODEMIX     1
#define BIB_NEEDSALIGNMENT      2
/* Bits 3..7 from canonical Picasso96 boardinfo.h (P96CardDevelop SDK).
 * Bit numbers verified against AOS4 RadeonRX.chip disassembly: at
 * 0x1001b70 it does `ori r10,r10,16` (= 1<<4) before storing back to
 * bi->Flags, which is canonical BIB_VBLANKINTERRUPT.  Bits 25/27 in
 * earlier versions of this header were guesses and were not actually
 * exercised by working code -- the real numbers are below. */
#define BIB_CACHEMODECHANGE     3
#define BIB_VBLANKINTERRUPT     4
#define BIB_HASSPRITEBUFFER     5
#define BIB_VGASCREENSPLIT      6
#define BIB_DBLCLOCKHALFSPRITEX 7
#define BIB_DBLSCANDBLSPRITEY   8
#define BIB_ILACEHALFSPRITEY    9
#define BIB_ILACEDBLROWOFFSET   10
#define BIB_FLICKERFIXER        12
#define BIB_VIDEOCAPTURE        13
#define BIB_VIDEOWINDOW         14
#define BIB_BLITTER             15
#define BIB_HIRESSPRITE         16
#define BIB_BIGSPRITE           17
#define BIB_BORDEROVERRIDE      18
#define BIB_BORDERBLANK         19
#define BIB_INDISPLAYCHAIN      20
#define BIB_QUIET               21
#define BIB_NOMASKBLITS         22
#define BIB_NOC2PBLITS          23
#define BIB_NOBLITTER           24
#define BIB_SYSTEM2SCREENBLITS  25
#define BIB_GRANTDIRECTACCESS   26
#define BIB_PALETTESWITCH       27
#define BIB_DACSWITCH           28
#define BIB_NOMASKEDBLITS       29
#define BIB_OVERCLOCK           31

#define BIF_HARDWARESPRITE      (1 << BIB_HARDWARESPRITE)
#define BIF_NOMEMORYMODEMIX     (1 << BIB_NOMEMORYMODEMIX)
#define BIF_HASSPRITEBUFFER     (1 << BIB_HASSPRITEBUFFER)
#define BIF_FLICKERFIXER        (1 << BIB_FLICKERFIXER)
#define BIF_VIDEOWINDOW         (1 << BIB_VIDEOWINDOW)
#define BIF_BLITTER             (1 << BIB_BLITTER)
#define BIF_HIRESSPRITE         (1 << BIB_HIRESSPRITE)
#define BIF_BIGSPRITE           (1 << BIB_BIGSPRITE)
#define BIF_INDISPLAYCHAIN      (1 << BIB_INDISPLAYCHAIN)
#define BIF_NOMASKBLITS         (1 << BIB_NOMASKBLITS)
#define BIF_NOC2PBLITS          (1 << BIB_NOC2PBLITS)
#define BIF_NOBLITTER           (1 << BIB_NOBLITTER)
#define BIF_VBLANKINTERRUPT     (1 << BIB_VBLANKINTERRUPT)
#define BIF_GRANTDIRECTACCESS   (1 << BIB_GRANTDIRECTACCESS)
#define BIF_CACHEMODECHANGE     (1 << BIB_CACHEMODECHANGE)
#define BIF_IGNOREMASK          BIF_NOMASKBLITS

/* -----------------------------------------------------------------------
 * DPMS level constants (for SetDPMSLevel)
 * ----------------------------------------------------------------------- */
#define DPMS_ON          0
#define DPMS_STANDBY     1
#define DPMS_SUSPEND     2
#define DPMS_OFF         3

/* -----------------------------------------------------------------------
 * BoardType constants (BoardInfo.GraphicsControllerType).
 *
 * Low numbers (1-13) are Hyperion/Picasso96 reserved for specific real
 * graphics-chip families (S3 Trio, Cirrus GD5426, Permedia2, etc.).
 * 14 was assigned to "uaegfx" -- the UAE emulator's reference driver --
 * and has become the informal "generic modern P96 board" value.
 * SM502.chip uses 11 (its own chip-specific value).
 *
 * BT_VIRTIOGPU chooses a value outside Hyperion's reserved range so we
 * are not masquerading as another chip.  rtg.library does not gate any
 * code paths on BoardType as far as reverse-engineering has shown -- it
 * is purely an identifier exposed to tools like gfxbench2d that display
 * "Graphics Controller Type: N".  Using a value >= 0x80 keeps us well
 * clear of any future Hyperion additions to the low range.
 * ----------------------------------------------------------------------- */
#define BT_NoBoard       0
#define BT_uaegfx        14     /* legacy: UAE emulator driver (historical) */
#define BT_VIRTIOGPU     0x56   /* 'V' -- identifies virtiogpu.chip */

/* -----------------------------------------------------------------------
 * PCIGraphics.card extended private area accessors.
 *
 * PCIGraphics.card allocates a BoardInfo buffer larger than the documented
 * struct (which ends at offset 1418).  The region from 1418 upward is
 * private to PCIGraphics.card and holds PCI device association, EDID
 * registration gates, and the display-mode mode list.  Offsets below were
 * determined by disassembling PCIGraphics.card -- they are stable across
 * v53.x releases but not part of any published API, so we keep this as a
 * small accessor layer rather than fabricating struct fields that a naive
 * reader might assume are authoritative.
 *
 * BOARDINFO_OFF_PCIDEVICE   offset of PCIDevice* -- set by PCIGraphics.card
 *                           during its PCI scan; our VirtIOGPUBoard hook
 *                           forces this so InitBoard dispatches to our
 *                           chip's FindCard vector.
 * BOARDINFO_OFF_EDID_GATE   a gate value graphics.library sets when the
 *                           EDID-registration mechanism is available;
 *                           callers check it's > 1777 before using
 *                           BOARDINFO_OFF_EDID_MODELIST.
 * BOARDINFO_OFF_EDID_MODELIST  MinList of EDID-registered mode-entry
 *                           records.  Each 128-byte entry holds a display
 *                           name, limits, BoardInfo back-pointer, an EDID
 *                           buffer, and a ConfigureDisplay callback.
 *                           Pattern from SM502.chip.
 * ----------------------------------------------------------------------- */
#define BOARDINFO_OFF_PCIDEVICE      1630
#define BOARDINFO_OFF_EDID_GATE      1634
#define BOARDINFO_OFF_EDID_MODELIST  1714
#define BOARDINFO_EDID_GATE_MIN      1777  /* gate must exceed this */

/* Canonical Picasso96 P96CardDevelop layout offsets in the BoardInfo
 * extension area, verified against AOS4 RadeonRX.chip disassembly.
 *
 * MemorySpaceBase / MemorySpaceSize (both ULONG): physical address
 * window for video RAM.  RadeonRX writes them at +1546 / +1550 in
 * InitChip alongside MemoryBase/MemorySize.
 *
 * SyncTime (struct timeval, 8 bytes): system time when the screen was
 * set up, used for pseudo-vblank arithmetic.  RadeonRX writes both
 * halves at +1558 / +1562 during init.
 *
 * EssentialFormats (LONG): RGBFormats used when the user has not
 * picked "all".  Mirrors the chip's RGBFormats mask.
 *
 * MaxBMWidth / MaxBMHeight (both ULONG): absolute upper bounds on
 * bitmap dimensions.  Used by p96AllocBitMap to clip oversize requests.
 *
 * **PCI VendorID / DeviceID at +1566 / +1568 (both UWORD)** -- this
 * is the layout AOS4 PCIGraphics.card uses.  Disassembly at
 * PCIGraphics.card 0x1001120 / 0x1001134 shows:
 *     IPCI->ReadConfigWord(self, 0) -> sth bi+1566   (Vendor)
 *     IPCI->ReadConfigWord(self, 2) -> sth bi+1568   (Device)
 * These two UWORDs occupy the same bytes that canonical Picasso96
 * uses for SyncPeriod (ULONG).  AOS4 PCIGraphics.card has repurposed
 * the slot for board identification; chip drivers MUST NOT write a
 * ULONG SyncPeriod here or they will trash the PCI IDs that
 * graphics.library v54 GBD_PCIVendorID / GBD_PCIProductID queries
 * read.  RadeonRX.chip ALSO writes a ULONG here in its InitChip --
 * this looks like a real bug in RadeonRX, not a counter-example.
 */
#define BOARDINFO_OFF_MEMORY_SPACE_BASE  1546
#define BOARDINFO_OFF_MEMORY_SPACE_SIZE  1550
#define BOARDINFO_OFF_SYNC_TIME_SECS     1558
#define BOARDINFO_OFF_SYNC_TIME_USECS    1562
#define BOARDINFO_OFF_PCI_VENDOR_ID      1566
#define BOARDINFO_OFF_PCI_DEVICE_ID      1568
#define BOARDINFO_OFF_ESSENTIAL_FORMATS  1616
#define BOARDINFO_OFF_MAX_BM_WIDTH       1642
#define BOARDINFO_OFF_MAX_BM_HEIGHT      1646

static inline struct PCIDevice *boardinfo_get_pcidevice(struct BoardInfo *bi)
{
    return *(struct PCIDevice **)((UBYTE *)bi + BOARDINFO_OFF_PCIDEVICE);
}

static inline void boardinfo_set_pcidevice(struct BoardInfo *bi, struct PCIDevice *pciDev)
{
    *(struct PCIDevice **)((UBYTE *)bi + BOARDINFO_OFF_PCIDEVICE) = pciDev;
}

static inline uint32 boardinfo_edid_gate(struct BoardInfo *bi)
{
    return *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_EDID_GATE);
}

static inline struct MinList *boardinfo_edid_modelist(struct BoardInfo *bi)
{
    return (struct MinList *)((UBYTE *)bi + BOARDINFO_OFF_EDID_MODELIST);
}

static inline void boardinfo_set_memory_space(struct BoardInfo *bi,
                                              APTR base, uint32 size)
{
    *(APTR   *)((UBYTE *)bi + BOARDINFO_OFF_MEMORY_SPACE_BASE) = base;
    *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_MEMORY_SPACE_SIZE) = size;
}

static inline void boardinfo_set_sync_time(struct BoardInfo *bi,
                                           uint32 secs, uint32 usecs)
{
    *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_SYNC_TIME_SECS)  = secs;
    *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_SYNC_TIME_USECS) = usecs;
}

/* Stash PCI Vendor/Device IDs in the location AOS4 PCIGraphics.card
 * uses for board identification (read by graphics.library v54
 * GBD_PCIVendorID / GBD_PCIProductID queries).  PCIGraphics.card
 * already populates these from the live PCIDevice during card
 * discovery, but our SetMethod-hook injection bypasses that path,
 * so call this from chip_InitCard_C to make sure the slots reflect
 * our real VirtIO GPU IDs (0x1AF4 / 0x1050). */
static inline void boardinfo_set_pci_id(struct BoardInfo *bi,
                                        uint16 vendor, uint16 device)
{
    *(uint16 *)((UBYTE *)bi + BOARDINFO_OFF_PCI_VENDOR_ID) = vendor;
    *(uint16 *)((UBYTE *)bi + BOARDINFO_OFF_PCI_DEVICE_ID) = device;
}

/* DO NOT USE on AOS4 -- left in place purely for documentation of the
 * canonical Picasso96 layout.  Disassembly of AOS4 graphics.library.kmod
 * shows bi+1646 is a hash-chain head (insert-at-head idiom around
 * 0x104db5c), bi+1642 is read as a pointer in mouse-positioning paths,
 * and bi+1616 is OR-accumulated bit-by-bit -- none behave as the
 * canonical EssentialFormats / MaxBMWidth / MaxBMHeight fields would.
 * Writing canonical values to these offsets in AOS4 would corrupt
 * graphics.library state. */
static inline void boardinfo_set_essential_formats__UNSAFE_AOS4(struct BoardInfo *bi,
                                                                 uint32 formats)
{
    *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_ESSENTIAL_FORMATS) = formats;
}

static inline void boardinfo_set_max_bm_size__UNSAFE_AOS4(struct BoardInfo *bi,
                                                          uint32 width, uint32 height)
{
    *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_MAX_BM_WIDTH)  = width;
    *(uint32 *)((UBYTE *)bi + BOARDINFO_OFF_MAX_BM_HEIGHT) = height;
}

#ifdef __GNUC__
# ifdef __PPC__
#  pragma pack()
# endif
#endif

#endif /* P96_BOARDINFO_H */
