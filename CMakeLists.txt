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
)

set(HEADERS
    MainWindow.h
    PointCloudWidget.h
    PCDLoader.h
    RosPointCloudClient.h  # Add ROS client header
    ImageViewerWidget.h    # Add Image viewer header
    RosImageClient.h       # Add Image client header
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
endif()

# Platform specific OpenGL
if(NOT ANDROID)
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
endif()
