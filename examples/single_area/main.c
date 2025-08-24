#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <string.h>
#include "error.h"
#include "util.h"

const char*version = xstr(GIT_VERSION);

//Application level HAL
void init(int argc, const char*argv[]);
void led1(bool on);
bool button();
void com_tx(const void *const buf, unsigned int size);
void com_rx(void *const buf, unsigned int size);
void delay_ms(unsigned int ms);
extern const uint32_t nvm_write_size;//min NVM write size
extern const uint32_t nvm_erase_size;//min NVM erase size
//lean-ftl low level NVM accessors
uint8_t nvm_erase(void*base_address, unsigned int n_pages);
uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size);
uint8_t nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size);


//Application

static jmp_buf exception_ctx;
void throw_exception(uint32_t err_code){
  longjmp(exception_ctx,err_code);
}

#include "ui.h"
#include <stdio.h>


#include "type.h"


data_flash_t nvm __attribute__ ((section (".data_flash")));
uint64_t nvm_data_state __attribute__ ((section (".data_flash")));
#define NVM_DATA_STATE_INITIALIZED 0x7EB5F0C28D3419A6 //could be any non trivial 64 bit value

lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = WU_SIZE,
    .erase_size = FLASH_SW_PAGE_SIZE,
  };
lftl_ctx_t nvdata = {
  .nvm_props = &nvm_props,
  .area = &nvm.nvdata_pages,
  .area_size = sizeof(nvm.nvdata_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.nvdata_data),
  .erase = nvm_erase,
  .write = nvm_write,
  .read = nvm_read,
  .error_handler = throw_exception,
  .transaction_tracker = LFTL_INVALID_POINTER
};

#define NVM_OFFSET(x) ((uintptr_t)&x - (uintptr_t)&nvm)
#define PRINT_NVM_VAR_INFO(x) \
  printf("INFO: offset of %12s = 0x%04lx, size = %4ld, physical size = %4ld\n",#x,NVM_OFFSET(x),sizeof(x),sizeof(x ## _phy));

void display_nvdata_state(){
  uint32_t cnt0;
  lftl_read(&nvdata, &cnt0, &nvm.cnt0, sizeof(nvm.cnt0));
  printf("INFO: %12s = 0x%08x\n","cnt0",cnt0);
}

void single_area_demo(){
  // Display info about our data in NVM
  PRINT_NVM_VAR_INFO(nvm.cnt0);
  PRINT_NVM_VAR_INFO(nvm.array0);
  PRINT_NVM_VAR_INFO(nvm.cnt1);
  PRINT_NVM_VAR_INFO(nvm.array1);
  PRINT_NVM_VAR_INFO(nvm.dat1);

  // LFTL operations
  lftl_init_lib();
  lftl_register_area(&nvdata);

  // Detect if we need to format the NVM (first use)
  if (nvm_data_state != NVM_DATA_STATE_INITIALIZED){ // Note that we do NOT use LFTL to read/write nvm_data_state
    printf("INFO: NVM not initialized, calling lftl_format\n");
    lftl_format(&nvdata);
    const uint64_t init = NVM_DATA_STATE_INITIALIZED;
    nvm_write(&nvm_data_state, &init, sizeof(init)); 
    // initialize all data to 0 (default state is erased state, typically 0xFF but that may vary depending on NVM technology)
    LFTL_MEMSET_WHOLE_AREA(nvdata,0);
  } else {
    printf("INFO: NVM already initialized\n");
  }
  
  // Read NVM
  display_nvdata_state();

  // Update each variable atomically
  uint32_t cnt0;
  lftl_read(&nvdata, &cnt0, &nvm.cnt0, sizeof(nvm.cnt0));
  cnt0++;
  lftl_write_any(&nvdata, &nvm.cnt0, &cnt0, sizeof(nvm.cnt0));
  
  // Read NVM
  display_nvdata_state();
}


void exception_handler(uint32_t err_code){
  #ifdef HAS_PRINTF
  printf("ERROR: test failed with error code 0x%08x\n",err_code);
  #endif
  ui_wait_button();
}

int main(int argc, const char*argv[]){
  init(argc,argv);
  led1(1);
  uint32_t err_code=-1;
  if(0 == (err_code = setjmp(exception_ctx))){
    single_area_demo();
    err_code = 0;
    led1(0);
  } else {
    exception_handler(err_code);
  }
  
  
  if(0==err_code){
    #ifdef LINUX
      printf("PASS\n");
    #else
      while(1){ui_led1_blink_ms(5000,DUTY_CYCLE_50);}
    #endif
  }else{
    led1(0);
     #ifdef LINUX
      printf("FAIL\n");
    #else
      while(1);
    #endif
  }
  return err_code;
}


