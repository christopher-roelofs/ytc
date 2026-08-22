cmake_minimum_required(VERSION 3.16)
project(ytnative CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)

# --- Core Innertube library (no GUI deps) ---
add_library(ytcore STATIC
  src/http.cpp
  src/innertube.cpp)
target_include_directories(ytcore PUBLIC ${CURL_INCLUDE_DIRS})
target_link_libraries(ytcore PUBLIC ${CURL_LIBRARIES})

# --- Milestone 1: resolver CLI (no SDL/mpv needed) ---
add_executable(yt_resolve src/main_resolve.cpp)
target_link_libraries(yt_resolve PRIVATE ytcore)

# --- Search CLI (no SDL/mpv needed) ---
add_executable(yt_search src/main_search.cpp)
target_link_libraries(yt_search PRIVATE ytcore)

# --- GLES2 UI (needs SDL2 + GLES2; no libmpv) ---
option(BUILD_UI "Build the SDL2 + GLES2 UI" ON)
if(BUILD_UI)
  pkg_check_modules(SDL2 sdl2)
  pkg_check_modules(GLES glesv2)
  if(SDL2_FOUND AND GLES_FOUND)
    add_library(ytgfx STATIC src/gfx.cpp src/ui.cpp src/player.cpp src/stream.cpp)
    target_include_directories(ytgfx PUBLIC ${SDL2_INCLUDE_DIRS} ${GLES_INCLUDE_DIRS})
    target_link_libraries(ytgfx PUBLIC ytcore ${SDL2_LIBRARIES} ${GLES_LIBRARIES} Threads::Threads)

    # Playback is a compile-time capability: real when libmpv is present,
    # otherwise a no-op stub so the UI still builds/iterates without mpv.
    pkg_check_modules(MPV mpv)
    if(MPV_FOUND)
      target_compile_definitions(ytgfx PUBLIC YTNATIVE_HAVE_MPV)
      target_include_directories(ytgfx PUBLIC ${MPV_INCLUDE_DIRS})
      target_link_libraries(ytgfx PUBLIC ${MPV_LIBRARIES})
      message(STATUS "libmpv found: yt_ui will have real playback")
    else()
      message(STATUS "libmpv NOT found: yt_ui playback is a stub")
    endif()

    add_executable(yt_ui src/main_ui.cpp)
    target_link_libraries(yt_ui PRIVATE ytgfx)
  else()
    message(WARNING "SDL2 or GLESv2 not found; skipping yt_ui")
  endif()
endif()

# --- Milestone 2: libmpv + SDL2 playback spike (optional) ---
option(BUILD_PLAYER "Build the SDL2 + libmpv playback spike" ON)
if(BUILD_PLAYER)
  pkg_check_modules(SDL2 sdl2)
  pkg_check_modules(MPV mpv)
  if(SDL2_FOUND AND MPV_FOUND)
    add_executable(yt_play src/main_play.cpp)
    target_include_directories(yt_play PRIVATE ${SDL2_INCLUDE_DIRS} ${MPV_INCLUDE_DIRS})
    target_link_libraries(yt_play PRIVATE ytcore ${SDL2_LIBRARIES} ${MPV_LIBRARIES})
  else()
    message(WARNING "SDL2 or libmpv not found; skipping yt_play")
  endif()
endif()
