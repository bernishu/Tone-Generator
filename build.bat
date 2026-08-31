@rem Build on Windows with MinGW-w64 (MSYS2: pacman -S mingw-w64-x86_64-gcc)
windres app.rc -O coff -o app.res
gcc -O2 -Wall -Wextra -mwindows DualToneGenerator.c app.res -o DualToneGenerator.exe -lwinmm -lcomctl32 -lcomdlg32 -lgdi32 -static -s
