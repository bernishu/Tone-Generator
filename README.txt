Dual Tone Generator
===================
A single-file native Windows app (no installer, no runtime) that plays an
independent sine tone in the left and right channel, like the dual tone
generator on onlinesound.net.

Usage
-----
Run DualToneGenerator.exe.
  * Left / Right channel  - drag the slider or type an exact value.
                            Range 1 - 20000 Hz, log-scaled slider.
                            Both can be changed while the tone is playing.
  * Volume                - master level, starts at 35%.
  * Play / Stop           - starts and stops playback (short fades, no clicks).
  * Save WAV...           - writes the current pair of tones to a
                            16-bit 44100 Hz stereo WAV of the length in
                            the "seconds" box (default 10).

Hearing safety: start quiet, especially with headphones and at high
frequencies where the tone can be louder than it sounds.

Build
-----
Windows/MSYS2:  build.bat        Linux (mingw-w64): sh build.sh
Output depends only on Windows system DLLs (winmm, comctl32, comdlg32).
