/* mempeek -- read a memory range and summarise its content.
 *
 * Diagnostic for the screen-drag investigation: lets us inspect RTG
 * board-memory bitmaps (e.g. the screen-drag scratch bitmap) from a
 * shell while the display is wedged, to tell "bitmap never written"
 * apart from "flush task displaying the wrong source".
 *
 * Usage: mempeek <hexaddr> <len> [dumplongs]
 *   Prints: count of nonzero ULONGs in the range, offset+value of the
 *   first and last nonzero ULONG, and optionally the first N ULONGs.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mempeek <hexaddr> <len> [dumplongs]\n");
        return 10;
    }

    unsigned long addr = strtoul(argv[1], NULL, 16);
    unsigned long len  = strtoul(argv[2], NULL, 0);
    unsigned long dump = (argc > 3) ? strtoul(argv[3], NULL, 0) : 0;

    volatile unsigned long *p = (volatile unsigned long *)addr;
    unsigned long longs = len / 4;
    unsigned long nonzero = 0, first_off = 0, last_off = 0;
    unsigned long first_val = 0, last_val = 0;

    for (unsigned long i = 0; i < longs; i++) {
        unsigned long v = p[i];
        if (v != 0) {
            if (nonzero == 0) { first_off = i * 4; first_val = v; }
            last_off = i * 4;
            last_val = v;
            nonzero++;
        }
    }

    printf("mempeek %08lx len=%lu: nonzero=%lu/%lu longs\n",
           addr, len, nonzero, longs);
    if (nonzero) {
        printf("  first nonzero @+0x%06lx = %08lx\n", first_off, first_val);
        printf("  last  nonzero @+0x%06lx = %08lx\n", last_off, last_val);
    }
    for (unsigned long i = 0; i < dump && i < longs; i++) {
        if (i % 8 == 0) printf("  +%06lx:", i * 4);
        printf(" %08lx", p[i]);
        if (i % 8 == 7) printf("\n");
    }
    if (dump && dump % 8) printf("\n");
    return 0;
}
