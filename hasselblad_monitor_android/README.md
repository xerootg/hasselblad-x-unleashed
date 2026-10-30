# UVCMonitor Android — X2D II External Monitor (Android Display)

## Features

Zebra stripes(threshold slider) / False color(9 bands) / Focus peaking(Sobel red highlights,sensitivity slider) / RGB histogram(NV21 sampling,updated every 0.5s) / Grid(rule of thirds,diagonals,center,golden ratio) / Aspect ratio mask(16:9 4:3 1:1 **65:24** 3:2) / Monochrome / Digital zoom 1-8×(pinch+drag,double-tap to reset) / Freeze frame / Save screenshots to gallery(MediaStore,no permission required) / Top-bar HUD(device name=USB iProduct/format/measured fps) / Tap to hide UI / Automatically launch app when a UVC device is connected(device_filter).

## Architecture

```
app/src/main/java/com/x2dii/uvcmonitor/
  MainActivity.kt   Entry point + state + listener callbacks
  UvcEngine.kt      UsbManager(permissions/hotplug) + UVCCamera(YUYV/MJPEG→NV21 frame callbacks) + fps/histogram/screenshots
  GlRenderer.kt     GLSurfaceView + GLSL ES2 single pass (NV21 dual planes→RGB + zoom/peaking/monochrome/zebra/false color)
  Ui.kt             Complete Compose UI (top bar/toolbar/sliders/overlay/gestures/histogram)
  MonitorSettings.kt State + enums
app/src/main/java/com/serenegiant/utils/
  HandlerThreadHandler.java / BuildCheck.java   ← Compatibility classes (see below)
```