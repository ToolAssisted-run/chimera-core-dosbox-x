/* chimabs.c - Chimera's absolute pointer for Windows 95 and 98 (chimera#135).
 *
 * Chimera places the mouse by POSITION: Mouse Position X/Y is a fraction of
 * the screen, 0..65535. Windows 95 and 98 have no driver that takes a
 * position, so their own PS/2 driver only ever sees relative motion, run
 * through Windows' pointer acceleration - and a TAS that knows where it wants
 * to click has to steer there instead of pointing.
 *
 * This is the missing half, as a program rather than a driver. The DOSBox-X
 * core offers the position on an I/O port of its own: a dword read of 0x5664
 * is X << 16 | Y, and a word read of 0x5666 is "CP". Not VMware's backdoor,
 * which answers in EBX/ECX/EDX: Windows 9x traps a Win32 program's IN and
 * performs the read itself, in ring 0, so the program sees only the value
 * read (measured: the backdoor came back with EAX 0 and EBX untouched).
 * The position goes to Windows through mouse_event(MOUSEEVENTF_ABSOLUTE),
 * whose 0..65535 is the same fraction of the screen the wire already is. The
 * first read tells the core this machine places its own pointer, and from
 * then on it sends no relative motion, so the two never fight; the buttons
 * still arrive through the PS/2 mouse as before.
 *
 * Run from anywhere but the Windows directory (the install floppy), it
 * installs itself: copies itself there, adds itself to WIN.INI's run= line so
 * it starts with Windows, and starts that copy. Run from the Windows
 * directory, it is the pointer.
 *
 * Built with no C runtime (Windows 95 may not have one), importing only
 * KERNEL32 and USER32. It runs inside the emulated machine, so it is as
 * deterministic as the machine is.
 *
 * Copyright (C) 2026 the Chimera authors; GPL-2.0-or-later.
 */
#include <windows.h>

#define CHIMERA_POINTER_PORT 0x5664
#define CHIMERA_POINTER_ID_PORT 0x5666
#define CHIMERA_POINTER_ID 0x5043
#define TITLE "Chimera absolute pointer"

static DWORD in32(WORD port)
{
	DWORD v;
	__asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static WORD in16(WORD port)
{
	WORD v;
	__asm__ __volatile__("inw %w1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static char upper(char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }

/* does s name CHIMABS.EXE anywhere, in any case */
static BOOL mentionsUs(const char *s)
{
	static const char us[] = "CHIMABS.EXE";
	for (; *s; s++)
	{
		int i = 0;
		while (us[i] && upper(s[i]) == us[i]) i++;
		if (!us[i]) return TRUE;
	}
	return FALSE;
}

static void fail(const char *why)
{
	MessageBoxA(NULL, why, TITLE, MB_OK | MB_ICONSTOP);
	ExitProcess(1);
}

static void install(const char *self, const char *dest)
{
	char shortDest[MAX_PATH], run[1024];

	/* a running copy holds its file: then the one already there stays */
	if (!CopyFileA(self, dest, FALSE) && GetFileAttributesA(dest) == 0xFFFFFFFFu)
		fail("Could not copy CHIMABS.EXE into the Windows directory. Nothing was installed.");

	/* run= splits on spaces, so it gets the 8.3 name */
	if (!GetShortPathNameA(dest, shortDest, sizeof shortDest)) lstrcpyA(shortDest, dest);
	GetProfileStringA("windows", "run", "", run, sizeof run);
	if (!mentionsUs(run))
	{
		if (lstrlenA(run) + 1 + lstrlenA(shortDest) + 1 > (int)sizeof run)
			fail("WIN.INI's run= line is too long to add CHIMABS.EXE to. Add it by hand.");
		if (run[0]) lstrcatA(run, " ");
		lstrcatA(run, shortDest);
		if (!WriteProfileStringA("windows", "run", run))
			fail("Could not write WIN.INI. CHIMABS.EXE is copied but will not start with Windows.");
	}

	/* a copy already running keeps running, and this new one bows out */
	WinExec(shortDest, SW_SHOWNORMAL);
	MessageBoxA(NULL,
		"Installed. Mouse Position now places the Windows pointer exactly, "
		"and CHIMABS.EXE starts with Windows from now on.\n\n"
		"To remove it, delete CHIMABS.EXE from the run= line in WIN.INI.",
		TITLE, MB_OK | MB_ICONINFORMATION);
	ExitProcess(0);
}

void WinMainCRTStartup(void)
{
	char self[MAX_PATH], dest[MAX_PATH];
	DWORD last = 0xFFFFFFFFu;

	/* Windows NT and later fault on a ring-3 IN: say so instead */
	if (!(GetVersion() & 0x80000000u))
		fail("This is for Windows 95 and 98 only.");
	/* not under Chimera's DOSBox-X: no port to read */
	if (in16(CHIMERA_POINTER_ID_PORT) != CHIMERA_POINTER_ID)
		fail("This machine is not Chimera's DOSBox-X core, so there is no position to read. Nothing was installed.");

	GetModuleFileNameA(NULL, self, sizeof self);
	GetWindowsDirectoryA(dest, sizeof dest - 13);
	if (dest[lstrlenA(dest) - 1] != '\\') lstrcatA(dest, "\\");
	lstrcatA(dest, "CHIMABS.EXE");
	if (lstrcmpiA(self, dest) != 0) install(self, dest);

	/* one copy, however many ways it was started */
	CreateMutexA(NULL, FALSE, "ChimeraAbsolutePointer");
	if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

	for (;;)
	{
		const DWORD at = in32(CHIMERA_POINTER_PORT);
		if (at != last)
		{
			mouse_event(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE, at >> 16, at & 0xFFFFu, 0, 0);
			last = at;
		}
		Sleep(10);
	}
}
