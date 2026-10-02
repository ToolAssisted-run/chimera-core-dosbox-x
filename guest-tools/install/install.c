/* install.c - installs Chimera's absolute pointer into the Windows on C:
 * (chimera#135). A 16-bit DOS program: it runs where Windows is NOT running,
 * which is the only time the files below may be changed safely.
 *
 *   Windows 95/98  CHIMABS.EXE into the Windows directory, and onto the
 *                  run= line of WIN.INI, so it starts with Windows.
 *   Windows 3.x    VBMOUSE.DRV into SYSTEM, and mouse.drv=vbmouse.drv in
 *                  SYSTEM.INI's [boot]. Under the core's own DOS that is all:
 *                  its INT 33h tells VBMOUSE.DRV where the pointer is. A DOS
 *                  booted from C: has no INT 33h of ours, so there VBMOUSE.EXE
 *                  goes into the Windows directory and AUTOEXEC.BAT loads it
 *                  before Windows.
 *
 * Nothing is written when everything is already in place, so running it at
 * every start (Use Chimera Mouse Driver does, with /AUTO) changes the disk
 * once. /AUTO says something only when it changed something; /BOOT says C:
 * boots its own DOS. Run inside Windows 95/98 it starts CHIMABS.EXE, which
 * installs itself the Windows way.
 *
 * Built with Open Watcom (build.sh). Copyright (C) 2026 the Chimera authors;
 * GPL-2.0-or-later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <dos.h>
#include <process.h>

#define MAXFILE 60000u

static int quiet, bootsDos;
static char src[80];          /* where INSTALL.EXE is, with a trailing backslash */
static char windir[80];
static int changed, failed;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

static int sameText(const char *a, const char *b, unsigned n)
{
	while (n--) if (lower(*a++) != lower(*b++)) return 0;
	return 1;
}

/* does [s, e) contain t, in any case */
static int contains(const char *s, const char *e, const char *t)
{
	unsigned n = strlen(t);
	for (; s + n <= e; s++) if (sameText(s, t, n)) return 1;
	return 0;
}

static void note(const char *what, const char *path)
{
	if (!quiet) printf("  %s %s\n", what, path);
	changed = 1;
}

static void problem(const char *what, const char *path)
{
	printf("Chimera mouse driver: %s %s\n", what, path);
	failed = 1;
}

/* the whole file, NUL-terminated; NULL if it is not there */
static char *load(const char *path, unsigned *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *buf;
	if (!f) return NULL;
	n = filelength(fileno(f));
	if (n < 0 || n > (long)MAXFILE) { fclose(f); problem("too large to edit:", path); return NULL; }
	buf = malloc((unsigned)n + 1);
	if (!buf) { fclose(f); problem("out of memory reading", path); return NULL; }
	*len = (unsigned)fread(buf, 1, (unsigned)n, f);
	fclose(f);
	buf[*len] = 0;
	return buf;
}

static int save(const char *path, const char *a, unsigned alen, const char *b, unsigned blen, const char *c, unsigned clen)
{
	FILE *f;
	unsigned attr;
	if (_dos_getfileattr(path, &attr) == 0 && (attr & _A_RDONLY)) _dos_setfileattr(path, attr & ~_A_RDONLY);
	f = fopen(path, "wb");
	if (!f) { problem("cannot write", path); return 0; }
	if (fwrite(a, 1, alen, f) != alen || fwrite(b, 1, blen, f) != blen || fwrite(c, 1, clen, f) != clen) {
		fclose(f); problem("cannot write", path); return 0;
	}
	if (fclose(f) != 0) { problem("cannot write", path); return 0; }
	return 1;
}

/* dst becomes a copy of src's NAME, unless it already is one */
static void ensureCopy(const char *name, const char *dst)
{
	char from[96];
	unsigned flen, dlen;
	char *want, *have;
	strcpy(from, src); strcat(from, name);
	want = load(from, &flen);
	if (!want) { problem("missing", from); return; }
	have = load(dst, &dlen);
	if (!have || dlen != flen || memcmp(have, want, flen) != 0) {
		if (save(dst, want, flen, "", 0, "", 0)) note("copied", dst);
	}
	free(want); free(have);
}

/* ---- INI files ---------------------------------------------------------- */
typedef struct {
	char *afterHeader;  /* start of the line after [section]; NULL: no section */
	char *value, *valueEnd;  /* the key's value; NULL: no such key there */
} IniSpot;

