/*
 * chip_alloc.c -- Picasso96 board memory + bitmap allocator vtable
 * callbacks.  Split from chip_p96.c in v53.112.
 *
 * Provides:
 *   - First-fit + adjacent-free coalescing allocator over gs->board_mem
 *     (board_alloc / board_free, file-scope statics).
 *   - chip_AllocBitMap / chip_FreeBitMap -- vtable slots 58/59.
 *   - chip_AllocCardMem / chip_FreeCardMem -- vtable slots 0/1.
 *   - chip_GetBitMapAttr -- vtable slot 60.
 *   - chip_alloc_fill_vtable() -- wire all of the above into BoardInfo.
 *
 * gs->alloc_mutex (allocated in chip_init.c) protects the file-scope
 * board_allocs[] table from concurrent vtable callers.
 */

#include "chip/chip_state.h"

/* MAX_BOARD_ALLOCS: 64 was too small in v53.111/.112 -- AOS4
 * graphics.library + rtg.library together can hold ~120 concurrent
 * AllocCardMem-style allocations during Workbench startup (BitMapExtras,
 * sprite save buffers, mode info caches, etc.).  When the table fills,
 * subsequent AllocCardMem calls fail with "out of board memory" even
 * though there's plenty of free RAM in board_mem -- the slot table is
 * the bottleneck, not the memory itself.  Bumped to 512 to leave headroom
 * for heavy MUI / Workbench loads.  Per-slot cost is ~12 bytes; 512 slots
 * = ~6 KB BSS.  Negligible against 128 MB board memory. */
#define MAX_BOARD_ALLOCS 512

/* Tracked board memory allocator -- first-fit with adjacent-free
 * coalescing.  ScreenMode prefs cycles through resolutions allocating
 * a new bitmap and freeing the old one; this often results in middle-
 * of-heap holes that a simple top-compaction allocator can't reclaim.
 * First-fit + coalescing reuses any hole large enough.
 *
 * Slot table is kept sorted by offset so coalescing is just a check of
 * the entry above and below the freed slot.  See MAX_BOARD_ALLOCS note
 * above for sizing rationale. */
static struct {
    uint32 offset;   /* offset within board_mem */
    uint32 size;     /* aligned size */
    BOOL   used;     /* TRUE = allocated, FALSE = free hole */
} board_allocs[MAX_BOARD_ALLOCS];
static int num_board_allocs = 0;

/* Recompute the high-water mark used for "out of memory" checks: it's
 * the end of the highest-offset slot, used or not, since freed slots
 * still consume table space until they collapse with their neighbours. */
static void board_recalc_top(struct ChipGPUState *gs)
{
    if (num_board_allocs == 0) {
        gs->board_alloc_offset = 0;
        return;
    }
    uint32 last_idx = (uint32)num_board_allocs - 1;
    gs->board_alloc_offset = board_allocs[last_idx].offset
                           + board_allocs[last_idx].size;
}

/* Coalesce slot `i` with adjacent free neighbours.  Walks down then up
 * since either side may be free.  Caller has already set used=FALSE on
 * the slot being released. */
static void board_coalesce_at(struct ChipGPUState *gs, int i)
{
    /* Merge with successor if also free. */
    if (i + 1 < num_board_allocs && !board_allocs[i + 1].used) {
        board_allocs[i].size += board_allocs[i + 1].size;
        for (int j = i + 1; j < num_board_allocs - 1; j++)
            board_allocs[j] = board_allocs[j + 1];
        num_board_allocs--;
    }
    /* Merge with predecessor if also free. */
    if (i > 0 && !board_allocs[i - 1].used) {
        board_allocs[i - 1].size += board_allocs[i].size;
        for (int j = i; j < num_board_allocs - 1; j++)
            board_allocs[j] = board_allocs[j + 1];
        num_board_allocs--;
    }
    /* Drop trailing free slot -- top-compaction case. */
    while (num_board_allocs > 0
           && !board_allocs[num_board_allocs - 1].used) {
        num_board_allocs--;
    }
    board_recalc_top(gs);
}

