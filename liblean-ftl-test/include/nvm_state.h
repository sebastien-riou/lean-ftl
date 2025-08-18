#pragma once

#include "util.h"
#include "type.h"
#include "error.h"

data_flash_t nvm __attribute__ ((section (".data_flash"))) = {
  .a_pages = {{0}},
  .b_pages = {{0}},
};


