//Minimal use of a device library (the release's stm32u5a5xj, stm32l552xe,
//m5531h2ljae): lean-ftl with the flash accessors of the device in one archive.
//The application includes lftl-device.h, which sits next to the library, and
//links the library: no accessor to write, no geometry to look up.
//
//This file is also the link check of the device libraries
//(test/device-lib-check): it must link against the archive alone.
#include <setjmp.h>
#include <stdint.h>

#include "lftl-device.h" //LFTL_PAGE_SIZE, LFTL_WU_SIZE, LFTL_ZERO_OVERWRITE, nvm_erase, nvm_write, nvm_read
#include "lean-ftl.h"

typedef struct data_flash_struct {
  LFTL_AREA(nvdata,
    LFTL_VAR(uint32_t, boot_count);
    ,LFTL_WEAR_LEVELING_FACTOR(2)
  )
} __attribute__ ((aligned (LFTL_PAGE_SIZE))) data_flash_t;

//The linker script places this section in the flash, inside the range the
//accessors accept (LFTL_DEVICE_NVM_BASE, LFTL_DEVICE_NVM_SIZE) and away from the code
data_flash_t nvm __attribute__ ((section (".data_flash")));

static jmp_buf exception_ctx;
static void throw_exception(uint32_t err_code){
  longjmp(exception_ctx,err_code);
}

static lftl_nvm_props_t nvm_props = {
  .base = &nvm,
  .size = sizeof(nvm),
  .write_size = LFTL_WU_SIZE,
  .erase_size = LFTL_PAGE_SIZE,
  .zero_overwrite = LFTL_ZERO_OVERWRITE,
  .erase = nvm_erase,
  .write = nvm_write,
  .read = nvm_read,
  .error_handler = throw_exception,
};

static lftl_ctx_t nvdata = {
  .nvm_props = &nvm_props,
  .area = &nvm.nvdata_pages,
  .area_size = sizeof(nvm.nvdata_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.nvdata_data),
  .transaction_tracker = LFTL_INVALID_POINTER
};

//Counts the boots. `first_boot`: the area was never formatted (an application
//keeps that fact outside the area, see examples/single_area).
//Returns 0, or the error code lean-ftl raised
uint32_t count_boot(int first_boot){
  uint32_t err_code;
  if(0 != (err_code = setjmp(exception_ctx))) return err_code;
  lftl_init_lib();
  lftl_register_area(&nvdata);
  uint32_t boot_count = 0;
  if(first_boot){
    lftl_format(&nvdata);
  } else {
    lftl_read(&nvdata, &boot_count, &nvm.boot_count, sizeof(nvm.boot_count));
  }
  boot_count++;
  lftl_write_any(&nvdata, &nvm.boot_count, &boot_count, sizeof(nvm.boot_count));
  return 0;
}

int main(void){
  return (int)count_boot(0);
}
