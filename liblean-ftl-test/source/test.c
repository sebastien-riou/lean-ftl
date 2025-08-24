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

extern data_flash_t nvm;
extern lftl_nvm_props_t nvm_props;
extern lftl_ctx_t nvma;
extern lftl_ctx_t nvmb;

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

static void stateful_prng_fill(void*buf, uint32_t size){
  static uint8_t rng_state = 0;
  prng_fill(&rng_state,buf,size);
}

typedef void (*write_func_t)(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size);

void (*format_func)(lftl_ctx_t*ctx);
uint8_t (*raw_nvm_write_func)(void*dst_nvm_addr, const void*const src, uintptr_t size);
uint8_t (*raw_nvm_erase_func)(void*base_address, unsigned int n_pages);
void (*erase_all_func)(lftl_ctx_t*ctx);
write_func_t write_func;
void (*transaction_start_func)(lftl_ctx_t*ctx, void *const transaction_tracker);
void (*transaction_write_func)(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);
void (*transaction_write_any_func)(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);
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
uint8_t tearing_sim_nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  //update previous state
  nvm_ref_previous_state = nvm_ref;
  //compute new state
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memcpy(dst+offset,src,size);
  //emulate call to nvm_write rather than calling it, to make sure we do not trigger tearing here
  memcpy(dst_nvm_addr,src,size);
  return 0;
}
uint8_t tearing_sim_nvm_erase(void*base_address, unsigned int n_pages){
  //update previous state
  nvm_ref_previous_state = nvm_ref;
  //compute new state
  uintptr_t offset = (uintptr_t)base_address - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  uintptr_t size = n_pages*FLASH_SW_PAGE_SIZE;
  memset(dst+offset,0xFF,size);
  //emulate call to nvm_erase rather than calling it, to make sure we do not trigger tearing here
  memset(base_address,0xFF,size);
  return 0;
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
  lftl_memread(dst+offset,src,size);
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
  lftl_memread(dst+offset,src,size);
  //call LFTL
  lftl_transaction_write(ctx,dst_nvm_addr,src,size);
}
void tearing_sim_lftl_transaction_write_any(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  //write into transaction buffer
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&transaction_buf_ref;
  lftl_memread(dst+offset,src,size);
  //call LFTL
  lftl_transaction_write_any(ctx,dst_nvm_addr,src,size);
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
  //printf("Simulated tearing\n");//too verbose
  //simulate a reboot
  nvma.data = LFTL_INVALID_POINTER;
  nvma.transaction_tracker = LFTL_INVALID_POINTER;
  nvmb.data = LFTL_INVALID_POINTER;
  nvmb.transaction_tracker = LFTL_INVALID_POINTER;
  check_nvm();
}
void tearing_sim_init();
uint32_t tearing_sim_get_max_target();
void tearing_sim_set_target(uint64_t target_write);


#define PBSTR "||||||||||||||||||||||||||||||||||||||||||||||||||"
#define PBWIDTH 50

void print_progress_bar(size_t val, size_t total) {
  float pf = (val*1.0) / total;
  unsigned int p = (unsigned int)(pf*100);
  unsigned int lpad = (unsigned int) (pf * PBWIDTH);
  unsigned int rpad = PBWIDTH - lpad;
  printf("\r%3d%% [%.*s%*s]", p, lpad, PBSTR, rpad, "");
  fflush(stdout);
}
void print_progress_bar_done(){
  print_progress_bar(1,1);
  printf("\n");
  fflush(stdout);
}

#define SANITY_CHECK() do{if(!nvm_is_equal(&nvm_ref)) throw_exception(INTERNAL_ERROR_CORRUPT);}while(0)
#else
#define SANITY_CHECK()
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

