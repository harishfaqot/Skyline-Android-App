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
    Concurrent  # Off-GUI-thread image decode for RosImageClient (fixes camera latency)
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
    RosImageClient.cpp       # Add Image client
    OverlayWidgets.cpp       # Dashboard overlay widgets (top pills, sidebar, legend, compass, minimap, stats)
    IconRenderer.cpp         # Vector icon drawing (replaces emoji-based icons)
    UpdateClient.cpp         # Client for the drone's OTA update agent (update_agent/agent.py)
    FirmwareUpdateDialog.cpp # DJI-style in-app firmware update dialog
)

set(HEADERS
    MainWindow.h
    PointCloudWidget.h
    PCDLoader.h
    RosPointCloudClient.h  # Add ROS client header
    ImageViewerWidget.h    # Add Image viewer header
    RosImageClient.h       # Add Image client header
    OverlayWidgets.h        # Dashboard overlay widgets header
    IconRenderer.h           # Vector icon drawing header
    UpdateClient.h           # Update agent client header
    FirmwareUpdateDialog.h   # Firmware update dialog header
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

# Platform specific OpenGL
if(NOT ANDROID)
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
endif()
