# Skyline Ground Control App

Qt6 (C++17) Android/desktop ground control app for the Skyline drone: live
RTSP/H.265 video, point-cloud map streaming, telemetry over rosbridge
websockets, and in-app firmware updates.

Extracted from the main [Skyline-Drone](https://github.com/harishfaqot/Skyline-Drone)
monorepo so the app can be developed and versioned on its own; see that repo
for the drone-side ROS stack (SLAM, MAVROS, the video/pointcloud servers this
app talks to).

## Build

Requires Qt 6.8+ with the `Widgets`, `OpenGLWidgets`, `WebSockets`,
`Concurrent`, and `Network` modules, and GStreamer (for the RTSP video
client). Open `CMakeLists.txt` in Qt Creator, pick a kit (desktop or
Android), and build.

Firmware updates need a GitHub token: create `github_token.txt` next to
`CMakeLists.txt` (gitignored) or set `SKYLINE_GITHUB_TOKEN` — see
`CMakeLists.txt` for details.

## Layout

- `MainWindow.*` — main window, UI wiring
- `PointCloudWidget.*`, `PCDLoader.*` — 3D point-cloud rendering/loading
- `RosPointCloudClient.*`, `RtspVideoClient.*`, `TelemetryClient.*`,
  `PointCloudClient.*` — the drone's data feeds (rosbridge, RTSP video,
  telemetry, point-cloud websockets)
- `OverlayWidgets.*`, `ImageViewerWidget.*`, `IconRenderer.*`,
  `DroneMarkerGeometry.*` — HUD/overlay UI
- `FirmwareUpdateDialog.*`, `UpdateClient.*` — in-app firmware update flow
- `android/`, `android_openssl/` — Android packaging (manifest, GStreamer
  JNI loader, prebuilt OpenSSL)
