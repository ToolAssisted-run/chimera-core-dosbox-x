CHIMERA MOUSE DRIVERS FOR WINDOWS 3.1, 95 AND 98
================================================

This is drive B: of a Chimera DOSBox-X machine with Use Chimera Mouse
Driver on.

Mouse Position X/Y is a point on the screen: 0..65535 across and
0..65535 down, whatever the resolution. It applies while the Mouse Set
Position button is held: then the pointer is put there. Let go, the
pointer stays where it was put. Windows keeps its own pointer,
and left to itself it only hears relative motion, which it accelerates,
so the pointer lands near the target instead of on it. The drivers here
tell Windows where the pointer IS.


NOTHING TO DO

Before anything else starts, the machine runs B:\INSTALL /AUTO. It finds
the Windows on C: and installs the right driver if it is not there yet:

  Windows 95/98  CHIMABS.EXE into the Windows directory, started from
                 the run= line of WIN.INI.
  Windows 3.1    VBMOUSE.DRV into SYSTEM, and mouse.drv=vbmouse.drv in
                 the [boot] section of SYSTEM.INI. When C: boots a DOS
                 of its own, also VBMOUSE.EXE, loaded from AUTOEXEC.BAT
                 before WIN.

It writes nothing when the driver is already there, and leaves a disk
without Windows alone. Turning the setting off takes this drive away but
not an installed driver, which keeps working.


BY HAND

  B:\INSTALL     at a DOS prompt while Windows is not running, or from
                 Start > Run in Windows 95/98.

To remove the driver: on Windows 95/98, delete CHIMABS.EXE from the run=
line in C:\WINDOWS\WIN.INI; on Windows 3.1, set mouse.drv=mouse.drv in
SYSTEM.INI's [boot] section again.


WHERE THE POINTER LANDS

The pointer lands on pixel  P = X * width / 65536  (and the same for Y),
on the frame the position is set. To be sure of pixel P, ask for the
middle of it:

       X = (P * 65536 + 32768) / width

At 1024x768, pixel 512 across is X = 32800 and pixel 384 down is
Y = 32810. Windows 95 and 98 land any X inside the pixel on it; Windows
3.1 can land the first X of a pixel one pixel short, so use the middle.
Windows 95 and 98 follow a position held still; Windows 3.1 hears it
when it changes.


WHAT STAYS RELATIVE

Mouse Speed X/Y is still relative motion, while Mouse Set Position is
not held (held, the position wins and the speed is ignored). It is what
games that read the mouse themselves (DirectInput, mouselook) see, and it
does not move the position these drivers read. The buttons are unchanged.


FILES

  INSTALL.EXE   the installer (GPL version 2 or later)
  INSTALL.C     its source
  CHIMABS.EXE   the Windows 95/98 driver (GPL version 2 or later)
  CHIMABS.C     its source
  VBMOUSE.DRV   VBADOS 0.67 by Javier S. Pedro, unmodified (GNU GPL
                version 2): the Windows 3.1 mouse driver
  VBMOUSE.EXE   VBADOS 0.67: the DOS driver it needs when C: boots its
                own DOS
  OEMSETUP.INF  VBADOS 0.67: lets Windows Setup install VBMOUSE.DRV
  VBMOUSE.EN    VBMOUSE.EXE's messages and options
  VBADOS.TGZ    the source of VBADOS 0.67 (tag v0.67 of
                https://git.javispedro.com/cgit/vbados.git)
  COPYING       the GNU General Public License, version 2