static char *lineEnd(char *s, char *end) { while (s < end && *s != '\r' && *s != '\n') s++; return s; }
static char *nextLine(char *s, char *end)
{
	s = lineEnd(s, end);
	if (s < end && *s == '\r') s++;
	if (s < end && *s == '\n') s++;
	return s;
}

static void iniFind(char *text, unsigned len, const char *section, const char *key, IniSpot *at)
{
	char *end = text + len, *s, *e, *p;
	int in = 0;
	unsigned slen = strlen(section), klen = strlen(key);
	at->afterHeader = at->value = at->valueEnd = NULL;
	for (s = text; s < end; s = nextLine(s, end)) {
		e = lineEnd(s, end);
		for (p = s; p < e && (*p == ' ' || *p == '\t'); p++);
		if (p < e && *p == '[') {
			if (in) return;  /* past the section */
			p++;
			in = (unsigned)(e - p) > slen && sameText(p, section, slen) && p[slen] == ']';
			if (in) at->afterHeader = nextLine(s, end);
			continue;
		}
		if (!in || (unsigned)(e - p) < klen || !sameText(p, key, klen)) continue;
		p += klen;
		while (p < e && (*p == ' ' || *p == '\t')) p++;
		if (p >= e || *p != '=') continue;
		p++;
		while (p < e && (*p == ' ' || *p == '\t')) p++;
		at->value = p;
		while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
		at->valueEnd = e;
		return;
	}
}

/* writes TEXT with [cut, cutEnd) replaced by INS */
static void splice(const char *path, char *text, unsigned len, char *cut, char *cutEnd, const char *ins)
{
	if (save(path, text, (unsigned)(cut - text), ins, strlen(ins), cutEnd, (unsigned)(text + len - cutEnd)))
		note("updated", path);
}

static void setIniKey(const char *path, const char *section, const char *key, const char *value, int append, const char *mustMention)
{
	unsigned len;
	char *text = load(path, &len);
	IniSpot at;
	static char line[300];
	if (!text) { problem("missing", path); return; }
	iniFind(text, len, section, key, &at);
	if (at.value) {
		if (mustMention ? contains(at.value, at.valueEnd, mustMention)
		                : ((unsigned)(at.valueEnd - at.value) == strlen(value) && sameText(at.value, value, strlen(value)))) {
			free(text); return;
		}
		if (append && at.valueEnd > at.value) {
			sprintf(line, " %s", value);
			splice(path, text, len, at.valueEnd, at.valueEnd, line);
		} else {
			splice(path, text, len, at.value, at.valueEnd, value);
		}
	} else if (at.afterHeader) {
		/* a header on the file's last line has no line break to insert after */
		int bare = at.afterHeader == text + len && len && text[len - 1] != '\n';
		sprintf(line, "%s%s=%s\r\n", bare ? "\r\n" : "", key, value);
		splice(path, text, len, at.afterHeader, at.afterHeader, line);
	} else {
		int bare = len && text[len - 1] != '\n';
		sprintf(line, "%s[%s]\r\n%s=%s\r\n", bare ? "\r\n" : "", section, key, value);
		splice(path, text, len, text + len, text + len, line);
	}
	free(text);
}

/* ---- AUTOEXEC.BAT ------------------------------------------------------- */
/* is this line's command WIN, WIN.COM or a path ending in either */
static int startsWindows(char *s, char *e)
{
	char *p, *w;
	for (p = s; p < e && (*p == ' ' || *p == '\t' || *p == '@'); p++);
	for (w = p; w < e && *w != ' ' && *w != '\t' && *w != '/'; w++);
	if (w - p >= 4 && sameText(w - 4, ".com", 4)) w -= 4;
	if (w - p < 3 || !sameText(w - 3, "win", 3)) return 0;
	return w - p == 3 || w[-4] == '\\' || w[-4] == ':';
}

static void loadTsrBeforeWindows(void)
{
	static const char path[] = "C:\\AUTOEXEC.BAT";
	static char line[100];
	unsigned len = 0;
	char *text = load(path, &len), *end, *s;
	if (!text) { text = malloc(1); text[0] = 0; len = 0; }
	end = text + len;
	if (contains(text, end, "VBMOUSE")) { free(text); return; }
	sprintf(line, "%s\\VBMOUSE.EXE\r\n", windir);
	for (s = text; s < end; s = nextLine(s, end)) {
		if (startsWindows(s, lineEnd(s, end))) { splice(path, text, len, s, s, line); free(text); return; }
	}
	if (len && text[len - 1] != '\n') { memmove(line + 2, line, strlen(line) + 1); line[0] = '\r'; line[1] = '\n'; }
	splice(path, text, len, end, end, line);
	free(text);
}

