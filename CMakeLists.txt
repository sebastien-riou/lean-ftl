cmake_minimum_required(VERSION 3.22)

project(lean-ftl-test)

add_subdirectory(liblean-ftl)

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
)

set(link_LIBS
	lean-ftl
)

include("cmake/generic_binary.cmake")

include(CTest)
add_test(NAME lean-ftl-test
					COMMAND lean-ftl-test)

