cmake_minimum_required(VERSION 3.21)
project(Onimusha_Recomp LANGUAGES CXX)

# Onimusha: Warlords (NTSC-U, SLUS-20018) on BozoEngine: the game's EE code recompiled by bozo-recomp, every library and
# kernel call answered natively by the engine's runtime. No PS2 hardware model.
# The ELF and the overlay modules come from the player's own disc (scripts/extract_iso.sh puts them in game/).

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(ONI_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "User-owned extracted disc files")
set(ONI_ISO "${CMAKE_SOURCE_DIR}/Onimusha - Warlords (USA).iso" CACHE FILEPATH "User-owned NTSC-U disc image")
set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "Pinned native engine source")
option(ONI_WINDOW "Build the SDL3 native game window (onimusha) next to oni-headless" ON)
set(BOZO_APP ${ONI_WINDOW} CACHE BOOL "" FORCE)
set(BOZO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
# FFmpeg: the backgrounds' IPU decoder (runtime/oni_bg.cpp) and the movies' MPEG-2 (BozoEngine's libmpeg). The minimal
# static build of scripts/build_ffmpeg.sh, handed to BozoEngine as BOZO_FFMPEG_ROOT.
set(ONI_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/build/ffmpeg" CACHE PATH "Minimal static FFmpeg (scripts/build_ffmpeg.sh)")
if(NOT EXISTS "${ONI_FFMPEG_ROOT}/lib/libavcodec.a")
  message(FATAL_ERROR "${ONI_FFMPEG_ROOT}: run scripts/build_ffmpeg.sh first (FFmpeg with the IPU decoder)")
endif()
set(BOZO_FFMPEG_ROOT "${ONI_FFMPEG_ROOT}" CACHE PATH "FFmpeg folder (include/, lib/) for libmpeg's decoder" FORCE)
set(BOZO_REQUIRE_FFMPEG ON CACHE BOOL "The movies need native decoding" FORCE)
add_subdirectory("${BOZO_ENGINE_DIR}" BozoEngine)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(oni_elf ${ONI_GAME_DIR}/SLUS_200.18)
# oni_prep: the prepared images and lists (tools/oni_prepare.py, the externals, the module registration);
# oni_gen: bozo-recomp's output only (it removes any other <prefix>_* file there).
set(oni_prep ${CMAKE_BINARY_DIR}/oni_prep)
set(oni_gen ${CMAKE_BINARY_DIR}/oni_gen)
file(MAKE_DIRECTORY ${oni_prep} ${oni_gen})

# The resident image and the overlay modules (enemy slots, rooms) are prepared at configure time: the list of modules
# comes from the disc, and every module is its own generated image. Re-run cmake after changing tools/oni_prepare.py.
if(NOT EXISTS ${oni_elf})
  message(FATAL_ERROR "${oni_elf} not found: run scripts/extract_iso.sh <iso> first")
endif()
execute_process(
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_prepare.py ${ONI_GAME_DIR} ${oni_prep}
  RESULT_VARIABLE oni_prepare_result OUTPUT_VARIABLE oni_prepare_output)
if(NOT oni_prepare_result EQUAL 0)
  message(FATAL_ERROR "tools/oni_prepare.py failed:\n${oni_prepare_output}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/tools/oni_prepare.py ${CMAKE_SOURCE_DIR}/tools/wrap_elf.py ${oni_elf})
include(${oni_prep}/oni_modules.cmake) # oni_modules: em000 em010 ... room530

file(GLOB oni_port_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/oni_*.cpp)

# The externals: library and kernel functions named by the ELF's own symbols become stubs the runtime answers; every
# address the port registers (tools/oni_overrides_list.py scans runtime/) becomes an override. The overlays share the
# stubs and overrides of the resident program (never an override inside an overlay slot: modules share addresses).
set(oni_externals ${oni_prep}/oni_externals.txt)
add_custom_command(OUTPUT ${oni_externals}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_overrides_list.py ${CMAKE_SOURCE_DIR}/runtime ${oni_prep}/oni_overrides.txt
  COMMAND bozo-externals ${oni_prep}/oni.elf ${oni_externals} --overrides ${oni_prep}/oni_overrides.txt
  DEPENDS bozo-externals ${oni_prep}/oni.elf ${CMAKE_SOURCE_DIR}/tools/oni_overrides_list.py ${oni_port_sources}
  COMMENT "Listing the externals of SLUS_200.18")
set(oni_overlay_externals ${oni_prep}/oni_overlay_externals.txt)
add_custom_command(OUTPUT ${oni_overlay_externals}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_overlay_externals.py ${oni_externals} ${oni_overlay_externals}
  DEPENDS ${oni_externals} ${CMAKE_SOURCE_DIR}/tools/oni_overlay_externals.py)

set(oni_sources)
set(oni_chunks 32)
foreach(image oni ${oni_modules})
  if(image STREQUAL "oni")
    set(chunks ${oni_chunks})
    set(symbol_prefix)
    set(image_externals ${oni_externals})
  else()
    set(chunks 1)
    set(symbol_prefix --symbol-prefix ${image}_)
    set(image_externals ${oni_overlay_externals})
  endif()
  set(files ${oni_gen}/${image}_functions.h ${oni_gen}/${image}_table.cpp)
  math(EXPR last "${chunks} - 1")
  foreach(i RANGE ${last})
    if(i LESS 10)
      list(APPEND files ${oni_gen}/${image}_00${i}.cpp)
    else()
      list(APPEND files ${oni_gen}/${image}_0${i}.cpp)
    endif()
  endforeach()
  add_custom_command(OUTPUT ${files}
    COMMAND bozo-recomp ${oni_prep}/${image}.elf ${oni_gen} --prefix ${image} ${symbol_prefix} --files ${chunks}
            --no-comments --externals ${image_externals} --report ${oni_prep}/${image}_report.txt
    DEPENDS bozo-recomp ${oni_prep}/${image}.elf ${image_externals}
    COMMENT "Recompiling ${image}")
  list(APPEND oni_sources ${files})
endforeach()

# The overlay registration: every module's tables, its identity and its address range (tools/oni_modules_gen.py).
set(oni_modules_cpp ${oni_prep}/modules.cpp)
add_custom_command(OUTPUT ${oni_modules_cpp}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_modules_gen.py ${oni_prep}/oni_modules.txt ${oni_prep} ${oni_modules_cpp}
  DEPENDS ${oni_prep}/oni_modules.txt ${CMAKE_SOURCE_DIR}/tools/oni_modules_gen.py
  COMMENT "Listing the overlay modules")
list(APPEND oni_sources ${oni_modules_cpp})
add_custom_target(oni_generate DEPENDS ${oni_sources})

add_library(oni_game STATIC ${oni_sources})
target_link_libraries(oni_game PUBLIC bozo_runtime_headers)
target_include_directories(oni_game PUBLIC ${oni_gen})
target_compile_options(oni_game PRIVATE -O1 -w -ffp-contract=off)

# The IOP sound modules, statically compiled from the player's disc (tools/oni_bozo_audio.py, docs/native-irx.md in
# BozoEngine).
set(oni_audio_gen ${CMAKE_BINARY_DIR}/oni_audio_gen)
set(oni_audio_names LIBSD SDRDRV MODMIDI MODHSYN MODMSIN SNDDRVCD)
set(oni_audio_outputs ${oni_audio_gen}/oni_audio_modules.h)
set(oni_audio_inputs)
set(oni_audio_code)
foreach(module ${oni_audio_names})
  string(TOLOWER "${module}" lower)
  list(APPEND oni_audio_outputs ${oni_audio_gen}/${lower}_iop.cpp)
  list(APPEND oni_audio_code ${oni_audio_gen}/${lower}_iop.cpp)
  list(APPEND oni_audio_inputs ${ONI_GAME_DIR}/IMAGE/MODULES/${module}.IRX)
endforeach()
add_custom_command(OUTPUT ${oni_audio_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_bozo_audio.py ${ONI_GAME_DIR} ${oni_audio_gen} ${BOZO_ENGINE_DIR}
  DEPENDS ${oni_audio_inputs} ${CMAKE_SOURCE_DIR}/tools/oni_bozo_audio.py
          ${BOZO_ENGINE_DIR}/tools/native_irx/relocate.py ${BOZO_ENGINE_DIR}/tools/native_irx/recompile.py
  COMMENT "Statically recompiling the IOP sound modules")
add_custom_target(oni_native_audio DEPENDS ${oni_audio_outputs})
add_library(oni_audio_code OBJECT ${oni_audio_code})
target_link_libraries(oni_audio_code PRIVATE bozo_runtime_headers)
target_compile_options(oni_audio_code PRIVATE -O1 -w)

# The VU0 microprogram (POLYCALC, pCalcPolygon's), translated to C++ from the player's ELF (tools/oni_vu0_gen.py).
set(oni_vu0_cpp ${CMAKE_BINARY_DIR}/oni_vu0_gen/oni_vu0.cpp)
add_custom_command(OUTPUT ${oni_vu0_cpp}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/oni_vu0_gen
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_vu0_gen.py ${oni_elf} ${oni_vu0_cpp}
  DEPENDS ${oni_elf} ${CMAKE_SOURCE_DIR}/tools/oni_vu0_gen.py
  COMMENT "Translating the VU0 microprogram")
add_library(oni_vu0_code OBJECT ${oni_vu0_cpp})
target_link_libraries(oni_vu0_code PRIVATE bozo_runtime_headers)
target_compile_options(oni_vu0_code PRIVATE -O2 -w -ffp-contract=off)

# The models' VU1 microprograms (the Vu1mc_* images), translated the same way (tools/oni_vu1_gen.py).
set(oni_vu1_cpp ${CMAKE_BINARY_DIR}/oni_vu1_gen/oni_vu1_programs.cpp)
add_custom_command(OUTPUT ${oni_vu1_cpp}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/oni_vu1_gen
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/oni_vu1_gen.py ${oni_elf} ${oni_vu1_cpp}
  DEPENDS ${oni_elf} ${CMAKE_SOURCE_DIR}/tools/oni_vu1_gen.py ${CMAKE_SOURCE_DIR}/tools/oni_vu0_gen.py
  COMMENT "Translating the VU1 microprograms")
add_library(oni_vu1_code OBJECT ${oni_vu1_cpp})
target_link_libraries(oni_vu1_code PRIVATE bozo_runtime_headers)
target_include_directories(oni_vu1_code PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_compile_options(oni_vu1_code PRIVATE -O2 -w -ffp-contract=off)

set(oni_runtime_sources ${oni_port_sources} $<TARGET_OBJECTS:oni_audio_code> $<TARGET_OBJECTS:oni_vu0_code>
    $<TARGET_OBJECTS:oni_vu1_code>)
list(FILTER oni_runtime_sources EXCLUDE REGEX "oni_main\\.cpp$")

add_executable(oni-headless runtime/oni_main.cpp ${oni_runtime_sources})
target_link_libraries(oni-headless PRIVATE oni_game bozo_host)
target_include_directories(oni-headless PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${oni_audio_gen} ${ONI_FFMPEG_ROOT}/include)
add_dependencies(oni-headless oni_native_audio)
target_compile_definitions(oni-headless PRIVATE ONI_DEFAULT_ELF="${oni_prep}/oni.elf" ONI_DEFAULT_ISO="${ONI_ISO}")
if(MINGW)
  target_link_options(oni-headless PRIVATE -static)
endif()

if(ONI_WINDOW)
  add_executable(onimusha runtime/oni_main.cpp ${oni_runtime_sources})
  target_link_libraries(onimusha PRIVATE oni_game bozo_host bozo_app)
  target_include_directories(onimusha PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${oni_audio_gen} ${ONI_FFMPEG_ROOT}/include)
  add_dependencies(onimusha oni_native_audio)
  if(WIN32)
    # The window's icon (docs/branding/make_logo.py oni), the UTF-8 manifest and the version information.
    enable_language(RC)
    set_source_files_properties(runtime/app/onimusha.rc PROPERTIES OBJECT_DEPENDS
      "${CMAKE_SOURCE_DIR}/runtime/app/onimusha.ico;${CMAKE_SOURCE_DIR}/runtime/app/onimusha.manifest")
    target_sources(onimusha PRIVATE runtime/app/onimusha.rc)
  endif()
  target_compile_definitions(onimusha PRIVATE ONI_WITH_WINDOW ONI_DEFAULT_ELF="${oni_prep}/oni.elf" ONI_DEFAULT_ISO="${ONI_ISO}")
  if(MINGW)
    get_filename_component(oni_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET onimusha POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:onimusha> -DSEARCH=${oni_toolchain_bin}
              -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake)
  endif()
endif()
