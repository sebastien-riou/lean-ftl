set(LINUX_TARGET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/linux)
set(target_SRCS 
	${LINUX_TARGET_DIR}/linux.c
)

add_definitions( -DHAS_SAFE_BUTTON )
add_definitions( -DHAS_TEARING_SIMULATION )

set(target_include_sys_c_DIRS 
	${LINUX_TARGET_DIR}
)

