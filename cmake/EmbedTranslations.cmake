# Writes OUTPUT: the lang/*.po files given in FILES ("|"-separated) as byte arrays, so
# the translations are built into the launcher. Run by the build (cmake -P), with no
# tools beyond CMake.

string(REPLACE "|" ";" files "${FILES}")
set(arrays "")
set(entries "")
set(index 0)
foreach(file IN LISTS files)
    if(file STREQUAL "")
        continue()
    endif()
    get_filename_component(code "${file}" NAME_WE)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n" bytes "${bytes}")
    string(APPEND arrays "// ${code}.po\nconstexpr unsigned char Po${index}[] = {\n${bytes}0x00};\n\n")
    string(APPEND entries "    {\"${code}\", Po${index}, sizeof(Po${index}) - 1},\n")
    math(EXPR index "${index} + 1")
endforeach()
if(index EQUAL 0)
    # No translations: an empty table still has to be a valid array.
    set(arrays "constexpr unsigned char PoNone[] = {0x00};\n\n")
    set(entries "    {\"en\", PoNone, 0},\n")
endif()
set(content "// Made by cmake/EmbedTranslations.cmake from lang/*.po; don't edit.\n\n${arrays}constexpr EmbeddedPo EmbeddedTranslations[] = {\n${entries}};\n")
# Only rewrite it when it changed, so a configure alone doesn't rebuild.
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
    if(old STREQUAL content)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
