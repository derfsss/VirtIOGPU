/*
 * setup_monitor — Create P96 monitor driver file for VirtIOGPU chip
 *
 * Copies monitor_stub to DEVS:Monitors/SiliconMotion 502 and creates
 * an icon with tooltypes defining screen modes and sync limits.
 * Run once from Shell, then reboot.
 *
 * Usage: setup_monitor
 *   (monitor_stub must be in the same directory)
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <stdio.h>

static const char *monitor_path = "DEVS:Monitors/SiliconMotion 502";
static const char *stub_name    = "monitor_stub";

static STRPTR tooltypes[] = {
    "CMPLENGTH=17",
    "BOARDNAME=SiliconMotion 502",
    "VSYNCMIN=56",
    "VSYNCMAX=75",
    "HSYNCMIN=15000",
    "HSYNCMAX=70000",
    "MODE=640x480@60",
    "MODE=800x600@60",
    "MODE=1024x768@60",
    "MODE=1280x800@60",
    "DISABLEFAKENATIVE=Yes",
    "INTERRUPT=No",
    NULL
};

/* Copy a file byte-by-byte */
static BOOL copy_file(const char *src, const char *dst)
{
    BPTR in = IDOS->Open(src, MODE_OLDFILE);
    if (!in) return FALSE;

    BPTR out = IDOS->Open(dst, MODE_NEWFILE);
    if (!out) { IDOS->Close(in); return FALSE; }

    UBYTE buf[4096];
    int32 n;
    BOOL ok = TRUE;
    while ((n = IDOS->Read(in, buf, sizeof(buf))) > 0) {
        if (IDOS->Write(out, buf, n) != n) { ok = FALSE; break; }
    }

    IDOS->Close(out);
    IDOS->Close(in);

    /* Make executable */
    if (ok) IDOS->SetProtection(dst, 0);

    return ok;
}

int main(void)
{
    /* Find stub_name relative to our own location (PROGDIR:) */
    char stub_path[256];
    snprintf(stub_path, sizeof(stub_path), "PROGDIR:%s", stub_name);

    /* Check stub exists */
    BPTR lock = IDOS->Lock(stub_path, ACCESS_READ);
    if (!lock) {
        IDOS->Printf("Cannot find %s\n", stub_path);
        IDOS->Printf("Place monitor_stub in the same directory as setup_monitor.\n");
        return 1;
    }
    IDOS->UnLock(lock);

    /* Copy stub to DEVS:Monitors/ */
    if (!copy_file(stub_path, monitor_path)) {
        IDOS->Printf("Failed to copy %s -> %s\n", stub_path, monitor_path);
        return 1;
    }
    IDOS->Printf("Copied %s -> %s\n", stub_path, monitor_path);

    /* Open icon.library */
    struct Library *IconBase = IExec->OpenLibrary("icon.library", 0);
    if (!IconBase) {
        IDOS->Printf("Failed to open icon.library\n");
        return 1;
    }
    struct IconIFace *IIcon = (struct IconIFace *)
        IExec->GetInterface(IconBase, "main", 1, NULL);
    if (!IIcon) {
        IDOS->Printf("Failed to get Icon interface\n");
        IExec->CloseLibrary(IconBase);
        return 1;
    }

    /* Create icon with tooltypes */
    struct DiskObject *dobj = IIcon->GetIconTags(NULL,
        ICONGETA_GetDefaultType, WBTOOL,
        TAG_DONE);

    BOOL ok = FALSE;
    if (dobj) {
        dobj->do_ToolTypes = tooltypes;
        ok = IIcon->PutIconTags(monitor_path, dobj,
            ICONPUTA_NotifyWorkbench, TRUE,
            TAG_DONE);
        IIcon->FreeDiskObject(dobj);
    }

    IExec->DropInterface((struct Interface *)IIcon);
    IExec->CloseLibrary(IconBase);

    if (ok) {
        IDOS->Printf("Created icon with modes:\n");
        IDOS->Printf("  640x480@60, 800x600@60, 1024x768@60, 1280x800@60\n");
        IDOS->Printf("Reboot to activate new screen modes.\n");
    } else {
        IDOS->Printf("Failed to create icon for %s\n", monitor_path);
        return 1;
    }

    return 0;
}
