CHIMERA ABSOLUTE POINTER FOR WINDOWS 3.1, 95 AND 98
===================================================

In a Chimera DOSBox-X project, Mouse Position X/Y is a point on the screen:
0..65535 across and 0..65535 down, whatever the resolution. DOS programs
that use the INT 33h mouse follow it with nothing installed. Windows keeps
its own pointer and only hears relative motion, which it accelerates, so
without help the pointer lands near the target instead of on it. This disk
is the help.

Add chimera-mouse.img to the project as a floppy disk (it is A:), then
follow the steps for your Windows.


WINDOWS 95 AND 98

  1. Start > Run..., type  A:\CHIMABS.EXE  and press Enter.
  2. A message says it is installed. Press Enter.

CHIMABS.EXE has copied itself into the Windows directory, added itself to
the run= line in WIN.INI, and started. It starts with Windows from now on.
To remove it, delete CHIMABS.EXE from the run= line in C:\WINDOWS\WIN.INI.


WINDOWS 3.1 (VBADOS 0.67 by Javier S. Pedro, GPL version 2)

A project with a floppy starts its DOS prompt on A:. If the project starts
Windows from its own autoexec (cd windows, win), set the project's Initial
Drive setting to c first.

  1. Exit Windows. At the DOS prompt:
       MD C:\VBADOS
       COPY A:\VBMOUSE.EXE C:\VBADOS
       COPY A:\VBMOUSE.DRV C:\WINDOWS\SYSTEM
  2. Start Windows (WIN), open C:\WINDOWS\SYSTEM.INI in Notepad, and in
     the [boot] section change the mouse.drv= line to
       mouse.drv=vbmouse.drv
     ([boot.description] has a mouse.drv= line too; leave that one.)
     Save, and exit Windows.
  3. Type  C:\VBADOS\VBMOUSE.EXE  and then  WIN.

From now on VBMOUSE.EXE must be loaded before Windows every time: put the
line  C:\VBADOS\VBMOUSE.EXE  before the line that starts Windows - in the
project's configuration file ([autoexec]) if the project starts Windows,
or in C:\AUTOEXEC.BAT if the hard disk boots its own DOS. VBMOUSE.DRV
without VBMOUSE.EXE sends the pointer to the wrong place.


KEEPING IT

The install writes to the emulated hard disk. Do it at the start of the
movie, where it replays like any other input, or do it once and export the
disk (Emulator > Export Save Data...) to use as the hard disk of the
project you record in. Shut Windows down before exporting, or Windows 95
runs ScanDisk on its next start.


WHERE THE POINTER LANDS

The pointer lands on pixel  P = X * width / 65536  (and the same for Y),
on the same frame the position is set. To be sure of pixel P, ask for the
middle of it:

       X = (P * 65536 + 32768) / width

At 1024x768, pixel 512 across is X = 32800 and pixel 384 down is
Y = 32810. Windows 95 and 98 land any X inside the pixel on it; Windows
3.1 can land the first X of a pixel one pixel short, so use the middle.


WHAT STAYS RELATIVE

Mouse Speed X/Y is still relative motion. It is what games that read the
mouse themselves (DirectInput, mouselook) see, and it does not move the
position these drivers read. The buttons are unchanged.


FILES

  CHIMABS.EXE   the Windows 95/98 helper (GPL version 2 or later)
  CHIMABS.C     its source
  VBMOUSE.EXE   VBADOS 0.67, unmodified: the DOS driver Windows 3.1 needs
  VBMOUSE.DRV   VBADOS 0.67, unmodified: the Windows 3.1 mouse driver
  OEMSETUP.INF  VBADOS 0.67: lets Windows Setup install VBMOUSE.DRV
  VBMOUSE.EN    VBMOUSE.EXE's messages and options
  VBADOS.TGZ    the source of VBADOS 0.67 (tag v0.67 of
                https://git.javispedro.com/cgit/vbados.git)
  COPYING       the GNU General Public License, version 2
