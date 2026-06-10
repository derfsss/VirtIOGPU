"""InstallerScriptGen fixture for the VirtIOGPU chip driver installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. copies virtiogpu.chip into SYS:Kickstart/
  2. inserts `MODULE Kickstart/virtiogpu.chip` into
     SYS:Kickstart/Kicklayout, immediately BEFORE the PCIGraphics.card
     line (the chip's VirtIOGPUBoard resident must hook
     PCIGraphics.card before graphics.library initialises) -- with a
     Kicklayout.bak backup, LF-only line endings, and idempotency
  3. offers a reboot on the finish page

Regenerate install.py + VirtIOGPUInstallerLocale.py with:

    PYTHONPATH=<InstallerScriptGen>/src:installer \
        python -m installergen emit virtiogpu_installer_fixture installer/

(InstallerScriptGen: https://github.com/derfsss -- separate project;
the emitted files are committed here so building the distribution
archive needs no extra tooling.)

Archive layout consumed by the script (see `make dist`):

    VirtIOGPU/
      install.py
      VirtIOGPUInstallerLocale.py
      content/virtiogpu.chip

IMPORTANT: the installer must be launched with the drawer as the
current directory (double-clicking install.py in the extracted drawer
does this; from a shell, CD into the drawer first) -- the package uses
drawer-relative content/ paths, exactly like the OS's own update
installers.  Launching with PACKAGE=<absolute path> from elsewhere
fails with "Python Runtime Error" (verified on Installation Utility
under QEMU amigaone; with the correct CWD the wizard renders fully).
"""

from installergen import (
    Project, Page, PageKind, Package, PackageKind, PostInstallAction,
    LocaleString, LocaleRef,
)
from installergen.model import Handler


locale = [
    LocaleString(
        "MSG_WELCOME",
        "Welcome to the VirtIOGPU chip driver installer.\n\n"
        "virtiogpu.chip is a Picasso96 RTG driver for the VirtIO GPU "
        "exposed by QEMU (-device virtio-gpu-pci).  It is for AmigaOS "
        "4.1 Final Edition systems running inside QEMU; it does nothing "
        "useful on real hardware.\n\n"
        "This installer will:\n"
        "1. Copy virtiogpu.chip to SYS:Kickstart/\n"
        "2. Add it to SYS:Kickstart/Kicklayout before PCIGraphics.card\n"
        "   (a backup is saved as Kicklayout.bak)\n\n"
        "A reboot is required afterwards to load the new Kickstart "
        "module."),
    LocaleString(
        "MSG_FINISH",
        "virtiogpu.chip has been installed.\n\n"
        "SYS:Kickstart/Kicklayout was updated (the previous version was "
        "saved as Kicklayout.bak).  The driver loads on the next reboot; "
        "make sure QEMU is started with a VirtIO GPU device:\n\n"
        "    -device virtio-gpu-pci\n\n"
        "or, for Virgl GPU acceleration:\n\n"
        "    -device virtio-gpu-gl-pci -display sdl,gl=on"),
    LocaleString(
        "MSG_REBOOT",
        "Reboot now (required to activate the new Kickstart module)"),
]


# Inserts the MODULE line into Kicklayout.  Pure Python 2.5; binary
# file modes keep the LF-only line endings Kickstart loaders require.
# Returns an error string, or None on success (including the
# already-installed case).
update_kicklayout = Handler(
    name="updateKicklayout",
    params=[],
    body=(
        "kl = \"SYS:Kickstart/Kicklayout\"\n"
        "module_line = \"MODULE Kickstart/virtiogpu.chip\"\n"
        "try:\n"
        "    f = open(kl, \"rb\")\n"
        "    data = f.read()\n"
        "    f.close()\n"
        "except IOError:\n"
        "    return \"could not read \" + kl\n"
        "lines = data.split(\"\\n\")\n"
        "for ln in lines:\n"
        "    if ln.strip() == module_line:\n"
        "        return None        # already installed\n"
        "out = []\n"
        "inserted = 0\n"
        "for ln in lines:\n"
        "    stripped = ln.strip()\n"
        "    if ((not inserted) and stripped.startswith(\"MODULE\")\n"
        "            and stripped.find(\"PCIGraphics.card\") != -1):\n"
        "        out.append(module_line)\n"
        "        inserted = 1\n"
        "    out.append(ln)\n"
        "if not inserted:\n"
        "    return (\"no 'MODULE Kickstart/PCIGraphics.card' line found \"\n"
        "            \"in \" + kl)\n"
        "try:\n"
        "    b = open(kl + \".bak\", \"wb\")\n"
        "    b.write(data)\n"
        "    b.close()\n"
        "except IOError:\n"
        "    pass                   # backup is best-effort\n"
        "try:\n"
        "    f = open(kl, \"wb\")\n"
        "    f.write(\"\\n\".join(out))\n"
        "    f.close()\n"
        "except IOError:\n"
        "    return \"could not write \" + kl\n"
        "return None\n"
    ),
)


welcome_page = Page(
    var_name="welcomePage",
    kind=PageKind.WELCOME,
    strings={"message": LocaleRef("MSG_WELCOME")},
)

# The Kicklayout edit runs when the INSTALL page is left in the forward
# direction -- i.e. after the file copy has completed.  Errors are
# reported via asl.MessageBox with manual-fix instructions; the wizard
# still completes so the copied chip isn't left half-installed silently.
install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
    exit_handler=Handler(
        name="installExitHandler",
        params=["page_nr", "direction"],
        body=(
            "if direction != 1:\n"
            "    return True\n"
            "err = updateKicklayout()\n"
            "if err:\n"
            "    try:\n"
            "        import asl\n"
            "        asl.MessageBox(\"VirtIOGPU installer\",\n"
            "            \"Kicklayout update failed: \" + err + \"\\n\\n\"\n"
            "            \"Please add this line to SYS:Kickstart/Kicklayout\\n\"\n"
            "            \"manually, BEFORE the PCIGraphics.card line:\\n\\n\"\n"
            "            \"MODULE Kickstart/virtiogpu.chip\",\n"
            "            \"OK\")\n"
            "    except StandardError:\n"
            "        pass\n"
            "return True\n"
        ),
    ),
)

finish_page = Page(
    var_name="finishPage",
    kind=PageKind.FINISH,
    strings={"message": LocaleRef("MSG_FINISH")},
)


chip_package = Package(
    name="VirtIOGPU chip driver",
    files=["content/virtiogpu.chip"],
    kind=PackageKind.FILEPACKAGE,
    # Fixed destination: Kickstart modules must land on the boot
    # volume regardless of any user preference, so there is no
    # DESTINATION page and the path is hardwired.  Emitted verbatim,
    # hence the embedded quotes.
    alternatepath="\"SYS:Kickstart\"",
    register_in="top",
)

reboot_action = PostInstallAction(
    name="Reboot",
    description=LocaleRef("MSG_REBOOT"),
    visible=True,
    default=True,
    callback=Handler(
        name="rebootHandler",
        params=[],
        body="amiga.system(\"reboot SYNC\")\nreturn True\n",
    ),
)


project = Project(
    name="VirtIOGPU Chip Driver Install",
    short_name="VirtIOGPU",
    version="53.158",
    date="10.06.2026",
    locale_strings=locale,
    helpers=[update_kicklayout],
    pages=[welcome_page, install_page, finish_page],
    packages=[chip_package],
    post_install_actions=[reboot_action],
)