void test_write2(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size, const void*const expected){
  { // sanity check
    uint8_t srcbuf[size];
    lftl_memread(srcbuf,src,size);
    if(memcmp(srcbuf,expected,size)) throw_exception(ERROR_VERIFICATION_FAIL); // error is most likely in calling code
  }
  write_func(ctx,dst_nvm_addr,src,size);
  read_and_check(ctx,dst_nvm_addr,expected,size);
  //force a search of the slot
  ctx->data = LFTL_INVALID_POINTER;
  read_and_check(ctx,dst_nvm_addr,expected,size); 
}



void randomized_test_write(lftl_ctx_t*ctx,void*dst_nvm_addr, uintptr_t size){
  uint8_t wbuf[size];
  stateful_prng_fill(wbuf,size);
  test_write(ctx,dst_nvm_addr,wbuf,size);
}

void basic_test(){
  randomized_test_write(&nvma,nvm.data0,sizeof(lftl_wu_t));
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
  //start at various offset
  uint8_t*base = (uint8_t*)&nvm.b_data;
  for(unsigned int i = 0; i < sizeof(nvm.b_data)-write_size; i+=write_size){
    randomized_test_write(&nvmb,base+i,write_size);
  }
}


#define MAX_NVM_TO_NVM_TEST_SIZE_IN_WU 2
#define MAX_DST_OFFSET_IN_WU 2
void write_nvm_to_nvm_test_core(unsigned int dst_offset, void*test_storage, unsigned int test_storage_size){
  uint8_t*dst;
  if(dst_offset>=MAX_DST_OFFSET_IN_WU*sizeof(lftl_wu_t)) throw_exception(INTERNAL_ERROR_CORRUPT);
  if(test_storage_size>MAX_NVM_TO_NVM_TEST_SIZE_IN_WU*sizeof(lftl_wu_t)) throw_exception(INTERNAL_ERROR_CORRUPT);
  stateful_prng_fill(test_storage, test_storage_size);
  SANITY_CHECK();
  raw_nvm_erase_func(&nvm.unmanaged_data0,1);
  lftl_wu_t aligned_test_storage[4] = {0};//always write 4 WU because implementations may need a multiple of WU size
  memcpy(aligned_test_storage,test_storage,test_storage_size);
  raw_nvm_write_func(&nvm.unmanaged_data0,aligned_test_storage ,sizeof(aligned_test_storage));
  SANITY_CHECK();

  lftl_wu_t*base_b = (lftl_wu_t*)&nvm.b_data;
  //try src in NVM outside of LFTL areas
  dst = ((uint8_t*)base_b) + dst_offset;
  uint8_t*src = dst;
  test_write2(&nvmb,dst,&nvm.unmanaged_data0,test_storage_size,test_storage);
  SANITY_CHECK();
  //set src in area b with same alignement as test_storage
  const bool test_storage_aligned = (uintptr_t)test_storage%2 ? 0 : 1;
  const bool src_aligned = (uintptr_t)src%2 ? 0 : 1;
  if(test_storage_aligned & !src_aligned){
    src = ((uint8_t*)base_b);
    test_write2(&nvmb,src,&nvm.unmanaged_data0,test_storage_size,test_storage);
    SANITY_CHECK();
  }
  if(!test_storage_aligned & src_aligned){
    src = ((uint8_t*)base_b) + 1;
    test_write2(&nvmb,src,&nvm.unmanaged_data0,test_storage_size,test_storage);
    SANITY_CHECK();
  }
  //try dst and src in same LFTL area
  test_write2(&nvmb,base_b+MAX_NVM_TO_NVM_TEST_SIZE_IN_WU+MAX_DST_OFFSET_IN_WU,src,test_storage_size,test_storage);
  SANITY_CHECK();
  //try dst and src in other LFTL area
  lftl_wu_t*base_a = (lftl_wu_t*)&nvm.a_data;
  test_write2(&nvma,base_a,src,test_storage_size,test_storage);
  SANITY_CHECK();
  //write the other LFTL area to move the physical address of the valid data (previous test may have passed by chance)
  test_write(&nvmb,base_b+MAX_NVM_TO_NVM_TEST_SIZE_IN_WU+MAX_DST_OFFSET_IN_WU,test_storage,test_storage_size);
  SANITY_CHECK();
  //retry dst and src in other LFTL area
  test_write2(&nvma,base_a,src,test_storage_size,test_storage);
  SANITY_CHECK();
  
  //now try the 'mirror' cases
  dst_offset = 2*DATA_SIZE - test_storage_size;

  //try src in NVM outside of LFTL areas
  dst = ((uint8_t*)base_b) + dst_offset;
  src = dst;
  test_write2(&nvmb,dst,&nvm.unmanaged_data0,test_storage_size,test_storage);
  SANITY_CHECK();
  //try dst and src in same LFTL area
  test_write2(&nvmb,base_b,src,test_storage_size,test_storage);
  SANITY_CHECK();
  //try dst and src in other LFTL area
  test_write2(&nvma,base_a,src,test_storage_size,test_storage);
  SANITY_CHECK();
  //write the other LFTL area to move the physical address of the valid data (previous test may have passed by chance)
  test_write(&nvmb,base_b,test_storage,test_storage_size);
  SANITY_CHECK();
  //retry dst and src in other LFTL area
  test_write2(&nvma,base_a,src,test_storage_size,test_storage);
  SANITY_CHECK();
}

