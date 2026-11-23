function(launcher_require_submodule path)
    if(NOT EXISTS ${LAUNCHER_3RDPARTY}/${path}/.git)
        message(FATAL_ERROR "3rdparty/${path} is missing. Run tools/bootstrap.ps1 once, or: git submodule update --init --recursive")
    endif()
endfunction()

launcher_require_submodule(json)
add_library(launcher_json INTERFACE)
target_include_directories(launcher_json SYSTEM INTERFACE ${LAUNCHER_3RDPARTY}/json/single_include)

launcher_require_submodule(zstd)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(${LAUNCHER_3RDPARTY}/zstd/build/cmake ${CMAKE_BINARY_DIR}/3rdparty/zstd EXCLUDE_FROM_ALL)

launcher_require_submodule(ZArchive)
set(LAUNCHER_ZARCHIVE ${LAUNCHER_3RDPARTY}/ZArchive)
add_library(launcher_zarchive STATIC EXCLUDE_FROM_ALL
    ${LAUNCHER_ZARCHIVE}/src/zarchivereader.cpp
    ${LAUNCHER_ZARCHIVE}/src/zarchivewriter.cpp
    ${LAUNCHER_ZARCHIVE}/src/sha_256.c
)
target_include_directories(launcher_zarchive SYSTEM PUBLIC ${LAUNCHER_ZARCHIVE}/include)
target_include_directories(launcher_zarchive PRIVATE ${LAUNCHER_3RDPARTY}/zstd/lib)
target_compile_features(launcher_zarchive PUBLIC cxx_std_20)
target_link_libraries(launcher_zarchive PRIVATE libzstd_static)

if(LAUNCHER_BUILD_APP)
    launcher_require_submodule(SDL)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${LAUNCHER_3RDPARTY}/SDL ${CMAKE_BINARY_DIR}/3rdparty/SDL EXCLUDE_FROM_ALL)

    launcher_require_submodule(imgui)
    set(LAUNCHER_IMGUI ${LAUNCHER_3RDPARTY}/imgui)
    add_library(launcher_imgui STATIC EXCLUDE_FROM_ALL
        ${LAUNCHER_IMGUI}/imgui.cpp
        ${LAUNCHER_IMGUI}/imgui_draw.cpp
        ${LAUNCHER_IMGUI}/imgui_tables.cpp
        ${LAUNCHER_IMGUI}/imgui_widgets.cpp
        ${LAUNCHER_IMGUI}/imgui_demo.cpp
        ${LAUNCHER_IMGUI}/backends/imgui_impl_sdl3.cpp
        ${LAUNCHER_IMGUI}/backends/imgui_impl_sdlrenderer3.cpp
        ${LAUNCHER_IMGUI}/misc/cpp/imgui_stdlib.cpp
    )
    target_include_directories(launcher_imgui SYSTEM PUBLIC ${LAUNCHER_IMGUI} ${LAUNCHER_IMGUI}/backends ${LAUNCHER_IMGUI}/misc/cpp)
    target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3-static)

    launcher_require_submodule(stb)
    add_library(launcher_stb INTERFACE)
    target_include_directories(launcher_stb SYSTEM INTERFACE ${LAUNCHER_3RDPARTY}/stb)
endif()
