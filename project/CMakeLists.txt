cmake_minimum_required(VERSION 3.25)
project(black LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp" CACHE PATH "Existing toolkit")
option(BLACK_BUILD_COMPAT_HOST "Build bring-up host with XboxRecomp compatibility/emulation layers" OFF)
option(BLACK_BUILD_NATIVE_HOST "Build diagnostic AOT host with Win32 kernel and native GPU" ON)
file(GLOB BLACK_AOT CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT BLACK_AOT)
  message(FATAL_ERROR "Run generate-native.cmd before configuring")
endif()
if(BLACK_BUILD_NATIVE_HOST AND NOT BLACK_BUILD_COMPAT_HOST)
  add_subdirectory("${XBOXRECOMP_DIR}/src/platform" "${CMAKE_BINARY_DIR}/platform")
  add_subdirectory("${XBOXRECOMP_DIR}/src/kernel" "${CMAKE_BINARY_DIR}/kernel")
  get_target_property(BLACK_KERNEL_SOURCES xbox_kernel SOURCES)
  list(FILTER BLACK_KERNEL_SOURCES EXCLUDE REGEX "nv2a_.*\\.c$")
  set_property(TARGET xbox_kernel PROPERTY SOURCES ${BLACK_KERNEL_SOURCES})
  # Native DirectSound output (project/src/native_d3d/black_dsound.c) goes to the host through XAudio2.
  add_executable(black_native src/main.c src/native_hardware_boundary.c "${XBOXRECOMP_DIR}/src/apu/apu_xaudio2.c"
    "${XBOXRECOMP_DIR}/src/usb/usb_gamepad.c" "${XBOXRECOMP_DIR}/src/input/xinput_device.c")
  target_compile_definitions(black_native PRIVATE BLACK_NATIVE_HOST=1)
  target_include_directories(black_native PRIVATE src/recomp src/recomp/gen)
  target_link_libraries(black_native PRIVATE black_aot xbox_kernel xaudio2 ole32 xinput)
  # Symbol map for translating crash addresses back to guest functions (sub_XXXXXXXX).
  target_link_options(black_native PRIVATE /MAP)
endif()
# Native replacements of library functions (scripts/apply_native_overrides.py renames the translated ones to *_gen).
file(GLOB BLACK_NATIVE_D3D CONFIGURE_DEPENDS "src/native_d3d/*.c")
add_library(black_aot STATIC ${BLACK_AOT} src/integration.c src/black_timing.c src/native_gpu_calls.c ${BLACK_NATIVE_D3D})
# Preserve the generated clock as a helper without editing/regenerating AOT sources.
set_source_files_properties(src/recomp/gen/recomp_0023.c PROPERTIES
  COMPILE_DEFINITIONS "sub_00082210=black_animation_clock_original")
target_include_directories(black_aot PRIVATE src/recomp src/recomp/gen
  "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src/kernel")
target_compile_definitions(black_aot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# The default CPU archive has no NV2A/APU emulator library dependency.
# Native GPU/audio/kernel integration is still required by its external ABI.
if(BLACK_BUILD_COMPAT_HOST)
  add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")
  add_executable(black src/main.c)
  target_include_directories(black PRIVATE src/recomp src/recomp/gen "${XBOXRECOMP_DIR}/include")
  target_link_libraries(black PRIVATE black_aot xboxrecomp d3d11 dxgi dxguid xinput winmm dbghelp)
endif()
if(MSVC)
  target_compile_options(black_aot PRIVATE /bigobj /GS- /O1)
  target_compile_definitions(black_aot PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(black_native_gpu SHARED
  src/native_gpu/hle_d3d11.c src/native_gpu/hle_draw.c
  src/native_gpu/hle_vsh.c src/native_gpu/hle_native_present.c
  src/native_gpu/hle_native_geometry.c src/native_gpu/hle_native_quality.c src/native_gpu/black_gpu_bridge.c
  src/native_gpu/black_gpu_host.c src/native_gpu/hle_options.c src/native_gpu/hle_gpu_api.c
  src/native_gpu/black_mouse.c)
target_include_directories(black_native_gpu PRIVATE src/native_gpu
  "${XBOXRECOMP_DIR}/src/d3d")
target_compile_definitions(black_native_gpu PRIVATE BLACK_GPU_BUILD WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(black_native_gpu PRIVATE d3d11 dxgi dxguid d3dcompiler user32 gdi32 winmm xinput delayimp)
# Vulkan option: d3d11/dxgi are delay-loaded so hle_gpu_api_load() can bind DXVK (vulkan\ next to the exe) first.
target_link_options(black_native_gpu PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll)
set(BLACK_DXVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dxvk" CACHE PATH "DXVK Windows DLLs (d3d11.dll, dxgi.dll)")
if(EXISTS "${BLACK_DXVK_DIR}/d3d11.dll")
  add_custom_command(TARGET black_native_gpu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:black_native_gpu>/vulkan"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BLACK_DXVK_DIR}/d3d11.dll" "${BLACK_DXVK_DIR}/dxgi.dll"
            "${BLACK_DXVK_DIR}/LICENSE.dxvk.txt" "$<TARGET_FILE_DIR:black_native_gpu>/vulkan")
endif()
target_include_directories(black_aot PRIVATE src src/native_d3d)
target_link_libraries(black_aot PUBLIC black_native_gpu)
