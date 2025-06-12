cmake_minimum_required(VERSION 3.22)

MESSAGE(STATUS "Using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")

message(PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
message(CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
message(CMAKE_CURRENT_LIST_DIR="${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/3rd/cmake-modules")
list(APPEND CMAKE_MODULE_PATH "{{sr:cmake_path}}")
message("Build CMAKE_MODULE_PATH: " ${CMAKE_MODULE_PATH})

include("${CMAKE_CURRENT_LIST_DIR}/git_version.cmake")
add_definitions( -DGIT_VERSION=${GIT_VERSION} )
add_definitions( -DVERSION_TIMESTAMP=${VERSION_TIMESTAMP} )

add_definitions( -DBUILD_TYPE=${CMAKE_BUILD_TYPE})
# Core project settings
enable_language(C CXX ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Sources
if(NOT sources_SRCS)
    set(sources_SRCS)
endif()
if(NOT target_SRCS)
    set(target_SRCS)
endif()

# Include directories for all compilers
if(NOT include_DIRS)
    set(include_DIRS)
endif()

if(NOT target_include_DIRS)
    set(target_include_DIRS)
endif()

# Include directories for each compiler
if(NOT include_c_DIRS)
    set(include_c_DIRS)
endif()
if(NOT include_cxx_DIRS)
    set(include_cxx_DIRS)
endif()
if(NOT include_asm_DIRS)
    set(include_asm_DIRS)
endif()


# Symbols definition for all compilers
if(NOT symbols_SYMB)
    set(symbols_SYMB)
endif()

# Symbols definition for each compiler
if(NOT symbols_c_SYMB)
    set(symbols_c_SYMB)
endif()
if(NOT symbols_cxx_SYMB)
    set(symbols_cxx_SYMB)
endif()
if(NOT symbols_asm_SYMB)
    set(symbols_asm_SYMB)
endif()

# Link directories and names of libraries
if(NOT link_DIRS)
    set(link_DIRS)
endif()

if(NOT link_LIBS)
    set(link_LIBS)
endif()

# Compiler options
if(NOT compiler_OPTS)
    set(compiler_OPTS)
endif()

# Linker options
if(NOT linker_OPTS)
    set(linker_OPTS)
endif()

if(linker_script_SRC)
    set(linker_script_arg -T${linker_script_SRC})
else()
    set(linker_script_arg)
endif()

# Link directories setup
# Must be before executable is added
link_directories(${CMAKE_PROJECT_NAME} ${link_DIRS})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} 
    PUBLIC ${sources_SRCS} 
    PRIVATE ${target_SRCS}
    )

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${include_DIRS}
    $<$<COMPILE_LANGUAGE:C>: ${include_c_DIRS} ${target_include_c_DIRS}> 
    $<$<COMPILE_LANGUAGE:CXX>: ${include_cxx_DIRS}>
    $<$<COMPILE_LANGUAGE:ASM>: ${include_asm_DIRS}>
)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PUBLIC
    ${target_include_sys_c_DIRS}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    ${symbols_SYMB}
    $<$<COMPILE_LANGUAGE:C>: ${symbols_c_SYMB}>
    $<$<COMPILE_LANGUAGE:CXX>: ${symbols_cxx_SYMB}>
    $<$<COMPILE_LANGUAGE:ASM>: ${symbols_asm_SYMB}>

    # Configuration specific
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>: >
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} ${link_LIBS})

# Compiler options
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
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
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb>
    $<$<CONFIG:Release>:-O3 -g0>
)

# Linker options
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    ${linker_script_arg}
    ${cpu_PARAMS}
    ${linker_OPTS}
)

# Execute post-build to print size, generate hex and bin
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJDUMP} -h -D $<TARGET_FILE:${CMAKE_PROJECT_NAME}> > $<TARGET_FILE:${CMAKE_PROJECT_NAME}>.sections
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
)

