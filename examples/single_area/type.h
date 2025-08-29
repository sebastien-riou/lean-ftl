#pragma once

#include "util.h"

#define LFTL_DEFINE_HELPERS
#include <lean-ftl.h>

typedef struct data_flash_struct {
  LFTL_AREA(
    // Name of the area
    nvdata
    ,
    // Content of the area
    LFTL_VAR(uint32_t, cnt0);//this is the generic way to declare a variable in an area
    LFTL_ARRAY(uint8_t, array0, 3);//this is the generic way to declare an array in an area
    uint32_t dat0[4];//this works as long as the size is a multiple of WU_SIZE (the minimum write size of the flash)
    LFTL_VAR(uint32_t, cnt1);
    LFTL_COMPACT_ARRAY(uint8_t, array1, 3);//Compact storage: requires buffering in RAM for writes
    LFTL_RAW_DATA(dat1,sizeof(uint8_t)*4);
    ,
    // wear leveling factor for that area
    LFTL_WEAR_LEVELING_FACTOR(2)
  )
    
} __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;

#define SIMULATED_TEARING 0xFF
