#include <stdint.h>
#include <stdbool.h>
#include "util.h"
#define STM32U5A5xx
#include <stm32u5xx.h>

#define FLASH_WRITE_SIZE 16

void init(){

}

void led1(bool on){

}

bool button(){
  return false;
}

void delay_ms(unsigned int ms){

}

#include "type.h"
extern data_flash_t nvm __attribute__ ((section (".data_flash")));

#include "lean-ftl.h"
lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = FLASH_WRITE_SIZE,
    .erase_size = FLASH_PAGE_SIZE,
  };

uint8_t nvm_erase(void*base_address, unsigned int n_pages){
  return 1;
}

uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  return 1;
}

uint8_t nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  return 1;
}