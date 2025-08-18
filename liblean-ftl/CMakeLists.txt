set(LIB lean-ftl)
add_library(${LIB} STATIC ${CMAKE_CURRENT_SOURCE_DIR}/source/ftl.c)

target_include_directories(${LIB}
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Compiler options
target_compile_options(${LIB} PRIVATE
    ${cpu_PARAMS}
    ${compiler_OPTS}
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:C>: >
    $<$<COMPILE_LANGUAGE:CXX>:

    # -Wno-volatile
    # -Wold-style-cast
    # -Wuseless-cast
    # -Wsuggest-override
    >
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp -MMD -MP>
    $<$<CONFIG:Debug>:-Og -g3 -ggdb>
    $<$<CONFIG:Release>:-Og -g0>
)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/git_version.cmake")
add_definitions( -DGIT_VERSION=${GIT_VERSION} )
add_definitions( -DVERSION_TIMESTAMP=${VERSION_TIMESTAMP} )
add_definitions( -DBUILD_TYPE=${CMAKE_BUILD_TYPE})

add_custom_command(
  #OUTPUT disassembly.asm
  TARGET lean-ftl
  POST_BUILD
  COMMAND ${CMAKE_AR} -x $<TARGET_FILE:lean-ftl>
  COMMAND ${CMAKE_OBJDUMP} -D --visualize-jumps *.${CMAKE_AR_O_EXT} > disassembly.asm
)