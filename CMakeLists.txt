cmake_minimum_required(VERSION 3.20)

project(Fillema VERSION 0.1.0 LANGUAGES CXX)

add_executable(Fillema WIN32 src/main.cpp)
target_compile_features(Fillema PRIVATE cxx_std_17)
target_compile_definitions(Fillema PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

if(MSVC)
    target_compile_options(Fillema PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(Fillema PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

target_link_libraries(Fillema PRIVATE user32 gdi32 comdlg32 shell32)
set_target_properties(Fillema PROPERTIES OUTPUT_NAME "Fillema")
