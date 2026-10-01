cmake_minimum_required(VERSION 3.25)

project(CloneWarsNativePort LANGUAGES C CXX)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Xbox game code is 32-bit x86; configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CW_GAME_ROOT "${CMAKE_SOURCE_DIR}/../extracted_iso" CACHE PATH "Directory containing the extracted game files (default.xbe, data.zwp)")
set(CW_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")

foreach(config IN ITEMS "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${config} "${CW_OUTPUT_DIR}")
endforeach()

# C4100: kernel/HLE entry points must keep the Xbox ABI signature even when a parameter is unused.
add_compile_options(/W4 /permissive- /utf-8 /wd4100)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_executable(xbe2exe tools/xbe2exe/main.cpp)

# MinHook: function detours with relocated prologues (BSD-2-Clause).
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
)
FetchContent_MakeAvailable(minhook)
target_compile_options(minhook PRIVATE /W0)

add_library(cw_runtime SHARED
    runtime/audio/XboxAdpcm.cpp
    runtime/d3d/Device.cpp
    runtime/d3d/Textures.cpp
    runtime/d3d/PixelShader.cpp
    runtime/d3d/VertexShader.cpp
    runtime/Hle.cpp
    runtime/GameOptions.cpp
    runtime/HleAudio.cpp
    runtime/HleInput.cpp
    runtime/HleXapi.cpp
    runtime/Hooks.cpp
    runtime/Kernel.cpp
    runtime/KernelCrypto.cpp
    runtime/KernelFile.cpp
    runtime/KernelMemory.cpp
    runtime/KernelSync.cpp
    runtime/KernelSystem.cpp
    runtime/Log.cpp
    runtime/ModLoader.cpp
    runtime/Runtime.cpp
    runtime/Threads.cpp
    runtime/Trace.cpp
)
target_include_directories(cw_runtime PRIVATE runtime "${minhook_SOURCE_DIR}/src")
target_link_libraries(cw_runtime PRIVATE ntdll d3d9 d3dcompiler Xinput9_1_0 winmm xaudio2 ole32 minhook)
target_link_options(cw_runtime PRIVATE /SAFESEH:NO)

set(CW_GAME_EXE "${CW_OUTPUT_DIR}/clone_wars.exe")
add_custom_command(
    OUTPUT "${CW_GAME_EXE}"
    COMMAND xbe2exe "${CW_GAME_ROOT}/default.xbe" "${CW_GAME_EXE}"
    DEPENDS xbe2exe "${CW_GAME_ROOT}/default.xbe"
    COMMENT "Converting default.xbe into clone_wars.exe"
    VERBATIM
)
add_custom_target(clone_wars ALL DEPENDS "${CW_GAME_EXE}" cw_runtime)

file(WRITE "${CW_OUTPUT_DIR}/game_root.txt" "${CW_GAME_ROOT}\n")

# Plugins: every mods/<name>/plugin folder builds <name>.dll into bin/mods (tools/build_mod.py copies it into the mod).
file(GLOB CW_PLUGIN_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/../mods/*/plugin")
foreach(plugin_dir IN LISTS CW_PLUGIN_DIRS)
    get_filename_component(mod_dir "${plugin_dir}" DIRECTORY)
    get_filename_component(mod_name "${mod_dir}" NAME)
    file(GLOB plugin_sources CONFIGURE_DEPENDS "${plugin_dir}/*.cpp")
    add_library(mod_${mod_name} SHARED ${plugin_sources})
    target_include_directories(mod_${mod_name} PRIVATE sdk runtime)
    set_target_properties(mod_${mod_name} PROPERTIES OUTPUT_NAME "${mod_name}")
    foreach(config IN ITEMS "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
        set_target_properties(mod_${mod_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY${config} "${CW_OUTPUT_DIR}/mods")
    endforeach()
    add_dependencies(clone_wars mod_${mod_name})
endforeach()
