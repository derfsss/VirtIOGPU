/*
 * chip_state.h -- VirtIO GPU chip driver shared definitions.
 *
 * Shared header for all chip driver source files.  Contains the
 * ChipGPUState struct, MMIO inline helpers, VIO_* macros, mode table
 * type, globals, and forward declarations.
 */

#ifndef CHIP_STATE_H
#define CHIP_STATE_H

#include <exec/exec.h>
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/semaphores.h>

#include <proto/exec.h>

#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <interfaces/exec.h>
#include <interfaces/timer.h>

#include <graphics/gfx.h>
#include <libraries/Picasso96.h>
#include "p96/boardinfo.h"
#include "virtio/virtio_pci_modern.h"
#include "virtio/virtio_pci.h"
#include "virtio/virtqueue.h"
#include "gpu/virtio_gpu_proto.h"
#include "virtio_config.h"

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
extern struct ExecIFace *g_IExec_early;

/* Serial debug via DebugPrintF.  Gated on -DDEBUG so release builds
 * (Makefile without -DDEBUG) compile to zero-instruction no-ops. */
#ifdef DEBUG
#define DCHIP(fmt, ...) \
    do { if (g_IExec_early) g_IExec_early->DebugPrintF("[virtiogpu.chip] " fmt "\n", ##__VA_ARGS__); } while(0)
#else
#define DCHIP(fmt, ...) do {} while(0)
#endif

/* Verbose debug -- compile with -DCHIP_DEBUG_VERBOSE to enable (implies
 * -DDEBUG).  Used for per-mode, per-alloc, per-palette and other
 * high-frequency messages that are useful during development but noisy
 * in normal operation. */
#if defined(DEBUG) && defined(CHIP_DEBUG_VERBOSE)
#define DCHIP_V(fmt, ...) DCHIP(fmt, ##__VA_ARGS__)
#else
#define DCHIP_V(fmt, ...) do {} while(0)
#endif

/* Hot-path stage tracer -- one store, no function call, no format.
 * HOT_STAGE(gs, "string") records that the flush task (or one of its
 * callees) has reached a labelled point.  String literals only -- they
 * live in rodata and are safe to read from the heartbeat concurrently
 * with writes from the flush task. */
#define HOT_STAGE(gs, s) \
    do { if ((gs)) (gs)->hot_stage = (s); } while(0)

/* Board-level debug (chip_board.c -- uses g_IExec_early like DCHIP).
 * Gated on -DDEBUG the same way. */
#ifdef DEBUG
#define DBOARD(fmt, ...) \
    do { if (g_IExec_early) g_IExec_early->DebugPrintF("[VirtIOGPUBoard] " fmt "\n", ##__VA_ARGS__); } while(0)
#else
#define DBOARD(fmt, ...) do {} while(0)
#endif
#if defined(DEBUG) && defined(CHIP_DEBUG_VERBOSE)
#define DBOARD_V(fmt, ...) DBOARD(fmt, ##__VA_ARGS__)
#else
#define DBOARD_V(fmt, ...) do {} while(0)
#endif

/* -----------------------------------------------------------------------
 * ChipGPUState -- minimal state for embedded VirtIO GPU init.
 *
 * Mirrors the subset of VirtIOGPUBase fields used by the VirtIO functions.
 * Allocated on heap in chip_InitCard_C; stored in g_chip_state for the
 * lifetime of the chip (no shutdown path needed for a Kickstart chip).
 * ----------------------------------------------------------------------- */
struct ChipGPUState {
    struct ExecIFace    *IExec;
    struct PCIDevice    *pciDevice;
    BOOL    pci_locked;     /* TRUE if Lock(PCI_LOCK_EXCLUSIVE) held */

    /* BAR info from GetResourceRange scan (populated by chip_scan_bars) */
    struct {
        uint32  base;       /* Virtual base address */
        uint32  size;       /* Size in bytes */
        uint32  physical;   /* Physical base address */
        uint32  flags;      /* PCI_RANGE_IO / PCI_RANGE_MEMORY / PCI_RANGE_PREFETCH */
        BOOL    valid;      /* TRUE if BAR exists */
    } bars[6];

    /* Modern VirtIO config region -- virtual addresses computed from
     * BAR base (from GetResourceRange) + BAR-relative offset (from PCI caps).
     * Access via stwbrx/lwbrx + mbar inline asm (Pegasos2 / MV64361).
     * On AmigaOne (Articia S), direct MMIO fails -- use PCI_CFG fallback. */
    uint32  common_cfg_base;    /* Virtual address of COMMON_CFG region */
    uint32  notify_cfg_base;    /* Virtual address of NOTIFY_CFG region */
    uint32  notify_off_mult;
    uint32  isr_cfg_base;       /* Virtual address of ISR_CFG region */
    uint32  device_cfg_base;    /* Virtual address of DEVICE_CFG region */
    BOOL    modern_mode;

    /* PCI_CFG fallback (VirtIO spec 4.1.4.7) -- for platforms where direct
     * MMIO doesn't work (AmigaOne / Articia S).  BAR register access goes
     * through PCI config space via the PCI_CFG capability instead. */
    BOOL    use_pci_cfg;        /* TRUE = PCI_CFG path, FALSE = direct MMIO */
    uint8   pci_cfg_cap_off;    /* Config space offset of PCI_CFG capability */
    uint8   common_cfg_bar;     /* BAR number for COMMON_CFG region */
    uint32  common_cfg_off;     /* BAR-relative offset of COMMON_CFG */
    uint8   notify_cfg_bar;     /* BAR number for NOTIFY_CFG region */
    uint32  notify_cfg_off;     /* BAR-relative offset of NOTIFY_CFG */
    uint8   device_cfg_bar;     /* BAR number for DEVICE_CFG region */
    uint32  device_cfg_off;     /* BAR-relative offset of DEVICE_CFG */
    uint8   isr_cfg_bar;
    uint32  isr_cfg_off;

    /* Virtqueues: 0 = control, 1 = cursor */
    struct virtqueue   *vqs[VIRTIO_MAX_QUEUES];
    uint16              num_queues;

    /* DMA command/response buffers (4K each, page-aligned MEMF_SHARED) */
    APTR    cmd_buf;
    uint32  cmd_buf_phys;
    APTR    resp_buf;
    uint32  resp_buf_phys;

    /* Dedicated cursor command buffer -- separate from cmd_buf so cursor
     * moves don't block on control queue I/O (and vice versa). */
    APTR    cursor_cmd_buf;
    uint32  cursor_cmd_phys;

    /* Serialisation -- io_lock protects cmd_buf + control queue,
     * cursor_lock protects cursor_cmd_buf + cursor queue.
     *
     * AOS4 ASOT_MUTEX rather than struct SignalSemaphore + InitSemaphore:
     * we never use ObtainSemaphoreShared (every protected section is a
     * write to cmd_buf/resp_buf or virtqueue rings), and the SDK
     * recommends mutexes when shared mode isn't needed -- "Mutexes will
     * not use Forbid/Permit locking internally so the overall system
     * will be less affected" (Exec_Semaphores wiki).  Allocated in
     * chip_InitCard_C; a NULL value means alloc failed and the chip
     * driver bails out cleanly. */
    APTR    io_lock;        /* ASOT_MUTEX, non-recursive */
    APTR    cursor_lock;    /* ASOT_MUTEX, non-recursive */

    /* Bitmap allocator lock.  Wraps the file-scope statics in
     * chip_p96.c (board_allocs[], num_board_allocs) which are
     * touched by chip_AllocCardMem/FreeCardMem and chip_AllocBitMap/
     * FreeBitMap from any task that opens or closes an RTG screen
     * or bitmap.  rtg.library typically holds bi->BoardLock around
     * vtable callbacks but that's an external invariant we don't
     * own; this mutex makes our state self-consistent regardless. */
    APTR    alloc_mutex;    /* ASOT_MUTEX, non-recursive */

    /* Interrupt handler state.
     * irq_installed = FALSE -> polling fallback in chip_do_io. */
    BOOL              irq_installed;
    uint32            irq_number;
    struct Interrupt  irq_handler;
    struct Task      *init_wait_task;
    uint32            init_signal_mask;

    /* Per-queue waiters: DoIO stores its task pointer + signal bit here
     * before kick.  IRQ handler signals the task when ISR bit 0 is set.
     * Cleared after Wait returns (or on polling timeout). */
    struct Task      *ctrlq_wait_task;
    uint32            ctrlq_wait_mask;
    struct Task      *cursorq_wait_task;
    uint32            cursorq_wait_mask;

    /* Framebuffer for scanout 0 (GPU backing -- always 32bpp) */
    APTR    fb_mem;
    uint32  fb_phys;
    uint32  fb_size;
    uint32  fb_stride;
    uint32  fb_width;
    uint32  fb_height;
    uint32  resource_id;    /* active scanout resource (1=2D, 3+=3D) */
    uint32  fb_resource_id; /* 2D resource ID (always 1, for fallback) */

    /* FB DMA scatter-gather list (for multi-entry ATTACH_BACKING) */
    uint32  fb_dma_count;
    struct DMAEntry *fb_dma_list;

    /* Double-buffering -- second framebuffer for tear-free scanout swap.
     * flush_all writes to back buffer, then SET_SCANOUT + RESOURCE_FLUSH swaps.
     * chip_flush continues targeting front (primary fields) for immediate updates.
     * On swap, primary fb fields are exchanged with *2 variants so primary = front. */
    APTR    fb_mem2;
    uint32  fb_phys2;
    uint32  fb_dma_count2;
    struct DMAEntry *fb_dma_list2;
    uint32  fb_resource_id2;        /* 2D resource for second buffer */
    uint32  virgl_2d_resource2;     /* 3D resource for second buffer */
    BOOL    double_buffer;          /* TRUE if double-buffering active */

    /* Board memory -- P96's MemoryBase (where P96 draws, may be CLUT or 32bpp).
     * Separate from fb_mem because CLUT modes need palette->32bpp conversion. */
    APTR    board_mem;
    uint32  board_mem_size;

    /* Active display format (set by SetGC/SetPanning) */
    uint32  active_format;   /* RGBFB_* */
    uint32  active_bpr;      /* bytes per row for active screen */
    uint32  active_width;    /* visible screen width (from SetGC) */
    uint32  active_height;   /* visible screen height (from SetGC) */
    APTR    panning_mem;     /* visible screen base in board_mem (from SetPanning) */
    uint32  panning_width;   /* buffer width in pixels (from SetPanning) */
    WORD    pan_xoff, pan_yoff;  /* panning viewport offset (from SetPanning) */

    /* Palette for CLUT mode -- 256 entries in X8R8G8B8 byte order (BE uint32).
     * VirtIO GPU resource is X8R8G8B8_UNORM: byte 0=X, 1=R, 2=G, 3=B.
     * On PPC big-endian, uint32 0x00RRGGBB stores bytes 00,RR,GG,BB. */
    uint32  palette[256];

    /* Hardware cursor (VirtIO GPU cursor queue) */
    APTR    cursor_mem;         /* 64x64 ARGB pixel buffer */
    uint32  cursor_phys;        /* physical address for DMA */
    uint32  cursor_resource_id; /* GPU resource ID (2) */
    BOOL    cursor_created;     /* TRUE if cursor resource exists */
    BOOL    cursor_visible;     /* TRUE if cursor is shown */
    WORD    cursor_x, cursor_y;
    UWORD   cursor_hot_x, cursor_hot_y; /* last sent hotspot offsets */
    UWORD   cursor_w, cursor_h;
    UBYTE   cursor_colors[3][3]; /* 3 sprite colors, RGB each */
    uint32  cursor_oob_last_log; /* SetSpritePosition counter at last OOB warning */
    BOOL    cursor_needs_refresh;           /* SetGC sets, SetSpritePosition clears after MOVE_CURSOR */
    WORD    cursor_last_sent_x, cursor_last_sent_y; /* last pos pushed to GPU via UPDATE/MOVE_CURSOR */
    /* Per-scanout last-rect cache so chip_SetScanout only logs on real changes */
    uint32  scanout_log_x[4];
    uint32  scanout_log_y[4];
    uint32  scanout_log_w[4];
    uint32  scanout_log_h[4];

    /* Simple bump allocator for board memory (P96 AllocBitMap) */
    uint32  board_alloc_offset;  /* next free offset in board_mem */

    /* DOS library -- opened in InitCard_C for Process creation + LoadMonDrvs */
    struct Library     *DOSBase;
    struct DOSIFace    *IDOS;

    /* Graphics library -- opened in chip_comp_install_hook for compositing.
     * Kept open for bitmap format queries (GetBitMapAttr). */
    struct Library     *GfxBase;
    struct GraphicsIFace *IGraphics;

    /* Signal-driven flush task.
     * Created as exec Task (AddTask) at SetDisplay(1) for early boot,
     * or as dos Process (CreateNewProc) at SetSwitch(1) as fallback.
     *
     * Other tasks notify the flush task of drawing activity by writing
     * IExec->Signal(flush_task, flush_sig_mask).  The flush task wakes
     * immediately, flushes once, then loops -- no cap on flush rate.
     * When no signal arrives within refresh_us microseconds, a
     * timer.device backstop fires so GRANTDIRECTACCESS writes still
     * get picked up at the screenmode refresh cadence. */
    struct Process *flush_proc;
    volatile BOOL   flush_task_quit;
    BOOL            flush_running;      /* TRUE if any flush mechanism active */
    BOOL            clut_debug_done;    /* one-shot CLUT conversion debug */
    struct Task    *flush_task;         /* populated in flush task entry */
    int8            flush_sig_bit;      /* AllocSignal(-1) result, -1 if none */
    uint32          flush_sig_mask;     /* 1UL << flush_sig_bit */

    /* Idle backstop interval in microseconds.  Recomputed from the active
     * ModeInfo in chip_SetGC: refresh_us = 1e6 / refresh_hz.  Clamped to
     * 30..240 Hz; defaults to 60 Hz (16667 us) before any mode is set. */
    uint32          refresh_us;

    /* Synthetic vblank phase, toggled by the flush task on each refresh
     * tick.  Read by chip_GetVSyncState so apps that gate animation on
     * vsync transitions see a real changing signal rather than a stuck
     * FALSE.  volatile because it's written from the flush task and
     * read from any task without a lock. */
    volatile UBYTE  vsync_phase;

    /* Latched state of chip_SetInterrupt (BIF_VBLANKINTERRUPT enable).
     * Currently only stored -- we don't actually generate VBlank IRQs
     * because rtg.library's WaitTOF queue node format is unknown.
     * Keeping the field lets a future implementation gate the
     * eventual interrupt path. */
    volatile UBYTE  vbi_enabled;

    /* Single-slot wait-for-vsync rendezvous, used by chip_WaitVerticalSync
     * (vtable slot 18).  The caller publishes its task pointer + signal
     * mask under vsync_mutex; the flush task's refresh-tick wake-up
     * grabs the same mutex, takes a snapshot, clears the slot, and
     * Signal()s the task outside the critical section.  Single waiter
     * at a time is sufficient -- typical callers serialise
     * WaitVerticalSync.
     *
     * vsync_mutex is allocated as a non-recursive ASOT_MUTEX in
     * chip_InitCard_C.  A mutex (rather than Forbid/Permit) is correct
     * here because we have real cross-task contention: any task can
     * call WaitVerticalSync, and the flush task races with them at
     * refresh rate.  See https://wiki.amigaos.net/wiki/Exec_Mutexes
     * -- "less overhead on the system as a whole" than Forbid/Permit
     * because only contending tasks are blocked, not everything. */
    APTR                  vsync_mutex;
    volatile struct Task *wait_vsync_task;
    volatile uint32       wait_vsync_mask;

    /* Frame-time fields published by the flush task for chip_GetCurrentY
     * (vtable slot 54).  ITimer is acquired in the flush task entry
     * (and dropped on exit); eclock_freq is the ITimer->ReadEClock
     * tick rate.  last_vsync_eclock_lo is updated on every refresh
     * wake so GetCurrentY can compute "fraction of refresh elapsed". */
    struct TimerIFace *ITimer;
    uint32           eclock_freq;
    volatile uint32  last_vsync_eclock_lo;

    /* Counter for hotplug-added modes: incremented each time
     * chip_handle_display_change registers a new host resolution.
     * Used to compose unique DisplayIDs for the dynamic entries. */
    uint32           hotplug_mode_count;

    /* Total count of heartbeat lines emitted by the flush task.  Used
     * to step the heartbeat interval down from once per 10 seconds to
     * once per minute after the first hour, so long-running session
     * logs aren't dominated by routine pulses.  Reset only on driver
     * reload. */
    uint32           hb_count_total;

    /* Diagnostic counters bumped from vtable hot paths and read by the
     * heartbeat.  Lets us see what kind of drawing was happening just
     * before a hang, even when the kernel cannot produce a normal
     * exception dump.  volatile because they are written from any task
     * and read from the flush task without a lock; exact-once accuracy
     * is not required. */
    volatile uint32 stat_signal_calls;   /* chip_flush_signal_activity calls */
    volatile uint32 stat_fillrect;       /* chip_FillRect calls */
    volatile uint32 stat_blitrect;       /* chip_BlitRect calls */
    volatile uint32 stat_invertrect;     /* chip_InvertRect calls */
    volatile uint32 stat_blitrect_nmc;   /* chip_BlitRectNoMaskComplete calls */
    volatile uint32 stat_blittemplate;   /* chip_BlitTemplate calls */
    volatile uint32 stat_blitpattern;    /* chip_BlitPattern calls */
    /* Snapshots of the above counters at last SetGC (for per-mode op deltas) */
    uint32 prev_fillrect, prev_blitrect, prev_blitrect_nmc,
           prev_blittemplate, prev_blitpattern;
    /* CLUT-only FillRect rect-size histogram (area in pixels) */
    volatile uint32 stat_fr_clut_le4, stat_fr_clut_le16, stat_fr_clut_le256,
                    stat_fr_clut_le4k, stat_fr_clut_big;
    uint32          prev_fr_clut_le4, prev_fr_clut_le16, prev_fr_clut_le256,
                    prev_fr_clut_le4k, prev_fr_clut_big;
    volatile uint32 stat_drawline;       /* chip_DrawLine calls */
    volatile uint32 stat_aborts;         /* AbortIO calls (for race diagnostics) */
    /* Cursor / mode-switch path counters -- often invisible in vtable
     * counters above because GRANTDIRECTACCESS bypasses drawing ops, but
     * Intuition still calls these on mouse moves and screen activity. */
    volatile uint32 stat_setspritepos;   /* chip_SetSpritePosition calls */
    volatile uint32 stat_setspriteimg;   /* chip_SetSpriteImage calls */
    volatile uint32 stat_setspritecol;   /* chip_SetSpriteColor calls */
    volatile uint32 stat_setsprite;      /* chip_SetSprite (activate/deactivate) */
    volatile uint32 stat_setgc;          /* chip_SetGC calls (incl. no-op skip) */
    volatile uint32 stat_setpanning;     /* chip_SetPanning calls */
    volatile uint32 stat_setdisplay;     /* chip_SetDisplay calls */
    volatile uint32 stat_setswitch;      /* chip_SetSwitch calls */
    volatile uint32 stat_setcolor;       /* chip_SetColorArray calls */

    /* Hot-path trace state -- updated by the flush task + its callees at
     * every major step.  On next heartbeat we dump these, giving us a
     * snapshot of where the task was spending its time.  If the task
     * crashed we won't see a heartbeat, but DCHIP writes serial
     * immediately, so the LAST DCHIP line in the log (throttled below)
     * tells us the last-known flush-task stage before the fault.
     *
     * hot_stage: string literal pointer updated on each step.  String
     *   literals live in rodata -- safe to read concurrently with the
     *   heartbeat's printf.
     * hot_iter: flush-task loop iteration counter.
     * hot_wait_entries / hot_wait_exits: chip_wait_ctrlq enter/exit
     *   count.  If enters > exits we are stuck inside chip_wait_ctrlq.
     * hot_flush_entries / hot_flush_exits: chip_flush_all enter/exit
     *   count.  Same stuck-detection as above.
     * hot_timers_armed / hot_timers_fired: timer.device SendIO vs
     *   reply-received count.  If armed > fired+1 we have leaked timer
     *   requests (would mean our loop is broken). */
    volatile const char *hot_stage;
    volatile uint32 hot_iter;
    volatile uint32 hot_wait_entries;
    volatile uint32 hot_wait_exits;
    volatile uint32 hot_flush_entries;
    volatile uint32 hot_flush_exits;
    volatile uint32 hot_timers_armed;
    volatile uint32 hot_timers_fired;

    /* Bitmap-tracking diagnostics: did the most recent chip vtable
     * drawing op address the SAME bitmap that the flush task is
     * sampling (gs->panning_mem)?  If not, the screen will appear
     * frozen because we keep transferring stale memory.
     *   last_fill_dst -- ri->Memory from the most recent chip_FillRect
     *   last_blit_src / last_blit_dst -- src/dst Memory from the
     *     most recent chip_BlitRectNoMaskComplete
     *   last_panning_mem -- value of gs->panning_mem at last SetPanning
     * Heartbeat dumps these so we can compare. */
    volatile APTR   last_fill_dst;
    volatile APTR   last_blit_src;
    volatile APTR   last_blit_dst;
    volatile APTR   last_panning_mem;

    /* DPMS state -- flush task skips when display is off */
    uint32          dpms_level;         /* 0=ON, 1=STANDBY, 2=SUSPEND, 3=OFF */

    /* RTG output enable -- canonical SetSwitch contract.  TRUE means
     * "RTG screen is the active video output"; FALSE means "the
     * display chain is showing native Amiga or another board" and we
     * shouldn't drive the host scanout.  Toggled by chip_SetSwitch.
     * Initialised TRUE in chip_InitCard_C since the first thing that
     * happens after init is normally a screen open / SetDisplay(TRUE). */
    BOOL            rtg_output_enabled;
    BOOL            displayinfo_dumped;  /* one-shot diag flag, see chip_SetSwitch */
    BOOL            null_vtable_checked; /* one-shot ENV: check, see chip_apply_null_vtable_overrides */

    /* Back-reference to BoardInfo for diagnostic dumps */
    struct BoardInfo *bi;

    /* EDID data (from VIRTIO_GPU_CMD_GET_EDID) */
    BOOL    has_edid;           /* TRUE if GET_EDID succeeded */
    uint32  edid_size;          /* actual EDID blob size (typically 128 or 256) */
    uint8   edid[1024];         /* raw EDID blob */
    uint32  edid_native_w;      /* preferred timing width (0 if no EDID) */
    uint32  edid_native_h;      /* preferred timing height */

    /* All distinct (W, H) timings extracted from EDID DTDs (base block:
     * 4 DTDs at bytes 54..125; CEA-861 extension blocks add up to 6
     * more per block at bytes 4..125 of each).  edid_dtd_count is the
     * number of valid entries; edid_native_w/h is always entry 0 when
     * the count is non-zero. */
    uint32  edid_dtd_count;
    uint16  edid_dtd_w[16];
    uint16  edid_dtd_h[16];

    /* 3D / Virgl state (Phase 6) */
    BOOL    has_virgl;          /* TRUE if VIRTIO_GPU_F_VIRGL negotiated */
    uint32  num_capsets;        /* from device config num_capsets field */
    uint32  virgl_capset_id;    /* best capset ID (VIRGL or VIRGL2) */
    uint32  virgl_capset_ver;   /* negotiated capset version */
    uint32  virgl_capset_size;  /* actual capset blob size */
    uint8   virgl_caps[VIRTIO_GPU_MAX_CAPSET_SIZE]; /* raw capset blob */
    BOOL    virgl_caps_valid;   /* TRUE if capset query succeeded */

    /* 3D command buffer -- 64K, separate from 4K cmd_buf.
     * Used for SUBMIT_3D virgl command streams. */
    APTR    cmd3d_buf;
    uint32  cmd3d_phys;
    APTR    cmd3d_lock;     /* ASOT_MUTEX, non-recursive (see io_lock note) */

    /* Resource/context ID allocation (1=fb, 2=cursor, 3+=3D) */
    uint32  next_resource_id;
    uint32  next_ctx_id;        /* context IDs start at 1 */

    /* Fence tracking for async 3D operations */
    uint64  next_fence_id;
    uint64  last_completed_fence;

    /* Blob resource support (VIRTIO_GPU_F_RESOURCE_BLOB) */
    BOOL    has_blob;           /* TRUE if RESOURCE_BLOB negotiated */
    /* When TRUE, fb_resource_id is a BLOB_MEM_GUEST resource: the host
     * imports our DMA pages directly and reads them on RESOURCE_FLUSH,
     * so chip_flush / chip_flush_all can skip TRANSFER_TO_HOST_2D
     * entirely.  Set in chip_init when the blob path succeeds; cleared
     * when we fall back to the classic CREATE_2D + ATTACH_BACKING. */
    BOOL    fb_is_blob;

    /* Virgl 2D acceleration state (Phase 5) */
    BOOL    virgl_2d_ready;     /* TRUE if 2D context + pipeline initialized */
    BOOL    virgl_ctx_error;    /* TRUE if virgl context is poisoned (needs recovery) */
    uint32  virgl_2d_ctx;       /* 3D context ID for 2D accel */
    uint32  virgl_2d_blend;     /* opaque blend object handle */
    uint32  virgl_2d_blend_alpha; /* alpha blend object handle */
    uint32  virgl_2d_dsa;       /* depth/stencil object handle */
    uint32  virgl_2d_rast;      /* rasterizer object handle (scissor enabled) */
    uint32  virgl_2d_vs;        /* passthrough vertex shader handle */
    uint32  virgl_2d_fs;        /* color fragment shader handle */
    uint32  virgl_2d_fs_tex;    /* texture-sampling fragment shader handle */
    uint32  virgl_2d_resource;  /* 3D resource ID used as scanout */
    uint32  virgl_2d_res_w;     /* width of virgl_2d_resource (and _resource2) */
    uint32  virgl_2d_res_h;     /* height of virgl_2d_resource (and _resource2) */
    uint32  virgl_2d_surface;   /* Virgl surface handle for render target */
    uint32  virgl_2d_sampler;   /* sampler state handle (nearest filter) */
    uint32  virgl_2d_sampler_linear; /* sampler state handle (linear filter) */
    BOOL    virgl_shaders_ok;   /* TRUE if shaders created successfully */
    BOOL    virgl_samplers_ok;  /* TRUE if sampler states created successfully */

    /* Vertex pipeline (for textured quad drawing / compositing) */
    uint32  virgl_2d_ve;        /* vertex elements handle */
    uint32  virgl_2d_vbuf_res;  /* vertex buffer resource ID (PIPE_BUFFER) */
    uint32  virgl_test_quad;    /* 0=off, 1=colored quad, 2=textured quad (debug) */

    /* Compositing -- temporary texture for source bitmap upload.
     * A single temp resource + sampler view is allocated per compositing
     * call and freed afterwards.  Handles are recycled from the pool. */
    uint32  comp_tex_res;       /* temp 3D resource ID (0 = none allocated) */
    uint32  comp_tex_view;      /* sampler view handle for temp texture */
    uint32  comp_tex_w;         /* width of current temp texture */
    uint32  comp_tex_h;         /* height of current temp texture */
    uint32  comp_next_handle;   /* next available Virgl object handle for compositing */

    /* Compositing blend state handles -- one per Porter-Duff operator.
     * Index = enum enPDOperator value (0..14).  0 = unsupported. */
    uint32  comp_blends[15];
};

extern struct ChipGPUState *g_chip_state;

/* Allocate the next free virgl-side resource ID, skipping reserved IDs
 * used by the 2D framebuffer (`fb_resource_id`, `fb_resource_id2`) and
 * the cursor (`cursor_resource_id`).  These three IDs are fixed at
 * init and never freed, so they must not be reused for transient 3D
 * resources -- otherwise mode-change recovery (which cycles through
 * IDs as it destroys and recreates 3D scanout resources) eventually
 * collides with them and the GPU returns
 * VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID.
 *
 * Defined inline here so every caller (chip_virgl_2d.c, chip_composite.c)
 * gets the same skip logic without an extra translation unit. */
static inline uint32 chip_alloc_resource_id(struct ChipGPUState *gs)
{
    uint32 id;
    do {
        id = gs->next_resource_id++;
    } while (id == gs->fb_resource_id      ||
             id == gs->fb_resource_id2     ||
             id == gs->cursor_resource_id);
    return id;
}

/* -----------------------------------------------------------------------
 * Lightweight wall-clock profiling primitive.
 *
 * Used to instrument hot paths without the overhead/noise of a full
 * heartbeat block.  Each call site declares a static `struct ChipProf`,
 * brackets the work with chip_prof_begin / chip_prof_end (the latter
 * accumulates ticks + bytes and emits an always-on log when interesting).
 *
 * Logging policy:
 *  - first sample is always logged (one-shot "first call took Xus, Y MB/s").
 *  - subsequent samples log every `summary_interval` calls with rolling
 *    totals (calls=N avg=Xus max=Yus rate=Z MB/s).
 *  - a sample whose elapsed exceeds 4x the rolling average also logs as
 *    "outlier" once, so a stutter spike is visible.
 *
 * Time source: gs->ITimer / gs->eclock_freq, published by the flush task
 * once it has opened timer.device.  Before that point chip_prof_begin
 * returns 0 and chip_prof_end is a no-op -- this is intentional, profiling
 * is meaningless until the time-source is alive.
 *
 * Wrap-safety: the EClock low word is a 32-bit free-running counter at
 * `eclock_freq` ticks/sec (typically 1 MHz on Pegasos2, wraps every
 * ~71 minutes).  All deltas are computed via unsigned subtraction so a
 * single wrap during a measurement is benign.  Single samples spanning
 * more than one wrap are not realistic for our hot paths.
 * ----------------------------------------------------------------------- */
struct ChipProf {
    const char     *name;             /* human-readable label */
    uint32          summary_interval; /* calls between rolling-summary logs */
    /* state -- written in chip_prof_end; volatile because the same
     * struct may see concurrent vtable callbacks from different tasks */
    volatile uint32 calls;
    volatile uint32 max_us;
    volatile uint32 sum_us_lo;        /* 64-bit accumulator (lo) */
    volatile uint32 sum_us_hi;        /* 64-bit accumulator (hi) */
    volatile uint32 sum_bytes_lo;     /* 64-bit accumulator (lo) */
    volatile uint32 sum_bytes_hi;     /* 64-bit accumulator (hi) */
    volatile uint32 next_summary_at;  /* next `calls` value at which to summary-log */
    volatile uint32 logged_first;     /* 0 until first-call one-shot logged */
};

/* CHIP_PROF_DEFINE -- static-init a ChipProf bucket for use by a single
 * call site.  `interval` is calls between summary-logs (e.g. 100).  Pass
 * 0 to disable summary logs (only the first-call one-shot will fire). */
#define CHIP_PROF_DEFINE(varname, label, interval)                            \
    static struct ChipProf varname = {                                        \
        .name             = (label),                                          \
        .summary_interval = (interval),                                       \
        .calls            = 0,                                                \
        .max_us           = 0,                                                \
        .sum_us_lo        = 0,                                                \
        .sum_us_hi        = 0,                                                \
        .sum_bytes_lo     = 0,                                                \
        .sum_bytes_hi     = 0,                                                \
        .next_summary_at  = (interval),                                       \
        .logged_first     = 0,                                                \
    }

/* chip_prof_begin -- snapshot the EClock low word.  Returns 0 if the
 * time-source isn't ready yet, in which case chip_prof_end skips the
 * accumulation (no time can be measured). */
static inline uint32 chip_prof_begin(struct ChipGPUState *gs)
{
    if (!gs || !gs->ITimer || !gs->eclock_freq) return 0;
    struct EClockVal ev;
    gs->ITimer->ReadEClock(&ev);
    /* Reserve 0 as "no measurement" -- offset by 1 so a real reading of
     * exactly 0 doesn't collide with the sentinel.  chip_prof_end undoes
     * the offset before computing delta. */
    return ev.ev_lo + 1u;
}

/* chip_prof_end -- declared in chip_state.h, defined in chip_flush.c.
 * Records (now - start) into the bucket and emits log lines per the
 * documented policy.  `bytes` is an optional throughput input (0 = unused). */
void chip_prof_end(struct ChipGPUState *gs, struct ChipProf *p,
                    uint32 start_token, uint32 bytes);

/* -----------------------------------------------------------------------
 * Mode table
 * ----------------------------------------------------------------------- */
struct ChipModeEntry {
    UWORD w, h;
    ULONG clock;       /* pixel clock in Hz */
    UWORD hor_total, hor_sync_start, hor_sync_size;
    UWORD ver_total, ver_sync_start, ver_sync_size;
    UBYTE flags;       /* GMF_HPOLARITY | GMF_VPOLARITY */
};

extern const struct ChipModeEntry chip_modes[];
extern const ULONG chip_num_modes;

/* -----------------------------------------------------------------------
 * MMIO access helpers -- inline PPC asm with byte-reversed load/store
 * and mbar (full memory barrier).
 *
 * eieio (lightweight I/O barrier) is theoretically sufficient for CI+G
 * memory, but QEMU's TCG does not implement it with enough strength for
 * MMIO -- stores to PCI registers are not visible to the virtual device
 * before the next instruction.  mbar (= sync) forces full ordering and
 * works on both QEMU and real hardware.
 *
 * 8-bit: no byte-swap needed, just lbz/stb + mbar.
 * 16-bit: lhbrx/sthbrx (hardware byte-reversed) + mbar.
 * 32-bit: lwbrx/stwbrx (hardware byte-reversed) + mbar.
 * ----------------------------------------------------------------------- */
static inline uint8 mmio_read8(uint32 addr)
{
    volatile uint8 *a = (volatile uint8 *)addr;
    uint8 r;
    __asm__ volatile("lbz %0,0(%1); mbar" : "=r"(r) : "r"(a) : "memory");
    return r;
}
static inline void mmio_write8(uint32 addr, uint8 v)
{
    volatile uint8 *a = (volatile uint8 *)addr;
    __asm__ volatile("stb %1,0(%0); mbar" : : "r"(a), "r"(v) : "memory");
}
static inline uint16 mmio_read16(uint32 addr)
{
    volatile uint16 *a = (volatile uint16 *)addr;
    uint16 r;
    __asm__ volatile("lhbrx %0,0,%1; mbar" : "=r"(r) : "r"(a) : "memory");
    return r;
}
static inline void mmio_write16(uint32 addr, uint16 v)
{
    volatile uint16 *a = (volatile uint16 *)addr;
    __asm__ volatile("sthbrx %1,0,%0; mbar" : : "r"(a), "r"(v) : "memory");
}
static inline uint32 mmio_read32(uint32 addr)
{
    volatile uint32 *a = (volatile uint32 *)addr;
    uint32 r;
    __asm__ volatile("lwbrx %0,0,%1; mbar" : "=r"(r) : "r"(a) : "memory");
    return r;
}
static inline void mmio_write32(uint32 addr, uint32 v)
{
    volatile uint32 *a = (volatile uint32 *)addr;
    __asm__ volatile("stwbrx %1,0,%0; mbar" : : "r"(a), "r"(v) : "memory");
}

/* -----------------------------------------------------------------------
 * VIO_* register access -- dispatch between direct MMIO (Pegasos2) and
 * PCI_CFG fallback (AmigaOne).  Auto-detected at init time.
 * ----------------------------------------------------------------------- */
static inline uint8 vio_r8(struct ChipGPUState *gs, uint32 reg) {
    if (gs->use_pci_cfg)
        return cfg_r8(gs->pciDevice, gs->pci_cfg_cap_off, gs->common_cfg_bar, gs->common_cfg_off + reg);
    return mmio_read8(gs->common_cfg_base + reg);
}
static inline void vio_w8(struct ChipGPUState *gs, uint32 reg, uint8 v) {
    if (gs->use_pci_cfg)
        cfg_w8(gs->pciDevice, gs->pci_cfg_cap_off, gs->common_cfg_bar, gs->common_cfg_off + reg, v);
    else
        mmio_write8(gs->common_cfg_base + reg, v);
}
static inline uint16 vio_r16(struct ChipGPUState *gs, uint32 reg) {
    if (gs->use_pci_cfg)
        return cfg_r16(gs->pciDevice, gs->pci_cfg_cap_off, gs->common_cfg_bar, gs->common_cfg_off + reg);
    return mmio_read16(gs->common_cfg_base + reg);
}
static inline void vio_w16(struct ChipGPUState *gs, uint32 reg, uint16 v) {
    if (gs->use_pci_cfg)
        cfg_w16(gs->pciDevice, gs->pci_cfg_cap_off, gs->common_cfg_bar, gs->common_cfg_off + reg, v);
    else
        mmio_write16(gs->common_cfg_base + reg, v);
}
static inline uint32 vio_r32(struct ChipGPUState *gs, uint32 reg) {
    if (gs->use_pci_cfg)
        return cfg_r32(gs->pciDevice, gs->pci_cfg_cap_off, gs->common_cfg_bar, gs->common_cfg_off + reg);
    return mmio_read32(gs->common_cfg_base + reg);
}
static inline void vio_w32(struct ChipGPUState *gs, uint32 reg, uint32 v) {
    if (gs->use_pci_cfg)
        cfg_w32(gs->pciDevice, gs->pci_cfg_cap_off, gs->common_cfg_bar, gs->common_cfg_off + reg, v);
    else
        mmio_write32(gs->common_cfg_base + reg, v);
}

#define VIO_R8(gs, reg)     vio_r8 ((gs), (reg))
#define VIO_W8(gs, reg, v)  vio_w8 ((gs), (reg), (v))
#define VIO_R16(gs, reg)    vio_r16((gs), (reg))
#define VIO_W16(gs, reg, v) vio_w16((gs), (reg), (v))
#define VIO_R32(gs, reg)    vio_r32((gs), (reg))
#define VIO_W32(gs, reg, v) vio_w32((gs), (reg), (v))

/* Device config access -- also dispatches MMIO vs PCI_CFG */
static inline uint32 devcfg_r32(struct ChipGPUState *gs, uint32 off) {
    if (gs->use_pci_cfg)
        return cfg_r32(gs->pciDevice, gs->pci_cfg_cap_off, gs->device_cfg_bar, gs->device_cfg_off + off);
    return mmio_read32(gs->device_cfg_base + off);
}
static inline void devcfg_w32(struct ChipGPUState *gs, uint32 off, uint32 v) {
    if (gs->use_pci_cfg)
        cfg_w32(gs->pciDevice, gs->pci_cfg_cap_off, gs->device_cfg_bar, gs->device_cfg_off + off, v);
    else
        mmio_write32(gs->device_cfg_base + off, v);
}
#define DEVCFG_R32(gs, off)    devcfg_r32((gs), (off))
#define DEVCFG_W32(gs, off, v) devcfg_w32((gs), (off), (v))

/* Notify (kick) a virtqueue -- dispatches MMIO vs PCI_CFG.
 * vq->notify_addr = MMIO address (Pegasos2).
 * vq->notify_bar_off = BAR-relative offset for PCI_CFG (AmigaOne). */
static inline void chip_notify_queue(struct ChipGPUState *gs,
                                      struct virtqueue *vq, uint16 queue_idx)
{
    if (gs->use_pci_cfg) {
        cfg_w16(gs->pciDevice, gs->pci_cfg_cap_off,
                gs->notify_cfg_bar, vq->notify_bar_off, queue_idx);
    } else if (vq->notify_addr) {
        mmio_write16(vq->notify_addr, queue_idx);
    }
}

/* -----------------------------------------------------------------------
 * chip_zero -- byte-wise zero of a memory region.
 *
 * Why not memset?  This driver is compiled -nostartfiles with
 * -fno-tree-loop-distribute-patterns to prevent GCC from synthesising
 * memset() calls from ordinary zeroing loops (memset is not resolvable
 * without the C startup code).  The `volatile` qualifier on the store
 * also inhibits loop-distribution optimisations that would otherwise
 * recreate the memset call.
 *
 * When to prefer `AVT_ClearWithValue, 0` over this function:
 *   - During allocation via IExec->AllocVecTags -- pass the tag instead
 *     of allocating then calling chip_zero.  AllocVecTags is allowed to
 *     use a fast kernel zeroing path (DMA, cache-zero, etc.) that this
 *     byte loop can't match.  chip_zero is only for clearing already-
 *     allocated buffers between uses (e.g. a reusable command buffer
 *     before constructing a new VirtIO command).
 *
 * Safe to call on MEMF_SHARED and CACHEINHIBIT memory -- the byte loop
 * with volatile stores cannot be reordered or gathered.
 * ----------------------------------------------------------------------- */
static inline void chip_zero(void *ptr, uint32 n)
{
    volatile uint8 *p = (volatile uint8 *)ptr;
    for (uint32 i = 0; i < n; i++)
        p[i] = 0;
}

static inline UWORD chip_format_bpp(RGBFTYPE format)
{
    switch (format) {
    case RGBFB_CLUT:
        return 1;
    case RGBFB_R8G8B8:
    case RGBFB_B8G8R8:
        return 3;
    case RGBFB_R5G6B5PC: case RGBFB_R5G5B5PC:
    case RGBFB_R5G6B5:   case RGBFB_R5G5B5:
    case RGBFB_B5G6R5PC: case RGBFB_B5G5R5PC:
        return 2;
    default: /* A8R8G8B8, A8B8G8R8, R8G8B8A8, B8G8R8A8 */
        return 4;
    }
}

/* -----------------------------------------------------------------------
 * Forward declarations -- cross-file functions
 * ----------------------------------------------------------------------- */

/* chip_init.c */
BOOL chip_InitCard_C(struct BoardInfo *bi, char **toolTypes, APTR cardDesc);
void chip_immu_set_coherent(struct ExecIFace *IExec, APTR addr, uint32 size);
void chip_immu_set_writethrough(struct ExecIFace *IExec, APTR addr, uint32 size);
BOOL chip_irq_install(struct ChipGPUState *gs);
void chip_irq_remove(struct ChipGPUState *gs);
BOOL chip_resize_fb_to_mode(struct ChipGPUState *gs, uint32 new_w, uint32 new_h);

/* chip_modes.c */
void chip_dump_displayinfobase(struct ChipGPUState *gs);

/* chip_gpu_cmds.c */
/* Shared wait-for-response helper.  Uses IRQ+Wait if irq_installed,
 * otherwise polling+yield.  Returns bytes_written, 0 on timeout.
 * Caller must AddBuf+notify before calling, and queue must be ctrlq. */
uint32 chip_wait_ctrlq(struct ChipGPUState *gs, struct virtqueue *vq, void *cookie);

/* Synchronous control-queue transaction.  Caller MUST hold gs->io_lock.
 * chip_do_io returns bytes written into resp_buf; chip_gpu_send extracts
 * and returns the response type (0 on short response).  Both are shared
 * between chip_gpu_cmds.c (2D path) and chip_gpu_3d.c (3D path) -- the
 * old per-file duplicates have been consolidated here. */
uint32 chip_do_io(struct ChipGPUState *gs, uint32 cmd_size, uint32 resp_size);
uint32 chip_gpu_send(struct ChipGPUState *gs, uint32 cmd_size, uint32 resp_size);
BOOL chip_GetDisplayInfo(struct ChipGPUState *gs, uint32 *width_out, uint32 *height_out);
BOOL chip_ResourceCreate2D(struct ChipGPUState *gs, uint32 resource_id,
                            uint32 format, uint32 width, uint32 height);
BOOL chip_ResourceAttachBacking(struct ChipGPUState *gs, uint32 resource_id,
                                 struct DMAEntry *dma_list, uint32 dma_count);
BOOL chip_SetScanout(struct ChipGPUState *gs, uint32 scanout_id, uint32 resource_id,
                      uint32 x, uint32 y, uint32 width, uint32 height);
BOOL chip_TransferToHost2D(struct ChipGPUState *gs, uint32 resource_id,
                            uint32 x, uint32 y, uint32 width, uint32 height, uint64 offset);
BOOL chip_ResourceFlush(struct ChipGPUState *gs, uint32 resource_id,
                         uint32 x, uint32 y, uint32 width, uint32 height);
BOOL chip_ResourceUnref(struct ChipGPUState *gs, uint32 resource_id);
BOOL chip_GetEDID(struct ChipGPUState *gs, uint32 scanout);
void chip_handle_display_change(struct ChipGPUState *gs);
BOOL chip_CursorCreate(struct ChipGPUState *gs);
void chip_CursorUpdate(struct ChipGPUState *gs, uint32 hot_x, uint32 hot_y);
void chip_CursorMove(struct ChipGPUState *gs, uint32 x, uint32 y);
void chip_CursorHide(struct ChipGPUState *gs);

/* chip_gpu_3d.c */
BOOL chip_GetCapsetInfo(struct ChipGPUState *gs, uint32 index,
                         uint32 *id_out, uint32 *max_ver_out, uint32 *max_size_out);
BOOL chip_GetCapset(struct ChipGPUState *gs, uint32 capset_id, uint32 capset_version,
                     uint8 *buf_out, uint32 buf_size, uint32 *actual_size_out);
BOOL chip_CTXCreate(struct ChipGPUState *gs, uint32 ctx_id,
                     const char *debug_name, uint32 context_init);
void chip_CTXDestroy(struct ChipGPUState *gs, uint32 ctx_id);
BOOL chip_CTXAttachResource(struct ChipGPUState *gs, uint32 ctx_id, uint32 resource_id);
BOOL chip_CTXDetachResource(struct ChipGPUState *gs, uint32 ctx_id, uint32 resource_id);
BOOL chip_ResourceCreate3D(struct ChipGPUState *gs, uint32 resource_id,
                             uint32 target, uint32 format, uint32 bind,
                             uint32 width, uint32 height, uint32 depth,
                             uint32 array_size, uint32 last_level, uint32 nr_samples,
                             uint32 flags);
BOOL chip_TransferToHost3D(struct ChipGPUState *gs, uint32 ctx_id, uint32 resource_id,
                             uint32 level, uint32 stride, uint32 layer_stride,
                             uint64 offset, struct virtio_gpu_box *box);
BOOL chip_TransferFromHost3D(struct ChipGPUState *gs, uint32 ctx_id, uint32 resource_id,
                               uint32 level, uint32 stride, uint32 layer_stride,
                               uint64 offset, struct virtio_gpu_box *box);
BOOL chip_Submit3D(struct ChipGPUState *gs, uint32 ctx_id,
                    void *cmd_data, uint32 cmd_size);
void chip_QueryCapsets(struct ChipGPUState *gs);

/* chip_gpu_3d.c (blob resources) */
BOOL chip_ResourceCreateBlob(struct ChipGPUState *gs, uint32 resource_id,
                              uint32 blob_mem, uint32 blob_flags,
                              uint64 blob_id, uint64 size,
                              struct DMAEntry *dma_list, uint32 dma_count);

/* chip_virgl_2d.c */
BOOL chip_virgl_init_2d(struct ChipGPUState *gs);
void chip_virgl_shutdown_2d(struct ChipGPUState *gs);
BOOL chip_virgl_recover_2d(struct ChipGPUState *gs);
BOOL chip_virgl_fill_rect(struct ChipGPUState *gs,
                           uint32 x, uint32 y, uint32 w, uint32 h,
                           uint32 color, RGBFTYPE format);
BOOL chip_virgl_blit_rect(struct ChipGPUState *gs,
                           uint32 sx, uint32 sy, uint32 dx, uint32 dy,
                           uint32 w, uint32 h);
BOOL chip_virgl_draw_colored_quad(struct ChipGPUState *gs,
                                   uint32 x, uint32 y, uint32 w, uint32 h,
                                   float r, float g, float b, float a);

/* chip_composite.c */
BOOL chip_comp_init_blends(struct ChipGPUState *gs);
BOOL chip_comp_upload_bitmap(struct ChipGPUState *gs,
                               const void *src_data, uint32 src_bpr,
                               uint32 src_w, uint32 src_h,
                               BOOL has_alpha);
void chip_comp_free_texture(struct ChipGPUState *gs);
BOOL chip_comp_draw_textured_quad(struct ChipGPUState *gs,
                                    uint32 dst_x, uint32 dst_y,
                                    uint32 dst_w, uint32 dst_h,
                                    float s0, float t0, float s1, float t1,
                                    BOOL use_linear);
BOOL chip_comp_test_textured_quad(struct ChipGPUState *gs);
uint32 chip_virgl_composite(struct ChipGPUState *gs,
                              uint32 op,
                              struct BitMap *src_bm,
                              const void *src_data,
                              uint32 src_bpr,
                              uint32 src_format,
                              int32 src_x, int32 src_y,
                              int32 src_w, int32 src_h,
                              int32 dst_x, int32 dst_y,
                              int32 dst_w, int32 dst_h,
                              uint32 flags,
                              uint32 color0);
BOOL chip_comp_install_hook(struct ChipGPUState *gs);
void chip_comp_set_dipf_flags(struct ChipGPUState *gs);

/* chip_flush.c */
void chip_flush(WORD x, WORD y, UWORD w, UWORD h);
void chip_flush_all(void);
void chip_flush_task_entry(void);
/* Wake the flush task immediately -- replaces direct chip_flush_all() calls
 * from vtable ops so they return quickly and let the flush task coalesce. */
void chip_flush_signal_activity(struct ChipGPUState *gs);

/* chip_modes.c */
void chip_register_modes(struct ChipGPUState *gs);

/* Vtable wirers split out of chip_p96.c (v53.112).
 * chip_alloc.c owns the memory + BitMap allocator callbacks (slots 0,
 * 1, 58, 59, 60).  chip_blit.c owns FillRect/InvertRect/BlitRect/
 * BlitRectNoMaskComplete/BlitTemplate/BlitPattern/DrawLine/
 * BlitPlanar2Chunky/BlitPlanar2Direct (slots 25, 27, 29, 31, 33, 35,
 * 37, 39, 41 + their FB twins).  chip_p96.c keeps display config,
 * pixel clock, vsync, and sprite callbacks. */
void chip_alloc_fill_vtable(struct BoardInfo *bi);
void chip_blit_fill_vtable(struct BoardInfo *bi);
BOOL chip_add_mode(struct ChipGPUState *gs, UWORD w, UWORD h,
                   UWORD hor_total, UWORD hor_sync_start, UWORD hor_sync_size,
                   UWORD ver_total, UWORD ver_sync_start, UWORD ver_sync_size,
                   ULONG mode_clock, UBYTE mode_flags, ULONG slot_idx);

/* chip_p96.c */
void chip_fill_boardinfo_vtable(struct BoardInfo *bi);

#endif /* CHIP_STATE_H */
