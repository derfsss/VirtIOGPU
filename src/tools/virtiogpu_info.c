/*
 * virtiogpu_info -- Shell utility, prints VirtIOGPU chip diagnostics.
 *
 * Replaces the -DDEBUG heartbeat for release-build users -- queries
 * graphics.library V54 GetBoardDataTagList for each registered RTG
 * board, prints the standard GBD_* fields plus our custom VirtIOGPU
 * identification.
 *
 * Usage from Shell:
 *   virtiogpu_info             # print all RTG boards
 *   virtiogpu_info -v          # verbose: include all queryable tags
 *
 * Built with -lauto so we don't manage interface lifecycle.
 *
 * Exit codes:
 *   0   success
 *   5   no RTG board found
 *   10  graphics.library v54 unavailable
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <graphics/gfxbase.h>
#include <graphics/board.h>
#include <stdio.h>
#include <string.h>

static int dump_board(ULONG bn, BOOL verbose)
{
    char  name[64]   = "";
    char  chip_drv[64]  = "";
    char  board_drv[64] = "";
    ULONG total          = 0;
    ULONG free_bytes     = 0;
    ULONG largest        = 0;
    ULONG internal_total = 0;
    ULONG internal_free  = 0;
    UWORD vendor     = 0;
    UWORD device     = 0;

    /* Tag set is whatever AOS4 V54 graphics/board.h actually defines.
     * Older P96 had additional tags (ChipName, MonitorSwitch,
     * RGBFormats, MemoryClock) that the AOS4 V54 surface does not
     * expose -- those we simply omit. */
    struct TagItem tags[] = {
        { GBD_BoardName,                (ULONG)name      },
        { GBD_ChipDriver,               (ULONG)chip_drv  },
        { GBD_BoardDriver,              (ULONG)board_drv },
        { GBD_TotalMemory,              (ULONG)&total    },
        { GBD_FreeMemory,               (ULONG)&free_bytes },
        { GBD_LargestFreeMemory,        (ULONG)&largest  },
        { GBD_InternalMemorySize,       (ULONG)&internal_total },
        { GBD_FreeInternalMemorySize,   (ULONG)&internal_free  },
        { GBD_PCIVendorID,              (ULONG)&vendor   },
        { GBD_PCIProductID,             (ULONG)&device   },
        { TAG_DONE,                     0                },
    };

    LONG got = IGraphics->GetBoardDataTagList(bn, tags);
    if (got <= 0) return 0;

    printf("\nBoard %lu:\n", bn);
    if (name[0])      printf("  Board name:     %s\n", name);
    if (chip_drv[0])  printf("  Chip driver:    %s\n", chip_drv);
    if (board_drv[0]) printf("  Board driver:   %s\n", board_drv);
    if (vendor || device)
        printf("  PCI ID:         %04x:%04x\n",
               (unsigned)vendor, (unsigned)device);
    if (total)        printf("  Memory total:   %lu bytes (%lu KB)\n",
                              total, total / 1024);
    if (free_bytes)   printf("  Memory free:    %lu bytes (%lu KB)\n",
                              free_bytes, free_bytes / 1024);
    if (largest)      printf("  Largest free:   %lu bytes\n", largest);
    if (internal_total)
        printf("  Internal mem:   %lu total, %lu free\n",
               internal_total, internal_free);

    if (verbose) {
        ULONG used = (total > free_bytes) ? (total - free_bytes) : 0;
        if (total) printf("  Memory used:    %lu bytes (%lu%% of total)\n",
                          used, total ? (used * 100 / total) : 0);
        if (largest && free_bytes && largest < free_bytes) {
            printf("  Fragmentation:  %lu%% (largest %lu of %lu free)\n",
                   100 - (largest * 100 / free_bytes), largest, free_bytes);
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    BOOL verbose = FALSE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = TRUE;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            printf("Usage: %s [-v]\n", argv[0]);
            printf("  -v  verbose (include memory usage / fragmentation)\n");
            return 0;
        }
    }

    if (!IGraphics) {
        fprintf(stderr, "graphics.library v54 not available\n");
        return 10;
    }

    printf("VirtIOGPU chip info -- via graphics.library v54 GetBoardData\n");
    printf("============================================================\n");

    int found = 0;
    for (ULONG bn = 0; bn < 16; bn++) {
        found += dump_board(bn, verbose);
    }

    if (found == 0) {
        printf("\n(no RTG boards registered)\n");
        return 5;
    }

    printf("\n%d RTG board%s reported.\n", found, found == 1 ? "" : "s");
    return 0;
}
