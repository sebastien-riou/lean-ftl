set(CMAKE_SYSTEM_NAME               Generic)

execute_process (
    COMMAND bash -c "which arm-none-eabi-ar"
    OUTPUT_VARIABLE ar_path
)
#message(WARNING "ar_path is ${ar_path}")
string(STRIP ${ar_path} ar_path)

# Some default GCC settings
# arm-none-eabi- must be part of path environment
set(TOOLCHAIN_PREFIX                arm-none-eabi-)
set(FLAGS                           "-fdata-sections -ffunction-sections -Wl,--gc-sections")
set(CPP_FLAGS                       "${FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_C_FLAGS                   ${FLAGS})
set(CMAKE_CXX_FLAGS                 ${CPP_FLAGS})

set(CMAKE_C_COMPILER                ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER              ${CMAKE_C_COMPILER})
set(CMAKE_CXX_COMPILER              ${TOOLCHAIN_PREFIX}g++)
#set(CMAKE_OBJCOPY                   ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                      ${TOOLCHAIN_PREFIX}size)
#set(CMAKE_AR                        ${CMAKE_C_COMPILER}-ar CACHE FILEPATH "Archiver" FORCE)
set(CMAKE_AR                        ${ar_path})
set(CMAKE_AR_O_EXT                  "obj")

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

#set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
#set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
#set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)

#message(WARNING "CMAKE_C_COMPILER is ${CMAKE_C_COMPILER}")
#message(WARNING "CMAKE_AR is ${CMAKE_AR}")

set(OBJDUMP_OPTIONS "--visualize-jumps")
