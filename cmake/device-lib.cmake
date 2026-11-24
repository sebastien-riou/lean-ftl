# A device library: lean-ftl plus the flash accessors of one device, taken from the lean-bsp BSP
# that holds them (its src/bsp_nvm.c) and compiled with the bounds given here. The accessors exist
# in the BSP only; lean-bsp/bsps.lock names the commit a release is built from, and ./fetch-bsps
# clones the BSPs at those commits.
#
# Set by the on/<device> file before it includes this one:
#   LFTL_DEVICE_BSP           the BSP's name: its repository is lean-bsp-<name>
#   LFTL_DEVICE_BSP_INCLUDES  include directories bsp_nvm.c needs, relative to the BSP
#   LFTL_DEVICE_BSP_DEFINES   the defines the BSP compiles its sources with
#   LFTL_DEVICE_NVM_BASE      first address the accessors accept
#   LFTL_DEVICE_NVM_SIZE      size in bytes of the range they accept
#
# The BSPs are looked for in the directory LEAN_BSP_WORKSPACE names (environment), by default the
# one that holds this repository, which is where lean-bsp expects them too.

if(DEFINED ENV{LEAN_BSP_WORKSPACE})
  set(LFTL_BSP_WORKSPACE "$ENV{LEAN_BSP_WORKSPACE}")
else()
  get_filename_component(LFTL_BSP_WORKSPACE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
get_filename_component(LFTL_DEVICE_NAME "${CMAKE_TOOLCHAIN_FILE}" NAME)
set(LFTL_DEVICE_BSP_ROOT "${LFTL_BSP_WORKSPACE}/lean-bsp-${LFTL_DEVICE_BSP}")
set(LFTL_DEVICE_BSP_NVM "${LFTL_DEVICE_BSP_ROOT}/src/bsp_nvm.c")

if(NOT EXISTS "${LFTL_DEVICE_BSP_NVM}")
  message(FATAL_ERROR "${LFTL_DEVICE_BSP_NVM} not found: this build takes the flash accessors from the BSP "
                      "lean-bsp-${LFTL_DEVICE_BSP}. Run ./fetch-bsps, or set LEAN_BSP_WORKSPACE to the directory that holds it.")
endif()
# a BSP from before the bounds could be given would ignore them and build its own region
file(STRINGS "${LFTL_DEVICE_BSP_NVM}" LFTL_DEVICE_BSP_HAS_BOUNDS REGEX "BSP_NVM0_BASE")
if(NOT LFTL_DEVICE_BSP_HAS_BOUNDS)
  message(FATAL_ERROR "${LFTL_DEVICE_BSP_NVM} does not take BSP_NVM0_BASE and BSP_NVM0_SIZE: "
                      "the checkout of lean-bsp-${LFTL_DEVICE_BSP} is too old, run ./fetch-bsps")
endif()

set(LFTL_ACCESSORS
  ${LFTL_DEVICE_BSP_NVM}
  ${CMAKE_CURRENT_LIST_DIR}/../lean-bsp/lftl_device_nvm.c
)
set(LFTL_ACCESSORS_INCLUDES "${LFTL_DEVICE_BSP_ROOT}/src")
foreach(dir ${LFTL_DEVICE_BSP_INCLUDES})
  list(APPEND LFTL_ACCESSORS_INCLUDES "${LFTL_DEVICE_BSP_ROOT}/${dir}")
endforeach()
# for the accessors only: the BSP's defines do not reach ftl.c
set(LFTL_ACCESSORS_DEFINES
  ${LFTL_DEVICE_BSP_DEFINES}
  BSP_NVM0_BASE=${LFTL_DEVICE_NVM_BASE}
  BSP_NVM0_SIZE=${LFTL_DEVICE_NVM_SIZE}
)