void write_nvm_to_nvm_vs_size(unsigned int size){
  lftl_wu_t test_storage[4];//write_nvm_to_nvm_test_core assumes test_storage size is 4 WU.
  if(size>sizeof(nvm.unmanaged_data0)) throw_exception(INTERNAL_ERROR_CORRUPT);
  if(size>=sizeof(test_storage)) throw_exception(INTERNAL_ERROR_CORRUPT);
  {
    //aligned src
    write_nvm_to_nvm_test_core(0,&test_storage,size);//aligned dst
    write_nvm_to_nvm_test_core(1,&test_storage,size);//unaligned dst
    write_nvm_to_nvm_test_core(sizeof(lftl_wu_t),&test_storage,size);//aligned dst in second WU
    write_nvm_to_nvm_test_core(1+sizeof(lftl_wu_t),&test_storage,size);//unaligned dst in second WU
  }
  {
    //unaligned src
    uint8_t*test_storage8 = (uint8_t*)test_storage;
    write_nvm_to_nvm_test_core(0,test_storage8+1,size);//aligned dst
    write_nvm_to_nvm_test_core(1,test_storage8+1,size);//unaligned dst
    write_nvm_to_nvm_test_core(sizeof(lftl_wu_t),test_storage8+1,size);//aligned dst in second WU
    write_nvm_to_nvm_test_core(1+sizeof(lftl_wu_t),test_storage8+1,size);//unaligned dst in second WU
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

void write_func_using_transaction(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  uint8_t transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(ctx)];
  transaction_start_func(ctx,transaction_tracker);
  transaction_write_any_func(ctx,dst_nvm_addr,src,size);
  transaction_commit_func(ctx);
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
    printf("ERROR: test failed with error code 0x%08x\n",err_code);
    ui_wait_button();
  }
  #else
  ui_wait_button();
  #endif
}

