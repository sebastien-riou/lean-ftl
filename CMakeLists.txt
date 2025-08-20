cmake_minimum_required(VERSION 3.22)

project(lean-ftl-test-fw)

#message(WARNING "CMAKE_C_COMPILER is ${CMAKE_C_COMPILER}")
#message(WARNING "CMAKE_AR is ${CMAKE_AR}")

add_subdirectory(liblean-ftl)
add_subdirectory(liblean-ftl-test)

if(BUILD_TEST)

# Sources
set(sources_SRCS
	${CMAKE_CURRENT_SOURCE_DIR}/test/source/main.c
)

# Include directories
set(include_c_DIRS
	${CMAKE_CURRENT_SOURCE_DIR}/test/include
	${CMAKE_CURRENT_SOURCE_DIR}/liblean-ftl/include
)

# libs
set(link_DIRS
	${CMAKE_CURRENT_BINARY_DIR}/liblean-ftl
	${CMAKE_CURRENT_BINARY_DIR}/liblean-ftl-test
)

set(link_LIBS
	lean-ftl-test
	lean-ftl
)

include("cmake/generic_binary.cmake")

include(CTest)
add_test(NAME lean-ftl-test-fw
					COMMAND lean-ftl-test-fw)

endif(BUILD_TEST)