/* draginput -- inject mouse events through input.device.
 *
 * Diagnostic for the screen-drag investigation: performs a screen
 * title-bar drag without host-side input (QMP is busy / SDL focus
 * unreliable).  Sequence: park pointer at top-left, move to (tx,ty),
 * LMB down, drag by (0,dy) in small steps, LMB up.
 *
 * Usage: draginput <tx> <ty> <dy>
 *   tx,ty  target position reached with relative moves from the
 *          parked top-left corner
 *   dy     drag distance (positive = down, negative = up)
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <stdlib.h>
#include <stdio.h>

static struct MsgPort *mp;
static struct IOStdReq *io;

static void send_event(UWORD code, UWORD qual, WORD dx, WORD dy)
{
    struct InputEvent ie;
    for (unsigned i = 0; i < sizeof(ie); i++) ((char *)&ie)[i] = 0;
    ie.ie_Class     = IECLASS_RAWMOUSE;
    ie.ie_Code      = code;
    ie.ie_Qualifier = qual;
    ie.ie_X         = dx;
    ie.ie_Y         = dy;

    io->io_Command = IND_WRITEEVENT;
    io->io_Data    = &ie;
    io->io_Length  = sizeof(ie);
    DoIO((struct IORequest *)io);
}

static void rel_move(WORD dx, WORD dy)
{
    send_event(IECODE_NOBUTTON, IEQUALIFIER_RELATIVEMOUSE, dx, dy);
}

static void chunked_move(LONG dx, LONG dy)
{
    while (dx || dy) {
        WORD cx = (dx > 50) ? 50 : (dx < -50 ? -50 : (WORD)dx);
        WORD cy = (dy > 50) ? 50 : (dy < -50 ? -50 : (WORD)dy);
        rel_move(cx, cy);
        dx -= cx; dy -= cy;
        Delay(1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: draginput <tx> <ty> <dy>\n");
        return 10;
    }
    LONG tx = atol(argv[1]);
    LONG ty = atol(argv[2]);
    LONG dy = atol(argv[3]);

    mp = (struct MsgPort *)AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!mp) return 20;
    io = (struct IOStdReq *)AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size, sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, mp, TAG_DONE);
    if (!io) return 20;
    if (OpenDevice("input.device", 0, (struct IORequest *)io, 0) != 0)
        return 21;

    /* park at top-left */
    for (int i = 0; i < 10; i++) { rel_move(-400, -400); Delay(1); }
    /* go to target */
    chunked_move(tx, ty);
    Delay(5);
    /* LMB down */
    send_event(IECODE_LBUTTON, IEQUALIFIER_RELATIVEMOUSE, 0, 0);
    Delay(5);
    /* drag in 20px steps */
    {
        LONG remaining = dy;
        while (remaining) {
            WORD step = (remaining > 20) ? 20 : (remaining < -20 ? -20 : (WORD)remaining);
            send_event(IECODE_NOBUTTON,
                       IEQUALIFIER_RELATIVEMOUSE | IEQUALIFIER_LEFTBUTTON,
                       0, step);
            remaining -= step;
            Delay(3);
        }
    }
    Delay(5);
    /* LMB up */
    send_event(IECODE_LBUTTON | IECODE_UP_PREFIX, IEQUALIFIER_RELATIVEMOUSE, 0, 0);
    Delay(5);

    printf("drag done: target=(%ld,%ld) dy=%ld\n", tx, ty, dy);

    CloseDevice((struct IORequest *)io);
    FreeSysObject(ASOT_IOREQUEST, io);
    FreeSysObject(ASOT_PORT, mp);
    return 0;
}
