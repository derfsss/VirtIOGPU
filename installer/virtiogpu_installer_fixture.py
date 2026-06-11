"""Installer-script fixture for the VirtIOGPU chip driver installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. copies virtiogpu.chip into SYS:Kickstart/
  2. inserts `MODULE Kickstart/virtiogpu.chip` into
     SYS:Kickstart/Kicklayout, immediately BEFORE the PCIGraphics.card
     line (the chip's VirtIOGPUBoard resident must hook
     PCIGraphics.card before graphics.library initialises) -- with a
     Kicklayout.bak backup, LF-only line endings, and idempotency
  3. offers a reboot on the finish page

install.py + VirtIOGPUInstallerLocale.py are emitted from this fixture
by an in-house installer-script generator and committed, so building
the distribution archive needs no extra tooling.  This fixture is the
authoritative description of the installer's pages, messages, and
behaviour.  The page idioms and the Kicklayout edit are expanded from
`installergen.presets` -- the field-tested templates shared by all of
this author's driver installers.

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

Full manual click-through VERIFIED 2026-06-10 on QEMU amigaone
(Installation Utility, drawer served over 9p SHARED:): chip copied to
SYS:Kickstart/ (250968 bytes), MODULE line inserted immediately before
PCIGraphics.card, Kicklayout.bak created.  The default-on Reboot
post-install action runs `reboot SYNC`; on QEMU amigaone a guest
reboot EXITS QEMU (clean exit 0) rather than resetting -- the finish
text warns about this.
"""

from installergen import (
    Project, Page, PageKind, Package, PackageKind, PostInstallAction,
    LocaleString, LocaleRef, Handler,
)
from installergen.presets import (
    README_BUTTON_LOCALE, InsertBeforeFirst, welcome_with_readme,
    finish_page, system_edit_helper, system_edit_exit_handler,
)


# NOTE: the Installation Utility's page text is PLAIN TEXT only and the
# label does NOT scroll -- keep pages inside the ~20-rendered-line lint
# budget and defer detail to the bundled readme.  Formatting follows
# Hyperion's own Update installers: leading blank line, paragraph
# spacing, quoted file and button names, explicit navigation cue.
locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the VirtIOGPU graphics "
        "driver.\n\n"
        "virtiogpu.chip is a Picasso96 (RTG) graphics driver for the "
        "VirtIO GPU device provided by the QEMU emulator.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  virtiogpu.chip will be copied to \"SYS:Kickstart\"\n\n"
        "    2.  \"SYS:Kickstart/Kicklayout\" will be updated to load "
        "the driver during startup (backup: \"Kicklayout.bak\")\n\n"
        "A system restart completes the installation.  Click "
        "\"View Readme\" below for manual installation details and "
        "general instructions on use.\n\n\n"
        "Press \"Next\" to continue."),
    README_BUTTON_LOCALE,
    LocaleString(
        "MSG_FINISH",
        "\nThe installation has finished.\n\n"
        "virtiogpu.chip has been copied to \"SYS:Kickstart\" and "
        "\"SYS:Kickstart/Kicklayout\" has been updated (backup: "
        "\"Kicklayout.bak\").  The driver activates on the next "
        "system restart.\n\n"
        "QEMU must provide a VirtIO GPU display device "
        "(-device virtio-gpu-pci); see the README.md file in this "
        "drawer for details and the Virgl acceleration setup.\n\n"
        "Please note: when restarting from within QEMU, the virtual "
        "machine may power off instead of restarting.  Should this "
        "occur, simply start QEMU again.\n\n\n"
        "Press \"Finish\" to exit the installation."),
    LocaleString(
        "MSG_REBOOT",
        "Restart the system now (required to activate the driver)"),
]


# Welcome page with the View Readme button (proven preset).
welcome_page = welcome_with_readme(LocaleRef("MSG_WELCOME"), "README.md")

# Kicklayout edit: the chip MUST load before PCIGraphics.card, so the
# MODULE line is inserted directly before it (proven preset:
# idempotent, .bak backup, LF-only; errors if the anchor is missing).
update_kicklayout = system_edit_helper(
    "SYS:Kickstart/Kicklayout",
    "MODULE Kickstart/virtiogpu.chip",
    InsertBeforeFirst(contains="PCIGraphics.card",
                      describe="MODULE Kickstart/PCIGraphics.card"),
)

# The edit runs when the INSTALL page is left in the forward direction
# -- i.e. after the file copy has completed.
install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
    exit_handler=system_edit_exit_handler(
        "SYS:Kickstart/Kicklayout",
        "MODULE Kickstart/virtiogpu.chip",
        "VirtIOGPU installer",
        "manually, BEFORE the PCIGraphics.card line:",
    ),
)

finish = finish_page(LocaleRef("MSG_FINISH"))


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
    version="53.161",
    date="11.06.2026",
    locale_strings=locale,
    helpers=[update_kicklayout],
    pages=[welcome_page, install_page, finish],
    packages=[chip_package],
    post_install_actions=[reboot_action],
)
