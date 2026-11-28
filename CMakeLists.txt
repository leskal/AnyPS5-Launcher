cmake_minimum_required(VERSION 3.25)

project(AnyPS5Launcher VERSION 0.1.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LAUNCHER_BUILD_APP "Build the SDL3 + Dear ImGui launcher" ON)
option(LAUNCHER_BUILD_TESTS "Build the core library tests" ON)

set(LAUNCHER_3RDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
include(cmake/Dependencies.cmake)

add_library(launcher_core STATIC
    src/core/AnyPS5.cpp
    src/core/Aes.cpp
    src/core/Archive.cpp
    src/core/Encryption.cpp
    src/core/Executable.cpp
    src/core/Diagnosis.cpp
    src/core/FileOps.cpp
    src/core/GameSettings.cpp
    src/core/Http.cpp
    src/core/Install.cpp
    src/core/Launch.cpp
    src/core/LaunchLinks.cpp
    src/core/Links.cpp
    src/core/Metadata.cpp
    src/core/Modules.cpp
    src/core/Package.cpp
    src/core/InputMapping.cpp
    src/core/Trophies.cpp
    src/core/PatchInfo.cpp
    src/core/Playtime.cpp
    src/core/I18n.cpp
    src/core/Profiles.cpp
    src/core/SaveBackup.cpp
    src/core/SaveSync.cpp
    src/core/SelfUpdate.cpp
    src/core/Scanner.cpp
    src/core/Session.cpp
    src/core/SelfUnwrap.cpp
    src/core/Settings.cpp
    src/core/Unwrap.cpp
    src/core/Versions.cpp
    src/core/WinShim.cpp
    src/core/ZipStore.cpp
)
target_include_directories(launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The translations: every lang/*.po is built in (see src/core/I18n.hpp).
file(GLOB LAUNCHER_TRANSLATIONS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lang/*.po)
list(SORT LAUNCHER_TRANSLATIONS)
string(REPLACE ";" "|" LAUNCHER_TRANSLATION_LIST "${LAUNCHER_TRANSLATIONS}")
set(LAUNCHER_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${LAUNCHER_GENERATED}/LauncherTranslations.inc
    COMMAND ${CMAKE_COMMAND} "-DFILES=${LAUNCHER_TRANSLATION_LIST}" -DOUTPUT=${LAUNCHER_GENERATED}/LauncherTranslations.inc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedTranslations.cmake
    DEPENDS ${LAUNCHER_TRANSLATIONS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedTranslations.cmake
    COMMENT "Building in the translations"
    VERBATIM
)
target_sources(launcher_core PRIVATE ${LAUNCHER_GENERATED}/LauncherTranslations.inc)
target_include_directories(launcher_core PRIVATE ${LAUNCHER_GENERATED})
target_link_libraries(launcher_core PUBLIC launcher_zarchive launcher_json)
target_include_directories(launcher_core PRIVATE ${LAUNCHER_3RDPARTY}/ZArchive/src)
if(WIN32)
    target_link_libraries(launcher_core PUBLIC shell32 winhttp advapi32)
endif()
target_compile_options(launcher_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion>
)

if(LAUNCHER_BUILD_APP)
    add_executable(anyps5-launcher WIN32
        src/app/Main.cpp
        src/app/FolderPicker.cpp
        src/app/IconCache.cpp
        src/app/Processes.cpp
    )
    target_link_libraries(anyps5-launcher PRIVATE launcher_core launcher_imgui launcher_stb)
    target_compile_definitions(anyps5-launcher PRIVATE LAUNCHER_VERSION="${PROJECT_VERSION}")
    if(MINGW)
        target_link_options(anyps5-launcher PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

# `cmake --build --preset release --target update-texts` rewrites lang/launcher.pot
# from the T("...") and N("...") texts in src.
add_executable(update_texts EXCLUDE_FROM_ALL tools/texts/UpdateTexts.cpp tools/texts/TextScan.cpp)
if(MINGW)
    target_link_options(update_texts PRIVATE -static)
endif()
add_custom_target(update-texts
    COMMAND update_texts ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS update_texts
    COMMENT "Updating lang/launcher.pot"
    VERBATIM
)

if(LAUNCHER_BUILD_TESTS)
    enable_testing()
    add_executable(launcher_tests
        tests/Main.cpp
        tests/AnyPS5Tests.cpp
        tests/ArchiveTests.cpp
        tests/ExecutableTests.cpp
        tests/MetadataTests.cpp
        tests/PatchInfoTests.cpp
        tests/TrophyInputTests.cpp
        tests/FeatureTests.cpp
        tests/I18nTests.cpp
        tests/ScannerTests.cpp
        tests/SettingsTests.cpp
        tests/UnwrapTests.cpp
        tests/VersionsTests.cpp
    )
    target_link_libraries(launcher_tests PRIVATE launcher_core)
    target_include_directories(launcher_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${LAUNCHER_3RDPARTY}/ZArchive/src)
    if(MINGW)
        target_link_options(launcher_tests PRIVATE -static)
    endif()
    target_sources(launcher_tests PRIVATE tools/texts/TextScan.cpp)
    target_include_directories(launcher_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/texts)
    target_compile_definitions(launcher_tests PRIVATE LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME launcher_tests COMMAND launcher_tests)

    add_executable(unwrap_self_cli EXCLUDE_FROM_ALL tests/tools/UnwrapSelfCli.cpp)
    target_link_libraries(unwrap_self_cli PRIVATE launcher_core)
endif()
