cmake_minimum_required(VERSION 3.16)
project(skyline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt components
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    OpenGLWidgets
    WebSockets  # WebSocket support (rosbridge uses ws://)
    Concurrent  # Off-GUI-thread work (kept for other QtConcurrent uses)
    Network     # GitHub API + archive download for in-app firmware update
)

# Source files
set(SOURCES
    main.cpp
    MainWindow.cpp
    PointCloudWidget.cpp
    PCDLoader.cpp
    RosPointCloudClient.cpp  # Add ROS client
    ImageViewerWidget.cpp    # Add Image viewer
    RtspVideoClient.cpp      # RTSP/H.265 video client (replaces old per-frame JSON image feed)
    PointCloudClient.cpp     # Binary point-cloud downlink client (isolated from telemetry, see pointcloud_ws_server.py)
    OverlayWidgets.cpp       # Dashboard overlay widgets (top pills, sidebar, legend, compass, minimap, stats)
    IconRenderer.cpp         # Vector icon drawing (replaces emoji-based icons)
    UpdateClient.cpp         # Client for the drone's OTA update agent (update_agent/agent.py)
    FirmwareUpdateDialog.cpp # DJI-style in-app firmware update dialog
    DroneMarkerGeometry.cpp  # Procedural geometry for the point cloud's drone marker
)

set(HEADERS
    MainWindow.h
    PointCloudWidget.h
    PCDLoader.h
    RosPointCloudClient.h  # Add ROS client header
    ImageViewerWidget.h    # Add Image viewer header
    RtspVideoClient.h      # RTSP/H.265 video client header
    PointCloudClient.h     # Binary point-cloud downlink client header
    OverlayWidgets.h        # Dashboard overlay widgets header
    IconRenderer.h           # Vector icon drawing header
    UpdateClient.h           # Update agent client header
    FirmwareUpdateDialog.h   # Firmware update dialog header
    DroneMarkerGeometry.h    # Procedural geometry for the point cloud's drone marker
)

# Add resource file
set(RESOURCES
    resources.qrc
)

# Create executable for all platforms.
# FIX: RESOURCES must be included on Android too so that Qt compiles
# resources.qrc into the APK and :/... paths work at runtime.
# Previously RESOURCES was missing from the Android branch, causing
# QPixmap(":/android/res/drawable/skyline.png") to always return null.
qt_add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

# Link Qt libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::CorePrivate
    Qt6::WebSockets  # Link WebSocket library
    Qt6::Concurrent
    Qt6::Network
)

# Android-specific configuration
if(ANDROID)
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")

    set_target_properties(${PROJECT_NAME} PROPERTIES
        QT_ANDROID_MIN_SDK_VERSION "28"
        QT_ANDROID_TARGET_SDK_VERSION "36"
        QT_ANDROID_VERSION_CODE "1"
        QT_ANDROID_VERSION_NAME "1.0"
        QT_ANDROID_ABIS "${ANDROID_ABI}"
    )

    # Ensure it's treated as an application
    set_target_properties(${PROJECT_NAME} PROPERTIES
        QT_ANDROID_APPLICATION TRUE
    )

    message(STATUS "Building for Android ABI: ${ANDROID_ABI}")
    message(STATUS "Android package source dir: ${CMAKE_CURRENT_SOURCE_DIR}/android")

    # Android doesn't ship OpenSSL for Qt to use, and Qt's own TLS backend
    # plugin (bundled automatically) only dlopen()s libssl_3.so/libcrypto_3.so
    # at runtime rather than providing them — without these, any HTTPS
    # request (e.g. FirmwareUpdateDialog's GitHub API calls) fails with
    # "TLS initialization failed". These are prebuilt OpenSSL 3.x shared
    # libraries (from https://github.com/KDAB/android_openssl, trimmed to
    # just the two .so files per ABI this app actually needs) that get
    # copied into the APK alongside the app's own native libraries.
    set(SKYLINE_OPENSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android_openssl/${CMAKE_ANDROID_ARCH_ABI}")
    if(EXISTS "${SKYLINE_OPENSSL_DIR}/libssl_3.so")
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY QT_ANDROID_EXTRA_LIBS
            "${SKYLINE_OPENSSL_DIR}/libcrypto_3.so"
            "${SKYLINE_OPENSSL_DIR}/libssl_3.so"
        )
    else()
        message(WARNING "No bundled OpenSSL libs for ABI ${CMAKE_ANDROID_ARCH_ABI} at ${SKYLINE_OPENSSL_DIR} — HTTPS (firmware update GitHub check) will fail at runtime with 'TLS initialization failed'.")
    endif()
endif()

