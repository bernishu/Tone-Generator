#!/bin/sh
# Cross-compile from Linux with mingw-w64 (apt install mingw-w64)
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows DualToneGenerator.c app.res \
    -o DualToneGenerator.exe -lwinmm -lcomctl32 -lcomdlg32 -lgdi32 -static -s
