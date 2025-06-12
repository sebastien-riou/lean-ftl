#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <string.h>
#include "error.h"
#include "util.h"

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

//order of these headers matters
#include "ui.h"
#include "nvm_state.h"

const char*version = xstr(GIT_VERSION);

//A very bad PRNG, but it is good enough for our testing purpose + it facilitate our debugging
static void prng_fill(uint8_t*rng_state, void*buf, uint32_t size){
  uint8_t cnt=*rng_state;
  uint8_t*buf8 = (uint8_t*)buf;
  for(unsigned int i=0;i<size;i++){
    buf8[i] = cnt++;
  }
  *rng_state = cnt;
}

extern lftl_nvm_props_t nvm_props;
lftl_ctx_t nvma = {
  .nvm_props = &nvm_props,
  .area = &nvm.a_pages,
  .area_size = sizeof(nvm.a_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.a_data),
  .erase = nvm_erase,
  .write = nvm_write,
  .read = nvm_read,
  .error_handler = throw_exception,
  .transaction_tracker = LFTL_INVALID_POINTER
};
lftl_ctx_t nvmb = {
  .nvm_props = &nvm_props,
  .area = &nvm.b_pages,
  .area_size = sizeof(nvm.b_pages),
  .data = LFTL_INVALID_POINTER,
  .data_size = sizeof(nvm.b_data),
  .erase = nvm_erase,
  .write = nvm_write,
  .read = nvm_read,
  .error_handler = throw_exception,
  .transaction_tracker = LFTL_INVALID_POINTER
};

void (*format_func)(lftl_ctx_t*ctx);
void (*erase_all_func)(lftl_ctx_t*ctx);
void (*write_func)(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size);
void (*transaction_start_func)(lftl_ctx_t*ctx, void *const transaction_tracker);
void (*transaction_write_func)(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);
void (*transaction_commit_func)(lftl_ctx_t*ctx);
void (*transaction_abort_func)(lftl_ctx_t*ctx);

#ifdef HAS_TEARING_SIMULATION
#include <stdio.h>
#include <stdlib.h>
data_flash_t nvm_ref;
data_flash_t nvm_ref_previous_state;
data_flash_t transaction_buf_ref;

