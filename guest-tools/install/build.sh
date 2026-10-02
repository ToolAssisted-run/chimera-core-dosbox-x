#!/bin/sh
# Builds INSTALL.EXE, a 16-bit DOS program, with Open Watcom v2
# (WATCOM may point at the install; ~/tools/openwatcom by default).
set -eu
cd "$(dirname "$0")"
WATCOM="${WATCOM:-$HOME/tools/openwatcom}"
export WATCOM PATH="$WATCOM/binl64:$WATCOM/binl:$PATH" INCLUDE="$WATCOM/h"
wcl -q -bt=dos -ml -os -wx -fe=INSTALL.EXE install.c
rm -f install.o install.obj
