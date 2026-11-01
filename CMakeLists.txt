cmake_minimum_required(VERSION 3.16)
project(IC7300_IC9700Console LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # M_PI and friends
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd in the editor
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- everything below the window: no GUI, no audio, so the tests and the probe can use it ----
add_library(rigcore STATIC src/rig.cpp src/serial.cpp src/sim_rig.cpp src/store.cpp src/bands.cpp src/clock_sync.cpp src/decoder.cpp src/chart_overlay.cpp)
target_include_directories(rigcore PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(rigcore SYSTEM PRIVATE third_party/morse third_party/stb)

# The band plans and overlays go inside the executable too, and are written out the first time it runs.
function(embed_folder folder extension table output)
  file(GLOB files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${folder}/*.${extension})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${files})
  set(inc "")
  set(rows "")
  set(index 0)
  foreach(file ${files})
    get_filename_component(id ${file} NAME_WE)
    file(READ ${file} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(APPEND inc "static const unsigned char ${table}${index}[] = {${bytes}};\n")
    string(APPEND rows "  {\"${id}\", ${table}${index}, sizeof ${table}${index}},\n")
    math(EXPR index "${index} + 1")
  endforeach()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${output}
    "${inc}static const struct { const char* id; const unsigned char* data; size_t size; } ${table}[] = {\n${rows}};\n")
endfunction()
embed_folder(bandplans json kBuiltinPlans builtin_bandplans.inc)
embed_folder(stations json kBuiltinStations builtin_stations.inc)
embed_folder(overlays png kBuiltinOverlays builtin_overlays.inc)

find_package(Threads REQUIRED)
target_link_libraries(rigcore PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(rigcore PUBLIC setupapi shell32 ws2_32 ole32)
  target_compile_definitions(rigcore PUBLIC _USE_MATH_DEFINES)
endif()
if(MSVC)
  target_compile_options(rigcore PRIVATE /W3)
else()
  target_compile_options(rigcore PRIVATE -Wall -Wextra)
endif()

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE rigcore)
enable_testing()
add_test(NAME core COMMAND test_core)

# Read-only check of a real radio, for the console.
add_executable(ic7300probe tools/probe.cpp)
target_link_libraries(ic7300probe PRIVATE rigcore)

if(MINGW)
  target_link_options(test_core PRIVATE -static)
  target_link_options(ic7300probe PRIVATE -static)
endif()

# ---- the window ----
option(BUILD_GUI "Build the GUI" ON)
if(BUILD_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
  set(OpenGL_GL_PREFERENCE LEGACY)  # libGL.so.1 is on nearly every Linux desktop; libOpenGL.so.0 often isn't
  find_package(OpenGL REQUIRED)

  set(IMGUI third_party/imgui)
  add_library(imgui STATIC
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_glfw.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

  add_executable(ic7300console src/main.cpp src/app.cpp src/audio.cpp src/waterfall.cpp)
  target_include_directories(ic7300console PRIVATE third_party/miniaudio ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(ic7300console PRIVATE rigcore imgui ${CMAKE_DL_LIBS})

  # The fonts go inside the executable, so there's nothing to install beside it.
  function(embed_font input symbol output)
    file(READ ${input} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE ${output} "static const unsigned char ${symbol}[] = {${bytes}};\n")
  endfunction()
  embed_font(${CMAKE_CURRENT_SOURCE_DIR}/${IMGUI}/fonts/Roboto-Medium.ttf kRobotoMedium ${CMAKE_CURRENT_BINARY_DIR}/font_roboto.inc)
  embed_font(${CMAKE_CURRENT_SOURCE_DIR}/${IMGUI}/fonts/Cousine-Regular.ttf kCousine ${CMAKE_CURRENT_BINARY_DIR}/font_cousine.inc)

  if(WIN32)
    enable_language(RC)
    target_sources(ic7300console PRIVATE res/ic7300console.rc)
    set_target_properties(ic7300console PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(ic7300console PRIVATE ole32 winmm)
  endif()
  if(MINGW)
    target_link_options(ic7300console PRIVATE -static)
  endif()
  if(APPLE)
    target_link_libraries(ic7300console PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    target_compile_definitions(ic7300console PRIVATE GL_SILENCE_DEPRECATION)
    target_compile_definitions(imgui PRIVATE GL_SILENCE_DEPRECATION)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(ic7300console PRIVATE m)
  endif()
endif()