void tearing_sim_lftl_format(lftl_ctx_t*ctx){
  //note: we do not support anti tearing during format
  //we implement this function merely to keep nvm_ref in sync with nvm
  lftl_format(ctx);
  void*base = ctx->area;
  uintptr_t offset = (uintptr_t)base - (uintptr_t)&nvm;
  uintptr_t size = ctx->data_size;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memcpy(dst+offset,base,size);
}
void tearing_sim_lftl_erase_all(lftl_ctx_t*ctx){
  //update previous state
  nvm_ref_previous_state = nvm_ref;
  //compute new state
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memset(dst,0xFF,ctx->data_size);
  //call LFTL
  lftl_erase_all(ctx);
}
void tearing_sim_lftl_write(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  //update previous state
  nvm_ref_previous_state = nvm_ref;
  //compute new state
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memcpy(dst+offset,src,size);
  //call LFTL
  lftl_write(ctx,dst_nvm_addr,src,size);
}
void tearing_sim_lftl_transaction_start(lftl_ctx_t*ctx, void *const transaction_tracker){
  //copy nvm to transaction buffer
  uintptr_t offset = (uintptr_t)ctx->area - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&transaction_buf_ref;
  uint8_t*src = (uint8_t*)&nvm_ref;
  memcpy(dst+offset,src+offset,ctx->area_size);
  //call LFTL
  lftl_transaction_start(ctx,transaction_tracker);
}
void tearing_sim_lftl_transaction_write(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  //write into transaction buffer
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&transaction_buf_ref;
  memcpy(dst+offset,src,size);
  //call LFTL
  lftl_transaction_write(ctx,dst_nvm_addr,src,size);
}
void tearing_sim_lftl_transaction_commit(lftl_ctx_t*ctx){
  //update previous state
  nvm_ref_previous_state = nvm_ref;
  //copy transaction buffer to nvm
  uintptr_t offset = (uintptr_t)ctx->area - (uintptr_t)&nvm;
  uint8_t*src = (uint8_t*)&transaction_buf_ref;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memcpy(dst+offset,src+offset,ctx->area_size);
  //call LFTL
  lftl_transaction_commit(ctx);
}
void tearing_sim_lftl_transaction_abort(lftl_ctx_t*ctx){
  //call LFTL
  lftl_transaction_abort(ctx);
}
bool nvm_is_equal(data_flash_t*expected){
  data_flash_t read_val;
  lftl_read(&nvma,&read_val.a_data,&nvm.a_data,sizeof(nvm.a_data));
  if(memcmp(&read_val.a_data,&expected->a_data,sizeof(nvm.a_data))) return 0;
  lftl_read(&nvmb,&read_val.b_data,&nvm.b_data,sizeof(nvm.b_data));
  if(memcmp(&read_val.b_data,&expected->b_data,sizeof(nvm.b_data))) return 0;
  return 1;
}
void check_nvm(){
  if(nvm_is_equal(&nvm_ref)) return;
  if(nvm_is_equal(&nvm_ref_previous_state)) return;
  printf("NVM corrupted\n");
  abort();
}
void tearing_sim_check_nvm(){
  printf("Simulated tearing\n");
  //simulate a reboot
  nvma.data = LFTL_INVALID_POINTER;
  nvma.transaction_tracker = LFTL_INVALID_POINTER;
  nvmb.data = LFTL_INVALID_POINTER;
  nvmb.transaction_tracker = LFTL_INVALID_POINTER;
  check_nvm();
}
uint32_t tearing_sim_get_max_target();
void tearing_sim_set_target(uint64_t target_write);
#endif

void read_and_check(lftl_ctx_t*ctx,void*nvm_addr, const void*const expected, uintptr_t size){
  uint8_t rbuf[size];
  lftl_read(ctx,rbuf,nvm_addr,size);
  const uint8_t*const expected8 = (const uint8_t*const)expected;
  for(unsigned int i=0;i<size;i++){
    if(rbuf[i]!=expected8[i]){
      throw_exception(ERROR_VERIFICATION_FAIL);
    }
  }
}

void read_newer_and_check(lftl_ctx_t*ctx,void*nvm_addr, const void*const expected, uintptr_t size){
  uint8_t rbuf[size];
  lftl_read_newer(ctx,rbuf,nvm_addr,size);
  const uint8_t*const expected8 = (const uint8_t*const)expected;
  for(unsigned int i=0;i<size;i++){
    if(rbuf[i]!=expected8[i]){
      throw_exception(ERROR_VERIFICATION_FAIL);
    }
  }
}

void test_write(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  write_func(ctx,dst_nvm_addr,src,size);
  read_and_check(ctx,dst_nvm_addr,src,size);
  //force a search of the slot
  ctx->data = LFTL_INVALID_POINTER;
  read_and_check(ctx,dst_nvm_addr,src,size);
}

void randomized_test_write(lftl_ctx_t*ctx,void*dst_nvm_addr, uintptr_t size){
  static uint8_t rng_state = 0;
  uint8_t wbuf[size];
  prng_fill(&rng_state,wbuf,size);
  test_write(ctx,dst_nvm_addr,wbuf,size);
}

void basic_test(){
  randomized_test_write(&nvma,nvm.data0,sizeof(nvm.data0));
  randomized_test_write(&nvma,nvm.data1,sizeof(nvm.data1));
  randomized_test_write(&nvmb,nvm.data2,sizeof(nvm.data2));
  randomized_test_write(&nvmb,nvm.data3,sizeof(nvm.data3));
  randomized_test_write(&nvma,&nvm.a_data,sizeof(nvm.a_data));
  randomized_test_write(&nvmb,&nvm.b_data,sizeof(nvm.b_data));
}

