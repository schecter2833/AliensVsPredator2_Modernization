cmake_minimum_required(VERSION 3.16)
project(AliensVsPredator2_Modernization C ASM)

# The game is 32-bit x86; every module we build must be too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build 32-bit: use -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake (or an MSYS2 MINGW32 shell).")
endif()
if(NOT MINGW)
  message(FATAL_ERROR "The trace renderer's thunks are GNU assembler; build with MinGW-w64 for now.")
endif()

set(CMAKE_C_STANDARD 99)
add_compile_options(-O2 -Wall -Wextra -march=i686 -mno-sse -mno-mmx)
add_link_options(-static-libgcc $<$<CONFIG:Release>:-s>)

# avp2m_trace.ren: logging proxy renderer
add_library(avp2m_trace SHARED
  src/trace/avp2m_trace.c
  src/trace/thunks.S
  src/trace/avp2m_trace.def)
set_target_properties(avp2m_trace PROPERTIES PREFIX "" SUFFIX ".ren")
target_link_libraries(avp2m_trace PRIVATE user32 kernel32)

# Tests: a fake stock renderer and a fake engine (run on Windows)
add_library(fake_renderer SHARED tests/trace/fake_renderer.c)
set_target_properties(fake_renderer PROPERTIES PREFIX "" OUTPUT_NAME "d3d" SUFFIX ".ren"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test)
add_executable(fake_engine tests/trace/fake_engine.c)
set_target_properties(fake_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test)
add_custom_command(TARGET avp2m_trace POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:avp2m_trace> ${CMAKE_BINARY_DIR}/test/)
