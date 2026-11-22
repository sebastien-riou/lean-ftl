#pragma once

#include "util.h"

#define LFTL_DEFINE_HELPERS
#if defined(LEAN_BSP_NVM_REGIONS) && !defined(LFTL_PAGE_SIZE)
//a lean-bsp build (lean-bsp/): the geometry of the BSP's NVM region 0, which
//lean-bsp passes to the compilation
#define LFTL_PAGE_SIZE LEAN_BSP_NVM0_PAGE_SIZE
#define LFTL_WU_SIZE LEAN_BSP_NVM0_WRITE_SIZE
#define LFTL_ZERO_OVERWRITE LEAN_BSP_NVM0_ZERO_OVERWRITE
#endif
#ifdef LFTL_STM32U5
#define LFTL_PAGE_SIZE (8*1024)
#define LFTL_WU_SIZE 16
#endif
#ifdef LFTL_STM32L5
#define LFTL_PAGE_SIZE (2*1024)
#define LFTL_WU_SIZE 8
#endif
//the STM32L5 and STM32U5 document it, the Linux simulator models it (and
//--no-pinning turns it off at run time)
#ifndef LFTL_ZERO_OVERWRITE
#define LFTL_ZERO_OVERWRITE 1
#endif

#include "lean-ftl.h"

#define DATA_SIZE (4*LFTL_WU_SIZE)

//Use a huge size for EWLF data to reduce the number of tests
#define EWLF_DATA_SIZE (LFTL_PAGE_SIZE / 3)

//A large area of 3 chunks, the last one in part
#define LARGE_DATA_SIZE (2*LFTL_PAGE_SIZE + LFTL_PAGE_SIZE/2)
//its pool: the 3 chunks and room for a transaction of the whole data
#define LARGE_POOL_PAGES 6

typedef struct data_flash_struct {

  LFTL_AREA(a,
    uint64_t data0[SIZE64(DATA_SIZE)];
    uint64_t data1[SIZE64(DATA_SIZE)];
    ,LFTL_WEAR_LEVELING_FACTOR(2)
  )
  
  LFTL_AREA(b,
    uint64_t data2[SIZE64(DATA_SIZE)];
    uint64_t data3[SIZE64(DATA_SIZE)];
    ,LFTL_WEAR_LEVELING_FACTOR(2)
  )

  LFTL_AREA_EWLF(ewlfa,
    uint64_t data4[SIZE64(EWLF_DATA_SIZE)];
    ,LFTL_WEAR_LEVELING_FACTOR(5)
  )

  LFTL_AREA_EWLF(ewlfb,
    uint64_t data5[SIZE64(EWLF_DATA_SIZE)];
    ,LFTL_WEAR_LEVELING_FACTOR(10)
  )

  //Area used by a second, independently registered lftl_nvm_props_t (multi_nvm_test)
  LFTL_AREA(c,
    uint64_t data6[SIZE64(DATA_SIZE)];
    ,LFTL_WEAR_LEVELING_FACTOR(2)
  )

  //Area used by a third, independently registered lftl_nvm_props_t (multi_nvm_test)
  LFTL_AREA(d,
    uint64_t data7[SIZE64(DATA_SIZE)];
    ,LFTL_WEAR_LEVELING_FACTOR(2)
  )

  //After area d, which test 37 needs where it is: a large area
  LFTL_AREA_LARGE(largea,
    uint64_t data8[SIZE64(LARGE_DATA_SIZE)];
    ,LARGE_POOL_PAGES
  )

  //Pages for the tests that build their own EWLF or large area: 2 log
  //units, and up to 8 data or pool units
  flash_sw_page_t scratch_pages[10];

  union {
  flash_sw_page_t unmanaged_page;
    struct {
      uint64_t unmanaged_data0[SIZE64(DATA_SIZE)];
      uint64_t unmanaged_data1[SIZE64(DATA_SIZE)];
    };
  };
} __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;

#define SIMULATED_TEARING 0xFF