void erase_all_test(){
  erase_all_func(&nvma);
  const uint32_t nwords = SIZE64(nvma.data_size);
  uint8_t*addr = (uint8_t*)nvma.area;
  for(unsigned int i=0;i < nwords;i++){
    uint64_t word = -1;
    read_and_check(&nvma,addr,&word,sizeof(word));
    addr += sizeof(word);
  }
}

void write_size_test(){
  const unsigned int write_size = nvmb.nvm_props->write_size;
  //start at 0, end at various offsets
  for(unsigned int i = write_size; i < sizeof(nvm.b_data); i+=write_size){
    randomized_test_write(&nvmb,&nvm.b_data,i);
  }
  //starts at various offset, always end at the end
  uint8_t*base = (uint8_t*)&nvm.b_data;
  for(unsigned int i = 0; i < sizeof(nvm.b_data)-write_size; i+=write_size){
    randomized_test_write(&nvmb,base+i,sizeof(nvm.b_data)-i);
  }
}

void write_offset_test(){
  const unsigned int write_size = nvmb.nvm_props->write_size;
  uint8_t*base = (uint8_t*)&nvm.b_data;
  for(unsigned int i = 0; i < sizeof(nvm.b_data)-write_size; i+=write_size){
    randomized_test_write(&nvmb,base+i,write_size);
  }
}

void transaction_basic_test(){
  uint8_t rng_state = 0;
  for(unsigned int i=0;i<2;i++){
    const size_t size = sizeof(nvm.a_data);
    //initialize the whole data
    uint8_t wbuf0[size];
    prng_fill(&rng_state,wbuf0,size);
    test_write(&nvma,&nvm.a_data,wbuf0,sizeof(nvm.a_data));
    //write 1st word of each data in one transaction
  uint8_t nvma_transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
    transaction_start_func(&nvma,nvma_transaction_tracker);
    uint8_t wbuf[size];
    prng_fill(&rng_state,wbuf,size);
    const uint32_t write_size = nvm_props.write_size;
    const uint32_t data_size = sizeof(nvm.data0);
    transaction_write_func(&nvma,nvm.data0,wbuf,write_size);
    transaction_write_func(&nvma,nvm.data1,wbuf+write_size,write_size);
    read_and_check(&nvma,nvm.data0,wbuf0,write_size);//read current data
    read_and_check(&nvma,nvm.data1,wbuf0+data_size,write_size);//read current data
    read_newer_and_check(&nvma,nvm.data0,wbuf,write_size);//read new data
    read_newer_and_check(&nvma,nvm.data1,wbuf+write_size,write_size);//read new data
    transaction_commit_func(&nvma);
    read_and_check(&nvma,nvm.data0,wbuf,write_size);//read commited data
    read_and_check(&nvma,(void*)(((uintptr_t)&nvm.data0)+write_size),wbuf0+write_size,sizeof(nvm.data0) - write_size);//read original data
    read_and_check(&nvma,nvm.data1,wbuf+write_size,write_size);//read commited data
    read_and_check(&nvma,(void*)(((uintptr_t)&nvm.data1)+write_size),wbuf0+data_size+write_size,sizeof(nvm.data1) - write_size);//read original data

    read_newer_and_check(&nvma,nvm.data0,wbuf,write_size);//read commited data
    read_newer_and_check(&nvma,(void*)(((uintptr_t)&nvm.data0)+write_size),wbuf0+write_size,sizeof(nvm.data0) - write_size);//read original data
    read_newer_and_check(&nvma,nvm.data1,wbuf+write_size,write_size);//read commited data
    read_newer_and_check(&nvma,(void*)(((uintptr_t)&nvm.data1)+write_size),wbuf0+data_size+write_size,sizeof(nvm.data1) - write_size);//read original data
  }
}

