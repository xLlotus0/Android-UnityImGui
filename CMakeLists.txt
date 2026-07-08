cmake_minimum_required(VERSION 3.22.1)

set(ANDROID_PLATFORM "android-25" CACHE STRING "" FORCE)
set(ANDROID_NATIVE_API_LEVEL 25 CACHE STRING "" FORCE)

project(Android-UnityImGui C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-fvisibility=hidden -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL)
endif()

file(GLOB_RECURSE COMMON_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/*.c
)
file(GLOB_RECURSE ASM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/*.s
)

set(IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_android.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
)

set(KITTY_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/KittyMemoryEx)
file(GLOB KITTY_SOURCES CONFIGURE_DEPENDS ${KITTY_INCLUDE_DIR}/*.cpp)

set(LIBRARY_SOURCES
    ${COMMON_SOURCES}
    ${IMGUI_SOURCES}
    ${KITTY_SOURCES}
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_SOURCE_DIR}/source/Foundation
    ${CMAKE_CURRENT_SOURCE_DIR}/../DFM/include/ImGui/font
    ${CMAKE_CURRENT_SOURCE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${KITTY_INCLUDE_DIR}
)

set(LIBS
    android
    log
    vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/external/Dobby/arm64-v8a/libdobby.a
)

add_library(${PROJECT_NAME}_asm OBJECT ${ASM_SOURCES})
set_target_properties(${PROJECT_NAME}_asm PROPERTIES EXPORT_COMPILE_COMMANDS OFF)

add_library(${PROJECT_NAME} SHARED
    ${LIBRARY_SOURCES}
    $<TARGET_OBJECTS:${PROJECT_NAME}_asm>
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    kUSE_LOGCAT
    kANDROID_LOG
    kANDROID_LOG_TAG="INJECT"
    kPROJECT_NAME="${PROJECT_NAME}"
)

target_link_libraries(${PROJECT_NAME} ${LIBS})
