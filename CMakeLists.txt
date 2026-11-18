cmake_minimum_required(VERSION 3.21)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment version")
endif()

project(OmniConvert VERSION 2.0.0 LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(MSVC)
    add_compile_options(/utf-8 /W3)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Core dependencies: Qt6 (Core, Gui, Widgets) + optional Qt6::Pdf + ZLIB
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt6 QUIET COMPONENTS Pdf)
find_package(ZLIB QUIET)

# Legacy Carbon AGL.framework is absent from modern macOS SDKs (macOS 26+) and unused by Qt6 Cocoa/Metal
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "-framework OpenGL")
endif()

if(NOT ZLIB_FOUND)
    find_package(Qt6 QUIET COMPONENTS ZlibPrivate)
    if(NOT TARGET Qt6::ZlibPrivate)
        include(FetchContent)
        FetchContent_Declare(
            zlib_fetched
            URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        )
        set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(zlib_fetched)
    endif()
endif()

set(OMNI_CORE_SOURCES
    src/core/types.cpp
    src/core/dual_pool_scheduler.cpp
    src/core/qt_scheduler_bridge.cpp
    src/infra/atomic_file_tx.cpp
    src/infra/portable_env.cpp
    src/infra/zip_archive.cpp
    src/infra/text_encoding.cpp
    src/engines/conversion_engine.cpp
    src/engines/image_engine.cpp
    src/engines/media_engine.cpp
    src/engines/novel_engine.cpp
    include/omni/core/qt_scheduler_bridge.hpp
)

if(APPLE)
    list(APPEND OMNI_CORE_SOURCES
        src/engines/image_imageio_mac.mm
        src/engines/pdf_reader_mac.mm
    )
    set_source_files_properties(
        src/engines/image_imageio_mac.mm
        src/engines/pdf_reader_mac.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
endif()

add_library(omni_core STATIC ${OMNI_CORE_SOURCES})

target_include_directories(omni_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(omni_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if(ZLIB_FOUND)
    target_link_libraries(omni_core PUBLIC ZLIB::ZLIB)
elseif(TARGET Qt6::ZlibPrivate)
    target_link_libraries(omni_core PUBLIC Qt6::ZlibPrivate)
elseif(TARGET zlibstatic)
    target_link_libraries(omni_core PUBLIC zlibstatic)
    target_include_directories(omni_core PUBLIC ${zlib_fetched_SOURCE_DIR} ${zlib_fetched_BINARY_DIR})
endif()

if(TARGET Qt6::Pdf)
    target_compile_definitions(omni_core PUBLIC OMNI_HAVE_QTPDF=1)
    target_link_libraries(omni_core PUBLIC Qt6::Pdf)
endif()

if(APPLE)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
    find_library(PDFKIT_FRAMEWORK PDFKit REQUIRED)
    find_library(ICONV_LIB iconv REQUIRED)

    target_link_libraries(omni_core PUBLIC
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
        ${IMAGEIO_FRAMEWORK}
        ${PDFKIT_FRAMEWORK}
        ${ICONV_LIB}
    )
elseif(UNIX AND NOT APPLE)
    find_library(ICONV_LIB iconv)
    if(ICONV_LIB)
        target_link_libraries(omni_core PUBLIC ${ICONV_LIB})
    endif()
endif()

# Main Desktop GUI Application
set(OMNI_UI_SOURCES
    src/main.cpp
    src/ui/theme.cpp
    src/ui/drop_zone_frame.cpp
    src/ui/main_window.cpp
    include/omni/ui/drop_zone_frame.hpp
    include/omni/ui/main_window.hpp
    resources/resources.qrc
)

if(APPLE)
    set(MACOS_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.icns")
    list(APPEND OMNI_UI_SOURCES "${MACOS_ICON_FILE}")
    set_source_files_properties("${MACOS_ICON_FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
elseif(WIN32)
    list(APPEND OMNI_UI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_win.rc")
endif()

qt_add_executable(OmniConvert
    MANUAL_FINALIZATION
    ${OMNI_UI_SOURCES}
)

target_link_libraries(OmniConvert PRIVATE omni_core)

if(APPLE)
    set_target_properties(OmniConvert PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE "app_icon.icns"
        OUTPUT_NAME "OmniConvert"
    )
elseif(WIN32)
    set_target_properties(OmniConvert PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "OmniConvert"
    )
endif()

qt_finalize_executable(OmniConvert)

# Automated Unit & Integration Test Suite
option(OMNI_BUILD_TESTS "Build OmniConvert core test suite" ON)
if(OMNI_BUILD_TESTS)
    enable_testing()
    add_executable(test_m1_core tests/test_m1_core.cpp)
    target_link_libraries(test_m1_core PRIVATE omni_core)
    add_test(NAME OmniConvertCoreSuite COMMAND test_m1_core)
endif()
