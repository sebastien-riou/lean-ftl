#pragma once

#include "util.h"
#include "type.h"
#include "error.h"

//in a lean-bsp build, at the start of the BSP's NVM region 0, which the BSP's
//linker script reserves for the sections named .bsp_nvm0
#ifdef LEAN_BSP_NVM_REGIONS
#define LFTL_TEST_NVM_SECTION ".bsp_nvm0"
#else
#define LFTL_TEST_NVM_SECTION ".data_flash"
#endif

data_flash_t nvm __attribute__ ((section (LFTL_TEST_NVM_SECTION))) = {
  .a_pages = {{0}},
  .b_pages = {{0}},
};


