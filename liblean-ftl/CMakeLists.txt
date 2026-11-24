set(LIB lean-ftl)
if(LFTL_ACCESSORS)
  add_library(${LIB} STATIC ${CMAKE_CURRENT_SOURCE_DIR}/source/ftl.c ${LFTL_ACCESSORS})
else()
  add_library(${LIB} STATIC ${CMAKE_CURRENT_SOURCE_DIR}/source/ftl.c)
endif(LFTL_ACCESSORS)

if(LFTL_ACCESSORS_DEFINES)
  #what the accessors are compiled with, and only them
  set_source_files_properties(${LFTL_ACCESSORS} PROPERTIES COMPILE_DEFINITIONS "${LFTL_ACCESSORS_DEFINES}")
endif()

target_include_directories(${LIB}
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LFTL_ACCESSORS_INCLUDES}
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
    $<$<CONFIG:Release>:-Os -g0>
)

if(LFTL_COVERAGE)
  target_compile_options(${LIB} PRIVATE --coverage)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/git_version.cmake")
add_definitions( -DGIT_VERSION=${GIT_VERSION} )
add_definitions( -DVERSION_TIMESTAMP=${VERSION_TIMESTAMP} )
add_definitions( -DBUILD_TYPE=${CMAKE_BUILD_TYPE})

add_custom_command(
  TARGET lean-ftl POST_BUILD
  COMMAND ls -l ${CMAKE_BINARY_DIR}
  COMMAND ls -l $<TARGET_FILE:lean-ftl>
  COMMAND ${CMAKE_AR} -x $<TARGET_FILE:lean-ftl>
  COMMAND ${CMAKE_OBJDUMP} -D ${OBJDUMP_OPTIONS} *.${CMAKE_AR_O_EXT} > disassembly.asm
)

if(LFTL_DEVICE_BSP)
  #a device library (cmake/device-lib.cmake): the header its users include before lean-ftl.h,
  #next to the library
  add_custom_command(
    TARGET lean-ftl POST_BUILD
    COMMAND python3 -B ${CMAKE_CURRENT_LIST_DIR}/../lean-bsp/gen-device-header
            --bsp ${LFTL_DEVICE_BSP_ROOT} --device ${LFTL_DEVICE_NAME}
            --base ${LFTL_DEVICE_NVM_BASE} --size ${LFTL_DEVICE_NVM_SIZE}
            --out ${CMAKE_CURRENT_BINARY_DIR}/lftl-device.h
  )
endif()