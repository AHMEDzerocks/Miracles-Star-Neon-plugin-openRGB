Miracles Star Neon — Stage 3 controls

This update adds native protocol commands for brightness, speed, and built-in hardware effects.

Files to replace in D:\MiraclesStarNeon:
- NeonController.h
- NeonController.cpp
- MiraclesStarNeonPlugin.cpp
- MiraclesStarNeon.json (Qt plugin metadata)
- MiraclesStarNeonPlugin.json (reference metadata)

Keep MiraclesStarNeonPlugin.h and MiraclesStarNeon.pro unchanged. The project already includes Qt Bluetooth.

Build in x64 Native Tools Command Prompt:
  cd /d D:\MiraclesStarNeon
  "D:\AHMEDzerocks\QT\6.8.3\msvc2022_64\bin\qmake.exe" MiraclesStarNeon.pro
  nmake

IMPORTANT:
- Keep a backup of the currently working DLL.
- Stop OpenRGB before copying a new DLL.
- Stop the Python BLE bridge before testing native BLE.
- The documented protocol lists effect command 0x06 but does not provide the strip's named effect catalogue. This build exposes numeric Hardware Effect IDs 0–255; only IDs supported by the strip will do something. The protocol example explicitly demonstrates ID 5.
- The OpenRGB Effects tab is a separate software effect engine. A running global software effect can keep sending colors and overwrite Static Color; stop that effect in the Effects tab. Choosing a hardware effect mode in the Miracles Star device's Mode dropdown is separate.
- Brightness protocol range: 3–1000. Speed range: 0–100.

Stage 3.1 slider stability patch: brightness and speed updates are debounced by 200 ms. Only the latest slider values are applied after dragging pauses, preventing BLE packet queue flooding.

Stage 4 additions:
- Added Turn On / Turn Off buttons in the Miracles Star plugin tab.
- Uses documented protocol commands 0x07 (on) and 0x01 (off), framed as BC CMD 00 55.
- Brightness/speed debounce from Stage 3.1 is retained.
- LED-count configuration is not included: the supplied protocol reference does not establish a reliable LED-count command/packet.
