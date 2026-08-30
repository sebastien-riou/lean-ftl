#pragma once

#include "util.h"

#define LFTL_DEFINE_HELPERS
#ifdef LFTL_STM32U5
#define LFTL_PAGE_SIZE (8*1024)
#define LFTL_WU_SIZE 16
#endif
#ifdef LFTL_STM32L5
#define LFTL_PAGE_SIZE (2*1024)
#define LFTL_WU_SIZE 8
#endif
#ifdef LFTL_CH32V307
#define LFTL_PAGE_SIZE (4*1024)
#define LFTL_WU_SIZE 2
#endif

#include "lean-ftl.h"

#define DATA_SIZE (4*LFTL_WU_SIZE)

//Use a huge size for EWLF data to reduce the number of tests
#define EWLF_DATA_SIZE (LFTL_PAGE_SIZE / 3)

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

  union {
  flash_sw_page_t unmanaged_page;
    struct {
      uint64_t unmanaged_data0[SIZE64(DATA_SIZE)];
      uint64_t unmanaged_data1[SIZE64(DATA_SIZE)];
    };
  };
} __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;

#define SIMULATED_TEARING 0xFF
