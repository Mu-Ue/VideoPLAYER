# The tests, built apart from the player so its build stays as it is: test\build.bat, or
#   cmake -S test -B build\tests -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build\tests
cmake_minimum_required(VERSION 3.21)
project(VideoPLAYERTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # as the player
option(TEST_ASAN "Build with AddressSanitizer, which catches memory errors in the parsers" ON)

set(APP ${CMAKE_CURRENT_SOURCE_DIR}/../src CACHE PATH "The player's sources to test; another copy, to try the tests on changed ones")
# All of the player but its entry point and window, which test_window.cpp compiles itself to reach their insides.
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS ${APP}/*.cpp)
list(REMOVE_ITEM APP_SOURCES ${APP}/main.cpp ${APP}/ui/window.cpp)
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" TEST_DIR)
string(REPLACE "\\" "\\\\" TEST_DIR "${TEST_DIR}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/paths.h "#pragma once\n#define TEST_DIR L\"${TEST_DIR}\"\n")

add_executable(tests ${APP_SOURCES} ${TEST_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../res/app.manifest)
target_include_directories(tests PRIVATE ${APP} src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _MSVC_STL_HARDENING=1)
target_compile_options(tests PRIVATE /W4 /permissive- /utf-8 /sdl /EHsc /Zi)
target_link_options(tests PRIVATE /DEBUG /INCREMENTAL:NO)
if(TEST_ASAN)
    target_compile_options(tests PRIVATE /fsanitize=address)
    # Its runtime is a DLL, which the exe loads from its own folder.
    get_filename_component(CL_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    add_custom_command(TARGET tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       ${CL_DIR}/clang_rt.asan_dynamic-x86_64.dll $<TARGET_FILE_DIR:tests>)
endif()
target_link_libraries(tests PRIVATE kernel32 user32 gdi32 imm32 dwmapi shell32 comdlg32 ole32 oleaut32 uuid advapi32
                      mfplat mfreadwrite mfuuid avrt d3d11 dxgi dcomp d2d1 dwrite dxguid mmdevapi onecore_apiset)
