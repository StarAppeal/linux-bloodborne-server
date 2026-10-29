# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# bbport (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
# Without the system package (Arch: robin-map), the emulator's submodule.
find_package(tsl-robin-map QUIET)
if (NOT tsl-robin-map_FOUND)
    add_subdirectory(../../emulator/externals/robin-map ${CMAKE_CURRENT_BINARY_DIR}/robin-map
                     EXCLUDE_FROM_ALL)
endif()
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package; Arch has it
    # only in the AUR, so the emulator's submodule (emulator/externals/vma) is the last resort.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma
              HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../emulator/externals/vma/include REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
# Without the system package (Arch: zydis), the emulator's submodule (with zycore).
find_package(Zydis QUIET)
if (NOT Zydis_FOUND)
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(../../emulator/externals/zydis ${CMAKE_CURRENT_BINARY_DIR}/zydis
                     EXCLUDE_FROM_ALL)
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(hdr_output.frag hdr_output_frag)
add_host_shader(hdr_exposure.comp hdr_exposure_comp)
add_host_shader(hdr_gain.comp hdr_gain_comp)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(occlusion_resolve.comp occlusion_resolve_comp)
add_host_shader(gpu_timestamp.comp gpu_timestamp_comp)
add_host_shader(indirect_guard.comp indirect_guard_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
add_host_shader(screen_lighting.comp screen_lighting_trace_comp)
add_host_shader(screen_lighting.comp screen_lighting_trace_rt_comp RAY_QUERY=1)
add_host_shader(screen_lighting.comp screen_lighting_apply_comp APPLY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
# The port's own shaders (shim/shaders), into the same headers' folder.
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shim/shaders)
function(add_port_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
# Kaniel 2026-10-19: the map screen's live 3D view (shim/bbport_map_3d.cpp).
add_port_shader(map_3d.vert map_3d_vert)
add_port_shader(map_3d.frag map_3d_frag)
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/slot_vector.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
if (WIN32)
    # Linux-only (mprotect/SIGTRAP hooks): shim/bbport_win_stubs.cpp answers their interfaces.
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "shim/bbport_(free_check|gnm_hooks)\\.cpp$")
endif()
# The layer's own modules (independent of the shadPS4 code): layer/.
file(GLOB LAYER_SOURCES CONFIGURE_DEPENDS layer/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for bb-probe.exe to resolve, so the
# library is static there and linked into the executable.
if (WIN32)
    set(BBGPU_KIND STATIC)
else()
    set(BBGPU_KIND SHARED)
endif()
# Game profiles (what the translator knows about one game, out of its core): games/.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS games/*.cpp)
# The translator's own command processor (docs/TRANSLATOR_ARCHITECTURE.ru.md, step 2): cp/.
file(GLOB CP_SOURCES CONFIGURE_DEPENDS cp/*.cpp)
add_library(bbgpu ${BBGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES} ${GAME_SOURCES} ${CP_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
# bbport co-op: PS4 network and PSN libraries with the shadNet client (shadp2p,
# github.com/Wozzardman/shadp2p, GPL-2.0) and the seamless co-op code, all from the emulator in
# this repository (../../emulator/src), for private Bloodborne co-op servers. Its headers come
# last: this port's own shadPS4 copy (shadps4/) and shims (shim/) go first.
set(BB_EMULATOR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../emulator/src)
find_package(Protobuf CONFIG QUIET)
if (NOT Protobuf_FOUND)
    # Debian and Ubuntu ship protobuf without its CMake package; CMake's FindProtobuf module
    # provides the same protobuf::libprotobuf and protobuf::protoc targets.
    find_package(Protobuf REQUIRED)
endif()
set(SHADNET_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT}")
# shadNet's own files, one copy for both sides: its messages, and ours with it (the co-op world).
set(BB_SERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../server)
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT}/shadnet.pb.cc" "${SHADNET_PROTO_OUT}/shadnet.pb.h"
    COMMAND protobuf::protoc "--proto_path=${BB_SERVER_SRC}/src"
            "--cpp_out=${SHADNET_PROTO_OUT}" "${BB_SERVER_SRC}/src/shadnet.proto"
    DEPENDS "${BB_SERVER_SRC}/src/shadnet.proto")
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT}/coop.pb.cc" "${SHADNET_PROTO_OUT}/coop.pb.h"
    COMMAND protobuf::protoc "--proto_path=${BB_SERVER_SRC}/coop"
            "--cpp_out=${SHADNET_PROTO_OUT}" "${BB_SERVER_SRC}/coop/coop.proto"
    DEPENDS "${BB_SERVER_SRC}/coop/coop.proto")
file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS ${BB_EMULATOR_SRC}/core/libraries/network/*.cpp
    ${BB_EMULATOR_SRC}/core/libraries/np/*.cpp ${BB_EMULATOR_SRC}/shadnet/*.cpp)
# Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
# emulator's UI layers.
list(FILTER BBNET_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
# Seamless co-op: the emulator's code unchanged, with this port's hook installer and the services
# it needs (bbcoop/, called through bbcoop.h).
file(GLOB BBCOOP_SOURCES CONFIGURE_DEPENDS bbcoop/*.cpp)
set(BBCOOP_SOURCES ${BBCOOP_SOURCES}
    ${BB_EMULATOR_SRC}/core/bloodborne_re.cpp ${BB_EMULATOR_SRC}/core/bloodborne_party.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_patches.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_join.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_travel.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_flags_store.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_kills_hp.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_party_rules.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_party_death.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_portrait.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_altar.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_compass.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_pings.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_host_rules.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_host_rules_game.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_pings_game.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_duel.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_duel_game.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_spectate.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_camera.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_map.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_map_screen.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_boards.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_world_log.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_server_world.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_team.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_invasion.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_trade.cpp
    ${BB_SERVER_SRC}/coop/world_log.cpp ${BB_SERVER_SRC}/coop/coop_messages.cpp
    ${BB_SERVER_SRC}/coop/trade.cpp
    ${BB_SERVER_SRC}/coop/link_crypto.cpp
    ${BB_SERVER_SRC}/coop/duel.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_autoplay.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_trace_diag.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_re_tracer.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_crash_dump.cpp
    ${BB_EMULATOR_SRC}/core/bloodborne_write_watch.cpp ${BB_EMULATOR_SRC}/core/guest_hook.cpp
    ${BB_EMULATOR_SRC}/core/file_format/psf.cpp)
# The build's revision for the log, refreshed at every build (bbcoop/revision.cmake).
add_custom_target(bbcoop_revision
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/include/bbcoop_revision.h
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/bbcoop/revision.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/include/bbcoop_revision.h)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(bbnet STATIC ${BBNET_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc"
    "${SHADNET_PROTO_OUT}/coop.pb.cc" bbnet/bbnet_glue.cpp
    ${BBCOOP_SOURCES})
add_dependencies(bbnet bbcoop_revision)
target_include_directories(bbnet PRIVATE shim shadps4 . "${SHADNET_PROTO_OUT}" ${SHADER_INCLUDE}
    third_party/imgui third_party/httplib third_party/xbyak ${BB_EMULATOR_SRC}
    ${BB_SERVER_SRC})
target_link_libraries(bbnet PRIVATE nlohmann_json::nlohmann_json)
# Party death is always on in the port (it has the party HUD; no setting turns it off); the
# emulator's stays a setting, off by default.
target_compile_definitions(bbnet PRIVATE BLOODBORNE_PARTY_DEATH_ALWAYS=1)
# The port's HUD is patched at its start (scripts/hud_patch.py): party death shows its downed banner.
target_compile_definitions(bbnet PRIVATE BLOODBORNE_PARTY_HUD=1)
# Co-op on our own networking (docs/SERVER_COOP_PLAN.md) is built for the port only.
target_compile_definitions(bbnet PRIVATE BLOODBORNE_NATIVE_PORT=1)
# HTTPS (the community game server only speaks TLS) and gzip replies in cpp-httplib.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_compile_definitions(bbnet PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_link_libraries(bbnet PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
if (WIN32)
    target_sources(bbnet PRIVATE third_party/wepoll/wepoll.c)
    target_include_directories(bbnet PRIVATE third_party/wepoll)
endif()
target_compile_definitions(bbnet PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbnet PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbnet PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    protobuf::libprotobuf)
if (WIN32)
    target_compile_definitions(bbnet PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbnet PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h>)
    target_link_libraries(bbnet PUBLIC ws2_32 iphlpapi bcrypt crypt32 dbghelp)
endif()
target_link_libraries(bbnet PUBLIC Zydis::Zydis)
target_link_libraries(bbgpu PUBLIC bbnet)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
# Dear ImGui stays private to libbbgpu.so. The library is linked into bb-probe, so its exports are
# in the global symbol scope; a Vulkan layer carrying its own Dear ImGui without -Bsymbolic
# (vkShade 0.1) had its ImGui_ImplVulkan_Init bound to this copy, which called the Vulkan loader
# with the layer's down-chain physical device handle ("Invalid physicalDevice", abort).
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf"
    BB_SERIF_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSerif.ttf"
    BB_PARTY_ICONS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/art/party_icons.bin"
    BB_COOP_ART_PATH="${CMAKE_CURRENT_SOURCE_DIR}/art/coop_art.bin")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSerif.ttf;${CMAKE_CURRENT_SOURCE_DIR}/art/party_icons.bin;${CMAKE_CURRENT_SOURCE_DIR}/art/coop_art.bin")
target_include_directories(bbgpu PRIVATE shim layer games cp shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include
    ${BB_EMULATOR_SRC})
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Loaded with bb-probe at startup (DT_NEEDED, never dlopen): thread_local access without a
# TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
if (NOT WIN32)
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
endif()
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
if (NOT WIN32)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
# xbyak (header-only) from the submodule everywhere: MSYS2 and Arch have no package (AUR only).
target_include_directories(bbgpu PRIVATE third_party/xbyak)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(bbgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h)
    # <sys/uio.h> (process_vm_readv), <dlfcn.h> and <execinfo.h> for the diagnostics.
    target_include_directories(bbgpu PRIVATE shim/win_posix)
    target_link_libraries(bbgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm)
else()
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
endif()

# bbport (Windows): bbgpu is a static library there, so the loader and the HLE runtime are
# linked here, where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32)
    enable_language(RC)
    file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
    file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${BB_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(bb-probe ../src/probe.c ${BB_RUNTIME} ../src/compat_win.c ../src/vulkan_smoke.c
        ../packaging/windows/bb-probe.rc)
    set_target_properties(bb-probe PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(bb-probe PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
    target_include_directories(bb-probe PRIVATE .. ../src)
    target_link_libraries(bb-probe PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(bb-probe PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    add_executable(bb-gpu-capabilities ../tools/gpu_capabilities.c ../packaging/windows/bb-gpu-capabilities.rc)
    target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan PkgConfig::SDL3)
    # bbport_dlss.dll on this PC's GPU without the game (tools/dlss_bridge_check.c).
    add_executable(dlss-bridge-check EXCLUDE_FROM_ALL ../tools/dlss_bridge_check.c)
    target_link_libraries(dlss-bridge-check PRIVATE Vulkan::Vulkan)
    # Bloodborne.exe (bb-play): the game with the launcher's saved settings, no launcher window.
    add_executable(bb-play WIN32 ../packaging/windows/play.c ../packaging/windows/play.rc)
    target_compile_options(bb-play PRIVATE -municode)
    target_link_options(bb-play PRIVATE -municode)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
if (NOT WIN32)
    # libbbgpu.so leaves the runtime's functions (runtime_*) to bb-probe, which exports them
    # (-rdynamic); the tests and tools below link the library without bb-probe and never call
    # them. On Windows bbgpu is static, so only the objects a test uses are linked at all.
    add_link_options(-Wl,--allow-shlib-undefined)
endif()
if (WIN32)
    add_executable(file-windows-test EXCLUDE_FROM_ALL ../tests/test_file_windows.c ../src/compat_win.c)
    set_target_properties(file-windows-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(file-windows-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(file-windows-test PRIVATE .. ../src)
    target_link_libraries(file-windows-test PRIVATE bcrypt -pthread)
    # The runtime's direct memory chunks for the PC memory model (no GPU: a recording allocator).
    add_executable(memory-chunks-test EXCLUDE_FROM_ALL ../tests/test_memory_chunks.c ../src/compat_win.c)
    set_target_properties(memory-chunks-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(memory-chunks-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(memory-chunks-test PRIVATE .. ../src)
    target_link_libraries(memory-chunks-test PRIVATE onecore bcrypt -pthread)
    target_link_options(memory-chunks-test PRIVATE -Wl,--disable-high-entropy-va)
    # Save data metadata replaced whole (rename over an existing file).
    add_executable(savedata-replace-test EXCLUDE_FROM_ALL ../tests/test_savedata_replace.c ../src/compat_win.c)
    set_target_properties(savedata-replace-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(savedata-replace-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(savedata-replace-test PRIVATE .. ../src)
    target_link_libraries(savedata-replace-test PRIVATE bcrypt -pthread)
    # Whether the GPU imports the runtime's section views (the PC memory model's host import).
    # Needs a Vulkan device, so it is run by hand, not by build-bbport -Test.
    add_executable(host-import-test EXCLUDE_FROM_ALL ../tests/test_host_import_windows.c)
    set_target_properties(host-import-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(host-import-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(host-import-test PRIVATE Vulkan::Vulkan onecore)
    # Save files written to a copy and replaced in one step. The manifest (UTF-8 code page) lets
    # it cover non-ASCII paths as the game runs.
    add_executable(save-commit-test EXCLUDE_FROM_ALL ../tests/test_save_commit_windows.c ../src/compat_win.c
        ../packaging/windows/bb-probe.rc)
    set_target_properties(save-commit-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(save-commit-test PRIVATE $<$<COMPILE_LANGUAGE:C>:-UNDEBUG -Wall -Wextra -Werror>)
    target_include_directories(save-commit-test PRIVATE .. ../src)
    target_link_libraries(save-commit-test PRIVATE bcrypt -pthread)
    # Stick neutral and dead zone on SDL virtual controllers (run from a scratch directory: it
    # writes two small files next to itself).
    add_executable(pad-calibration-test EXCLUDE_FROM_ALL ../tests/test_pad_calibration.c ../src/compat_win.c)
    set_target_properties(pad-calibration-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(pad-calibration-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(pad-calibration-test PRIVATE .. ../src)
    target_link_libraries(pad-calibration-test PRIVATE PkgConfig::SDL3 bcrypt -pthread)
    # The HLE runtime as bb-probe links it: thread handles within 40 bits, the guest's debug
    # exception stop (no exit handlers). No game, no Vulkan device.
    add_executable(runtime-windows-test EXCLUDE_FROM_ALL ../tests/test_runtime_windows.c ${BB_RUNTIME}
        ../src/compat_win.c)
    set_target_properties(runtime-windows-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(runtime-windows-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(runtime-windows-test PRIVATE .. ../src)
    target_link_libraries(runtime-windows-test PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
    target_link_options(runtime-windows-test PRIVATE -Wl,--disable-high-entropy-va)
    # Mouse inputs, wheel steps, the stick fallback and the camera hook's turn (no window, no game).
    add_executable(mouse-camera-test EXCLUDE_FROM_ALL ../tests/test_mouse_camera.c ../src/compat_win.c)
    set_target_properties(mouse-camera-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(mouse-camera-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(mouse-camera-test PRIVATE .. ../src)
    target_link_libraries(mouse-camera-test PRIVATE PkgConfig::SDL3 bcrypt -pthread)
endif()

add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(cache-consistency-test EXCLUDE_FROM_ALL ../tests/test_cache_consistency.cpp)
target_compile_options(cache-consistency-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(cache-consistency-test PRIVATE shadps4)
# The same checks plus the blobs on disk (Storage::DataBase), which uses the GPU library's file
# helpers and log: ninja -C out/gpu cache-storage-test && out/gpu/cache-storage-test.exe
add_executable(cache-storage-test EXCLUDE_FROM_ALL ../tests/test_cache_consistency.cpp)
target_compile_definitions(cache-storage-test PRIVATE BB_CACHE_STORAGE_TEST)
target_compile_options(cache-storage-test PRIVATE -UNDEBUG -fPIC)
target_include_directories(cache-storage-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(cache-storage-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(cache-storage-test PRIVATE onecore) # CreateFileMapping2, used by the library's memory code
endif()
add_executable(pm4-resync-test EXCLUDE_FROM_ALL ../tests/test_pm4_resync.cpp)
target_compile_options(pm4-resync-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pm4-resync-test PRIVATE ..)
add_executable(pm4-lod-stats-test EXCLUDE_FROM_ALL ../tests/test_pm4_lod_stats.cpp)
# pm4_cmds.h has static helpers the test does not call.
target_compile_options(pm4-lod-stats-test PRIVATE -UNDEBUG -Wall -Wextra -Werror
                       -Wno-unneeded-internal-declaration)
target_include_directories(pm4-lod-stats-test PRIVATE shim shadps4 third_party/gcn/include)
if (WIN32)
    target_link_libraries(pm4-lod-stats-test PRIVATE ws2_32) # the headers' inline socket setup
endif()
add_executable(texture-gc-test EXCLUDE_FROM_ALL ../tests/test_texture_gc.cpp)
target_compile_options(texture-gc-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(texture-gc-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
# bbport co-op: the shim's log (its file names the thread) for the tests below, with shadPS4's
# thread helpers built apart from the tests' -Werror.
add_library(test-log OBJECT EXCLUDE_FROM_ALL shim/common/logging/log.cpp shadps4/common/thread.cpp
    shadps4/common/error.cpp shadps4/common/string_util.cpp)
target_include_directories(test-log PUBLIC shim shadps4)
target_link_libraries(test-log PUBLIC fmt::fmt)
# bbport co-op: guest code hooks (bbcoop/guest_hooks.cpp) with the emulator's trampoline
# generator, run against small functions written by the test.
add_executable(coop-hooks-test EXCLUDE_FROM_ALL ../tests/test_coop_hooks.cpp bbcoop/guest_hooks.cpp
    ${BB_EMULATOR_SRC}/core/guest_hook.cpp shadps4/common/decoder.cpp)
target_include_directories(coop-hooks-test PRIVATE shim shadps4 third_party/xbyak ${BB_EMULATOR_SRC})
target_compile_options(coop-hooks-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(coop-hooks-test PRIVATE test-log Zydis::Zydis)
# The PC memory model's hooks in the game's code (bbport_guest_hooks.cpp), on functions the test
# writes at the hooked offsets: native detours, and int3 traps with the argument "traps".
add_executable(guest-hooks-test EXCLUDE_FROM_ALL ../tests/test_guest_hooks.cpp
    shim/bbport_guest_hooks.cpp shim/bbport_heap_sites.cpp)
target_include_directories(guest-hooks-test PRIVATE shim)
target_compile_options(guest-hooks-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if (WIN32)
    target_include_directories(guest-hooks-test PRIVATE shim/win_posix)
    target_compile_definitions(guest-hooks-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # The fake image at 0x800000000 must stay free, as in bb-probe.
    target_link_options(guest-hooks-test PRIVATE -Wl,--disable-high-entropy-va)
endif()
# bbport co-op: the log file the rig's tools read (BB_LOG_FILE), in shadPS4's line format.
add_executable(log-file-test EXCLUDE_FROM_ALL ../tests/test_log_file.cpp)
target_compile_options(log-file-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(log-file-test PRIVATE test-log)
# Buffer formats the renderer must take without stopping the game (a co-op player's crash).
add_executable(pixel-format-test EXCLUDE_FROM_ALL ../tests/test_pixel_format.cpp)
target_compile_options(pixel-format-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pixel-format-test PRIVATE shim shadps4)
target_link_libraries(pixel-format-test PRIVATE bbgpu)
# Vertex stream formats without dynamic vertex input (a player's RX 580 stopped on data format 15).
add_executable(vertex-formats-test EXCLUDE_FROM_ALL ../tests/test_vertex_formats.cpp)
target_compile_options(vertex-formats-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(vertex-formats-test PRIVATE shim shadps4)
target_link_libraries(vertex-formats-test PRIVATE bbgpu)
# Stencil-only depth target sizes (shadPS4 #5384).
add_executable(depth-stencil-test EXCLUDE_FROM_ALL ../tests/test_depth_stencil.cpp)
target_compile_options(depth-stencil-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(depth-stencil-test PRIVATE shim shadps4)
target_link_libraries(depth-stencil-test PRIVATE bbgpu)
if (NOT WIN32)
    # The shared library's runtime data (runtime_fault_recover, a TLS variable, and the toggles)
    # is bound when it loads, so these tests need the stand-ins the shader tests link; the static
    # Windows library only pulls in what a test uses.
    foreach(test cache-storage-test pixel-format-test vertex-formats-test depth-stencil-test)
        target_sources(${test} PRIVATE ../tests/test_gpu_runtime_stubs.cpp)
    endforeach()
endif()
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
# bbport co-op: our screens' key guides stay inside their panels (shim/bbport_key_guide.h).
add_executable(key-guide-test EXCLUDE_FROM_ALL ../tests/test_key_guide.cpp)
target_compile_options(key-guide-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(key-guide-test PRIVATE ..)
# bbport co-op: party death's HUD keeps clear of the game's own HUD (shim/bbport_party_layout.h).
add_executable(party-layout-test EXCLUDE_FROM_ALL ../tests/test_party_layout.cpp)
target_compile_options(party-layout-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-layout-test PRIVATE ..)
# bbport co-op: the party panel's HP bar from the game's own HUD art, read from its files
# (shim/bbport_game_art.h): BC7, the PS4's block order, a TPF in a DCX (made-up data).
# bbport co-op: party death's icons (art/party_icons.bin, drawn for the port) as the overlay embeds them.
add_executable(party-icons-test EXCLUDE_FROM_ALL ../tests/test_party_icons.cpp shim/bbport_party_icons.cpp)
target_compile_options(party-icons-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-icons-test PRIVATE ..)
target_compile_definitions(party-icons-test PRIVATE
    BB_PARTY_ICONS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/art/party_icons.bin")
target_link_libraries(party-icons-test PRIVATE miniz::miniz)
# bbport co-op: the co-op screens' own art (art/coop_art.bin, scripts/coop_art.py) as the overlay embeds it.
add_executable(coop-art-test EXCLUDE_FROM_ALL ../tests/test_coop_art.cpp shim/bbport_coop_art.cpp)
target_compile_options(coop-art-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(coop-art-test PRIVATE ..)
target_compile_definitions(coop-art-test PRIVATE
    BB_COOP_ART_PATH="${CMAKE_CURRENT_SOURCE_DIR}/art/coop_art.bin")
target_link_libraries(coop-art-test PRIVATE miniz::miniz)
# bbport co-op: the party portraits' textures (shim/bbport_portraits.h): placeholders by colour, mips.
add_executable(portraits-test EXCLUDE_FROM_ALL ../tests/test_portraits.cpp shim/bbport_portraits.cpp)
target_compile_options(portraits-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(portraits-test PRIVATE ..)
add_executable(game-art-test EXCLUDE_FROM_ALL ../tests/test_game_art.cpp shim/bbport_game_art.cpp)
target_compile_options(game-art-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(game-art-test PRIVATE ..)
target_link_libraries(game-art-test PRIVATE miniz::miniz)
add_executable(save-editor-test EXCLUDE_FROM_ALL ../tests/test_save_editor.cpp
               shim/bbport_save_editor_items.cpp)
target_compile_options(save-editor-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(save-editor-test PRIVATE .. shim)
target_link_libraries(save-editor-test PRIVATE nlohmann_json::nlohmann_json)
add_executable(cp-decoder-test EXCLUDE_FROM_ALL ../tests/test_cp_decoder.cpp cp/cp_decoder.cpp)
target_compile_options(cp-decoder-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(cp-decoder-test PRIVATE cp)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)
# bbport: the output resolutions the menus offer, borderless and 16:9 outputs (shim/bbport_output_sizes.h).
add_executable(output-sizes-test EXCLUDE_FROM_ALL ../tests/test_output_sizes.cpp)
target_compile_options(output-sizes-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(output-sizes-test PRIVATE ..)

# bbport co-op: trading's Give rows run for any item, weapons too (shim/bbport_trade_rows.h).
add_executable(trade-rows-test EXCLUDE_FROM_ALL ../tests/test_trade_rows.cpp shim/bbport_trade_rows.cpp)
target_compile_options(trade-rows-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(trade-rows-test PRIVATE .. shim)

# bbport: a gamepad button pressed in the Controls tab swaps with the input that had it
# (shim/bbport_pad_rebind.h).
add_executable(pad-rebind-test EXCLUDE_FROM_ALL ../tests/test_pad_rebind.cpp shim/bbport_pad_rebind.cpp)
target_compile_options(pad-rebind-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pad-rebind-test PRIVATE .. shim)

add_executable(spec-drop-test EXCLUDE_FROM_ALL ../tests/test_spec_drop.cpp)
target_compile_options(spec-drop-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(spec-drop-test PRIVATE shim)

add_executable(frame-slots-test EXCLUDE_FROM_ALL ../tests/test_frame_slots.cpp)
target_compile_options(frame-slots-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(frame-slots-test PRIVATE shim)

add_executable(async-claim-test EXCLUDE_FROM_ALL ../tests/test_async_claim.cpp)
target_compile_options(async-claim-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(async-claim-test PRIVATE shim)

# A shader table walker reading through a null pointer reads zeros for that walk only.
add_executable(srt-fault-test EXCLUDE_FROM_ALL ../tests/test_srt_fault.cpp)
target_compile_options(srt-fault-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(srt-fault-test PRIVATE shim)

# The push constant block the shaders read (header only: bbgpu's include directories, no link).
add_executable(push-data-test EXCLUDE_FROM_ALL ../tests/test_push_data.cpp)
target_compile_options(push-data-test PRIVATE -UNDEBUG)
target_include_directories(push-data-test PRIVATE $<TARGET_PROPERTY:bbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(push-data-test PRIVATE $<TARGET_PROPERTY:bbgpu,COMPILE_DEFINITIONS>)

add_executable(sendmsg-simm-test EXCLUDE_FROM_ALL ../tests/test_sendmsg_simm.cpp)
target_compile_options(sendmsg-simm-test PRIVATE -UNDEBUG)
target_include_directories(sendmsg-simm-test PRIVATE $<TARGET_PROPERTY:bbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(sendmsg-simm-test PRIVATE $<TARGET_PROPERTY:bbgpu,COMPILE_DEFINITIONS>)

add_executable(prompt-sets-test EXCLUDE_FROM_ALL ../tests/test_prompt_sets.cpp)
target_compile_options(prompt-sets-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(prompt-sets-test PRIVATE shim)

# The icons that follow the device in use, found again when the game loads its menu file anew.
add_executable(prompts-test EXCLUDE_FROM_ALL ../tests/test_prompts.cpp shim/bbport_prompts.cpp)
target_compile_options(prompts-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(prompts-test PRIVATE shim)
target_link_libraries(prompts-test PRIVATE PkgConfig::XXHASH)

add_executable(frame-gen-pace-test EXCLUDE_FROM_ALL ../tests/test_frame_gen_pace.cpp)
target_compile_options(frame-gen-pace-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(frame-gen-pace-test PRIVATE shim)

add_executable(hdr-highlight-test EXCLUDE_FROM_ALL ../tests/test_hdr_highlight.cpp)
target_compile_options(hdr-highlight-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(hdr-highlight-test PRIVATE shadps4/video_core/host_shaders)

add_executable(screen-lighting-test EXCLUDE_FROM_ALL ../tests/test_screen_lighting.cpp)
target_compile_options(screen-lighting-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(screen-lighting-test PRIVATE shadps4/video_core/host_shaders)

add_executable(flip-label-test EXCLUDE_FROM_ALL ../tests/test_flip_label.cpp)
target_compile_options(flip-label-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(flip-label-test PRIVATE shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)
# bbport (Windows): bb-probe gives bbgpu the loader's compat layer (bb_setjmp, compat_strcasestr),
# onecore (CreateFileMapping2) and winpthreads; the renderer tests link them themselves.
if(WIN32)
    foreach(test IN ITEMS motion-shader-test scene-resolution-test taa-shader-test camera-motion-test)
        target_sources(${test} PRIVATE ../src/compat_win.c)
        target_link_libraries(${test} PRIVATE onecore -pthread)
    endforeach()
endif()

add_executable(translation-memory-test EXCLUDE_FROM_ALL ../tests/test_translation_memory.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(translation-memory-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(translation-memory-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_translation_memory.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(translation-memory-test PRIVATE bbgpu)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)
# bbport co-op: the map screen's pictures of the areas from the game's map files (shim/bbport_map_art.h),
# on made-up MSB and FLVER data (and the game's Central Yharnam with BLOODBORNE_EBOOT).
add_executable(map-art-test EXCLUDE_FROM_ALL ../tests/test_map_art.cpp shim/bbport_map_art.cpp shim/bbport_map_collision.cpp
    shim/bbport_game_art.cpp)
target_compile_options(map-art-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(map-art-test PRIVATE ..)
target_link_libraries(map-art-test PRIVATE miniz::miniz)
add_executable(map-art-tool EXCLUDE_FROM_ALL tools/map_art_tool.cpp shim/bbport_map_art.cpp shim/bbport_map_collision.cpp
    shim/bbport_game_art.cpp)
target_link_libraries(map-art-tool PRIVATE miniz::miniz)