/* Free a board_mem pointer -- mark slot free, coalesce neighbours.
 *
 * Holds gs->alloc_mutex around the table mutation so concurrent
 * calls from rtg.library vtable callbacks (chip_FreeBitMap,
 * chip_FreeCardMem) running in different tasks don't corrupt the
 * shared file-scope state.  NULL alloc_mutex means chip init is
 * still in progress -- safe to skip locking, no other task can be
 * touching the table yet. */
static BOOL board_free(struct ChipGPUState *gs, APTR mem)
{
    if (!gs || !gs->board_mem || !mem) return FALSE;

    if (gs->alloc_mutex) gs->IExec->MutexObtain(gs->alloc_mutex);

    uint32 target_offset = (uint32)((UBYTE *)mem - (UBYTE *)gs->board_mem);
    BOOL found = FALSE;
    for (int i = 0; i < num_board_allocs; i++) {
        if (board_allocs[i].offset == target_offset && board_allocs[i].used) {
            DCHIP_V("board_free: slot %d offset=%lu size=%lu", i,
                    board_allocs[i].offset, board_allocs[i].size);
            board_allocs[i].used = FALSE;
            board_coalesce_at(gs, i);
            DCHIP_V("board_free: after coalesce alloc_offset=%lu/%lu slots=%d",
                    gs->board_alloc_offset, gs->board_mem_size, num_board_allocs);
            found = TRUE;
            break;
        }
    }

    if (gs->alloc_mutex) gs->IExec->MutexRelease(gs->alloc_mutex);

    if (!found) DCHIP("board_free: %p not found in alloc table", mem);
    return found;
}

/* Allocate from board_mem -- first-fit through any free hole big enough,
 * or extend at the top.  Returns 16-byte-aligned pointer or NULL.
 *
 * Holds gs->alloc_mutex around the table mutation; see board_free for
 * the rationale. */
static APTR board_alloc(struct ChipGPUState *gs, uint32 size)
{
    /* Round allocations up to 16-byte alignment so that every returned
     * buffer starts on a cache-line-friendly boundary (PPC G4 has 32-byte
     * L1 cache lines; 16-byte covers the AltiVec vector width) and back-
     * to-back allocations never share a 4-byte word.  Matches typical
     * graphics.library bitmap / render target stride expectations. */
    size = (size + 15) & ~15UL;
    if (size == 0) size = 16;

    if (gs->alloc_mutex) gs->IExec->MutexObtain(gs->alloc_mutex);
    APTR result = NULL;

    /* First-fit: scan the slot table for any unused entry of sufficient
     * size and reuse it (split if larger).  This recovers holes left by
     * out-of-order frees -- e.g. ScreenMode prefs cycling through
     * resolutions, where the freed bitmap is rarely the topmost one. */
    for (int i = 0; i < num_board_allocs; i++) {
        if (board_allocs[i].used) continue;
        if (board_allocs[i].size < size) continue;

        if (board_allocs[i].size == size) {
            board_allocs[i].used = TRUE;
        } else if (num_board_allocs < MAX_BOARD_ALLOCS) {
            /* Split: shrink this free slot, insert a new used slot
             * before it covering the front portion. */
            for (int j = num_board_allocs; j > i; j--)
                board_allocs[j] = board_allocs[j - 1];
            num_board_allocs++;
            board_allocs[i].size   = size;
            board_allocs[i].used   = TRUE;
            board_allocs[i + 1].offset += size;
            board_allocs[i + 1].size   -= size;
        } else {
            /* Table full, can't split.  Take the whole hole; the
             * unused tail is wasted but better than failing. */
            board_allocs[i].used = TRUE;
        }
        DCHIP_V("board_alloc: reused slot %d offset=%lu size=%lu (first-fit)",
                i, board_allocs[i].offset, size);
        result = (APTR)((UBYTE *)gs->board_mem + board_allocs[i].offset);
        goto out;
    }

    /* No suitable hole -- extend at the top. */
    if (gs->board_alloc_offset + size > gs->board_mem_size) {
        DCHIP("board_alloc: OUT OF BOARD MEMORY (need %lu, have %lu free at top)",
              size, gs->board_mem_size - gs->board_alloc_offset);
        goto out;
    }
    if (num_board_allocs >= MAX_BOARD_ALLOCS) {
        DCHIP("board_alloc: alloc table full (%d slots)", MAX_BOARD_ALLOCS);
        goto out;
    }

    {
        uint32 offset = gs->board_alloc_offset;
        board_allocs[num_board_allocs].offset = offset;
        board_allocs[num_board_allocs].size   = size;
        board_allocs[num_board_allocs].used   = TRUE;
        num_board_allocs++;

        /* High-water diagnostic -- log once when we cross 75 % of the
         * slot table.  Helps catch "table approaching full" before it
         * actually fills.  Static so the log fires once per session. */
        static BOOL hi_water_logged = FALSE;
        if (!hi_water_logged && num_board_allocs > (MAX_BOARD_ALLOCS * 3 / 4)) {
            DCHIP("board_alloc: slot table 75%% full (%d/%d) -- consider raising MAX_BOARD_ALLOCS",
                  num_board_allocs, MAX_BOARD_ALLOCS);
            hi_water_logged = TRUE;
        }

        gs->board_alloc_offset = offset + size;
        result = (APTR)((UBYTE *)gs->board_mem + offset);
    }

out:
    if (gs->alloc_mutex) gs->IExec->MutexRelease(gs->alloc_mutex);
    return result;
}