void test_and_simulate_tearing(void (*dut)()){
  static unsigned int test_cnt=0;
  uint32_t err_code=-1;
  led1(1);
  if(0 == (err_code = setjmp(exception_ctx))){
    #ifdef HAS_TEARING_SIMULATION
    tearing_sim_init(); // ensure any previous tearing sim is stopped at this point
    #endif
    lftl_init_lib();
    lftl_register_area(&nvma);
    lftl_register_area(&nvmb);
    format_func(&nvma);
    format_func(&nvmb);
    #ifdef HAS_TEARING_SIMULATION
    tearing_sim_init();
    #endif
    dut();//functional test
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
    format_func(&nvma);
    format_func(&nvmb);
    for(volatile unsigned int i=0;i<target_max+1;i++){//volatile to remove warning about setjump.
      //if(0 == (i%1000)) printf("tearing simulation target %u\n",i);
      if(0 == (i%50)) print_progress_bar(i,target_max);
      tearing_sim_set_target(i);
      if(0 == (err_code = setjmp(exception_ctx))){
        dut();//simulated tearing test
      } else {
        exception_handler(err_code);
      }
    }
    print_progress_bar_done();
    err_code = 0;
    led1(0);
  #endif
  test_cnt++;
  if(0==err_code){
    #ifdef HAS_TEARING_SIMULATION
      printf("Test %d PASS\n",test_cnt);
    #else
      
    #endif
  }else{
    #ifdef HAS_TEARING_SIMULATION
      exit(err_code);
    #endif
    ui_wait_button();
    led1(0);
    while(1);
  }
}


void write_nvm_to_nvm_vs_size_1()   {write_nvm_to_nvm_vs_size(1);}                    // smaller than WU
void write_nvm_to_nvm_vs_size_1wu() {write_nvm_to_nvm_vs_size(sizeof(lftl_wu_t));}    // single WU
void write_nvm_to_nvm_vs_size_2wu() {write_nvm_to_nvm_vs_size(sizeof(lftl_wu_t)*2);}  // multiple WU
void write_nvm_to_nvm_vs_size_1wu1(){write_nvm_to_nvm_vs_size(sizeof(lftl_wu_t)+1);}  // larger than WU size but not multiple of it

void write_nvm_to_nvm_seq(){
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_1)   ;
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_1wu) ;
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_2wu) ;
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_1wu1);
}
void transaction_nvm_to_nvm_seq(){
  write_func_t org_write_func = write_func;
  write_func = write_func_using_transaction;
  write_nvm_to_nvm_seq();
  write_func = org_write_func;
}


int test_main(){
  #ifdef HAS_TEARING_SIMULATION
    format_func = tearing_sim_lftl_format;
    raw_nvm_write_func = tearing_sim_nvm_write;
    raw_nvm_erase_func = tearing_sim_nvm_erase;
    erase_all_func = tearing_sim_lftl_erase_all;
    write_func = tearing_sim_lftl_write;
    transaction_start_func = tearing_sim_lftl_transaction_start;
    transaction_write_func = tearing_sim_lftl_transaction_write;
    transaction_write_any_func = tearing_sim_lftl_transaction_write_any;
    transaction_commit_func = tearing_sim_lftl_transaction_commit;
    transaction_abort_func = tearing_sim_lftl_transaction_abort;
    printf("version: %s\n",lftl_version());
    printf("version timestamp: %lu\n",lftl_version_timestamp());
    printf("build type: %s\n",lftl_build_type());
  #else
    format_func = lftl_format;
    raw_nvm_write_func = nvm_write;
    raw_nvm_erase_func = nvm_erase;
    erase_all_func = lftl_erase_all;
    write_func = lftl_write;
    transaction_start_func = lftl_transaction_start;
    transaction_write_func = lftl_transaction_write;
    transaction_write_any_func = lftl_transaction_write_any;
    transaction_commit_func = lftl_transaction_commit;
    transaction_abort_func = lftl_transaction_abort;
  #endif
  led1(1);
  test_and_simulate_tearing(basic_test);
  test_and_simulate_tearing(write_size_test);
  test_and_simulate_tearing(write_offset_test);
  test_and_simulate_tearing(transaction_basic_test);
  test_and_simulate_tearing(transaction_abort_test);
  test_and_simulate_tearing(erase_all_test);
  write_nvm_to_nvm_seq();
  transaction_nvm_to_nvm_seq();
  #ifdef HAS_TEARING_SIMULATION
    printf("All tests PASSED\n");
  #else
    while(1){ui_led1_blink_ms(1000,DUTY_CYCLE_75);}
  #endif
  return 0;
}