# ── GStreamer (RTSP/H.265 video receive — see RtspVideoClient.cpp) ─────────
# Replaces Qt Multimedia's QMediaPlayer, which has no exposed jitterbuffer
# control and blanked to black on any late/reordered RTP packet over the
# real RF link. This talks to GStreamer directly instead, same as
# QGroundControl and the drone-side encoder
# (skyline_ws/src/skyline/src/video_rtsp_server.py) already do.
#
# Desktop (Windows/Linux/macOS): uses pkg-config where available (default
# on Linux). On Windows, install GStreamer's "development" MSVC installer
# from https://gstreamer.freedesktop.org/download/ — it sets
# GSTREAMER_1_0_ROOT_MSVC_X86_64, used below as a fallback when pkg-config
# itself isn't on PATH (the common case on a fresh Windows dev machine).
#
# Android: there is no distro package for this. Steps (verified against
# gstreamer-1.0-android-universal-1.28.6):
#   1. Download "gstreamer-1.0-android-universal-<version>.tar.xz" from
#      https://gstreamer.freedesktop.org/download/ (Android tab) and
#      extract it, e.g. to D:/gstreamer-android — it unpacks into
#      arm64/armv7/x86/x86_64 subfolders, one full GStreamer install each.
#   2. Set the GSTREAMER_ROOT_ANDROID environment variable to that
#      extraction root (the folder containing arm64/armv7/x86/x86_64, NOT
#      one of those subfolders itself) before running CMake.
#   3. The SDK's include/lib layout has plugins as static libs
#      (lib/gstreamer-1.0/libgst*.a) but no prebuilt combined .so — you
#      must link one yourself via its ndk-build tooling
#      (<abi>/share/gst-android/ndk-build/gstreamer-1.0.mk). Set up a
#      throwaway ndk-build project:
#        jni/dummy.c        — one empty function, just needs to exist
#        jni/Android.mk      — LOCAL_MODULE with LOCAL_SRC_FILES := dummy.c
#                              and LOCAL_SHARED_LIBRARIES := gstreamer_android,
#                              then include .../gstreamer-1.0.mk with
#                              GSTREAMER_PLUGINS set to what you need.
#        jni/Application.mk  — APP_ABI, APP_PLATFORM (match
#                              QT_ANDROID_MIN_SDK_VERSION below), APP_STL := c++_shared
#      then run `ndk-build` from that project directory with
#      GSTREAMER_ROOT_ANDROID set. IMPORTANT: don't use the
#      GSTREAMER_PLUGINS_CORE/_NET/_CODECS group variables from plugins.mk
#      wholesale — several entries in them (rsinter, rstracers,
#      threadshare, rsrtp, rsrtsp, ...) are gst-plugins-rs (Rust) plugins
#      that link against libgstrsworkspace.a, and that link step fails on
#      the Windows NDK toolchain ("no such file or directory:
#      'gstrsworkspace'"). List the classic (C) elements by name instead:
#      this app only needs
#      "coreelements app videoconvertscale typefindfunctions playback rtsp rtp rtpmanager udp videoparsersbad androidmedia libav".
#      (playback is what actually provides decodebin, and udp is what
#      provides udpsrc/udpsink that rtspsrc creates internally for the
#      actual media transport — both are separate groups/plugins from
#      coreelements/rtsp/rtp/rtpmanager and easy to miss; missing either
#      one doesn't fail the pipeline string parse, since rtspsrc creates
#      udpsrc/udpsink internally rather than by name in the launch string,
#      it instead fails later at SETUP time with a generic "Could not
#      get/set settings from/on resource." error.)
#      The result lands at libs/<abi>/libgstreamer_android.so — copy that
#      into $GSTREAMER_ROOT_ANDROID/<abi-dir>/lib/libgstreamer_android.so
#      (abi-dir per GSTREAMER_ANDROID_ABI_DIR below, e.g. arm64-v8a -> arm64).
#   4. Re-run CMake. The block below picks it up automatically once found.
# main.cpp additionally calls libgstreamer_android.so's JNI_OnLoad once at
# startup on Android (see main.cpp) — required for GStreamer's Android JNI
# glue (used by the androidmedia plugin for hardware decode) to work at
# all. This is NOT the "gst_android_init(JNIEnv*, jobject)" function older
# tutorials describe — that name doesn't exist as an exported symbol in
# this SDK version; JNI_OnLoad is the real, verified entry point (checked
# with `nm` against the actual downloaded .a).
if(ANDROID)
    if(DEFINED ENV{GSTREAMER_ROOT_ANDROID})
        set(GSTREAMER_ROOT_ANDROID "$ENV{GSTREAMER_ROOT_ANDROID}")
    endif()

    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        set(GSTREAMER_ANDROID_ABI_DIR "arm64")
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
        set(GSTREAMER_ANDROID_ABI_DIR "armv7")
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
        set(GSTREAMER_ANDROID_ABI_DIR "x86_64")
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
        set(GSTREAMER_ANDROID_ABI_DIR "x86")
    endif()

    set(GSTREAMER_ANDROID_ROOT "${GSTREAMER_ROOT_ANDROID}/${GSTREAMER_ANDROID_ABI_DIR}")

    if(GSTREAMER_ROOT_ANDROID AND EXISTS "${GSTREAMER_ANDROID_ROOT}/lib/libgstreamer_android.so")
        message(STATUS "GStreamer (Android): using ${GSTREAMER_ANDROID_ROOT}")
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${GSTREAMER_ANDROID_ROOT}/include/gstreamer-1.0"
            "${GSTREAMER_ANDROID_ROOT}/include/glib-2.0"
            "${GSTREAMER_ANDROID_ROOT}/lib/glib-2.0/include"
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE
            "${GSTREAMER_ANDROID_ROOT}/lib/libgstreamer_android.so"
        )
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY QT_ANDROID_EXTRA_LIBS
            "${GSTREAMER_ANDROID_ROOT}/lib/libgstreamer_android.so"
        )
    else()
        message(WARNING
            "GStreamer Android SDK not found (looked for "
            "${GSTREAMER_ANDROID_ROOT}/lib/libgstreamer_android.so). "
            "RtspVideoClient will fail to build/link for Android until this is "
            "set up — see the GStreamer comment block just above in this file "
            "for the setup steps (download the SDK, set GSTREAMER_ROOT_ANDROID, "
            "build libgstreamer_android.so via their ndk-build tooling)."
        )
    endif()