/* ---- finding Windows ---------------------------------------------------- */
static int exists(const char *path) { return access(path, 0) == 0; }

static void findWindows(void)
{
	unsigned len;
	char *text = load("C:\\MSDOS.SYS", &len);
	IniSpot at;
	windir[0] = 0;
	if (text) {
		/* Windows 95/98 keep a text MSDOS.SYS that says where Windows is */
		iniFind(text, len, "Paths", "WinDir", &at);
		if (at.value && at.valueEnd - at.value < (int)sizeof windir - 20) {
			memcpy(windir, at.value, at.valueEnd - at.value);
			windir[at.valueEnd - at.value] = 0;
		}
		free(text);
	}
	if (!windir[0]) strcpy(windir, "C:\\WINDOWS");
	while (strlen(windir) > 3 && windir[strlen(windir) - 1] == '\\') windir[strlen(windir) - 1] = 0;
}

static int windowsRunning(void)
{
	union REGS r;
	r.x.ax = 0x1600;
	int86(0x2F, &r, &r);
	return (r.h.al == 0 || r.h.al == 0x80) ? 0 : r.h.al;
}

int main(int argc, char **argv)
{
	char path[100], *slash;
	int i, running;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "/AUTO") || !strcmp(argv[i], "/auto")) quiet = 1;
		else if (!strcmp(argv[i], "/BOOT") || !strcmp(argv[i], "/boot")) bootsDos = 1;
	}
	strncpy(src, argv[0], sizeof src - 1);
	slash = strrchr(src, '\\');
	if (slash) slash[1] = 0; else src[0] = 0;

	running = windowsRunning();
	if (running >= 4) {
		/* inside Windows 95/98: the Windows half installs itself */
		strcpy(path, src); strcat(path, "CHIMABS.EXE");
		if (spawnl(P_WAIT, path, path, NULL) == -1)
			printf("Run %s from Start > Run.\n", path);
		return 0;
	}
	if (running) {
		printf("Exit Windows, then run %sINSTALL at the DOS prompt.\n", src);
		return 1;
	}

	findWindows();
	sprintf(path, "%s\\SYSTEM\\VMM32.VXD", windir);
	if (exists(path)) {
		if (!quiet) printf("Windows 95/98 in %s:\n", windir);
		sprintf(path, "%s\\CHIMABS.EXE", windir);
		ensureCopy("CHIMABS.EXE", path);
		sprintf(path, "%s\\WIN.INI", windir);
		{
			char exe[96];
			sprintf(exe, "%s\\CHIMABS.EXE", windir);
			setIniKey(path, "windows", "run", exe, 1, "CHIMABS.EXE");
		}
		if (failed) return 1;
		if (changed) printf("Chimera mouse driver installed into Windows 95/98 (%s).\n", windir);
		else if (!quiet) printf("Already installed.\n");
		return 0;
	}
	sprintf(path, "%s\\SYSTEM.INI", windir);
	if (exists(path)) {
		if (!quiet) printf("Windows 3.x in %s:\n", windir);
		sprintf(path, "%s\\SYSTEM\\VBMOUSE.DRV", windir);
		ensureCopy("VBMOUSE.DRV", path);
		sprintf(path, "%s\\SYSTEM.INI", windir);
		setIniKey(path, "boot", "mouse.drv", "vbmouse.drv", 0, NULL);
		/* the core's own DOS has Z:, a DOS booted from C: does not */
		if (bootsDos || (!quiet && !exists("Z:\\COMMAND.COM"))) {
			sprintf(path, "%s\\VBMOUSE.EXE", windir);
			ensureCopy("VBMOUSE.EXE", path);
			loadTsrBeforeWindows();
		}
		if (failed) return 1;
		if (changed) printf("Chimera mouse driver installed into Windows 3.x (%s).\n", windir);
		else if (!quiet) printf("Already installed.\n");
		return 0;
	}
	if (!quiet) printf("No Windows found in %s: nothing to install.\n", windir);
	return 0;
}
