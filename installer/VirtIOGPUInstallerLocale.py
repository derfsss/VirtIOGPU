#
# VirtIOGPUInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class VirtIOGPUInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_README_BUTTON = 2
    MSG_FINISH = 3
    MSG_REBOOT = 4

    def __init__(self, language="", catalogName='VirtIOGPU.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the VirtIOGPU graphics driver.\n\nvirtiogpu.chip is a Picasso96 (RTG) graphics driver for the VirtIO GPU device provided by the QEMU emulator.\n\nThe following changes will be made to your system:\n\n    1.  virtiogpu.chip will be copied to "SYS:Kickstart"\n\n    2.  "SYS:Kickstart/Kicklayout" will be updated to load the driver during startup (backup: "Kicklayout.bak")\n\nA system restart completes the installation.  Click "View Readme" below for manual installation details and general instructions on use.\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_README_BUTTON] = 'View Readme...'
        self.strings[self.MSG_FINISH] = '\nThe installation has finished.\n\nvirtiogpu.chip has been copied to "SYS:Kickstart" and "SYS:Kickstart/Kicklayout" has been updated (backup: "Kicklayout.bak").  The driver activates on the next system restart.\n\nQEMU must provide a VirtIO GPU display device (-device virtio-gpu-pci); see the README.md file in this drawer for details and the Virgl acceleration setup.\n\nPlease note: when restarting from within QEMU, the virtual machine may power off instead of restarting.  Should this occur, simply start QEMU again.\n\n\nPress "Finish" to exit the installation.'
        self.strings[self.MSG_REBOOT] = 'Restart the system now (required to activate the driver)'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
