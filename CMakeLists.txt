cmake_minimum_required(VERSION 3.24)

project(Fillema VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(FILLEMA_BUILD_TESTS "Build the lightweight core tests" ON)

add_library(fillema_core STATIC
    src/json.cpp
    src/model.cpp
    src/ffmpeg.cpp
)

target_include_directories(fillema_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(fillema_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(fillema_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(fillema_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_executable(Fillema WIN32
        src/main.cpp
        src/app.cpp
        src/player.cpp
        src/win_process.cpp
        resources/fillema.rc
    )

    target_compile_features(Fillema PRIVATE cxx_std_20)
    target_compile_definitions(Fillema PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        FILLEMA_VERSION=L"0.1.0"
    )
    target_link_libraries(Fillema PRIVATE
        fillema_core
        comctl32
        comdlg32
        dwmapi
        gdi32
        mfplat
        mfplay
        mfuuid
        ole32
        propsys
        shell32
        shlwapi
        user32
        uxtheme
    )
    if(MSVC)
        target_compile_options(Fillema PRIVATE /W4 /permissive- /utf-8 /EHsc)
        target_link_options(Fillema PRIVATE /SUBSYSTEM:WINDOWS)
    endif()

    install(TARGETS Fillema RUNTIME DESTINATION .)
    install(FILES README.md LICENSE DESTINATION .)
endif()

if(FILLEMA_BUILD_TESTS)
    enable_testing()
    add_executable(fillema_core_tests tests/core_tests.cpp)
    target_link_libraries(fillema_core_tests PRIVATE fillema_core)
    target_compile_features(fillema_core_tests PRIVATE cxx_std_20)
    add_test(NAME fillema_core_tests COMMAND fillema_core_tests)
endif()
