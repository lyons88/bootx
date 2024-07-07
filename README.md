# bootx
Modified BootX for Mac OS 10.4.11

This version of BootX is an attept to build BootX in the ELF format to see is Open Firmware can load it and it will boot Mac OS X Tiger 10.4.11

You'll need to modify the Raid.c and correct the path to the Riad.h

Then a simple make at the top level should build BootX as an EFL format binary for PowerPC systems......or we hoee it will someday!

The main reason for this is to attept to boot Mac OS X on PowerPC systems that use Open Firmware, but lack CHRP or XCOFF booting.
