//This file has no "guard" nor #pragma once because it is meant to be included a single time in the main file.

#include "type.h"
extern data_flash_t nvm;
uint8_t nvm_erase(void*base_address, unsigned int n_pages);
uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size);
uint8_t nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size);
void throw_exception(uint32_t err_code);

lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = LFTL_WU_SIZE,
    .erase_size = LFTL_PAGE_SIZE,
    .erase = nvm_erase,
    .write = nvm_write,
    .read = nvm_read,
    .error_handler = throw_exception,
  };

lftl_ctx_t nvma = {
  .nvm_props = &nvm_props,
  .area = &nvm.a_pages,
  .area_size = sizeof(nvm.a_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.a_data),
  .transaction_tracker = LFTL_INVALID_POINTER,
  .next = LFTL_INVALID_POINTER
};

lftl_ctx_t nvmb = {
  .nvm_props = &nvm_props,
  .area = &nvm.b_pages,
  .area_size = sizeof(nvm.b_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.b_data),
  .transaction_tracker = LFTL_INVALID_POINTER,
  .next = LFTL_INVALID_POINTER
};

lftl_ctx_t ewlfa = {
  .nvm_props = &nvm_props,
  .area = &nvm.ewlfa_pages,
  .area_size = sizeof(nvm.ewlfa_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.ewlfa_data),
  .transaction_tracker = LFTL_INVALID_POINTER,
  .next = LFTL_INVALID_POINTER
};


lftl_ctx_t ewlfb = {
  .nvm_props = &nvm_props,
  .area = &nvm.ewlfb_pages,
  .area_size = sizeof(nvm.ewlfb_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.ewlfb_data),
  .transaction_tracker = LFTL_INVALID_POINTER,
  .next = LFTL_INVALID_POINTER
};

// A second, independently registered lftl_nvm_props_t, scoped to just the
// "c" area, used by multi_nvm_test to exercise the multi-NVM linked list.
lftl_nvm_props_t nvm_props2 = {
    .base = &nvm.c_pages,
    .size = sizeof(nvm.c_pages),
    .write_size = LFTL_WU_SIZE,
    .erase_size = LFTL_PAGE_SIZE,
    .erase = nvm_erase,
    .write = nvm_write,
    .read = nvm_read,
    .error_handler = throw_exception,
  };

lftl_ctx_t nvmc = {
  .nvm_props = &nvm_props2,
  .area = &nvm.c_pages,
  .area_size = sizeof(nvm.c_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.c_data),
  .transaction_tracker = LFTL_INVALID_POINTER,
  .next = LFTL_INVALID_POINTER
};


int test_main();
void test_callbacks();