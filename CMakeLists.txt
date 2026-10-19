cmake_minimum_required(VERSION 3.20)
project(Roomy VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One self-contained exe: no Visual C++ runtime to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(roomy WIN32
    src/main.cpp
    src/ui.cpp
    src/scan.cpp
    src/clean.cpp
    src/fs.cpp
    src/format.cpp
    src/cli.cpp
    res/roomy.rc
)

target_compile_definitions(roomy PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(roomy PRIVATE d2d1 dwrite dwmapi shell32 ole32 bcrypt advapi32 user32 windowscodecs)

if(MSVC)
    target_compile_options(roomy PRIVATE /W4 /utf-8 /permissive- /EHsc)
    # The manifest comes from roomy.rc.
    target_link_options(roomy PRIVATE /MANIFEST:NO)
else()
    target_compile_options(roomy PRIVATE -Wall -Wextra -municode)
    target_link_options(roomy PRIVATE -municode -static -s)
endif()
