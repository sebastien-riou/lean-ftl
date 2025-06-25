#pragma once

#include "util.h"

#define LFTL_DEFINE_HELPERS
#define FLASH_SW_PAGE_SIZE (8*1024) //STM32U5
#define WU_SIZE 16
#include <lean-ftl.h>

typedef struct data_flash_struct {
  LFTL_AREA(nvdata,
    uint64_t data0[4];
    uint64_t data1[4];
    ,2)
    
} __attribute__ ((aligned (FLASH_SW_PAGE_SIZE))) data_flash_t;

#define SIMULATED_TEARING 0xFF
