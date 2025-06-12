include("${CMAKE_CURRENT_LIST_DIR}/gcc-arm-none-eabi.cmake")
set(CMAKE_SYSTEM_PROCESSOR armv8-m)

# Core MCU flags, CPU, instruction set and FPU setup
set(cpu_PARAMS ${cpu_PARAMS}
    -mthumb

    # Other parameters
    # -mcpu, -mfloat, -mfloat-abi, ...
    -mcpu=cortex-m33
	#-mfpu=fpv5-sp-d16 #if we declare FPU here, FPU regs are backed up by setjmp and that can crash if FPU has not been enabled by software (it is disabled by default)
	#-mfloat-abi=hard
	
)