void transaction_abort_test(){
  const size_t size = sizeof(nvm.a_data);
  //initialize the whole data to 0
  uint8_t wbuf0[size];
  memset(wbuf0,0,sizeof(wbuf0));
  test_write(&nvma,&nvm.a_data,wbuf0,sizeof(nvm.a_data));
  //write 1st word of each data in one transaction
  uint8_t nvma_transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  transaction_start_func(&nvma,nvma_transaction_tracker);
  uint8_t wbuf[size];
  uint8_t rng_state = 0;
  prng_fill(&rng_state,wbuf,size);
  const uint32_t write_size = nvm_props.write_size;
  transaction_write_func(&nvma,nvm.data0,wbuf,write_size);
  transaction_write_func(&nvma,nvm.data1,wbuf+write_size,write_size);
  read_and_check(&nvma,nvm.data0,wbuf0,write_size);//read current data
  read_and_check(&nvma,nvm.data1,wbuf0,write_size);//read current data
  transaction_abort_func(&nvma);
  read_and_check(&nvma,&nvm.a_data,wbuf0,sizeof(nvm.a_data));//read current data
}

void exception_handler(uint32_t err_code){
  #ifdef HAS_TEARING_SIMULATION
  if((err_code & SIMULATED_TEARING) == SIMULATED_TEARING){
    //simulated tearing, see if we recover well
    tearing_sim_check_nvm();
    format_func(&nvma);
    format_func(&nvmb);
  } else {
    //a real error, that's unexpected
    ui_wait_button();
  }
  #else
  ui_wait_button();
  #endif
}

int main(int argc, const char*argv[]){
  #ifdef HAS_TEARING_SIMULATION
    format_func = tearing_sim_lftl_format;
    erase_all_func = tearing_sim_lftl_erase_all;
    write_func = tearing_sim_lftl_write;
    transaction_start_func = tearing_sim_lftl_transaction_start;
    transaction_write_func = tearing_sim_lftl_transaction_write;
    transaction_commit_func = tearing_sim_lftl_transaction_commit;
    transaction_abort_func = tearing_sim_lftl_transaction_abort;
    printf("version: %s\n",lftl_version());
    printf("version timestamp: %lu\n",lftl_version_timestamp());
    printf("build type: %s\n",lftl_build_type());
  #else
    format_func = lftl_format;
    erase_all_func = lftl_erase_all;
    write_func = lftl_write;
    transaction_start_func = lftl_transaction_start;
    transaction_write_func = lftl_transaction_write;
    transaction_commit_func = lftl_transaction_commit;
    transaction_abort_func = lftl_transaction_abort;
  #endif
  init(argc,argv);
  led1(1);
  uint32_t err_code=-1;
  if(0 == (err_code = setjmp(exception_ctx))){
    format_func(&nvma);
    format_func(&nvmb);
    basic_test();
    write_size_test();
    write_offset_test();
    transaction_basic_test();
    transaction_abort_test();
    erase_all_test();
    err_code = 0;
    led1(0);
  } else {
    exception_handler(err_code);
  }
  #ifdef HAS_TEARING_SIMULATION
    tearing_sim_check_nvm();//sanity check that model is in sync
    const unsigned int target_max = tearing_sim_get_max_target();
    printf("%u targets for tearing simulation\n",target_max);
    led1(1);
    for(volatile unsigned int i=0;i<target_max;i++){//volatile to remove warning about setjump
      printf("tearing simulation target %u\n",i);
      tearing_sim_set_target(i);
      if(0 == (err_code = setjmp(exception_ctx))){
        basic_test();
        write_size_test();
        write_offset_test();
        transaction_basic_test();
        transaction_abort_test();
        err_code = 0;
        led1(0);
      } else {
        exception_handler(err_code);
      }
    }
  #endif
  if(0==err_code){
    #ifdef HAS_TEARING_SIMULATION
      printf("PASS\n");
    #else
      while(1){ui_led1_blink_ms(5000,DUTY_CYCLE_50);}
    #endif
  }else{
    ui_wait_button();
    led1(0);
    while(1);
  }
  return err_code;
}


