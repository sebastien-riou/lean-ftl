
include("${CMAKE_CURRENT_LIST_DIR}/../../on/cortex-m33")

set(linker_script_SRC ${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/${DEVICE}_FLASH.ld)

set(target_SRCS 
    ${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/syscall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/sysmem.c
	${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/startup_${DEVICE}.s
	${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/nucleo-${DEVICE}.c
)

set(target_include_c_DIRS 
	${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/CMSIS/Include
	${CMAKE_CURRENT_SOURCE_DIR}/target/stm32/CMSIS/Device/ST/${DEVICE_FAMILY}/Include
)

set(linker_OPTS
    --specs=nosys.specs
    -Wl,-Map=${CMAKE_PROJECT_NAME}.map
    -Wl,--start-group
    -lc
    -lm
    -lstdc++
    -lsupc++
    -Wl,--end-group
    -Wl,-z,max-page-size=8 # Allow good software remapping across address space (with proper GCC section making)
    -Wl,--print-memory-usage
)