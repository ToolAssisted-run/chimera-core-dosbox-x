#!/bin/sh
# Builds CHIMABS.EXE for Windows 95/98: i486, no C runtime, PE version 4.0.
set -eu
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -Os -march=i486 -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
	-nostdlib -Wall -Wextra -o CHIMABS.EXE chimabs.c \
	-Wl,-e,_WinMainCRTStartup -Wl,--subsystem,windows:4.0 \
	-Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
	-Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
	-Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-insert-timestamp \
	-lkernel32 -luser32
