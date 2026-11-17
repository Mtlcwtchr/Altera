# Native NGX integration is optional; other platforms keep the normal renderer.
option(ASR_ENABLE_DLSS "Enable NVIDIA DLSS in Terrain Lab (Windows x64 / D3D12)" ${WIN32})
set(ASR_DLSS_SDK_ROOT "" CACHE PATH "Existing NVIDIA DLSS SDK checkout")
set(ASR_DLSS_AVAILABLE OFF)
if(ASR_ENABLE_DLSS)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "ASR_ENABLE_DLSS requires Windows x64")
    endif()
    if(NOT ASR_DLSS_SDK_ROOT)
        if(EXISTS "${CMAKE_SOURCE_DIR}/.cache/dlss-sdk/include/nvsdk_ngx.h")
            set(ASR_DLSS_SDK_ROOT "${CMAKE_SOURCE_DIR}/.cache/dlss-sdk")
        else()
            FetchContent_Declare(nvidia_dlss
                GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
                GIT_TAG 374959484e79a640feaba44c93ac8cfb0a03f5b5
                GIT_SUBMODULES ""
                EXCLUDE_FROM_ALL)
            FetchContent_MakeAvailable(nvidia_dlss)
            set(ASR_DLSS_SDK_ROOT "${nvidia_dlss_SOURCE_DIR}")
        endif()
    endif()
    set(ASR_DLSS_LIB_DIR "${ASR_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64")
    set(ASR_DLSS_RUNTIME "${ASR_DLSS_SDK_ROOT}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
    foreach(required include/nvsdk_ngx.h lib/Windows_x86_64/x64/nvsdk_ngx_d.lib lib/Windows_x86_64/rel/nvngx_dlss.dll)
        if(NOT EXISTS "${ASR_DLSS_SDK_ROOT}/${required}")
            message(FATAL_ERROR "Incomplete DLSS SDK: missing ${required}")
        endif()
    endforeach()

    # SDL_GPU deliberately hides native handles. Compile an altered copy of
    # the pinned backend with resource tracking and native NGX access.
    get_target_property(sdl_sources SDL3-static SOURCES)
    set(sdl_d3d12_found OFF)
    foreach(source IN LISTS sdl_sources)
        if(source MATCHES "[/\\]gpu[/\\]d3d12[/\\]SDL_gpu_d3d12\\.c$")
            set(sdl_d3d12_found ON)
            list(REMOVE_ITEM sdl_sources "${source}")
        endif()
    endforeach()
    if(NOT sdl_d3d12_found)
        message(FATAL_ERROR "DLSS interop requires SDL's D3D12 backend")
    endif()
    set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/campfire_sdl_d3d12.c")
    file(READ "${sdl3_SOURCE_DIR}/src/gpu/d3d12/SDL_gpu_d3d12.c" sdl_d3d12_source)
    # SDL 3.2.8 leaves PlaneSlice uninitialized on array UAVs. Terrain's
    # writable clipmap array otherwise removes the D3D12 device on creation.
    string(REPLACE "uavDesc.Texture2DArray.ArraySize = 1;"
        "uavDesc.Texture2DArray.ArraySize = 1;\n                    uavDesc.Texture2DArray.PlaneSlice = 0;"
        sdl_d3d12_source "${sdl_d3d12_source}")
    foreach(header ../../core/windows/SDL_windows.h ../../video/directx/SDL_d3d12.h ../SDL_sysgpu.h)
        string(REPLACE "\"${header}\"" "\"${sdl3_SOURCE_DIR}/src/gpu/d3d12/${header}\""
            sdl_d3d12_source "${sdl_d3d12_source}")
    endforeach()
    target_include_directories(SDL3-static PRIVATE "${sdl3_SOURCE_DIR}/src/gpu/d3d12")
    file(CONFIGURE OUTPUT "${wrapper}" CONTENT
        "/* Altered SDL: Campfire NGX interop and array UAV PlaneSlice fix. */\n${sdl_d3d12_source}\n#include \"${CMAKE_SOURCE_DIR}/src/engine/render/sdl_d3d12_interop.inl\"\n" @ONLY)
    set_property(TARGET SDL3-static PROPERTY SOURCES "${sdl_sources};${wrapper}")
    set(ASR_DLSS_AVAILABLE ON)
endif()

function(altera_configure_dlss library executable)
    target_sources(${library} PRIVATE src/terrain_core/dlss.cpp)
    target_compile_definitions(${library} PUBLIC ASR_HAS_DLSS=$<BOOL:${ASR_DLSS_AVAILABLE}>)
    if(ASR_DLSS_AVAILABLE)
        target_include_directories(${library} PRIVATE "${ASR_DLSS_SDK_ROOT}/include")
        target_link_libraries(${library} PRIVATE
            "$<IF:$<CONFIG:Debug>,${ASR_DLSS_LIB_DIR}/nvsdk_ngx_d_dbg.lib,${ASR_DLSS_LIB_DIR}/nvsdk_ngx_d.lib>")
        altera_copy_dlss_runtime(${executable})
    endif()
endfunction()

function(altera_copy_dlss_runtime executable)
    add_custom_command(TARGET ${executable} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASR_DLSS_RUNTIME}" "$<TARGET_FILE_DIR:${executable}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASR_DLSS_SDK_ROOT}/LICENSE.txt" "$<TARGET_FILE_DIR:${executable}>/NVIDIA-DLSS-LICENSE.txt"
        VERBATIM)
endfunction()