else()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(GST QUIET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
    endif()

    if(NOT GST_FOUND AND DEFINED ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64})
        set(GST_WIN_ROOT "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}")
        set(GST_INCLUDE_DIRS
            "${GST_WIN_ROOT}/include/gstreamer-1.0"
            "${GST_WIN_ROOT}/include/glib-2.0"
            "${GST_WIN_ROOT}/lib/glib-2.0/include"
        )
        set(GST_LIBRARY_DIRS "${GST_WIN_ROOT}/lib")
        set(GST_LIBRARIES gstreamer-1.0 gstapp-1.0 gstvideo-1.0 glib-2.0 gobject-2.0)
        set(GST_FOUND TRUE)
        message(STATUS "GStreamer (Windows): using ${GST_WIN_ROOT} (pkg-config not found)")
    endif()

    if(NOT GST_FOUND)
        message(WARNING
            "GStreamer 1.0 development files not found — RtspVideoClient won't "
            "build. Linux: install libgstreamer1.0-dev, "
            "libgstreamer-plugins-base1.0-dev, libgstreamer-plugins-bad1.0-dev "
            "(plus gstreamer1.0-plugins-{good,bad,ugly} and gstreamer1.0-libav at "
            "runtime for RTP/RTSP + H.265 decode). Windows: install the "
            "GStreamer 'development' MSVC installer from "
            "https://gstreamer.freedesktop.org/download/, which sets "
            "GSTREAMER_1_0_ROOT_MSVC_X86_64."
        )
    else()
        target_include_directories(${PROJECT_NAME} PRIVATE ${GST_INCLUDE_DIRS})
        target_link_directories(${PROJECT_NAME} PRIVATE ${GST_LIBRARY_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${GST_LIBRARIES})
    endif()
endif()

# Platform specific OpenGL
if(NOT ANDROID)
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
endif()

# ── Firmware update: bake in a default GitHub token ────────────────────────
# So end users of the built app never have to enter one themselves (see
# FirmwareUpdateDialog). The token itself must NEVER be a tracked file — it's
# read here from either the SKYLINE_GITHUB_TOKEN environment variable, or a
# local file at github_token.txt next to this CMakeLists.txt (create it
# yourself, containing just the token; it's listed in .gitignore so it can
# never be accidentally committed). Neither is required — if absent, the
# build just doesn't get a default token and the app fails firmware-update
# GitHub calls until QSettings has one (which nothing in this app's UI sets
# anymore, so in practice this must be provided for the feature to work).
set(SKYLINE_GITHUB_TOKEN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/github_token.txt")
# CMake only reads this file at configure time, not at every build — without
# this, creating/editing github_token.txt after the first configure would
# silently keep using whatever was (or wasn't) there before, until someone
# thought to force a reconfigure. This makes CMake treat the token file like
# any other configure input: touching it triggers an automatic reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SKYLINE_GITHUB_TOKEN_FILE}")

if(DEFINED ENV{SKYLINE_GITHUB_TOKEN})
    set(SKYLINE_BUILTIN_GITHUB_TOKEN "$ENV{SKYLINE_GITHUB_TOKEN}")
elseif(EXISTS "${SKYLINE_GITHUB_TOKEN_FILE}")
    file(READ "${SKYLINE_GITHUB_TOKEN_FILE}" SKYLINE_BUILTIN_GITHUB_TOKEN)
    string(STRIP "${SKYLINE_BUILTIN_GITHUB_TOKEN}" SKYLINE_BUILTIN_GITHUB_TOKEN)
endif()

if(SKYLINE_BUILTIN_GITHUB_TOKEN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        SKYLINE_BUILTIN_GITHUB_TOKEN="${SKYLINE_BUILTIN_GITHUB_TOKEN}"
    )
    message(STATUS "Firmware update: baking in a default GitHub token")
else()
    message(WARNING "Firmware update: no SKYLINE_GITHUB_TOKEN env var and no github_token.txt found — the built app won't have a working GitHub token. See CMakeLists.txt.")
endif()
