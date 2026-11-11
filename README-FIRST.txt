Miracles Star Neon — Stage 1

This starter targets your confirmed setup:
- OpenRGB source: D:\OpenRGB-Source
- Qt: D:\AHMEDzerocks\QT\6.8.3\msvc2022_64
- Compiler: MSVC 2022 64-bit
- OpenRGB plugin API: version 5

Stage 1 only registers a virtual OpenRGB device named "Miracles Star Neon".
It does NOT connect over Bluetooth yet. We first prove the plugin builds and loads.

Open the x64 Native Tools Command Prompt for VS 2022, then run:

cd /d D:\MiraclesStarNeon
"D:\AHMEDzerocks\QT\6.8.3\msvc2022_64\bin\qmake.exe" MiraclesStarNeon.pro
nmake

Do not copy the DLL into OpenRGB yet. Send me the full build output first.
