# x2nes — NES emulator on the X2D II 100C (Super Mario)

Status (evening of 2026-09-22): **all three entry layers verified on hardware**
1. **GAME tile in the native GUI's swipe-left menu** (full qrc mirror + injection into MainScreen.qml's 12th tile,
   tap → hook spawns x2nes)—final form
2. x2nes built-in ROM menu (runs directly without the GUI)
3. x2nes in-game virtual controller (wl_touch, 45fps)