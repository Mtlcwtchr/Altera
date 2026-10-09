# CLion Toolchain for Visual Studio 2022 x64
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0.26200)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Visual Studio 2022 compiler
set(MSVC_TOOLSET_VERSION 143)
set(CMAKE_CXX_COMPILER "cl.exe")
set(CMAKE_C_COMPILER "cl.exe")

# Enable ASM for proper Windows support
enable_language(ASM)
enable_language(RC)

# Windows specific settings
add_compile_definitions(
    WIN32
    _WINDOWS
    UNICODE
    _UNICODE
)

# Use Windows SDK
if(NOT DEFINED CMAKE_SYSTEM_VERSION)
    set(CMAKE_SYSTEM_VERSION 10.0.26100.0)
endif()

