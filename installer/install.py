#
# VirtIOGPU Chip Driver Install install.py
# $VER: VirtIOGPU Chip Driver Install 53.158 (10.06.2026)
# Auto-generated -- do not edit; regenerate from the fixture module.
#

from installer import *
from VirtIOGPUInstallerLocale import *
import amiga
import os

loc = VirtIOGPUInstallerLocale()

def updateKicklayout():
    kl = "SYS:Kickstart/Kicklayout"
    module_line = "MODULE Kickstart/virtiogpu.chip"
    try:
        f = open(kl, "rb")
        data = f.read()
        f.close()
    except IOError:
        return "could not read " + kl
    lines = data.split("\n")
    for ln in lines:
        if ln.strip() == module_line:
            return None        # already installed
    out = []
    inserted = 0
    for ln in lines:
        stripped = ln.strip()
        if ((not inserted) and stripped.startswith("MODULE")
                and stripped.find("PCIGraphics.card") != -1):
            out.append(module_line)
            inserted = 1
        out.append(ln)
    if not inserted:
        return ("no 'MODULE Kickstart/PCIGraphics.card' line found "
                "in " + kl)
    try:
        b = open(kl + ".bak", "wb")
        b.write(data)
        b.close()
    except IOError:
        pass                   # backup is best-effort
    try:
        f = open(kl, "wb")
        f.write("\n".join(out))
        f.close()
    except IOError:
        return "could not write " + kl
    return None

##############################################
# welcomePage
welcomePage = NewPage(GUI)

def readmeLaunch(page, id):
    amiga.system('notepad *>NIL: "README.md"')
    return True

StartGUI(welcomePage)
BeginGroup(GROUP_VERTICAL)
AddLabel(label=loc.GetString(loc.MSG_WELCOME))
BeginGroup(GROUP_HORIZONTAL, weight=0)
AddSpace(weight=1)
AddButton(label=loc.GetString(loc.MSG_README_BUTTON), frame=BUTTON_FRAME, onclick=readmeLaunch, weight=10)
AddSpace(weight=1)
EndGroup()
AddSpace()
EndGroup()
EndGUI()

##############################################
# installPage
installPage = NewPage(INSTALL)

def installExitHandler(page_nr, direction):
    if direction != 1:
        return True
    err = updateKicklayout()
    if err:
        try:
            import asl
            asl.MessageBox("VirtIOGPU installer",
                "Kicklayout update failed: " + err + "\n\n"
                "Please add this line to SYS:Kickstart/Kicklayout\n"
                "manually, BEFORE the PCIGraphics.card line:\n\n"
                "MODULE Kickstart/virtiogpu.chip",
                "OK")
        except StandardError:
            pass
    return True
SetObject(installPage, "exithandler", installExitHandler)

##############################################
# Post-install actions

def rebootHandler():
    amiga.system("reboot SYNC")
    return True

AddPostInstallAction(
    name='Reboot',
    description=loc.GetString(loc.MSG_REBOOT),
    visible=True,
    default=True,
    callback=rebootHandler,
    )

##############################################
# finishPage
finishPage = NewPage(FINISH)
SetString(finishPage, 'message', loc.GetString(loc.MSG_FINISH))

##############################################
# Top-level packages (always registered)

_pkg = AddPackage(FILEPACKAGE,
    name='VirtIOGPU chip driver',
    files=['content/virtiogpu.chip'],
    alternatepath="SYS:Kickstart"
    )

##############################################
# Run the installer
RunInstaller()