/* -----------------------------------------------------------------------
 * BoardInfo vtable slots 58/59/60 (AllocBitMap / FreeBitMap /
 * GetBitMapAttr) are intentionally NOT implemented.
 *
 * An ABI probe (v53.154) confirmed AOS4 rtg.library never invokes these
 * slots during normal Workbench use -- it allocates and manages RTG
 * bitmaps itself.  Per joerg (amigans.net): "nearly all BoardInfo
 * functions can be set to NULL on AmigaOS 4.x and a fallback function
 * in graphics.library is used instead."  The slots are left NULL in
 * chip_alloc_fill_vtable so graphics.library's fallback handles them.
 * The canonical signatures are still declared in boardinfo.h.
 * AllocCardMem / FreeCardMem (slots 0/1) ARE used and stay implemented.
 * ----------------------------------------------------------------------- */

static APTR chip_AllocCardMem(struct BoardInfo *bi, ULONG size, BOOL visible, BOOL display)
{
    (void)bi; (void)visible; (void)display;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || !gs->board_mem) return NULL;

    DCHIP_V("AllocCardMem(size=%lu vis=%ld disp=%ld) offset=%lu/%lu",
            size, (LONG)visible, (LONG)display, gs->board_alloc_offset, gs->board_mem_size);

    APTR result = board_alloc(gs, size);
    if (!result)
        DCHIP("AllocCardMem: OUT OF BOARD MEMORY");
    return result;
}

static BOOL chip_FreeCardMem(struct BoardInfo *bi, APTR mem)
{
    (void)bi;
    DCHIP_V("FreeCardMem: mem=%p", mem);
    return board_free(g_chip_state, mem);
}

/* -----------------------------------------------------------------------
 * chip_alloc_fill_vtable -- wire the allocator callbacks into BoardInfo.
 * Called from chip_fill_boardinfo_vtable in chip_p96.c.
 *
 * AllocCardMem / FreeCardMem (slots 0/1) are implemented.  AllocBitMap /
 * FreeBitMap / GetBitMapAttr (slots 58/59/60) are left NULL so
 * graphics.library's fallback handles them -- see the note above.
 * ----------------------------------------------------------------------- */
void chip_alloc_fill_vtable(struct BoardInfo *bi)
{
    bi->AllocCardMem  = chip_AllocCardMem;
    bi->FreeCardMem   = chip_FreeCardMem;
    bi->AllocBitMap   = NULL;
    bi->FreeBitMap    = NULL;
    bi->GetBitMapAttr = NULL;
}
