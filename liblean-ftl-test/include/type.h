#pragma once

#include "util.h"

#define LFTL_DEFINE_HELPERS
#ifdef LFTL_STM32U5
#define LFTL_PAGE_SIZE (8*1024) //STM32U5
#define LFTL_WU_SIZE 16
#endif
#ifdef LFTL_STM32L5
#define LFTL_PAGE_SIZE (2*1024) //STM32L5
#define LFTL_WU_SIZE 8
#endif

#include "lean-ftl.h"

#define DATA_SIZE (4*LFTL_WU_SIZE)

typedef struct data_flash_struct {

  LFTL_AREA(a,
    uint64_t data0[SIZE64(DATA_SIZE)];
    uint64_t data1[SIZE64(DATA_SIZE)];
    ,2)
  
  LFTL_AREA(b,
    uint64_t data2[SIZE64(DATA_SIZE)];
    uint64_t data3[SIZE64(DATA_SIZE)];
    ,2)
  union {
    flash_sw_page_t unmanaged_page;
    struct {
      uint64_t unmanaged_data0[SIZE64(DATA_SIZE)];
      uint64_t unmanaged_data1[SIZE64(DATA_SIZE)];
    };
  };
} __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;

#define SIMULATED_TEARING 0xFF
