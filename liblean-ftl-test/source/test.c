#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
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

//dummy implementations
void __attribute__((weak)) dump_core(uintptr_t addr, uintptr_t size, uintptr_t display_addr){}
void __attribute__((weak)) dump(uintptr_t addr, uintptr_t size){}

//Application
#include <stdio.h>
static jmp_buf exception_ctx;
void throw_exception(uint32_t err_code){
  longjmp(exception_ctx,err_code);
}
//abort() does not flush stdio: with the output in a file, the message that
//explains the failure would be lost
static void fail_abort(void){
  fflush(stdout);
  abort();
}

//order of these headers matters
#include "ui.h"
#include "nvm_state.h"

//LFTL_AREA reserves room for the meta-data as laid out at run time: items of at
//least 4 bytes, even with smaller write units. Data 8 bytes short of a page
//then needs 2 pages per slot, so a wear-leveling factor of 2 needs 4 pages.
typedef struct {
  LFTL_AREA(macro_check, uint8_t macro_check_content[LFTL_PAGE_SIZE-8];, LFTL_WEAR_LEVELING_FACTOR(2))
} lftl_area_macro_check_t;
_Static_assert(sizeof(((lftl_area_macro_check_t*)0)->macro_check_pages) == 4*LFTL_PAGE_SIZE,
  "LFTL_AREA must count meta-data items of at least 4 bytes");

extern data_flash_t nvm;
extern lftl_nvm_props_t nvm_props;
extern lftl_ctx_t nvma;
extern lftl_ctx_t nvmb;
extern lftl_ctx_t ewlfa;
extern lftl_ctx_t ewlfb;
extern lftl_nvm_props_t nvm_props2;
extern lftl_ctx_t nvmc;
extern lftl_nvm_props_t nvm_props3;
extern lftl_ctx_t nvmd;

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

//a better PRNG (but still very bad for anything else than testing/debugging)
static uint32_t xs_prng_seed;
static void xs_prng_set_seed(uint32_t seed){
	if(0==seed) seed = 0xFFFFFFFF;
	xs_prng_seed = seed;
}

static uint32_t xs_prng_get(){
	// Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs"
	uint32_t x = xs_prng_seed;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return xs_prng_seed = x;
}

static void xs_prng_fill(void*dst, uintptr_t size){
  uint8_t*dst8 = (uint8_t*)dst;
  uint32_t buf;
  uint8_t*buf8 = (uint8_t*)&buf;
  for(unsigned int i=0;i<size;i++){
    if(0 == i%4){
      buf = xs_prng_get();
    }
    dst8[i] = buf8[i%4];
  }
}

typedef void (*write_func_t)(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size);

void (*format_func)(lftl_ctx_t*ctx);
uint8_t (*raw_nvm_write_func)(void*dst_nvm_addr, const void*const src, uintptr_t size);
uint8_t (*raw_nvm_erase_func)(void*base_address, unsigned int n_pages);

//Raw NVM accesses build test states directly, bypassing lean-ftl. They must
//succeed: on real flash, that means programming erased, whole write units.
static void raw_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(raw_nvm_write_func(dst_nvm_addr,src,size)) throw_exception(INTERNAL_ERROR_NVM_WRITE);
}
static void raw_erase(void*base_address, unsigned int n_pages){
  if(raw_nvm_erase_func(base_address,n_pages)) throw_exception(INTERNAL_ERROR_NVM_WRITE);
}
//Whether every byte of a physical range reads erased. Read through the raw
//callback: lftl_memread would take an address inside an area's logical range
//for a logical one, bounded by its data size. A failure is raised as the
//library raises it, so that a simulated tearing is handled as one
static bool raw_reads_erased(const uint8_t*addr, uintptr_t size){
  uint8_t buf[64];
  while(size){
    const uintptr_t chunk = size < sizeof(buf) ? size : sizeof(buf);
    const uint8_t status = nvm_read(buf,addr,chunk);
    if(status) throw_exception(LFTL_ERROR_LOW_LEVEL_READ | status);
    for(uintptr_t i=0;i<chunk;i++) if(0xFF != buf[i]) return false;
    addr += chunk;
    size -= chunk;
  }
  return true;
}
void (*erase_all_func)(lftl_ctx_t*ctx);
write_func_t write_func;
write_func_t write_any_func;
void (*transaction_start_func)(lftl_ctx_t*ctx, void *const transaction_tracker);
void (*transaction_write_func)(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);
void (*transaction_write_any_func)(lftl_ctx_t*ctx, void*dst_nvm_addr, const void*const src, uintptr_t size);
void (*transaction_commit_func)(lftl_ctx_t*ctx);
void (*transaction_abort_func)(lftl_ctx_t*ctx);

//the one cut --tear-at= runs, 0 for all of them: parsed on every build, read
//by the tearing simulation
static uint64_t tear_at;

#ifdef HAS_TEARING_SIMULATION
#include <stdio.h>
#include <stdlib.h>
extern bool nvm_zero_overwrite; //the simulated flash's all-zero exception, see linux-accessors.c
//the weak word model of linux-accessors.c (T2 of the flash page without T3),
//its seed, the scopes it applies to, and the reading a test can script
extern bool nvm_weak_model;
extern uint64_t nvm_weak_seed;
extern bool (*nvm_weak_override)(const void*unit_nvm_addr, uint8_t*unit, uintptr_t size);
void tearing_sim_weak_scope_clear();
void tearing_sim_weak_scope_add(const void*lo, uintptr_t size);
void tearing_sim_weak_scope_pop();
void tearing_sim_weak_clear_range(const void*addr, uintptr_t size);
bool tearing_sim_is_weak(const void*addr, uintptr_t size);
void tearing_sim_weak_reseed(uint64_t seed);
void tearing_sim_weak_new_boot();
//a cut at a program call, for a second cut inside the recovery from the first
void tearing_sim_set_call_target(uint64_t k, bool at_last_unit);
void tearing_sim_clear_call_target();
uint64_t tearing_sim_get_call_count();
//what the sweep is doing, for the failure messages: the cut and the second
//cut (-1: none)
static uint64_t current_cut_target;
static int64_t current_secondary_target = -1;
static void print_cut_context(){
  PRINTF("  cut %llu, second cut %lld, weak words %s, weak seed %llu\n",
    (unsigned long long)current_cut_target,(long long)current_secondary_target,
    nvm_weak_model ? "on" : "off",(unsigned long long)nvm_weak_seed);
}
data_flash_t nvm_ref;
data_flash_t nvm_ref_previous_state;
data_flash_t transaction_buf_ref;

//The areas check_nvm verifies after a simulated cut, and whether each one was
//seen reading the new state since the reference last moved: a rollback is an
//area that reads the new state at one boot and the old one at a later boot,
//with no write in between
#define N_CHECKED_AREAS 4
static bool area_seen_new[N_CHECKED_AREAS];
//Every modelled operation moves the reference: the state it is to produce
//becomes the new one, the previous new state the old one
static void ref_advance(){
  nvm_ref_previous_state = nvm_ref;
  memset(area_seen_new,0,sizeof(area_seen_new));
}

#define N_ERASE_UNITS (sizeof(data_flash_t) / (LFTL_PAGE_SIZE))
uint32_t erase_cnt[N_ERASE_UNITS];
volatile bool erase_cnt_enabled;
void erase_cnt_init(){
  memset(erase_cnt,0,sizeof erase_cnt);
}
void erase_cnt_inc(unsigned int index){
  if(erase_cnt_enabled){
    const uint32_t current = erase_cnt[index];
    if(current != 0xFFFFFFFF){
      const uint32_t next = current + 1;
      erase_cnt[index] = next;
    }
  }
}
void erase_cnt_update(void*base_address, unsigned int n_pages){
  uintptr_t offset = (uintptr_t)base_address - (uintptr_t)&nvm;
  unsigned int first_page = offset/LFTL_PAGE_SIZE;
  for(unsigned int i=0;i<n_pages;i++){
    erase_cnt_inc(first_page+i);
  }
}
void print_erase_cnt_stats(){
  uint32_t min = 0xFFFFFFFF;
  uint32_t max = 0;
  uint64_t sum = 0;
  for(unsigned int i=0;i<N_ERASE_UNITS;i++){
    const uint32_t n = erase_cnt[i];
    sum += n;
    if(n<min) min = n;
    if(n>max) max = n;
  }
  uint32_t avg = sum / N_ERASE_UNITS;
  PRINTLN("Pages erase count statistics:");
  PRINTLN("   %lu page erase in total", sum);
  PRINTLN("   %lu pages (size of data_flash_t = %lu, page size = %u)", N_ERASE_UNITS, sizeof(data_flash_t), LFTL_PAGE_SIZE);
  PRINTLN("   Min: %u", min);
  PRINTLN("   Avg: %u", avg);
  PRINTLN("   Max: %u", max);
}

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
  //refuse what nvm_write refuses, so that tests only build states real flash can hold
  if(0 != ((uintptr_t)dst_nvm_addr % nvm_write_size)) return 3;
  if(0 != (size % nvm_write_size)) return 4;
  const uint8_t*const dst8 = (const uint8_t*)dst_nvm_addr;
  const uint8_t*const src8 = (const uint8_t*)src;
  for(uintptr_t i=0;i<size;i+=nvm_write_size){
    bool erased = 1;
    bool zero = 1;
    for(uintptr_t j=i;j<i+nvm_write_size;j++){
      if(0xFF != dst8[j]) erased = 0;
      if(0x00 != src8[j]) zero = 0;
    }
    if(!erased && !(nvm_zero_overwrite && zero)) return 5;
  }
  ref_advance();
  //compute new state
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memcpy(dst+offset,src,size);
  //emulate call to nvm_write rather than calling it, to make sure we do not trigger tearing here
  memcpy(dst_nvm_addr,src,size);
  tearing_sim_weak_clear_range(dst_nvm_addr,size);
  return 0;
}
uint8_t tearing_sim_nvm_erase(void*base_address, unsigned int n_pages){
  ref_advance();
  //compute new state
  uintptr_t offset = (uintptr_t)base_address - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  uintptr_t size = n_pages*LFTL_PAGE_SIZE;
  memset(dst+offset,0xFF,size);
  //emulate call to nvm_erase rather than calling it, to make sure we do not trigger tearing here
  memset(base_address,0xFF,size);
  tearing_sim_weak_clear_range(base_address,size);
  //for(unsigned int i=offset/LFTL_PAGE_SIZE;i<n_pages;i++){
  //  erase_cnt_inc(i);
  //}
  return 0;
}
void tearing_sim_lftl_erase_all(lftl_ctx_t*ctx){
  ref_advance();
  //compute new state
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memset(dst,0xFF,ctx->data_size);
  //call LFTL
  lftl_erase_all(ctx);
}
void tearing_sim_lftl_write(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  ref_advance();
  //compute new state
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  lftl_memread(dst+offset,src,size);
  //call LFTL
  lftl_write(ctx,dst_nvm_addr,src,size);
}
void tearing_sim_lftl_write_any(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  //lftl_write_any dispatches to lftl_basic_write or lftl_transaction_write_any
  //depending on whether a transaction is ongoing; mirror that here so the
  //reference model (nvm_ref vs transaction_buf_ref) is updated the same way
  //tearing_sim_lftl_write/tearing_sim_lftl_transaction_write_any do.
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  if(ctx->transaction_tracker == LFTL_INVALID_POINTER){
    ref_advance();
    //compute new state
    uint8_t*dst = (uint8_t*)&nvm_ref;
    lftl_memread(dst+offset,src,size);
  } else {
    //write into transaction buffer
    uint8_t*dst = (uint8_t*)&transaction_buf_ref;
    lftl_memread(dst+offset,src,size);
  }
  //call LFTL
  lftl_write_any(ctx,dst_nvm_addr,src,size);
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
  ref_advance();
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
  lftl_read(&ewlfa,&read_val.ewlfa_data,&nvm.ewlfa_data,sizeof(nvm.ewlfa_data));
  if(memcmp(&read_val.ewlfa_data,&expected->ewlfa_data,sizeof(nvm.ewlfa_data))) return 0;
  lftl_read(&ewlfb,&read_val.ewlfb_data,&nvm.ewlfb_data,sizeof(nvm.ewlfb_data));
  if(memcmp(&read_val.ewlfb_data,&expected->ewlfb_data,sizeof(nvm.ewlfb_data))) return 0;
  return 1;
}
//What an area reads after a simulated cut, against the reference model: the
//state the interrupted operation was to produce, the one before it, or
//neither. The operation touched one area, so the two references agree on the
//others, which then read as new.
typedef enum { AREA_NEW, AREA_OLD, AREA_CORRUPT } area_state_t;
static area_state_t area_state(lftl_ctx_t*ctx, void*data, const void*ref, const void*prev, uintptr_t size){
  uint8_t read_val[size];
  lftl_read(ctx,read_val,data,size);
  if(0 == memcmp(read_val,ref,size)) return AREA_NEW;
  if(0 == memcmp(read_val,prev,size)) return AREA_OLD;
  return AREA_CORRUPT;
}
static const char*const checked_area_names[N_CHECKED_AREAS] = {"a","b","ewlfa","ewlfb"};
static void nvm_states(area_state_t states[N_CHECKED_AREAS]){
  states[0] = area_state(&nvma,&nvm.a_data,&nvm_ref.a_data,&nvm_ref_previous_state.a_data,sizeof(nvm.a_data));
  states[1] = area_state(&nvmb,&nvm.b_data,&nvm_ref.b_data,&nvm_ref_previous_state.b_data,sizeof(nvm.b_data));
  states[2] = area_state(&ewlfa,&nvm.ewlfa_data,&nvm_ref.ewlfa_data,&nvm_ref_previous_state.ewlfa_data,sizeof(nvm.ewlfa_data));
  states[3] = area_state(&ewlfb,&nvm.ewlfb_data,&nvm_ref.ewlfb_data,&nvm_ref_previous_state.ewlfb_data,sizeof(nvm.ewlfb_data));
}
//One boot: every checked area reads the new state or the old one, and none
//that read the new state since the reference last moved falls back to the old
//one (the anti-tearing page's definition of a rollback)
void check_nvm(){
  area_state_t states[N_CHECKED_AREAS];
  nvm_states(states);
  for(unsigned int i=0;i<N_CHECKED_AREAS;i++){
    if(AREA_CORRUPT == states[i]){
      PRINTF("NVM corrupted: area %s reads neither the new state nor the old one\n",checked_area_names[i]);
      print_cut_context();
      fail_abort();
    }
    if(AREA_NEW == states[i]){
      area_seen_new[i] = 1;
    } else if(area_seen_new[i]){
      PRINTF("NVM rollback: area %s read the new state at an earlier boot and the old one now\n",checked_area_names[i]);
      print_cut_context();
      fail_abort();
    }
  }
}
static void simulate_reboot(){
  tearing_sim_weak_new_boot(); //a weak word reads a fresh value
  nvma.data = LFTL_INVALID_POINTER;
  nvma.transaction_tracker = LFTL_INVALID_POINTER;
  nvmb.data = LFTL_INVALID_POINTER;
  nvmb.transaction_tracker = LFTL_INVALID_POINTER;
  ewlfa.data = LFTL_INVALID_POINTER;
  ewlfa.transaction_tracker = LFTL_INVALID_POINTER;
  ewlfb.data = LFTL_INVALID_POINTER;
  ewlfb.transaction_tracker = LFTL_INVALID_POINTER;
  nvmc.data = LFTL_INVALID_POINTER;
  nvmc.transaction_tracker = LFTL_INVALID_POINTER;
  nvmd.data = LFTL_INVALID_POINTER;
  nvmd.transaction_tracker = LFTL_INVALID_POINTER;
}
//Boots in a row with no write in between: what a boot decides about the state
//a cut left must hold at the next ones. With the simulator's fixed cut model
//every boot reads the same words and decides the same, so one boot is enough;
//a flash model where a word reads differently at each boot asks for more.
static unsigned int n_check_boots = 1;
void tearing_sim_check_nvm(){
  for(unsigned int boot=0;boot<n_check_boots;boot++){
    simulate_reboot();
    check_nvm();
  }
}
void tearing_sim_write_area(lftl_ctx_t*ctx, void*area_data, uintptr_t size){
  static uint8_t rng_state = 0x80;
  uint8_t buf[size];
  prng_fill(&rng_state,buf,size);
  tearing_sim_lftl_write(ctx,area_data,buf,size);
}
//The first write after recovering from a tearing lands on the state the
//tearing left behind: write new data to every area check_nvm verifies, then
//check them again after another simulated reboot. nvmc and nvmd are left
//out: some tests use them as scratch for hand-built states.
void tearing_sim_write_after_recovery(){
  tearing_sim_write_area(&nvma,&nvm.a_data,sizeof(nvm.a_data));
  tearing_sim_write_area(&nvmb,&nvm.b_data,sizeof(nvm.b_data));
  tearing_sim_write_area(&ewlfa,&nvm.ewlfa_data,sizeof(nvm.ewlfa_data));
  tearing_sim_write_area(&ewlfb,&nvm.ewlfb_data,sizeof(nvm.ewlfb_data));
  tearing_sim_check_nvm();
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
  PRINTF("\r%3d%% [%.*s%*s]", p, lpad, PBSTR, rpad, "");
  fflush(stdout);
}
void print_progress_bar_done(){
  print_progress_bar(1,1);
  PRINTF("\n");
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
      PRINTF("READ VALUE:\n\r");
      dump_core((uintptr_t)rbuf,size,(uintptr_t)nvm_addr);
      PRINTF("EXPECTED:\n\r");
      dump((uintptr_t)expected,size);
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
      PRINTF("READ VALUE (newer):\n\r");
      dump_core((uintptr_t)rbuf,size,(uintptr_t)nvm_addr);
      PRINTF("EXPECTED:\n\r");
      dump((uintptr_t)expected,size);
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
  DEBUG_PRINTLN("basic_test");
  randomized_test_write(&nvma,nvm.data0,sizeof(lftl_wu_t));
  randomized_test_write(&nvma,nvm.data0,sizeof(nvm.data0));
  randomized_test_write(&nvma,nvm.data1,sizeof(nvm.data1));
  randomized_test_write(&nvmb,nvm.data2,sizeof(nvm.data2));
  randomized_test_write(&nvmb,nvm.data3,sizeof(nvm.data3));
  randomized_test_write(&nvma,&nvm.a_data,sizeof(nvm.a_data));
  randomized_test_write(&nvmb,&nvm.b_data,sizeof(nvm.b_data));
}

void erase_all_test_core(lftl_ctx_t*ctx){
  erase_all_func(ctx);
  const uintptr_t size = SIZE64(ctx->data_size);
  uint8_t*addr = (uint8_t*)ctx->area;
  uint64_t buf[SIZE64(size)];
  xs_prng_fill(buf,size);
  write_func(ctx,addr,buf,size);
  read_and_check(ctx,addr,buf,size);
}

void erase_all_test(){
  DEBUG_PRINTLN("erase_all_test");
  //test erase indirectly because some flash does not read as 0xFF
  //instead we merely check that we can write properly after calling erase_all
  //we do it several times to ensure we covered the whole area physically
  const unsigned int nloops = LFTL_DIV_CEIL(sizeof(nvm.a_pages),LFTL_PAGE_SIZE); 
  for(unsigned int i=0;i<nloops;i++){
    erase_all_test_core(&nvma);
  }
}

void write_size_test(){
  DEBUG_PRINTLN("write_size_test");
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
  DEBUG_PRINTLN("write_offset_test");
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
  DEBUG_PRINTLN("write_nvm_to_nvm_test_core(%u,%p,%u) entry",dst_offset,test_storage,test_storage_size);
  uint8_t*dst;
  if(dst_offset>=MAX_DST_OFFSET_IN_WU*sizeof(lftl_wu_t)) throw_exception(INTERNAL_ERROR_CORRUPT);
  if(test_storage_size>MAX_NVM_TO_NVM_TEST_SIZE_IN_WU*sizeof(lftl_wu_t)) throw_exception(INTERNAL_ERROR_CORRUPT);
  stateful_prng_fill(test_storage, test_storage_size);
  SANITY_CHECK();
  raw_erase(&nvm.unmanaged_data0,1);
  lftl_wu_t aligned_test_storage[4] = {0};//always write 4 WU because implementations may need a multiple of WU size
  memcpy(aligned_test_storage,test_storage,test_storage_size);
  raw_write(&nvm.unmanaged_data0,aligned_test_storage ,sizeof(aligned_test_storage));
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
  if(test_storage_aligned){//} & !src_aligned){
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
  DEBUG_PRINTLN("\ttry dst and src in same LFTL area");
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
  DEBUG_PRINTLN("\tnow try the 'mirror' cases");
  dst_offset = 2*DATA_SIZE - test_storage_size;
  //try src in NVM outside of LFTL areas
  dst = ((uint8_t*)base_b) + dst_offset;
  src = dst;
  test_write2(&nvmb,dst,&nvm.unmanaged_data0,test_storage_size,test_storage);
  SANITY_CHECK();
  //try dst and src in same LFTL area
  DEBUG_PRINTLN("\ttry dst and src in same LFTL area");
  test_write2(&nvmb,base_b,src,test_storage_size,test_storage);
  SANITY_CHECK();
  //try dst and src in other LFTL area
  DEBUG_PRINTLN("\ttry dst and src in other LFTL area");
  test_write2(&nvma,base_a,src,test_storage_size,test_storage);
  SANITY_CHECK();
  //write the other LFTL area to move the physical address of the valid data (previous test may have passed by chance)
  DEBUG_PRINTLN("\twrite the other LFTL area to move the physical address of the valid data");
  test_write(&nvmb,base_b,test_storage,test_storage_size);
  SANITY_CHECK();
  //retry dst and src in other LFTL area
  DEBUG_PRINTLN("\tretry dst and src in other LFTL area");
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
  DEBUG_PRINTLN("transaction_basic_test");
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
    //unaligned read spanning written and unwritten write units
    uint8_t newer[size];
    memcpy(newer,wbuf0,size);
    memcpy(newer,wbuf,write_size);
    memcpy(newer+data_size,wbuf+write_size,write_size);
    read_newer_and_check(&nvma,((uint8_t*)&nvm.a_data)+1,newer+1,size-2);
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
  DEBUG_PRINTLN("transaction_abort_test");
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

void ewlf_basic_test(){
  DEBUG_PRINTLN(__func__);
  lftl_ctx_t*ctx = &ewlfa;
  void*dst_nvm_addr = nvm.data4;
  uintptr_t size = sizeof(nvm.data4);
  uint8_t src[size];
  for(unsigned int i=0;i<50;i++){
    stateful_prng_fill(src,size);
    write_func(ctx,dst_nvm_addr,src,size);
    read_and_check(ctx,dst_nvm_addr,src,size);
  }
}

//Sum of erase_cnt[] over every physical page belonging to an EWLF area's
//own erase units (skips ctx->area's own page range computation duplicating
//ftl_internal.h's private record layout: this only needs page granularity,
//which is already public via ctx->area/ctx->area_size/LFTL_PAGE_SIZE).
#ifdef HAS_TEARING_SIMULATION
uint32_t ewlf_area_erase_cnt_sum(lftl_ctx_t*ctx){
  const unsigned int first_page = ((uintptr_t)ctx->area - (uintptr_t)&nvm) / LFTL_PAGE_SIZE;
  const unsigned int n_area_pages = ctx->area_size / LFTL_PAGE_SIZE;
  uint32_t sum = 0;
  for(unsigned int i=0;i<n_area_pages;i++){
    sum += erase_cnt[first_page+i];
  }
  return sum;
}
#else
//erase_cnt[] only exists in the tearing simulation: on a target there is
//nothing to count, and the checks based on it are skipped
uint32_t ewlf_area_erase_cnt_sum(lftl_ctx_t*ctx){
  (void)ctx;
  return 0;
}
static const bool erase_cnt_enabled = 0;
#endif

void ewlf_packing_test(){
  DEBUG_PRINTLN(__func__);
  lftl_ctx_t*ctx = &ewlfa;
  void*dst_nvm_addr = nvm.data4;
  uintptr_t size = sizeof(nvm.data4);
  uint8_t src[size];

  //right after the preamble's format, exactly one record has been written
  //(in the topmost erase unit); a real EWLF area packs several records per
  //erase unit, so one more write here must NOT need a fresh erase.
  //erase_cnt[] is only tracked during test_and_simulate_tearing's own
  //untorn reference run (erase_cnt_enabled is off for the rest of its
  //tearing-injection sweep), so this check is only meaningful then.
  const uint32_t before = ewlf_area_erase_cnt_sum(ctx);
  stateful_prng_fill(src,size);
  write_func(ctx,dst_nvm_addr,src,size);
  read_and_check(ctx,dst_nvm_addr,src,size);
  const uint32_t after = ewlf_area_erase_cnt_sum(ctx);
  if(erase_cnt_enabled && (after != before)) throw_exception(ERROR_VERIFICATION_FAIL);
}

void ewlf_rollover_test(){
  DEBUG_PRINTLN(__func__);
  lftl_ctx_t*ctx = &ewlfa;
  void*dst_nvm_addr = nvm.data4;
  uintptr_t size = sizeof(nvm.data4);
  uint8_t src[size];

  //enough writes to force at least one rollover to a fresh erase unit,
  //given only 2-3 records fit per unit for this area's sizing; also
  //exercises a rollover's erase of the unit it enters (and, under the
  //tearing-simulation sweep, its recovery) repeatedly. As above, the
  //erase_cnt[]-based check is only meaningful during the untorn reference
  //run (erase_cnt_enabled is off during the sweep itself), so only
  //enforce it then -- correctness of the writes/reads themselves is still
  //fully exercised either way via read_and_check.
  uint32_t prev_sum = ewlf_area_erase_cnt_sum(ctx);
  bool saw_new_erase = 0;
  for(unsigned int i=0;i<20;i++){
    stateful_prng_fill(src,size);
    write_func(ctx,dst_nvm_addr,src,size);
    read_and_check(ctx,dst_nvm_addr,src,size);
    const uint32_t sum = ewlf_area_erase_cnt_sum(ctx);
    if(sum > prev_sum) saw_new_erase = 1;
    prev_sum = sum;
  }
  if(erase_cnt_enabled && !saw_new_erase) throw_exception(ERROR_VERIFICATION_FAIL);
}

void multi_nvm_test(){
  DEBUG_PRINTLN(__func__);
  // nvmc/nvmd each use their own, independently registered lftl_nvm_props_t
  // (nvm_props2/nvm_props3), distinct from the shared nvm_props used by
  // nvma/nvmb/ewlfa/ewlfb. Writing to all three, each forcing a fresh slot
  // search (see test_write), exercises the nvm_props linked list
  // (lftl_register_nvm/addr_to_nvm) with three registered NVMs, not just two
  // — the case that actually requires lftl_register_nvm to correctly append
  // (rather than just insert-after-head-with-only-one-existing-node).
  randomized_test_write(&nvmc,nvm.data6,sizeof(nvm.data6));
  randomized_test_write(&nvmd,nvm.data7,sizeof(nvm.data7));
  randomized_test_write(&nvma,nvm.data0,sizeof(nvm.data0));
}

void write_any_test(){
  DEBUG_PRINTLN(__func__);
  const uint32_t write_size = nvmb.nvm_props->write_size;
  uint8_t wbuf[sizeof(nvm.b_data)];

  //no active transaction: lftl_write_any dispatches to lftl_basic_write
  stateful_prng_fill(wbuf,sizeof(nvm.data2));
  write_any_func(&nvmb,nvm.data2,wbuf,sizeof(nvm.data2));
  read_and_check(&nvmb,nvm.data2,wbuf,sizeof(nvm.data2));
  SANITY_CHECK();

  //unaligned range, still no active transaction
  uint8_t*const data3_base = (uint8_t*)nvm.data3;
  const uintptr_t unaligned_size = sizeof(nvm.data3)-write_size;
  stateful_prng_fill(wbuf,unaligned_size);
  write_any_func(&nvmb,data3_base+1,wbuf,unaligned_size);
  read_and_check(&nvmb,data3_base+1,wbuf,unaligned_size);
  SANITY_CHECK();

  //size==0 is a no-op
  write_any_func(&nvmb,nvm.data2,wbuf,0);
  SANITY_CHECK();

  //active transaction: lftl_write_any dispatches to lftl_transaction_write_any
  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvmb)];
  transaction_start_func(&nvmb,tracker);
  stateful_prng_fill(wbuf,write_size);
  write_any_func(&nvmb,nvm.data2,wbuf,write_size);
  read_newer_and_check(&nvmb,nvm.data2,wbuf,write_size);
  transaction_commit_func(&nvmb);
  read_and_check(&nvmb,nvm.data2,wbuf,write_size);
  SANITY_CHECK();
}

void get_ctx_test(){
  DEBUG_PRINTLN(__func__);

  //lftl_get_ctx_std only searches std areas
  if(lftl_get_ctx_std(nvm.data0) != &nvma) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_std(nvm.data2) != &nvmb) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_std(nvm.data6) != &nvmc) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_std(nvm.data4) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_std(&nvm.unmanaged_data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);

  //lftl_get_ctx_ewlf only searches EWLF areas
  if(lftl_get_ctx_ewlf(nvm.data4) != &ewlfa) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_ewlf(nvm.data5) != &ewlfb) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_ewlf(nvm.data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_ewlf(&nvm.unmanaged_data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);

  //lftl_get_ctx searches every area type
  if(lftl_get_ctx(nvm.data0) != &nvma) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx(nvm.data2) != &nvmb) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx(nvm.data6) != &nvmc) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx(nvm.data4) != &ewlfa) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx(nvm.data5) != &ewlfb) throw_exception(ERROR_VERIFICATION_FAIL);
  //an address not managed by any LFTL area
  if(lftl_get_ctx(&nvm.unmanaged_data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);
}

void memread_newer_test(){
  DEBUG_PRINTLN(__func__);
  const uint32_t write_size = nvm_props.write_size;

  //regular (non-NVM) memory: plain memcpy fallback
  uint8_t src_mem[16];
  uint8_t dst_mem[16];
  stateful_prng_fill(src_mem,sizeof(src_mem));
  lftl_memread_newer(dst_mem,src_mem,sizeof(src_mem));
  if(memcmp(dst_mem,src_mem,sizeof(src_mem))) throw_exception(ERROR_VERIFICATION_FAIL);

  //physical NVM address outside of any LFTL area: falls back to nvm_read
  uint8_t wbuf[16];
  stateful_prng_fill(wbuf,sizeof(wbuf));
  raw_erase(&nvm.unmanaged_data0,1);
  raw_write(&nvm.unmanaged_data0,wbuf,sizeof(wbuf));
  uint8_t rbuf[16];
  lftl_memread_newer(rbuf,&nvm.unmanaged_data0,sizeof(wbuf));
  if(memcmp(rbuf,wbuf,sizeof(wbuf))) throw_exception(ERROR_VERIFICATION_FAIL);
  SANITY_CHECK();

  //inside an LFTL area, no active transaction: same as current data
  uint8_t wbuf2[write_size];
  stateful_prng_fill(wbuf2,write_size);
  test_write(&nvma,nvm.data0,wbuf2,write_size);
  uint8_t rbuf2[write_size];
  lftl_memread_newer(rbuf2,nvm.data0,write_size);
  if(memcmp(rbuf2,wbuf2,write_size)) throw_exception(ERROR_VERIFICATION_FAIL);
  SANITY_CHECK();

  //inside an LFTL area, with an active transaction: reads the new (uncommitted)
  //data, which must differ from what lftl_memread (current data) still sees.
  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  transaction_start_func(&nvma,tracker);
  uint8_t wbuf3[write_size];
  stateful_prng_fill(wbuf3,write_size);
  transaction_write_func(&nvma,nvm.data0,wbuf3,write_size);

  uint8_t current[write_size];
  lftl_memread(current,nvm.data0,write_size);
  if(memcmp(current,wbuf2,write_size)) throw_exception(ERROR_VERIFICATION_FAIL);//still the old/committed value

  uint8_t newer[write_size];
  lftl_memread_newer(newer,nvm.data0,write_size);
  if(memcmp(newer,wbuf3,write_size)) throw_exception(ERROR_VERIFICATION_FAIL);//the new, uncommitted value

  transaction_commit_func(&nvma);
  lftl_memread_newer(newer,nvm.data0,write_size);
  if(memcmp(newer,wbuf3,write_size)) throw_exception(ERROR_VERIFICATION_FAIL);//now the committed value too
  SANITY_CHECK();
}

// Runs `stmt` expecting it to call raise_error (via longjmp to exception_ctx)
// with exactly `expected_err_code`; aborts (nonzero exit, no hang) if it
// raises the wrong code or doesn't raise at all. exception_ctx is a single
// global jmp_buf also used by test_and_simulate_tearing's own outer catch,
// so this saves/restores it around the local setjmp: without that, once
// this macro's setjmp target has been reached and left, it stays "primed"
// for the rest of the enclosing function, and an unrelated tearing fault
// raised by a later, unwrapped statement would wrongly longjmp back into
// this (already-returned) checkpoint instead of the outer handler. The
// restore runs before the match/mismatch check so a mismatch here is
// always a genuine unexpected code, not a stale-jmp_buf artifact.
#define EXPECT_ERROR(expected_err_code, stmt) do{\
    jmp_buf __saved_ctx;\
    memcpy(__saved_ctx, exception_ctx, sizeof(jmp_buf));\
    uint32_t __err_code = setjmp(exception_ctx);\
    if(0 == __err_code){\
      stmt;\
      memcpy(exception_ctx, __saved_ctx, sizeof(jmp_buf));\
      PRINTF("ERROR: expected err_code 0x%08x but none was raised\n",(unsigned)(expected_err_code));\
      fail_abort();\
    }\
    memcpy(exception_ctx, __saved_ctx, sizeof(jmp_buf));\
    if(__err_code != (uint32_t)(expected_err_code)){\
      PRINTF("ERROR: expected err_code 0x%08x but got 0x%08x\n",(unsigned)(expected_err_code),(unsigned)__err_code);\
      fail_abort();\
    }\
  }while(0)

void zero_size_test(){
  DEBUG_PRINTLN(__func__);
  uint8_t buf[16] = {0};
  lftl_basic_write(&nvma,nvm.data0,buf,0);
  lftl_read(&nvma,buf,nvm.data0,0);
  lftl_write(&nvma,nvm.data0,buf,0);
  lftl_read_newer(&nvma,buf,nvm.data0,0);
  SANITY_CHECK();

  //size==0 during an active transaction
  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  transaction_start_func(&nvma,tracker);
  lftl_transaction_read(&nvma,buf,nvm.data0,0);
  transaction_abort_func(&nvma);
  SANITY_CHECK();
}

static uint8_t failing_nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  (void)dst_nvm_addr;
  (void)src;
  (void)size;
  return 1;
}

void transaction_misuse_test(){
  DEBUG_PRINTLN(__func__);
  const uint32_t write_size = nvm_props.write_size;
  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  uint8_t tracker2[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  uint8_t buf[write_size];
  stateful_prng_fill(buf,write_size);

  //lftl_transaction_write / _commit / _read / _write_any with no active transaction
  EXPECT_ERROR(LFTL_ERROR_NO_TRANSACTION, lftl_transaction_write(&nvma,nvm.data0,buf,write_size));
  EXPECT_ERROR(LFTL_ERROR_NO_TRANSACTION, lftl_transaction_commit(&nvma));
  EXPECT_ERROR(LFTL_ERROR_NO_TRANSACTION, lftl_transaction_read(&nvma,buf,nvm.data0,write_size));
  //unaligned lftl_transaction_write_any with no active transaction
  EXPECT_ERROR(LFTL_ERROR_NO_TRANSACTION, lftl_transaction_write_any(&nvma,((uint8_t*)nvm.data0)+1,buf,write_size));
  SANITY_CHECK();

  //each misuse below raises an error while a transaction is active, which
  //aborts that transaction: it may be inconsistent, and the error handler may
  //unwind the frame holding its tracker
  #define EXPECT_ERROR_ABORTS(ctx, expected_err_code, stmt) do{\
      EXPECT_ERROR(expected_err_code, stmt);\
      if(LFTL_INVALID_POINTER != (ctx)->transaction_tracker) throw_exception(ERROR_VERIFICATION_FAIL);\
    }while(0)

  //starting a second transaction while one is already active
  transaction_start_func(&nvma,tracker);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_TRANSACTION_ONGOING, lftl_transaction_start(&nvma,tracker2));

  //calling the low-level lftl_basic_write directly while a transaction is
  //active, bypassing the public lftl_write dispatcher
  transaction_start_func(&nvma,tracker);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_TRANSACTION_ONGOING, lftl_basic_write(&nvma,nvm.data0,buf,write_size));

  //calling lftl_erase_all while a transaction is active
  transaction_start_func(&nvma,tracker);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_TRANSACTION_ONGOING, lftl_erase_all(&nvma));

  //write the same write unit twice in one transaction
  transaction_start_func(&nvma,tracker);
  transaction_write_func(&nvma,nvm.data0,buf,write_size);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_TRANSACTION_OVERWRITE, lftl_transaction_write(&nvma,nvm.data0,buf,write_size));

  //same, via the unaligned lftl_transaction_write_any path
  uint8_t*const data1_base = (uint8_t*)nvm.data1;
  transaction_start_func(&nvma,tracker);
  transaction_write_any_func(&nvma,data1_base+1,buf,write_size);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_TRANSACTION_OVERWRITE, lftl_transaction_write_any(&nvma,data1_base+1,buf,write_size));
  SANITY_CHECK();

  //once aborted, lftl_write is a plain write again instead of using the tracker
  uint8_t buf2[write_size];
  stateful_prng_fill(buf2,write_size);
  test_write(&nvma,nvm.data0,buf2,write_size);

  //an NVM error aborts the transaction of the area it happened for: nvmd
  //through a copy of its NVM properties whose write callback always fails
  lftl_nvm_props_t failing_props = nvm_props3;
  failing_props.write = failing_nvm_write;
  failing_props.next = LFTL_INVALID_POINTER;
  lftl_ctx_t failing = nvmd;
  failing.nvm_props = &failing_props;
  failing.data = LFTL_INVALID_POINTER;
  failing.transaction_tracker = LFTL_INVALID_POINTER;
  failing.next = LFTL_INVALID_POINTER;
  uint8_t failing_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&failing)];
  lftl_transaction_start(&failing,failing_tracker);
  EXPECT_ERROR_ABORTS(&failing, LFTL_ERROR_LOW_LEVEL_WRITE | 1, lftl_transaction_write(&failing,nvm.data7,buf,write_size));
  SANITY_CHECK();

  //lftl_write (the public dispatcher, not lftl_transaction_write directly)
  //routed to lftl_transaction_write while a transaction IS active -- this
  //one must succeed, not raise. Update transaction_buf_ref manually (mirroring
  //tearing_sim_lftl_transaction_write) since lftl_write itself isn't wrapped
  //for the transactional case the way write_func/write_any_func are.
  transaction_start_func(&nvma,tracker);
  #ifdef HAS_TEARING_SIMULATION
  {
    uintptr_t offset = (uintptr_t)nvm.data0 - (uintptr_t)&nvm;
    uint8_t*dst = (uint8_t*)&transaction_buf_ref;
    lftl_memread(dst+offset,buf,write_size);
  }
  #endif
  lftl_write(&nvma,nvm.data0,buf,write_size);
  read_newer_and_check(&nvma,nvm.data0,buf,write_size);
  transaction_commit_func(&nvma);
  read_and_check(&nvma,nvm.data0,buf,write_size);
  SANITY_CHECK();
}

void untouched_transaction_read_test(){
  DEBUG_PRINTLN(__func__);
  const uint32_t write_size = nvm_props.write_size;
  uint8_t wbuf0[sizeof(nvm.a_data)];
  stateful_prng_fill(wbuf0,sizeof(wbuf0));
  test_write(&nvma,&nvm.a_data,wbuf0,sizeof(wbuf0));

  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  transaction_start_func(&nvma,tracker);
  uint8_t wbuf[write_size];
  stateful_prng_fill(wbuf,write_size);
  transaction_write_func(&nvma,nvm.data0,wbuf,write_size);
  //nvm.data1 was NOT written in this transaction: lftl_transaction_read (via
  //read_newer_and_check) must take its "read current data" branch for it.
  read_newer_and_check(&nvma,nvm.data1,wbuf0+sizeof(nvm.data0),write_size);
  transaction_commit_func(&nvma);
  SANITY_CHECK();
}

void empty_registry_test(){
  DEBUG_PRINTLN(__func__);
  lftl_init_lib();
  if(lftl_get_ctx_std(nvm.data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx_ewlf(nvm.data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);
  if(lftl_get_ctx(nvm.data0) != LFTL_INVALID_POINTER) throw_exception(ERROR_VERIFICATION_FAIL);
  uint8_t dst[16];
  uint8_t src[16];
  stateful_prng_fill(src,sizeof(src));
  lftl_memread(dst,src,sizeof(src)); //regular memory; nothing registered yet at all
  if(memcmp(dst,src,sizeof(src))) throw_exception(ERROR_VERIFICATION_FAIL);

  //Deliberately leaves the registry empty on return: lftl_format is
  //documented as not tearing-safe (see tearing_sim_lftl_format), so calling
  //it here would put a real, swept NVM operation inside dut() itself, which
  //test_and_simulate_tearing's sweep never expects (it only times dut()'s
  //own body, assuming registration/formatting stays in its own untimed
  //preamble). Leaving registration to whichever test runs next is safe:
  //every test_and_simulate_tearing call re-registers and reformats
  //everything from scratch in its own preamble regardless of what a
  //previous test left behind.
}

void out_of_range_test(){
  DEBUG_PRINTLN(__func__);
  uint8_t buf[16];
  EXPECT_ERROR(LFTL_ERROR_FIRST_NOT_IN_DATA, lftl_read(&nvma,buf,((uint8_t*)nvma.area)-1,1));
  EXPECT_ERROR(LFTL_ERROR_LAST_NOT_IN_DATA, lftl_read(&nvma,buf,((uint8_t*)nvma.area)+nvma.data_size-1,2));
  //a size that makes offset+size wrap around must not pass the check, also
  //once a write extends it to whole write units
  const uintptr_t write_size = nvma.nvm_props->write_size;
  EXPECT_ERROR(LFTL_ERROR_LAST_NOT_IN_DATA, lftl_read(&nvma,buf,((uint8_t*)nvma.area)+write_size,(uintptr_t)0-write_size));
  EXPECT_ERROR(LFTL_ERROR_LAST_NOT_IN_DATA, lftl_read(&ewlfa,buf,((uint8_t*)ewlfa.area)+write_size,(uintptr_t)0-write_size));
  EXPECT_ERROR(LFTL_ERROR_LAST_NOT_IN_DATA, lftl_write(&nvma,((uint8_t*)nvma.area)+1,buf,(uintptr_t)0-1));
  EXPECT_ERROR(LFTL_ERROR_LAST_NOT_IN_DATA, lftl_write(&ewlfa,((uint8_t*)ewlfa.area)+1,buf,(uintptr_t)0-1));
  //a transaction write outside the area must not touch the tracker: its bit
  //for the write unit at beyond would be in the first guard byte
  const uintptr_t tracker_size = LFTL_TRANSACTION_TRACKER_SIZE(&nvma);
  uint8_t guarded_tracker[tracker_size+16];
  memset(guarded_tracker,0,sizeof(guarded_tracker));
  uint8_t*const beyond = ((uint8_t*)nvma.area) + tracker_size*BITS_PER_BYTE*write_size;
  transaction_start_func(&nvma,guarded_tracker);
  EXPECT_ERROR(LFTL_ERROR_FIRST_NOT_IN_DATA, lftl_transaction_write(&nvma,beyond,buf,write_size));
  transaction_abort_func(&nvma);
  transaction_start_func(&nvma,guarded_tracker);
  EXPECT_ERROR(LFTL_ERROR_FIRST_NOT_IN_DATA, lftl_transaction_write_any(&nvma,beyond+write_size+1,buf,1));
  transaction_abort_func(&nvma);
  //reads during a transaction are range checked too
  transaction_start_func(&nvma,guarded_tracker);
  EXPECT_ERROR(LFTL_ERROR_LAST_NOT_IN_DATA, lftl_transaction_read(&nvma,buf,((uint8_t*)nvma.area)+nvma.data_size-1,2));
  transaction_abort_func(&nvma);
  for(unsigned int i=tracker_size;i<sizeof(guarded_tracker);i++){
    if(guarded_tracker[i]) throw_exception(ERROR_VERIFICATION_FAIL);
  }
  SANITY_CHECK();
}

void unaligned_edge_test(){
  DEBUG_PRINTLN(__func__);
  //a 1-byte unaligned write, smaller than one write unit: the "first WU
  //fixup" in write_src_phy_addr_to_dst_phy_addr consumes more than the
  //whole requested size.
  uint8_t*const data0_base = (uint8_t*)nvm.data0;
  uint8_t val = 0x42;
  write_any_func(&nvma,data0_base+1,&val,1);
  read_and_check(&nvma,data0_base+1,&val,1);
  SANITY_CHECK();
}

void aligned_misuse_test(){
  DEBUG_PRINTLN(__func__);
  const uint32_t write_size = nvm_props.write_size;
  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  uint8_t buf[write_size*2];
  stateful_prng_fill(buf,sizeof(buf));

  //lftl_transaction_write requires an ALIGNED address and size. Each error
  //aborts the transaction, so each check starts its own.
  transaction_start_func(&nvma,tracker);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_BASE_MISALIGNED, lftl_transaction_write(&nvma,((uint8_t*)nvm.data0)+1,buf,write_size));
  transaction_start_func(&nvma,tracker);
  EXPECT_ERROR_ABORTS(&nvma, LFTL_ERROR_SIZE_MISALIGNED, lftl_transaction_write(&nvma,nvm.data1,buf,write_size+1));
  SANITY_CHECK();

  //lftl_format rejects an NVM whose write_size exceeds LFTL_WU_MAX_SIZE.
  //nvmd/nvm_props3 are a dedicated area+nvm_props so this doesn't disturb
  //the shared nvm_props used by nvma/nvmb/ewlfa/ewlfb. raise_error fires
  //before any low-level NVM access, so nvm_ref needs no manual update.
  const uint32_t saved_write_size = nvm_props3.write_size;
  nvm_props3.write_size = LFTL_WU_MAX_SIZE+1;
  EXPECT_ERROR(LFTL_ERROR_WU_SIZE_TOO_LARGE, lftl_format(&nvmd));
  nvm_props3.write_size = saved_write_size;
  SANITY_CHECK();
}

//A bad configuration is refused at registration, and the area is not linked
void bad_config_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props = nvm_props3;
  props.next = LFTL_INVALID_POINTER;
  lftl_ctx_t bad;
  #define RESET_BAD_CTX() do{\
      bad = nvmd;\
      bad.nvm_props = &props;\
      bad.data = LFTL_INVALID_POINTER;\
      bad.transaction_tracker = LFTL_INVALID_POINTER;\
      bad.next = LFTL_INVALID_POINTER;\
    }while(0)
  //an EWLF record larger than an erase unit (LF-11)
  RESET_BAD_CTX();
  bad.data_size = props.erase_size - 8;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area_ewlf(&bad));
  //a single EWLF record per erase unit: at least 2 must fit, a constraint
  //kept from when a torn record was copied forward within its unit (LFTL-008)
  RESET_BAD_CTX();
  bad.data_size = props.erase_size / 2;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area_ewlf(&bad));
  //an EWLF area of a single erase unit
  RESET_BAD_CTX();
  bad.area_size = props.erase_size;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area_ewlf(&bad));
  //less than 2 slots
  RESET_BAD_CTX();
  bad.data_size = props.erase_size;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area(&bad));
  //data that is not whole write units
  RESET_BAD_CTX();
  bad.data_size = props.write_size + 1;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area(&bad));
  //an area that does not start on an erase unit of its NVM
  RESET_BAD_CTX();
  bad.area = ((uint8_t*)nvmd.area) + props.write_size;
  bad.area_size = props.erase_size;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area(&bad));
  //an area that goes past the end of its NVM
  RESET_BAD_CTX();
  bad.area_size = props.size + props.erase_size;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area(&bad));
  //write units that are too large, or not a power of 2
  RESET_BAD_CTX();
  props.write_size = LFTL_WU_MAX_SIZE*2;
  EXPECT_ERROR(LFTL_ERROR_WU_SIZE_TOO_LARGE, lftl_register_area(&bad));
  props.write_size = 3;
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area(&bad));
  //none of them got linked: nvmd's data still resolves to nvmd
  if(&nvmd != lftl_get_ctx(nvm.data7)) throw_exception(ERROR_VERIFICATION_FAIL);
  SANITY_CHECK();
}

//Same CRC core as ftl_internal.h: CRC-64/NVME, reflected, all ones in and out.
//An EWLF record's checksum covers its version (little endian) then its data.
static uint64_t test_crc64(uint64_t crc, const uint8_t*buf, uintptr_t len){
  while(len--){
    crc ^= *buf++;
    for(unsigned int i=0;i<8;i++) crc = (crc >> 1) ^ (0x9A6C9329AC4BC9B5ull & -(crc & 1));
  }
  return crc;
}
static uint64_t test_ewlf_checksum(uint32_t version, const uint8_t*data, uintptr_t size){
  uint8_t v[4];
  for(unsigned int i=0;i<4;i++) v[i] = (uint8_t)(version >> (8*i));
  return ~test_crc64(test_crc64(~(uint64_t)0,v,sizeof(v)),data,size);
}
//The content of a record's meta field as written: version, checksum, zeros
static void test_ewlf_meta(uint8_t*meta, uintptr_t meta_size, uint32_t version, uint64_t checksum){
  memset(meta,0,meta_size);
  for(unsigned int i=0;i<4;i++) meta[i] = (uint8_t)(version >> (8*i));
  for(unsigned int i=0;i<8;i++) meta[4+i] = (uint8_t)(checksum >> (8*i));
}

static const uint8_t*checked_area_lo;
static const uint8_t*checked_area_hi;
static bool out_of_area_read;
static uint8_t area_checked_nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  const uint8_t*const src8 = (const uint8_t*)src_nvm_addr;
  if((src8 < checked_area_lo) || (src8 > checked_area_hi) || (size > (uintptr_t)(checked_area_hi - src8))) out_of_area_read = 1;
  return nvm_read(dst,src_nvm_addr,size);
}

//EWLF records are looked for only where records can start (LF-10, LF-12), on
//an EWLF area over nvmd's pages: a copy of nvmd's context and NVM properties,
//not registered, whose read callback flags any read outside the area.
void ewlf_grid_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props = nvm_props3;
  props.read = area_checked_nvm_read;
  props.next = LFTL_INVALID_POINTER;
  const uint32_t write_size = props.write_size;
  const uintptr_t flag_size = write_size;
  const uintptr_t meta_size = LFTL_ROUND_UP(12,write_size);
  const uintptr_t header_size = 3*flag_size + meta_size;
  //the data is 4 headers long, so a forged header fits in it whatever the write unit
  const uintptr_t data_size = 4*header_size;
  const uintptr_t record_size = header_size + data_size;
  lftl_ctx_t ewlf = nvmd;
  ewlf.nvm_props = &props;
  ewlf.data = LFTL_INVALID_POINTER;
  ewlf.data_size = data_size;
  ewlf.transaction_tracker = LFTL_INVALID_POINTER;
  ewlf.next = LFTL_INVALID_POINTER;
  ewlf.area_type = LFTL_AREA_TYPE_EWLF;
  uint8_t*const area = (uint8_t*)ewlf.area;
  checked_area_lo = area;
  checked_area_hi = area + ewlf.area_size;

  //LF-10: d2's write is cut before its meta field, and its data holds a whole
  //forged record, complete, at a write-unit aligned offset that is not a
  //record position.
  lftl_format(&ewlf);
  uint8_t d1[data_size];
  stateful_prng_fill(d1,data_size);
  lftl_write(&ewlf,area,d1,data_size);
  uint8_t*const current = ((uint8_t*)ewlf.data) - header_size;
  uint8_t*const torn = current - record_size;
  uint8_t*const torn_data = torn + header_size;
  const uintptr_t forged_offset = write_size;
  uint8_t d2[data_size];
  memset(d2,0xFF,data_size);
  const uint32_t version = 1000;
  uint8_t*const forged = d2+forged_offset;
  memset(forged,0,flag_size); //start marker
  memset(forged+flag_size+meta_size,0,2*flag_size); //both flags
  //its checksum covers the data_size bytes after its header: the end of d2,
  //then cells past d2 that are already programmed or erased
  uint8_t forged_data[data_size];
  for(uintptr_t i=0;i<data_size;i++){
    const uintptr_t d2_index = forged_offset + header_size + i;
    if(d2_index < data_size) forged_data[i] = d2[d2_index];
    else lftl_memread(forged_data+i,torn_data+d2_index,1);
  }
  test_ewlf_meta(forged+flag_size,meta_size,version,test_ewlf_checksum(version,forged_data,data_size));
  uint8_t zeros[flag_size];
  memset(zeros,0,flag_size);
  raw_write(torn,zeros,flag_size); //the torn write's start marker came first
  raw_write(torn_data,d2,data_size);
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d1,data_size);

  //LF-12: an area without any valid record, holding a plausible header (a
  //start marker and a meta field of version 1) in its last header_size bytes,
  //which is not a record position: the scan never reads it, nor past the area
  raw_erase(area,ewlf.area_size/props.erase_size);
  uint8_t last_header[header_size];
  memset(last_header,0xFF,header_size);
  memset(last_header,0,flag_size);
  test_ewlf_meta(last_header+flag_size,meta_size,1,0x0123456789ABCDEFull);
  raw_write(area+ewlf.area_size-header_size,last_header,header_size);
  ewlf.data = LFTL_INVALID_POINTER; //reset
  out_of_area_read = 0;
  uint8_t buf[64];
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_read(&ewlf,buf,area,sizeof(buf)));
  if(out_of_area_read) throw_exception(ERROR_VERIFICATION_FAIL);

  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

static int erase_calls_before_failure = -1;
static uint8_t failing_once_nvm_erase(void*base_address, unsigned int n_pages){
  if(0 == erase_calls_before_failure--) return 1;
  return nvm_erase(base_address,n_pages);
}

static int write_calls_before_failure = -1;
static uint8_t failing_once_nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0 == write_calls_before_failure--) return 1;
  return nvm_write(dst_nvm_addr,src,size);
}

//Power cuts in EWLF writes (LF-9, ISSUES.md LFTL-002, LFTL-013), on an EWLF
//area over nvmd's pages: a copy of nvmd's context and NVM properties, not
//registered. nvm_write refuses to program cells that are not erased, like
//STM32U5-class flash, so a write landing on a partial or complete record
//raises an error. A rollover erases the unit it enters before anything is
//programmed in it, and a power cut during that erase or during the record
//leaves the previous record current and the next write erasing again. 1000
//bytes of data make records of about 1 KB: 4 per 4 KB erase unit, 7 per 8 KB
//one; on 2 KB units (STM32L5) only one would fit, so the data is shrunk to
//fit two, the minimum the layout requires and this test assumes.
void ewlf_tearing_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props = nvm_props3;
  props.erase = failing_once_nvm_erase;
  props.write = failing_once_nvm_write;
  props.next = LFTL_INVALID_POINTER;
  const uint32_t write_size = props.write_size;
  const uintptr_t header_size = 3*write_size + LFTL_ROUND_UP(12,write_size);
  const uintptr_t record_size_1000 = header_size + LFTL_ROUND_UP(1000,write_size);
  const uintptr_t data_size = (props.erase_size / record_size_1000 >= 2) ? 1000 : props.erase_size/2 - header_size - 16;
  lftl_ctx_t ewlf = nvmd;
  ewlf.nvm_props = &props;
  ewlf.data = LFTL_INVALID_POINTER;
  ewlf.data_size = data_size;
  ewlf.transaction_tracker = LFTL_INVALID_POINTER;
  ewlf.next = LFTL_INVALID_POINTER;
  ewlf.area_type = LFTL_AREA_TYPE_EWLF;
  uint8_t*const area = (uint8_t*)ewlf.area;
  const uintptr_t record_size = header_size + LFTL_ROUND_UP(data_size,write_size);
  if(props.erase_size / record_size < 2) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d[data_size];

  //a write cut while writing data leaves a partial record right below the
  //current one, its start marker programmed: the next write must not program
  //over it
  lftl_format(&ewlf);
  stateful_prng_fill(d,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  uint8_t*const torn_data = ((uint8_t*)ewlf.data) - record_size;
  uint8_t zeros[write_size];
  memset(zeros,0,write_size);
  raw_write(torn_data - header_size,zeros,write_size); //start marker
  uint8_t partial[LFTL_ROUND_UP(100,write_size)];
  stateful_prng_fill(partial,sizeof(partial));
  raw_write(torn_data,partial,sizeof(partial));
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d,sizeof(d));
  stateful_prng_fill(d,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  read_and_check(&ewlf,area,d,sizeof(d));
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d,sizeof(d));

  //a wrap from the lowest unit back to the highest one, cut during the erase
  //of the highest unit, then during the new record after that erase: the
  //reset finds the previous record, and the next write erases the unit again
  //before programming it
  lftl_format(&ewlf);
  const unsigned int records_per_unit = props.erase_size / record_size;
  const unsigned int n_units = ewlf.area_size / props.erase_size;
  uint8_t d_prev[data_size];
  for(unsigned int i = 1; i < n_units*records_per_unit; i++){ //format wrote the first record
    stateful_prng_fill(d,sizeof(d));
    lftl_write(&ewlf,area,d,sizeof(d));
  }
  memcpy(d_prev,d,sizeof(d));
  stateful_prng_fill(d,sizeof(d));
  erase_calls_before_failure = 0;
  EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_ERASE | 1, lftl_write(&ewlf,area,d,sizeof(d)));
  erase_calls_before_failure = -1;
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d_prev,sizeof(d));
  //the unit erased, the start marker and the data of the new record written,
  //the power cut before its meta field
  write_calls_before_failure = 2;
  EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_WRITE | 1, lftl_write(&ewlf,area,d,sizeof(d)));
  write_calls_before_failure = -1;
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d_prev,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d,sizeof(d));
  if(!raw_reads_erased(area + (n_units-1)*props.erase_size + props.erase_size - 2*record_size,record_size)) throw_exception(ERROR_VERIFICATION_FAIL); //only the top record of the wrapped-to unit holds anything
  stateful_prng_fill(d,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d,sizeof(d));

  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

//EWLF areas accept any data size: each record pads the data to whole write
//units. An EWLF area over nvmd's pages whose data ends 1 byte into a write
//unit: write the whole data, then only its last bytes, and read it back after
//a reset (ISSUES.md LFTL-005).
void ewlf_unaligned_size_test(){
  DEBUG_PRINTLN(__func__);
  lftl_ctx_t ewlf = nvmd;
  ewlf.data = LFTL_INVALID_POINTER;
  ewlf.data_size = 8*nvm_props3.write_size + 1;
  ewlf.transaction_tracker = LFTL_INVALID_POINTER;
  ewlf.next = LFTL_INVALID_POINTER;
  ewlf.area_type = LFTL_AREA_TYPE_EWLF; //not registered, so set here
  uint8_t*const area = (uint8_t*)ewlf.area;
  const uintptr_t data_size = ewlf.data_size;
  uint8_t expected[data_size];
  lftl_format(&ewlf);
  memset(expected,0xFF,data_size); //a formatted EWLF area reads erased
  read_and_check(&ewlf,area,expected,data_size);
  stateful_prng_fill(expected,data_size);
  lftl_write(&ewlf,area,expected,data_size);
  read_and_check(&ewlf,area,expected,data_size);
  uint8_t last[2];
  stateful_prng_fill(last,sizeof(last));
  lftl_write(&ewlf,area+data_size-sizeof(last),last,sizeof(last));
  memcpy(expected+data_size-sizeof(last),last,sizeof(last));
  read_and_check(&ewlf,area,expected,data_size);
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,expected,data_size);
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

//Read an area's whole data back in small chunks and check every byte
static void check_whole_area(lftl_ctx_t*ctx, uint8_t value){
  uint8_t*const area = (uint8_t*)ctx->area;
  uint8_t expected[64];
  memset(expected,value,sizeof(expected));
  for(uintptr_t offset=0;offset<ctx->data_size;offset+=sizeof(expected)){
    const uintptr_t remaining = ctx->data_size - offset;
    read_and_check(ctx,area+offset,expected,remaining < sizeof(expected) ? remaining : sizeof(expected));
  }
}

//LFTL_MEMSET_WHOLE_AREA once the current slot is not slot 0, and on data of
//a whole number of pages (LFTL-006), in the scratch areas nvmc and nvmd: nvmd
//itself, then one page of data in a context spanning both areas' pages (2
//slots of 2 pages). That context is not registered, so area_type is set here.
void memset_whole_area_test(){
  DEBUG_PRINTLN(__func__);
  lftl_format(&nvmd);
  uint8_t buf[sizeof(nvm.data7)];
  stateful_prng_fill(buf,sizeof(buf));
  lftl_write(&nvmd,nvm.data7,buf,sizeof(buf)); //the current slot moves on
  LFTL_MEMSET_WHOLE_AREA(nvmd,0x5A);
  check_whole_area(&nvmd,0x5A);
  nvmd.data = LFTL_INVALID_POINTER; //reset
  check_whole_area(&nvmd,0x5A);

  if((uint8_t*)&nvm.d_pages != (uint8_t*)&nvm.c_pages + sizeof(nvm.c_pages)) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  lftl_ctx_t page = nvmc;
  page.nvm_props = &nvm_props;
  page.area_size = sizeof(nvm.c_pages) + sizeof(nvm.d_pages);
  page.data = LFTL_INVALID_POINTER;
  page.data_size = LFTL_PAGE_SIZE;
  page.transaction_tracker = LFTL_INVALID_POINTER;
  page.next = LFTL_INVALID_POINTER;
  page.area_type = LFTL_AREA_TYPE_STD;
  lftl_format(&page);
  LFTL_MEMSET_WHOLE_AREA(page,0xA5);
  check_whole_area(&page,0xA5);
  LFTL_MEMSET_WHOLE_AREA(page,0x3C); //from the other slot
  check_whole_area(&page,0x3C);
  page.data = LFTL_INVALID_POINTER; //reset
  check_whole_area(&page,0x3C);
  format_func(&nvmc); //resync nvm_ref with nvmc's and nvmd's pages
  format_func(&nvmd);
  SANITY_CHECK();
}

void version_collision_test(){
  DEBUG_PRINTLN(__func__);
  uint8_t buf[sizeof(nvm.data7)];
  stateful_prng_fill(buf,sizeof(buf));
  test_write(&nvmd,nvm.data7,buf,sizeof(buf));

  //Compute nvmd's physical slot/meta layout from public fields only,
  //mirroring the formulas in ftl_internal.h (meta_phy_size/n_pages_in_slot/
  //slot_size/n_slots/slot_base/meta_offset).
  const uint32_t write_size = nvmd.nvm_props->write_size;
  const uint32_t item_size = write_size > sizeof(uint32_t) ? write_size : sizeof(uint32_t);
  const uintptr_t meta_phy_size = LFTL_META_N_ITEMS * item_size;
  const uintptr_t erase_size = nvmd.nvm_props->erase_size;
  const uintptr_t n_pages_in_slot = LFTL_DIV_CEIL(nvmd.data_size + meta_phy_size, erase_size);
  const uintptr_t slot_size = n_pages_in_slot * erase_size;
  const unsigned int n_slots = nvmd.area_size / slot_size;
  if(n_slots < 2) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption

  const unsigned int current_index = ((uintptr_t)nvmd.data - (uintptr_t)nvmd.area) / slot_size;
  const unsigned int other_index = (current_index + 1) % n_slots;
  const uint8_t*const current_slot_base = (uint8_t*)nvmd.area + current_index*slot_size;
  uint8_t*const other_slot_base = (uint8_t*)nvmd.area + other_index*slot_size;

  //Duplicate the WHOLE current slot (data + meta-data) onto the other slot,
  //rather than just patching its version field: find_current_slot only
  //flags a slot as a genuine contender for "current" if its checksum (a
  //function of that slot's own data and version) is internally consistent,
  //so a version-only edit would leave the other slot's stale checksum
  //failing that check and never reach the collision comparison at all. A
  //full byte-for-byte copy makes both slots equally self-consistent for
  //the same version, which is what actually triggers the collision.
  //The other slot is erased first, since real flash only programs erased
  //cells, then the copy goes in chunks to keep the stack small on targets.
  raw_erase(other_slot_base, n_pages_in_slot);
  uint64_t chunk[32];//256 bytes, a whole number of write units
  for(uintptr_t offset=0;offset<slot_size;offset+=sizeof(chunk)){
    lftl_memread(chunk, current_slot_base+offset, sizeof(chunk));
    raw_write(other_slot_base+offset, chunk, sizeof(chunk));
  }
  SANITY_CHECK(); //nvmd is not among the areas SANITY_CHECK compares

  //force a fresh slot search: two slots now share the same version number
  nvmd.data = LFTL_INVALID_POINTER;
  EXPECT_ERROR(LFTL_ERROR_VERSION_COLLISION, lftl_read(&nvmd,buf,nvm.data7,sizeof(buf)));
  format_func(&nvmd); //leave nvmd clean afterward, this test intentionally corrupts it

  //LFTL_ERROR_NO_VALID_VERSION: erase nvmd's raw pages directly (bypassing
  //lftl_format's write_meta), so every slot reads as fully erased
  //(version == 0xFFFFFFFF) and none is valid.
  raw_erase(nvmd.area, nvmd.area_size/erase_size);
  nvmd.data = LFTL_INVALID_POINTER;
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_read(&nvmd,buf,nvm.data7,sizeof(buf)));
  //same when a transaction is the first access, and the failed start must
  //not leave a transaction behind
  uint8_t nvmd_transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvmd)];
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_transaction_start(&nvmd,nvmd_transaction_tracker));
  if(LFTL_INVALID_POINTER != nvmd.transaction_tracker) throw_exception(ERROR_VERIFICATION_FAIL);
  format_func(&nvmd); //leave nvmd clean for anything after this test
}

//Rebuild nvmd's newest slot as a tearing during the writes of its meta-data
//leaves it: data, version and checksum programmed, checksum2 erased or with
//the last programmed byte of its value still erased (ISSUES.md LFTL-009,
//LFTL-010, LFTL-011). The scan never erases. checksum2 erased: the cut came
//during the checksum word at the latest, that word may be weakly programmed,
//the previous version is current; on a flash with zero overwrite the torn
//slot is tombstoned, its version item pinned to zeros, otherwise it is left
//as it is. checksum2 partially programmed: its program had started, the
//checksum is complete, the slot is taken; with zero overwrite the unit is
//pinned to zeros first, so that it cannot read all erased at a later boot.
//Without zero overwrite that decision is not durable: on Linux, where the NVM
//is RAM, the unit is then made to read all erased, and the previous version
//comes back (the residual of the documentation's "Without pinning" section). checksum2 erased
//with no other valid slot: the slot is kept.
typedef enum {
  TORN_CHECKSUM2_ERASED,
  TORN_CHECKSUM2_PARTIAL,
  TORN_CHECKSUM2_ERASED_ONLY_SLOT,
} torn_checksum2_mode_t;
void torn_checksum2_test_core(torn_checksum2_mode_t mode){
  uint8_t previous[sizeof(nvm.data7)];
  uint8_t buf[sizeof(nvm.data7)];
  stateful_prng_fill(previous,sizeof(previous));
  test_write(&nvmd,nvm.data7,previous,sizeof(previous));
  stateful_prng_fill(buf,sizeof(buf));
  test_write(&nvmd,nvm.data7,buf,sizeof(buf));

  //nvmd's physical slot/meta layout, same formulas as version_collision_test
  const uint32_t write_size = nvmd.nvm_props->write_size;
  const uint32_t item_size = write_size > sizeof(uint32_t) ? write_size : sizeof(uint32_t);
  const uintptr_t meta_phy_size = LFTL_META_N_ITEMS * item_size;
  const uintptr_t erase_size = nvmd.nvm_props->erase_size;
  const uintptr_t n_pages_in_slot = LFTL_DIV_CEIL(nvmd.data_size + meta_phy_size, erase_size);
  const uintptr_t slot_size = n_pages_in_slot * erase_size;
  const unsigned int n_slots = nvmd.area_size / slot_size;
  const unsigned int current_index = ((uintptr_t)nvmd.data - (uintptr_t)nvmd.area) / slot_size;
  const unsigned int previous_index = (current_index + n_slots - 1) % n_slots;
  uint8_t*const slot = (uint8_t*)nvmd.area + current_index*slot_size;
  uint8_t*const meta = slot + slot_size - meta_phy_size;
  uint8_t*const checksum2 = meta + meta_phy_size - item_size;

  //Only erased write units are programmed below, so this works on real
  //flash too. If the slot is slot 0, its data bytes are also nvm_ref's
  //logical bytes for nvm.data7: they go through 0xFF and back to buf.
  uint8_t data_img[nvmd.data_size];
  uint8_t meta_img[meta_phy_size];
  lftl_memread(data_img,slot,nvmd.data_size);
  lftl_memread(meta_img,meta,meta_phy_size);
  raw_erase(slot,n_pages_in_slot);
  raw_write(slot,data_img,nvmd.data_size);
  raw_write(meta,meta_img,meta_phy_size - item_size); //version and checksum
  uint8_t*const checksum2_img = meta_img + meta_phy_size - item_size;
  if(TORN_CHECKSUM2_PARTIAL == mode){
    //the last programmed byte of checksum2's value still reads erased
    for(unsigned int i=sizeof(uint32_t);i>0;i--){
      if(0xFF != checksum2_img[i-1]){
        checksum2_img[i-1] = 0xFF;
        break;
      }
    }
    raw_write(checksum2,checksum2_img,item_size);
  }
  if(TORN_CHECKSUM2_ERASED_ONLY_SLOT == mode){
    raw_erase((uint8_t*)nvmd.area + previous_index*slot_size,n_pages_in_slot);
  }

  //force a fresh slot search
  const bool pinning = nvmd.nvm_props->zero_overwrite;
  const uint8_t*const expected = (TORN_CHECKSUM2_ERASED == mode) ? previous : buf;
  nvmd.data = LFTL_INVALID_POINTER;
  read_and_check(&nvmd,nvm.data7,expected,sizeof(buf));
  const unsigned int index = ((uintptr_t)nvmd.data - (uintptr_t)nvmd.area) / slot_size;
  uint8_t item_now[item_size];
  if(TORN_CHECKSUM2_ERASED == mode){
    //the previous version is current; the torn slot is not erased: its data
    //and checksum are still there, its version item pinned to zeros where the
    //flash allows it, untouched otherwise
    if(index != previous_index) throw_exception(ERROR_VERIFICATION_FAIL);
    if(raw_reads_erased(slot,slot_size)) throw_exception(ERROR_VERIFICATION_FAIL);
    lftl_memread(item_now,meta,item_size);
    if(pinning){
      for(unsigned int i=0;i<item_size;i++) if(0 != item_now[i]) throw_exception(ERROR_VERIFICATION_FAIL);
    } else {
      if(memcmp(item_now,meta_img,item_size)) throw_exception(ERROR_VERIFICATION_FAIL);
    }
    lftl_memread(item_now,meta+item_size,item_size);
    if(memcmp(item_now,meta_img+item_size,item_size)) throw_exception(ERROR_VERIFICATION_FAIL);
  } else {
    //the torn slot is current
    if(index != current_index) throw_exception(ERROR_VERIFICATION_FAIL);
    lftl_memread(item_now,checksum2,item_size);
    if((TORN_CHECKSUM2_PARTIAL == mode) && pinning){
      //pinned: the whole unit reads zeros
      for(unsigned int i=0;i<item_size;i++) if(0 != item_now[i]) throw_exception(ERROR_VERIFICATION_FAIL);
    } else if(TORN_CHECKSUM2_PARTIAL == mode){
      if(memcmp(item_now,checksum2_img,item_size)) throw_exception(ERROR_VERIFICATION_FAIL);
    } else {
      for(unsigned int i=0;i<item_size;i++) if(0xFF != item_now[i]) throw_exception(ERROR_VERIFICATION_FAIL);
    }
  }
  //it survives another reboot
  nvmd.data = LFTL_INVALID_POINTER;
  read_and_check(&nvmd,nvm.data7,expected,sizeof(buf));
  #ifdef HAS_TEARING_SIMULATION
  if((TORN_CHECKSUM2_PARTIAL == mode) && !pinning){
    //the weak word drifts to all erased: the previous version comes back
    memset(checksum2,0xFF,item_size);
    nvmd.data = LFTL_INVALID_POINTER;
    read_and_check(&nvmd,nvm.data7,previous,sizeof(buf));
  }
  #endif
  //and the area still takes updates
  randomized_test_write(&nvmd,nvm.data7,sizeof(buf));
  SANITY_CHECK();
}

void torn_checksum2_test(){
  DEBUG_PRINTLN(__func__);
  torn_checksum2_test_core(TORN_CHECKSUM2_ERASED);
  torn_checksum2_test_core(TORN_CHECKSUM2_PARTIAL);
  torn_checksum2_test_core(TORN_CHECKSUM2_ERASED_ONLY_SLOT);
}

//A power cut during the erase of the next slot leaves it partly erased: its
//content is still the previous data and meta-data, with random bits set. Its
//version word can then read as the current version (LFTL-007): with 2 slots,
//slot 0 holds the odd versions and slot 1 the even ones, so erasing bit 0 of
//the next slot's version gives the current one whenever it is odd. The scan
//must skip such a slot, whether it comes before or after the valid one.
void torn_erase_version_test_core(bool garbage_after_valid){
  //nvmd's physical slot/meta layout, same formulas as version_collision_test
  const uint32_t write_size = nvmd.nvm_props->write_size;
  const uint32_t item_size = write_size > sizeof(uint32_t) ? write_size : sizeof(uint32_t);
  const uintptr_t meta_phy_size = LFTL_META_N_ITEMS * item_size;
  const uintptr_t erase_size = nvmd.nvm_props->erase_size;
  const uintptr_t n_pages_in_slot = LFTL_DIV_CEIL(nvmd.data_size + meta_phy_size, erase_size);
  const uintptr_t slot_size = n_pages_in_slot * erase_size;
  const unsigned int n_slots = nvmd.area_size / slot_size;
  if(n_slots < 2) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption

  //the slot the garbage goes to is the next one: write until it is scanned
  //after the current slot (current slot 0), or before it (current slot n-1)
  uint8_t buf[sizeof(nvm.data7)];
  const unsigned int wanted_index = garbage_after_valid ? 0 : n_slots-1;
  unsigned int current_index = n_slots;
  for(unsigned int i=0;i<=n_slots;i++){
    if(LFTL_INVALID_POINTER == nvmd.data) lftl_read(&nvmd,buf,nvm.data7,sizeof(buf)); //locate the slot
    current_index = ((uintptr_t)nvmd.data - (uintptr_t)nvmd.area) / slot_size;
    if(current_index == wanted_index) break;
    stateful_prng_fill(buf,sizeof(buf));
    test_write(&nvmd,nvm.data7,buf,sizeof(buf));
  }
  if(current_index != wanted_index) throw_exception(INTERNAL_ERROR_CORRUPT);
  lftl_read(&nvmd,buf,nvm.data7,sizeof(buf)); //the current data, whatever wrote it
  const unsigned int next_index = (current_index + 1) % n_slots;
  uint8_t*const slot = (uint8_t*)nvmd.area + current_index*slot_size;
  uint8_t*const meta = slot + slot_size - meta_phy_size;
  uint8_t*const next = (uint8_t*)nvmd.area + next_index*slot_size;
  uint8_t*const next_meta = next + slot_size - meta_phy_size;

  //The torn erase leaves the next slot with a version word reading the
  //current version, and a checksum that is not the one of its data and that
  //version. Built with an erase and programming of erased cells only, so this
  //works on real flash too.
  uint8_t data_img[nvmd.data_size];
  uint8_t meta_img[meta_phy_size];
  lftl_memread(data_img,slot,nvmd.data_size);
  lftl_memread(meta_img,meta,meta_phy_size);
  meta_img[item_size] ^= 1; //checksum, wrong for this data and version
  memcpy(meta_img + 2*item_size,meta_img + item_size,item_size); //checksum2 == checksum
  raw_erase(next,n_pages_in_slot);
  raw_write(next,data_img,nvmd.data_size);
  raw_write(next_meta,meta_img,meta_phy_size);

  //force a fresh slot search: it must settle on the valid slot
  nvmd.data = LFTL_INVALID_POINTER;
  read_and_check(&nvmd,nvm.data7,buf,sizeof(buf));
  if(current_index != ((uintptr_t)nvmd.data - (uintptr_t)nvmd.area) / slot_size) throw_exception(ERROR_VERIFICATION_FAIL);
  //it survives another reboot, and the area still takes updates
  nvmd.data = LFTL_INVALID_POINTER;
  read_and_check(&nvmd,nvm.data7,buf,sizeof(buf));
  randomized_test_write(&nvmd,nvm.data7,sizeof(buf));
  SANITY_CHECK();
}

void torn_erase_version_test(){
  DEBUG_PRINTLN(__func__);
  torn_erase_version_test_core(1); //the garbage slot is scanned after the valid one
  torn_erase_version_test_core(0); //and before it
}

//Layout of an EWLF record, same formulas as ftl_internal.h: start marker (one
//write unit), meta field (version and checksum, 12 bytes rounded up to whole
//write units), flag-a and flag-b (one write unit each), data rounded up to
//whole write units
typedef struct {
  uintptr_t flag_size;
  uintptr_t meta_size;
  uintptr_t header_size;
  uintptr_t record_size;
} ewlf_layout_t;
static ewlf_layout_t ewlf_layout(const lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  ewlf_layout_t l;
  l.flag_size = write_size;
  l.meta_size = LFTL_ROUND_UP(12,write_size);
  l.header_size = 3*l.flag_size + l.meta_size;
  l.record_size = l.header_size + LFTL_ROUND_UP(ctx->data_size,write_size);
  return l;
}
static uint8_t*ewlf_raw_meta(const ewlf_layout_t*l, const uint8_t*record){ return (uint8_t*)record + l->flag_size; }
static uint8_t*ewlf_raw_flag_a(const ewlf_layout_t*l, const uint8_t*record){ return (uint8_t*)record + l->flag_size + l->meta_size; }
static uint8_t*ewlf_raw_flag_b(const ewlf_layout_t*l, const uint8_t*record){ return (uint8_t*)record + 2*l->flag_size + l->meta_size; }

//How far a hand-built record got: the states the scan tells apart, plus the
//two where the last word programmed is left partially programmed
typedef enum {
  EWLF_RAW_ABORTED,          //start marker, data, meta field
  EWLF_RAW_HALF,             //and flag-a
  EWLF_RAW_HALF_PARTIAL,     //and flag-a but for its last byte
  EWLF_RAW_COMPLETE_PARTIAL, //and flag-a, and flag-b but for its last byte
  EWLF_RAW_COMPLETE,         //and both flags
} ewlf_raw_state_t;

//Write a record as a write torn in one of its flags leaves it. Only erased
//cells are programmed, so this works on real flash too.
static void ewlf_raw_torn_record(const lftl_ctx_t*ctx, uint8_t*record, const uint8_t*d, uint32_t version, ewlf_raw_state_t state){
  const ewlf_layout_t l = ewlf_layout(ctx);
  uint8_t zeros[l.flag_size];
  memset(zeros,0,l.flag_size);
  raw_write(record, zeros, l.flag_size); //start marker
  raw_write(record + l.header_size, d, LFTL_ROUND_UP(ctx->data_size,ctx->nvm_props->write_size));
  uint8_t meta[l.meta_size];
  test_ewlf_meta(meta,l.meta_size,version,test_ewlf_checksum(version,d,ctx->data_size));
  raw_write(ewlf_raw_meta(&l,record),meta,l.meta_size);
  if(EWLF_RAW_ABORTED == state) return;
  uint8_t partial[l.flag_size];
  memset(partial,0,l.flag_size);
  partial[l.flag_size-1] = 0xFF;
  raw_write(ewlf_raw_flag_a(&l,record),(EWLF_RAW_HALF_PARTIAL == state) ? partial : zeros,l.flag_size);
  if((EWLF_RAW_HALF == state) || (EWLF_RAW_HALF_PARTIAL == state)) return;
  raw_write(ewlf_raw_flag_b(&l,record),(EWLF_RAW_COMPLETE_PARTIAL == state) ? partial : zeros,l.flag_size);
}

//Physical reads through the raw callback: lftl_memread would take an address
//inside a registered area's logical range (the bottom position of the first
//unit starts at the area's base) for a logical one
static void ewlf_raw_read(void*dst, const uint8_t*addr, uintptr_t size){
  const uint8_t status = nvm_read(dst,addr,size);
  if(status) throw_exception(LFTL_ERROR_LOW_LEVEL_READ | status);
}
static uint32_t ewlf_raw_version(const lftl_ctx_t*ctx, const uint8_t*record){
  const ewlf_layout_t l = ewlf_layout(ctx);
  uint8_t bytes[4];
  ewlf_raw_read(bytes,ewlf_raw_meta(&l,record),sizeof(bytes));
  uint32_t version = 0;
  for(unsigned int i=0;i<4;i++) version |= ((uint32_t)bytes[i]) << (8*i);
  return version;
}

static bool ewlf_raw_is_all(const uint8_t*addr, uintptr_t size, uint8_t value){
  uint8_t buf[size];
  ewlf_raw_read(buf,addr,size);
  for(uintptr_t i=0;i<size;i++) if(value != buf[i]) return false;
  return true;
}

//After the scan, nothing erased. Aborted (both flags erased): B, the record
//current before the torn write, is current; with zero overwrite R is
//tombstoned, its meta field programmed to zeros, otherwise R is left as it
//is. Half (flag-a programmed, in full or in part, flag-b erased): R's data is
//solid but its flag-a may be weak, so R is copied forward, to the position
//below it, and the copy, complete, is current; R's flags are left as they
//are. Complete but for the last byte of flag-b: flag-b has a bit, which
//proves flag-a solid, R is current and nothing is programmed.
static void check_torn_record(lftl_ctx_t*ewlf, uint8_t*R, uint8_t*B, uint32_t version, ewlf_raw_state_t state){
  const bool pinning = ewlf->nvm_props->zero_overwrite;
  const ewlf_layout_t l = ewlf_layout(ewlf);
  uint8_t*const current = (uint8_t*)ewlf->data - l.header_size;
  if(EWLF_RAW_ABORTED == state){
    if(current != B) throw_exception(ERROR_VERIFICATION_FAIL);
    if(!ewlf_raw_is_all(ewlf_raw_flag_a(&l,R),2*l.flag_size,0xFF)) throw_exception(ERROR_VERIFICATION_FAIL);
    if(pinning){
      if(!ewlf_raw_is_all(ewlf_raw_meta(&l,R),l.meta_size,0)) throw_exception(ERROR_VERIFICATION_FAIL);
    } else {
      if(ewlf_raw_version(ewlf,R) != version+1) throw_exception(ERROR_VERIFICATION_FAIL);
    }
  } else if(EWLF_RAW_COMPLETE_PARTIAL == state){
    if(current != R) throw_exception(ERROR_VERIFICATION_FAIL);
    if(!ewlf_raw_is_all(ewlf_raw_flag_a(&l,R),l.flag_size,0)) throw_exception(ERROR_VERIFICATION_FAIL);
    uint8_t flag_b[l.flag_size];
    ewlf_raw_read(flag_b,ewlf_raw_flag_b(&l,R),l.flag_size);
    if(0xFF != flag_b[l.flag_size-1]) throw_exception(ERROR_VERIFICATION_FAIL); //not pinned
  } else {
    //half: the copy, right below R
    if(current != R - l.record_size) throw_exception(ERROR_VERIFICATION_FAIL);
    if(ewlf_raw_version(ewlf,current) <= version+1) throw_exception(ERROR_VERIFICATION_FAIL);
    if(!ewlf_raw_is_all(ewlf_raw_flag_b(&l,current),l.flag_size,0)) throw_exception(ERROR_VERIFICATION_FAIL);
    if(ewlf_raw_version(ewlf,R) != version+1) throw_exception(ERROR_VERIFICATION_FAIL);
    if(!ewlf_raw_is_all(ewlf_raw_flag_b(&l,R),l.flag_size,0xFF)) throw_exception(ERROR_VERIFICATION_FAIL);
    uint8_t flag_a[l.flag_size];
    ewlf_raw_read(flag_a,ewlf_raw_flag_a(&l,R),l.flag_size);
    if((EWLF_RAW_HALF_PARTIAL == state) != (0xFF == flag_a[l.flag_size-1])) throw_exception(ERROR_VERIFICATION_FAIL); //not pinned
  }
}

//A record torn in one of its flags (LFTL-008, LFTL-010, LFTL-011, and the
//anti-tearing page's EWLF scan), on an EWLF area over nvmd's pages. Aborted:
//the previous record stays current, and with zero overwrite the torn one is
//tombstoned. Half: it is copied forward and the copy is current. Complete but
//for the last byte of flag-b: it is current and nothing is programmed. Within
//a unit, then as the rollover record at the top of a unit, where the unit the
//write order left is not erased. On Linux, where the NVM is RAM, the weak
//word is then made to read erased: an aborted record's meta field (nothing
//changes), a half record's flag-a (the copy stands), a complete record's
//flag-b (it becomes half and is copied): the data shown never goes back.
//On Linux, where the NVM is RAM, the word the torn write left weak drifts to
//all erased, then the area is read after a reset: the data shown never goes
//back. An aborted record's meta field: nothing changes (with zero overwrite it
//was tombstoned, and a tombstone is solid, so it is left alone). A half
//record's flag-a: its copy stands. A complete record's flag-b: it is half now,
//and copied.
static void ewlf_drift(lftl_ctx_t*ewlf, uint8_t*R, ewlf_raw_state_t state, const uint8_t*expected, uintptr_t size, bool pinning){
  #ifdef HAS_TEARING_SIMULATION
  const ewlf_layout_t l = ewlf_layout(ewlf);
  if(EWLF_RAW_ABORTED == state){
    if(pinning) return;
    memset(ewlf_raw_meta(&l,R),0xFF,l.meta_size);
  } else if(EWLF_RAW_COMPLETE_PARTIAL == state){
    memset(ewlf_raw_flag_b(&l,R),0xFF,l.flag_size);
  } else {
    memset(ewlf_raw_flag_a(&l,R),0xFF,l.flag_size);
  }
  ewlf->data = LFTL_INVALID_POINTER; //reset
  read_and_check(ewlf,ewlf->area,expected,size);
  if(EWLF_RAW_COMPLETE_PARTIAL == state){
    //copied: the current record is below R, complete
    uint8_t*const current = (uint8_t*)ewlf->data - l.header_size;
    if(current != R - l.record_size) throw_exception(ERROR_VERIFICATION_FAIL);
    if(!ewlf_raw_is_all(ewlf_raw_flag_b(&l,current),l.flag_size,0)) throw_exception(ERROR_VERIFICATION_FAIL);
  }
  #else
  (void)ewlf; (void)R; (void)state; (void)expected; (void)size; (void)pinning;
  #endif
}
//The new record is numbered above the record current before it and above R
//whenever R's checksum is still valid, so that the two can never share a
//version whatever R's flags read later; a tombstoned R, or one whose meta
//field the simulation wiped, no longer counts
static void ewlf_check_numbering(lftl_ctx_t*ewlf, uint8_t*R, uint32_t version_before_r){
  const ewlf_layout_t l = ewlf_layout(ewlf);
  uint8_t*const current = (uint8_t*)ewlf->data - l.header_size;
  const uint32_t new_version = ewlf_raw_version(ewlf,current);
  if(new_version <= version_before_r) throw_exception(ERROR_VERIFICATION_FAIL);
  if(current == R) return; //a rollover erased R's unit and the new record took its position
  const uint32_t r_version = ewlf_raw_version(ewlf,R);
  if((r_version != 0) && (r_version != 0xFFFFFFFF) && (new_version <= r_version)) throw_exception(ERROR_VERIFICATION_FAIL);
}
void torn_ewlf_checksum2_test_core(ewlf_raw_state_t state){
  const bool pinning = nvmd.nvm_props->zero_overwrite;
  lftl_ctx_t ewlf = nvmd;
  ewlf.data = LFTL_INVALID_POINTER;
  ewlf.data_size = 64;
  ewlf.transaction_tracker = LFTL_INVALID_POINTER;
  ewlf.next = LFTL_INVALID_POINTER;
  ewlf.area_type = LFTL_AREA_TYPE_EWLF; //not registered, so set here
  uint8_t*const area = (uint8_t*)ewlf.area;
  const uintptr_t erase_size = ewlf.nvm_props->erase_size;
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  const unsigned int records_per_unit = erase_size / l.record_size;
  const unsigned int n_units = ewlf.area_size / erase_size;
  if((records_per_unit < 3) || (n_units < 2)) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d1[64], d2[64], d3[64];

  const bool aborted = (EWLF_RAW_ABORTED == state);
  //within a unit: R right below the current record
  lftl_format(&ewlf);
  stateful_prng_fill(d1,sizeof(d1));
  lftl_write(&ewlf,area,d1,sizeof(d1));
  uint8_t*current = (uint8_t*)ewlf.data - l.header_size;
  uint32_t version = ewlf_raw_version(&ewlf,current);
  uint8_t*R = current - l.record_size;
  stateful_prng_fill(d2,sizeof(d2));
  ewlf_raw_torn_record(&ewlf,R,d2,version+1,state);
  const uint8_t*expected = aborted ? d1 : d2;
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,expected,sizeof(d2));
  check_torn_record(&ewlf,R,current,version,state);
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,expected,sizeof(d2));
  ewlf_drift(&ewlf,R,state,expected,sizeof(d2),pinning);
  stateful_prng_fill(d3,sizeof(d3));
  lftl_write(&ewlf,area,d3,sizeof(d3));
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d3,sizeof(d3));
  ewlf_check_numbering(&ewlf,R,version);

  //as the rollover record: fill the highest unit, R at the top of the unit below
  lftl_format(&ewlf);
  for(unsigned int i=1;i<records_per_unit;i++){ //format wrote the first record
    stateful_prng_fill(d1,sizeof(d1));
    lftl_write(&ewlf,area,d1,sizeof(d1));
  }
  current = (uint8_t*)ewlf.data - l.header_size;
  if(current != area + (n_units-1)*erase_size + erase_size - records_per_unit*l.record_size) throw_exception(INTERNAL_ERROR_CORRUPT);
  version = ewlf_raw_version(&ewlf,current);
  R = area + (n_units-2)*erase_size + erase_size - l.record_size;
  stateful_prng_fill(d2,sizeof(d2));
  ewlf_raw_torn_record(&ewlf,R,d2,version+1,state);
  expected = aborted ? d1 : d2;
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,expected,sizeof(d2));
  check_torn_record(&ewlf,R,current,version,state);
  //the unit the rollover left is not erased
  if(raw_reads_erased(area + (n_units-1)*erase_size,erase_size)) throw_exception(ERROR_VERIFICATION_FAIL);
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,expected,sizeof(d2));
  ewlf_drift(&ewlf,R,state,expected,sizeof(d2),pinning);
  stateful_prng_fill(d3,sizeof(d3));
  lftl_write(&ewlf,area,d3,sizeof(d3));
  ewlf.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ewlf,area,d3,sizeof(d3));
  ewlf_check_numbering(&ewlf,R,version);

  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

void torn_ewlf_checksum2_test(){
  DEBUG_PRINTLN(__func__);
  torn_ewlf_checksum2_test_core(EWLF_RAW_ABORTED);
  torn_ewlf_checksum2_test_core(EWLF_RAW_HALF_PARTIAL);
  torn_ewlf_checksum2_test_core(EWLF_RAW_HALF);
  torn_ewlf_checksum2_test_core(EWLF_RAW_COMPLETE_PARTIAL);
}

//LFTL-009, LFTL-010, LFTL-011: the first access after a reset that finds a
//slot whose checksum2 reads all erased erases nothing: on a flash with zero
//overwrite it tombstones the slot, one word program, and a power cut during
//that program leaves the previous version untouched, to be decided again at
//the next boot; on a flash without it the scan programs nothing at all. On an
//unregistered copy of nvmd (2 slots) with NVM properties whose erase callback
//counts, and whose write callback writes, then reports the cut once, or
//refuses every write.
static int writes_before_cut = -1;
static uint8_t write_then_cut(void*dst_nvm_addr, const void*const src, uintptr_t size){
  const uint8_t status = nvm_write(dst_nvm_addr,src,size);
  if(status) return status;
  if(0 == writes_before_cut--) return 0x7F;
  return 0;
}
static uint8_t refusing_nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  (void)dst_nvm_addr;
  (void)src;
  (void)size;
  return 0x7E;
}
static unsigned int counted_erases = 0;
static uint8_t counting_nvm_erase(void*base_address, unsigned int n_pages){
  counted_erases++;
  return nvm_erase(base_address,n_pages);
}
void torn_checksum2_cut_repair_test(){
  DEBUG_PRINTLN(__func__);
  const bool pinning = nvm_props3.zero_overwrite;
  lftl_nvm_props_t props = nvm_props3;
  props.erase = counting_nvm_erase;
  props.next = LFTL_INVALID_POINTER;
  lftl_ctx_t ctx = nvmd;
  ctx.nvm_props = &props;
  ctx.data = LFTL_INVALID_POINTER;
  ctx.transaction_tracker = LFTL_INVALID_POINTER;
  ctx.next = LFTL_INVALID_POINTER;
  ctx.area_type = LFTL_AREA_TYPE_STD; //not registered, so set here
  const uint32_t write_size = props.write_size;
  const uint32_t item_size = write_size > sizeof(uint32_t) ? write_size : sizeof(uint32_t);
  const uintptr_t meta_phy_size = LFTL_META_N_ITEMS * item_size;
  const uintptr_t erase_size = props.erase_size;
  const uintptr_t n_pages_in_slot = LFTL_DIV_CEIL(ctx.data_size + meta_phy_size, erase_size);
  const uintptr_t slot_size = n_pages_in_slot * erase_size;
  const unsigned int n_slots = ctx.area_size / slot_size;
  if(n_slots != 2) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t previous[sizeof(nvm.data7)];
  uint8_t buf[sizeof(nvm.data7)];
  uint8_t rbuf[sizeof(nvm.data7)];
  lftl_format(&ctx);
  stateful_prng_fill(previous,sizeof(previous));
  lftl_write(&ctx,nvm.data7,previous,sizeof(previous));
  stateful_prng_fill(buf,sizeof(buf));
  lftl_write(&ctx,nvm.data7,buf,sizeof(buf));
  const unsigned int torn_index = ((uintptr_t)ctx.data - (uintptr_t)ctx.area) / slot_size;
  const unsigned int previous_index = 1 - torn_index;
  uint8_t*const torn = (uint8_t*)ctx.area + torn_index*slot_size;
  uint8_t*const torn_meta = torn + slot_size - meta_phy_size;
  uint8_t*const previous_meta = (uint8_t*)ctx.area + previous_index*slot_size + slot_size - meta_phy_size;
  //the torn slot, as test 24 builds it: checksum2 erased
  uint8_t data_img[ctx.data_size];
  uint8_t meta_img[meta_phy_size];
  lftl_memread(data_img,torn,ctx.data_size);
  lftl_memread(meta_img,torn_meta,meta_phy_size);
  raw_erase(torn,n_pages_in_slot);
  raw_write(torn,data_img,ctx.data_size);
  raw_write(torn_meta,meta_img,meta_phy_size - item_size); //version and checksum
  uint32_t previous_version;
  lftl_memread(&previous_version,previous_meta,sizeof(previous_version));
  //the first access after a reset: no erase, and at most one word program
  counted_erases = 0;
  ctx.data = LFTL_INVALID_POINTER; //reset
  if(pinning){
    //the tombstone, cut right after it landed
    props.write = write_then_cut;
    writes_before_cut = 0;
    EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_WRITE | 0x7F, lftl_read(&ctx,rbuf,nvm.data7,sizeof(rbuf)));
    writes_before_cut = -1;
    //the program hit the torn slot's version item, and nothing else
    uint8_t item[item_size];
    lftl_memread(item,torn_meta,item_size);
    for(unsigned int i=0;i<item_size;i++) if(0 != item[i]) throw_exception(ERROR_VERIFICATION_FAIL);
    lftl_memread(item,torn_meta+item_size,item_size);
    if(memcmp(item,meta_img+item_size,item_size)) throw_exception(ERROR_VERIFICATION_FAIL);
    uint8_t data_now[ctx.data_size];
    lftl_memread(data_now,torn,ctx.data_size);
    if(memcmp(data_now,data_img,ctx.data_size)) throw_exception(ERROR_VERIFICATION_FAIL);
  } else {
    //nothing is programmed: a write callback that refuses every write is never called
    props.write = refusing_nvm_write;
    read_and_check(&ctx,nvm.data7,previous,sizeof(previous));
  }
  if(0 != counted_erases) throw_exception(ERROR_VERIFICATION_FAIL);
  //the previous version is untouched
  uint32_t version_now;
  lftl_memread(&version_now,previous_meta,sizeof(version_now));
  if(version_now != previous_version) throw_exception(ERROR_VERIFICATION_FAIL);
  props.write = nvm_write;
  //the previous version is read, and the area still takes updates
  ctx.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ctx,nvm.data7,previous,sizeof(previous));
  stateful_prng_fill(buf,sizeof(buf));
  lftl_write(&ctx,nvm.data7,buf,sizeof(buf));
  ctx.data = LFTL_INVALID_POINTER; //reset
  read_and_check(&ctx,nvm.data7,buf,sizeof(buf));
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

//After a reset, ctx->data is LFTL_INVALID_POINTER until something locates
//the current slot: a transaction started before any read must locate it too.
void transaction_after_reset_test(){
  DEBUG_PRINTLN(__func__);
  const size_t size = sizeof(nvm.a_data);
  uint8_t rng_state = 0;
  uint8_t wbuf0[size];
  prng_fill(&rng_state,wbuf0,size);
  test_write(&nvma,&nvm.a_data,wbuf0,size);
  //simulate a reset
  nvma.data = LFTL_INVALID_POINTER;
  uint8_t nvma_transaction_tracker[LFTL_TRANSACTION_TRACKER_SIZE(&nvma)];
  transaction_start_func(&nvma,nvma_transaction_tracker);
  uint8_t wbuf[size];
  prng_fill(&rng_state,wbuf,size);
  transaction_write_func(&nvma,nvm.data0,wbuf,sizeof(nvm.data0));
  transaction_commit_func(&nvma);
  read_and_check(&nvma,nvm.data0,wbuf,sizeof(nvm.data0));//read commited data
  read_and_check(&nvma,nvm.data1,wbuf0+sizeof(nvm.data0),sizeof(nvm.data1));//read original data
  //force a search of the slot
  nvma.data = LFTL_INVALID_POINTER;
  read_and_check(&nvma,nvm.data0,wbuf,sizeof(nvm.data0));
  SANITY_CHECK();
}

//Meta-data with write units larger than 16 bytes, for a regular and an EWLF
//area: nvmd's pages through copies of its context and NVM properties with
//32-byte write units. The copies are not registered, so area_type is set here.
void large_write_unit_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props32 = nvm_props3;
  props32.write_size = 32;
  props32.next = LFTL_INVALID_POINTER;
  for(unsigned int ewlf=0;ewlf<2;ewlf++){
    lftl_ctx_t ctx32 = nvmd;
    ctx32.nvm_props = &props32;
    ctx32.data = LFTL_INVALID_POINTER;
    ctx32.data_size = 64;
    ctx32.transaction_tracker = LFTL_INVALID_POINTER;
    ctx32.next = LFTL_INVALID_POINTER;
    ctx32.area_type = ewlf ? LFTL_AREA_TYPE_EWLF : LFTL_AREA_TYPE_STD;
    lftl_format(&ctx32);
    uint8_t buf[64];
    stateful_prng_fill(buf,sizeof(buf));
    lftl_write(&ctx32,ctx32.area,buf,sizeof(buf));
    //force a search of the slot or record
    ctx32.data = LFTL_INVALID_POINTER;
    read_and_check(&ctx32,ctx32.area,buf,sizeof(buf));
  }
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

//The searches of the EWLF scan (documentation, anti-tearing page) over an EWLF area with many
//erase units: every page of the test NVM as one area (17 units of 9 records
//on the Linux geometry), through three cycles of writes with a reset before
//every read, and power cuts injected at every stage of a record write and
//during a rollover's erase. With LFTL_SCAN_CROSS_CHECK (the Linux build) the
//library runs the fast search and the linear pass at every scan and raises
//LFTL_INTERNAL_ERROR if they disagree; the test checks that every read after
//a reset returns the last complete write. The registered areas share these
//pages: they are formatted again at the end.
void ewlf_many_units_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props = nvm_props;
  props.erase = failing_once_nvm_erase;
  props.write = failing_once_nvm_write;
  props.next = LFTL_INVALID_POINTER;
  lftl_ctx_t ewlf = {
    .nvm_props = &props,
    .area = &nvm,
    .area_size = sizeof(nvm),
    .data = LFTL_INVALID_POINTER,
    .data_size = 400,
    .transaction_tracker = LFTL_INVALID_POINTER,
    .next = LFTL_INVALID_POINTER,
    .area_type = LFTL_AREA_TYPE_EWLF, //not registered, so set here
  };
  uint8_t*const area = (uint8_t*)&nvm;
  const uintptr_t data_size = ewlf.data_size;
  const uintptr_t erase_size = props.erase_size;
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  const unsigned int n_units = ewlf.area_size / erase_size;
  const unsigned int records_per_unit = erase_size / l.record_size;
  if((n_units < 8) || (records_per_unit < 4)) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d[data_size];
  uint8_t expected[data_size];
  lftl_format(&ewlf);
  memset(expected,0xFF,data_size); //a formatted EWLF area reads erased
  read_and_check(&ewlf,area,expected,data_size);
  const unsigned int n_writes = 3*n_units*records_per_unit;
  unsigned int stage = 0;
  for(unsigned int i=0;i<n_writes;i++){
    stateful_prng_fill(d,data_size);
    //whether this write rolls over: the current record is the lowest of its unit
    const uintptr_t current = (uintptr_t)ewlf.data - l.header_size;
    const bool rollover = ((current - (uintptr_t)area) % erase_size) < l.record_size;
    if(rollover && (0 == (i % 5))){
      //the power cut during the erase of the unit the rollover enters
      erase_calls_before_failure = 0;
      EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_ERASE | 1, lftl_write(&ewlf,area,d,data_size));
      erase_calls_before_failure = -1;
    } else if(3 == (i % 7)){
      //the power cut at every stage of a record write in turn: start marker,
      //data, meta field, flag-a, flag-b. A dropped flag-a leaves an aborted
      //record, which is not taken; a dropped flag-b leaves a half record,
      //which the next scan copies forward: that write's data is the one read
      write_calls_before_failure = stage;
      if(4 == stage) memcpy(expected,d,data_size);
      stage = (stage + 1) % 5;
      EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_WRITE | 1, lftl_write(&ewlf,area,d,data_size));
      write_calls_before_failure = -1;
    } else {
      lftl_write(&ewlf,area,d,data_size);
      memcpy(expected,d,data_size);
    }
    ewlf.data = LFTL_INVALID_POINTER; //reset
    read_and_check(&ewlf,area,expected,data_size);
  }
  //the registered areas live in these pages
  format_func(&nvma);
  format_func(&nvmb);
  format_func(&ewlfa);
  format_func(&ewlfb);
  format_func(&nvmc);
  format_func(&nvmd);
  SANITY_CHECK();
}

#ifdef HAS_TEARING_SIMULATION
//The simulator must corrupt every byte past the cut and none before it, for a
//cut anywhere in a multi-unit write or erase: its corruption loop once stopped
//at tsize instead of size, so a cut in the second half left the write complete
//while reporting a tear, and the sweep passed positions it never exercised.
static void check_cut(const uint8_t*nvm_bytes, const uint8_t*intact, uintptr_t cut, uintptr_t size){
  for(uintptr_t i=0;i<size;i++){
    const bool same = nvm_bytes[i] == intact[i];
    if(same != (i < cut)) throw_exception(ERROR_VERIFICATION_FAIL);
  }
}
static bool buf_is_all(const uint8_t*buf, uint8_t val, uintptr_t size){
  for(uintptr_t i=0;i<size;i++) if(buf[i] != val) return false;
  return true;
}
static unsigned int scripted_readings_left;
static bool scripted_reading(const void*unit_nvm_addr, uint8_t*unit, uintptr_t size){
  (void)unit_nvm_addr;
  if(0 == scripted_readings_left) return false;
  scripted_readings_left--;
  memset(unit,0xA5,size);
  return true;
}
//The weak word model, on nvmd's page for the duration of this test: a write
//cut after unit k programs the units before k, leaves unit k weak and the
//units after it erased; a weak unit reads differently from one read to the
//next; a program of zeros over it makes it solid; a scripted reading wins
//over the model; a torn erase leaves the whole page weak, a complete one
//makes it solid.
static void tearing_sim_weak_self_test(uint8_t*const page){
  const bool model_was_on = nvm_weak_model;
  const uintptr_t ws = nvm_write_size;
  const unsigned int n_units = 4;
  const uintptr_t size = n_units * ws;
  uint8_t src[size];
  uint8_t buf[size];
  nvm_weak_model = 1;
  tearing_sim_weak_scope_add(page,nvm_erase_size);
  for(unsigned int k=0;k<n_units;k++){
    stateful_prng_fill(src,size);
    raw_erase(page,1);
    tearing_sim_set_target(k);
    if(SIMULATED_TEARING != nvm_write(page,src,size)) throw_exception(ERROR_VERIFICATION_FAIL);
    if(nvm_read(buf,page,size)) throw_exception(ERROR_VERIFICATION_FAIL);
    for(unsigned int u=0;u<n_units;u++){
      const uint8_t*const unit = buf + u*ws;
      if(tearing_sim_is_weak(page+u*ws,ws) != (u == k)) throw_exception(ERROR_VERIFICATION_FAIL);
      if((u < k) && memcmp(unit,src+u*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if((u > k) && !buf_is_all(unit,0xFF,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
    }
    //the weak unit: intended, erased, zeros or under a mask, the same within
    //a boot, at least two of them seen over 64 boots
    unsigned int seen_intended=0, seen_erased=0, seen_zero=0, seen_other=0;
    for(unsigned int r=0;r<64;r++){
      uint8_t unit[ws], again[ws];
      tearing_sim_weak_new_boot();
      if(nvm_read(unit,page+k*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(nvm_read(again,page+k*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(memcmp(unit,again,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(0 == memcmp(unit,src+k*ws,ws)) seen_intended++;
      else if(buf_is_all(unit,0xFF,ws)) seen_erased++;
      else if(buf_is_all(unit,0x00,ws)) seen_zero++;
      else seen_other++;
    }
    if((seen_intended?1:0)+(seen_erased?1:0)+(seen_zero?1:0)+(seen_other?1:0) < 2) throw_exception(ERROR_VERIFICATION_FAIL);
    //a weak unit is not erased: only zeros may be programmed over it, and
    //that makes it solid
    uint8_t values[ws];
    memset(values,0x5A,ws);
    if(5 != nvm_write(page+k*ws,values,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
    //a scripted reading wins over the model
    scripted_readings_left = 1;
    nvm_weak_override = scripted_reading;
    tearing_sim_weak_new_boot();
    uint8_t unit[ws];
    if(nvm_read(unit,page+k*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
    nvm_weak_override = 0;
    if(!buf_is_all(unit,0xA5,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
    memset(values,0,ws);
    if(nvm_zero_overwrite){
      //zeros over the weak unit: accepted, and the unit is solid
      if(nvm_write(page+k*ws,values,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(tearing_sim_is_weak(page+k*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(nvm_read(unit,page+k*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(!buf_is_all(unit,0x00,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
    } else {
      //a flash without the all-zero exception refuses it, and the unit stays weak
      if(5 != nvm_write(page+k*ws,values,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
      if(!tearing_sim_is_weak(page+k*ws,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
    }
  }
  //a torn erase, then a complete one
  tearing_sim_set_target(1);
  if(SIMULATED_TEARING != nvm_erase(page,1)) throw_exception(ERROR_VERIFICATION_FAIL);
  for(uintptr_t off=0;off<nvm_erase_size;off+=ws){
    if(!tearing_sim_is_weak(page+off,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
  }
  if(nvm_erase(page,1)) throw_exception(ERROR_VERIFICATION_FAIL);
  if(tearing_sim_is_weak(page,nvm_erase_size)) throw_exception(ERROR_VERIFICATION_FAIL);
  tearing_sim_weak_scope_pop();
  nvm_weak_model = model_was_on;
}
void tearing_sim_self_test(){
  DEBUG_PRINTLN(__func__);
  const unsigned int n_units = 4;
  const uintptr_t size = n_units * nvm_write_size;
  uint8_t*const page = (uint8_t*)nvmd.area;
  uint8_t src[size];
  uint8_t erased[nvm_erase_size];
  memset(erased,0xFF,nvm_erase_size);
  for(unsigned int k=0;k<=n_units;k++){
    //a write of n_units units cut after unit k; k == n_units: no cut
    stateful_prng_fill(src,size);
    raw_erase(page,1);
    tearing_sim_set_target(k);
    const uint8_t status = nvm_write(page,src,size);
    if(status != (k < n_units ? SIMULATED_TEARING : 0)) throw_exception(ERROR_VERIFICATION_FAIL);
    check_cut(page,src,k*nvm_write_size,size);
    //an erase of the page cut after unit k
    tearing_sim_set_target(k);
    if(SIMULATED_TEARING != nvm_erase(page,1)) throw_exception(ERROR_VERIFICATION_FAIL);
    check_cut(page,erased,k*nvm_write_size,nvm_erase_size);
  }
  tearing_sim_weak_self_test(page);
  tearing_sim_init();
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}
#endif

//The checksum of EWLF records, CRC-64/NVME, against its published check
//value, then a record the test forges with it: taken by the scan, while one
//whose checksum is off by one bit is not. On an EWLF area over nvmd's pages.
void crc64_test(){
  DEBUG_PRINTLN(__func__);
  if(0xAE8B14860A799888ull != ~test_crc64(~(uint64_t)0,(const uint8_t*)"123456789",9)) throw_exception(ERROR_VERIFICATION_FAIL);
  if(0 != ~test_crc64(~(uint64_t)0,(const uint8_t*)"",0)) throw_exception(ERROR_VERIFICATION_FAIL);
  lftl_ctx_t ewlf = nvmd;
  ewlf.data = LFTL_INVALID_POINTER;
  ewlf.data_size = 64;
  ewlf.transaction_tracker = LFTL_INVALID_POINTER;
  ewlf.next = LFTL_INVALID_POINTER;
  ewlf.area_type = LFTL_AREA_TYPE_EWLF; //not registered, so set here
  uint8_t*const area = (uint8_t*)ewlf.area;
  const uint32_t write_size = ewlf.nvm_props->write_size;
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  uint8_t d1[64], d2[64];
  for(unsigned int wrong=0;wrong<2;wrong++){
    lftl_format(&ewlf);
    stateful_prng_fill(d1,sizeof(d1));
    lftl_write(&ewlf,area,d1,sizeof(d1));
    uint8_t*const current = (uint8_t*)ewlf.data - l.header_size;
    const uint32_t version = ewlf_raw_version(&ewlf,current) + 1;
    uint8_t*const R = current - l.record_size;
    stateful_prng_fill(d2,sizeof(d2));
    uint8_t zeros[l.flag_size];
    memset(zeros,0,l.flag_size);
    raw_write(R,zeros,l.flag_size);
    raw_write(R + l.header_size,d2,LFTL_ROUND_UP(sizeof(d2),write_size));
    uint8_t meta[l.meta_size];
    test_ewlf_meta(meta,l.meta_size,version,test_ewlf_checksum(version,d2,sizeof(d2)) ^ (wrong ? 1 : 0));
    raw_write(ewlf_raw_meta(&l,R),meta,l.meta_size);
    raw_write(ewlf_raw_flag_a(&l,R),zeros,l.flag_size);
    raw_write(ewlf_raw_flag_b(&l,R),zeros,l.flag_size);
    ewlf.data = LFTL_INVALID_POINTER; //reset
    read_and_check(&ewlf,area,wrong ? d1 : d2,sizeof(d2));
  }
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

//Areas formatted and written by lean-ftl v0.1.0, whose on-flash format this
//version must still read: test/v010-compat/gen-images.sh builds them with
//the sources of that tag and records them in v010_images.h as runs of
//programmed write units, so that they are rebuilt here in nvmd's pages with
//an erase and programs of erased units only, as real flash allows. Each is
//read as after a reset, on an unregistered copy of nvmd whose NVM properties
//count erases and refuse every program: the scan must settle on the slot
//v0.1.0 had current, touch nothing, and read what v0.1.0 read back. Then the
//registered area must still take updates.
#include "v010_images.h"
void v010_compat_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props = nvm_props3;
  props.erase = counting_nvm_erase;
  props.write = refusing_nvm_write;
  props.next = LFTL_INVALID_POINTER;
  const uint32_t write_size = props.write_size;
  const uint32_t item_size = write_size > sizeof(uint32_t) ? write_size : sizeof(uint32_t);
  const uintptr_t meta_phy_size = LFTL_META_N_ITEMS * item_size;
  const uintptr_t erase_size = props.erase_size;
  const uintptr_t n_pages_in_slot = LFTL_DIV_CEIL(nvmd.data_size + meta_phy_size, erase_size);
  const uintptr_t slot_size = n_pages_in_slot * erase_size;
  const uintptr_t n_pages = nvmd.area_size / erase_size;
  const unsigned int n_images = V010_N_IMAGES;
  if(0 == n_images) PRINTLN("WARNING: no lean-ftl v0.1.0 images for this geometry, nothing checked");
  for(unsigned int i=0;i<n_images;i++){
    const v010_image_t*const img = &v010_images[i];
    DEBUG_PRINTLN("v0.1.0 image %s",img->name);
    //made for exactly this area
    if((img->write_size != write_size) || (img->erase_size != erase_size)) throw_exception(INTERNAL_ERROR_CORRUPT);
    if((img->data_size != nvmd.data_size) || (img->area_size != nvmd.area_size)) throw_exception(INTERNAL_ERROR_CORRUPT);
    uint8_t*const area = (uint8_t*)nvmd.area;
    raw_erase(area,n_pages);
    for(unsigned int r=0;r<img->n_runs;r++){
      raw_write(area + img->runs[r].offset, img->runs[r].bytes, img->runs[r].size);
    }
    //the first access after a reset
    lftl_ctx_t ctx = nvmd;
    ctx.nvm_props = &props;
    ctx.data = LFTL_INVALID_POINTER;
    ctx.transaction_tracker = LFTL_INVALID_POINTER;
    ctx.next = LFTL_INVALID_POINTER;
    ctx.area_type = LFTL_AREA_TYPE_STD; //not registered, so set here
    counted_erases = 0;
    read_and_check(&ctx,nvm.data7,img->expected,nvmd.data_size);
    if(0 != counted_erases) throw_exception(ERROR_VERIFICATION_FAIL);
    if(img->slot != ((uintptr_t)ctx.data - (uintptr_t)ctx.area) / slot_size) throw_exception(ERROR_VERIFICATION_FAIL);
    //the registered area reads the same, and still takes updates
    nvmd.data = LFTL_INVALID_POINTER;
    read_and_check(&nvmd,nvm.data7,img->expected,nvmd.data_size);
    randomized_test_write(&nvmd,nvm.data7,nvmd.data_size);
  }
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

#ifdef HAS_TEARING_SIMULATION
//A simulated cut is the status byte a callback returns on one of the three
//low-level errors. LFTL_INTERNAL_ERROR (-1) ends in the same byte and is a bug
//(a cross-check disagreement, in this build), never a cut.
static bool is_simulated_tearing(uint32_t err_code){
  return (err_code == (LFTL_ERROR_LOW_LEVEL_ERASE | SIMULATED_TEARING))
      || (err_code == (LFTL_ERROR_LOW_LEVEL_WRITE | SIMULATED_TEARING))
      || (err_code == (LFTL_ERROR_LOW_LEVEL_READ | SIMULATED_TEARING));
}

//--- EWLF v2 under weak words without T3, with two cuts in a row: the cases
//the anti-tearing page's argument for EWLF areas rests on (the residual
//table of the documentation), as regression tests.

//The reading of a scripted weak write unit, the same at every read of a
//boot: what its program meant to leave (the unit comes pre-filled with it),
//all erased, all zeros, or given bytes
typedef enum { WEAK_INTENDED, WEAK_ERASED, WEAK_ZEROS, WEAK_VALUE } weak_mode_t;
#define WEAK_SCRIPT_MAX 8
static struct {
  const void*unit;
  weak_mode_t mode;
  uint8_t value[LFTL_WU_MAX_SIZE];
} weak_script[WEAK_SCRIPT_MAX];
static unsigned int n_weak_script;
static bool weak_script_reading(const void*unit_nvm_addr, uint8_t*unit, uintptr_t size){
  for(unsigned int i=0;i<n_weak_script;i++){
    if(unit_nvm_addr != weak_script[i].unit) continue;
    switch(weak_script[i].mode){
      case WEAK_ERASED: memset(unit,0xFF,size); break;
      case WEAK_ZEROS: memset(unit,0x00,size); break;
      case WEAK_VALUE: memcpy(unit,weak_script[i].value,size); break;
      default: break;
    }
    return true;
  }
  return false;
}
static void weak_script_clear(){
  n_weak_script = 0;
}
static void weak_script_set(const void*unit, weak_mode_t mode, const uint8_t*value){
  unsigned int i = 0;
  while((i < n_weak_script) && (unit != weak_script[i].unit)) i++;
  if(WEAK_SCRIPT_MAX == i) throw_exception(INTERNAL_ERROR_CORRUPT);
  if(i == n_weak_script) n_weak_script++;
  weak_script[i].unit = unit;
  weak_script[i].mode = mode;
  if(WEAK_VALUE == mode) memcpy(weak_script[i].value,value,nvm_write_size);
}

//a statement that must end in a simulated cut
#define EXPECT_CUT(stmt) do{\
    jmp_buf __saved_ctx;\
    memcpy(__saved_ctx, exception_ctx, sizeof(jmp_buf));\
    uint32_t __err_code = setjmp(exception_ctx);\
    if(0 == __err_code){\
      stmt;\
      memcpy(exception_ctx, __saved_ctx, sizeof(jmp_buf));\
      PRINTF("ERROR: expected a simulated cut but none happened, line %d\n",__LINE__);\
      fail_abort();\
    }\
    memcpy(exception_ctx, __saved_ctx, sizeof(jmp_buf));\
    if(!is_simulated_tearing(__err_code)){\
      PRINTF("ERROR: expected a simulated cut but got 0x%08x, line %d\n",(unsigned)__err_code,__LINE__);\
      fail_abort();\
    }\
  }while(0)
//a statement that may end in a simulated cut; `cut` tells whether it did
#define MAYBE_CUT(cut, stmt) do{\
    jmp_buf __saved_ctx;\
    memcpy(__saved_ctx, exception_ctx, sizeof(jmp_buf));\
    uint32_t __err_code = setjmp(exception_ctx);\
    if(0 == __err_code){\
      stmt;\
      cut = 0;\
    } else {\
      if(!is_simulated_tearing(__err_code)){\
        PRINTF("ERROR: unexpected error 0x%08x, line %d\n",(unsigned)__err_code,__LINE__);\
        fail_abort();\
      }\
      cut = 1;\
    }\
    memcpy(exception_ctx, __saved_ctx, sizeof(jmp_buf));\
  }while(0)

//An unregistered EWLF area over the pages of a registered one, with NVM
//properties that count erases, under the weak word model with scripted
//readings. Every record write is 5 program calls, start marker, data, meta
//field, flag-a, flag-b, 6 when it rolls over (the erase first).
#define EWLF_CALL_START 0
#define EWLF_CALL_DATA 1
#define EWLF_CALL_META 2
#define EWLF_CALL_FLAG_A 3
#define EWLF_CALL_FLAG_B 4
typedef struct {
  lftl_ctx_t ctx;
  lftl_nvm_props_t props;
  ewlf_layout_t l;
  uint8_t*area;
  uintptr_t erase_size;
  unsigned int n_units;
  unsigned int records_per_unit;
  bool model_was_on;
} ewlf_scratch_t;
static void ewlf_scratch_init(ewlf_scratch_t*s, const lftl_ctx_t*base, const lftl_nvm_props_t*props, unsigned int records_per_unit){
  s->props = *props;
  s->props.erase = counting_nvm_erase;
  s->props.next = LFTL_INVALID_POINTER;
  s->ctx = *base;
  s->ctx.nvm_props = &s->props;
  s->ctx.data = LFTL_INVALID_POINTER;
  s->ctx.data_size = 64; //for the header size, which does not depend on it
  s->ctx.transaction_tracker = LFTL_INVALID_POINTER;
  s->ctx.next = LFTL_INVALID_POINTER;
  s->ctx.area_type = LFTL_AREA_TYPE_EWLF;
  s->l = ewlf_layout(&s->ctx);
  s->erase_size = s->props.erase_size;
  const uint32_t write_size = s->props.write_size;
  s->ctx.data_size = ((s->erase_size / records_per_unit - s->l.header_size) / write_size) * write_size;
  s->l = ewlf_layout(&s->ctx);
  s->area = (uint8_t*)s->ctx.area;
  s->n_units = s->ctx.area_size / s->erase_size;
  s->records_per_unit = s->erase_size / s->l.record_size;
  if(s->records_per_unit != records_per_unit) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  s->model_was_on = nvm_weak_model;
  nvm_weak_model = 1;
  tearing_sim_weak_scope_add(s->ctx.area,s->ctx.area_size);
  nvm_weak_override = weak_script_reading;
  weak_script_clear();
  tearing_sim_weak_reseed(0x5EED);
  counted_erases = 0;
}
static void ewlf_scratch_end(ewlf_scratch_t*s){
  weak_script_clear();
  nvm_weak_override = 0;
  tearing_sim_weak_scope_pop();
  nvm_weak_model = s->model_was_on;
  tearing_sim_clear_call_target();
}
//the record at position `index` of a unit, 0 being the top one
static uint8_t*ewlf_scratch_record(const ewlf_scratch_t*s, unsigned int unit, unsigned int index){
  return s->area + unit*s->erase_size + s->erase_size - (index+1)*s->l.record_size;
}
static uint8_t*ewlf_scratch_current(const ewlf_scratch_t*s){
  return (uint8_t*)s->ctx.data - s->l.header_size;
}
//a write cut at a program call, at its last write unit
static void ewlf_scratch_cut_write(ewlf_scratch_t*s, const uint8_t*d, unsigned int call){
  tearing_sim_set_call_target(call,true);
  EXPECT_CUT(lftl_write(&s->ctx,s->area,d,s->ctx.data_size));
  tearing_sim_clear_call_target();
}
//a boot: reset, then a read that must return `expected`
static void ewlf_scratch_boot(ewlf_scratch_t*s, const uint8_t*expected){
  tearing_sim_weak_new_boot();
  s->ctx.data = LFTL_INVALID_POINTER;
  read_and_check(&s->ctx,s->area,expected,s->ctx.data_size);
}
//a boot whose scan is cut at one of its program calls
static void ewlf_scratch_cut_boot(ewlf_scratch_t*s, unsigned int call){
  uint8_t buf[s->ctx.data_size];
  tearing_sim_weak_new_boot();
  s->ctx.data = LFTL_INVALID_POINTER;
  tearing_sim_set_call_target(call,true);
  EXPECT_CUT(lftl_read(&s->ctx,buf,s->area,s->ctx.data_size));
  tearing_sim_clear_call_target();
}
static bool ewlf_scratch_meta_valid(const ewlf_scratch_t*s, const uint8_t*record, uint32_t version){
  return ewlf_raw_version(&s->ctx,record) == version;
}
#define EWLF_CHECK(cond) do{ if(!(cond)){ PRINTF("ERROR: %s, line %d\n",#cond,__LINE__); throw_exception(ERROR_VERIFICATION_FAIL); } }while(0)

//H1: a write cut in flag-b at the top of a unit is exposed when flag-b reads
//a bit, copied when it reads erased; the copy is cut; when no room is left the
//half record is pinned in place, never tombstoned, and the data stays.
static void ewlf_h1_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const bool pinning = s->props.zero_overwrite;
  const uintptr_t size = s->ctx.data_size;
  uint8_t d2[size], d3[size];
  lftl_format(&s->ctx); //v1 at the top of the last unit
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size); //v2 below it
  stateful_prng_fill(d3,size);
  ewlf_scratch_cut_write(s,d3,1+EWLF_CALL_FLAG_B); //v3 rolls over to the top of the unit below, cut in flag-b
  uint8_t*const V3 = ewlf_scratch_record(s,s->n_units-2,0);
  EWLF_CHECK(tearing_sim_is_weak(ewlf_raw_flag_b(&s->l,V3),s->l.flag_size));
  //boot 1: flag-b reads as programmed: complete, exposed
  weak_script_set(ewlf_raw_flag_b(&s->l,V3),WEAK_INTENDED,0);
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == V3);
  //boot 2: flag-b reads erased: half, copied below; the copy's flag-b is cut
  weak_script_set(ewlf_raw_flag_b(&s->l,V3),WEAK_ERASED,0);
  ewlf_scratch_cut_boot(s,EWLF_CALL_FLAG_B);
  uint8_t*const C1 = ewlf_scratch_record(s,s->n_units-2,1);
  EWLF_CHECK(ewlf_scratch_meta_valid(s,C1,4));
  EWLF_CHECK(tearing_sim_is_weak(ewlf_raw_flag_b(&s->l,C1),s->l.flag_size));
  //boot 3: both flag-b read erased: the copy is the newest half record, no
  //room below it, the unit above holds the complete record: pinned in place
  weak_script_set(ewlf_raw_flag_b(&s->l,C1),WEAK_ERASED,0);
  counted_erases = 0;
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  EWLF_CHECK(0 == counted_erases);
  EWLF_CHECK(pinning != tearing_sim_is_weak(ewlf_raw_flag_b(&s->l,C1),s->l.flag_size));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,V3,3)); //no half record tombstoned
  EWLF_CHECK(ewlf_scratch_meta_valid(s,C1,4));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,ewlf_scratch_record(s,s->n_units-1,1),2)); //the complete record untouched
  //boot 4: nothing scripted, the copy is complete for good
  weak_script_clear();
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  EWLF_CHECK(0 == counted_erases);
}

//H2: a chain of copies, each cut in flag-a, rolls over into the next unit but
//never into the unit of the complete record; when it is stuck the newest half
//record is pinned in place and the data stays. On a 3-unit area with 2
//records per unit, the chain spans two units.
static void ewlf_h2_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const bool pinning = s->props.zero_overwrite;
  const uintptr_t size = s->ctx.data_size;
  if(s->n_units < 3) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d2[size], d3[size];
  lftl_format(&s->ctx);
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size);
  stateful_prng_fill(d3,size);
  ewlf_scratch_cut_write(s,d3,1+EWLF_CALL_FLAG_B); //v3 at the top of unit n-2, flag-b weak
  const unsigned int u = s->n_units-2;
  uint8_t*const V3 = ewlf_scratch_record(s,u,0);
  weak_script_set(ewlf_raw_flag_b(&s->l,V3),WEAK_INTENDED,0);
  ewlf_scratch_boot(s,d3); //exposed
  weak_script_set(ewlf_raw_flag_b(&s->l,V3),WEAK_ERASED,0);
  ewlf_scratch_cut_boot(s,EWLF_CALL_FLAG_A); //C1 below v3, cut in flag-a
  uint8_t*const C1 = ewlf_scratch_record(s,u,1);
  weak_script_set(ewlf_raw_flag_a(&s->l,C1),WEAK_INTENDED,0);
  counted_erases = 0;
  ewlf_scratch_cut_boot(s,1+EWLF_CALL_FLAG_A); //C2 rolls over to unit n-3, erased first, cut in flag-a
  EWLF_CHECK(1 == counted_erases);
  uint8_t*const C2 = ewlf_scratch_record(s,u-1,0);
  weak_script_set(ewlf_raw_flag_a(&s->l,C2),WEAK_INTENDED,0);
  ewlf_scratch_cut_boot(s,EWLF_CALL_FLAG_A); //C3 below C2, cut in flag-a
  uint8_t*const C3 = ewlf_scratch_record(s,u-1,1);
  weak_script_set(ewlf_raw_flag_a(&s->l,C3),WEAK_INTENDED,0);
  //stuck: the next unit in write order holds the complete record v2
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C3);
  EWLF_CHECK(1 == counted_erases);
  EWLF_CHECK(ewlf_scratch_meta_valid(s,ewlf_scratch_record(s,s->n_units-1,0),1));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,ewlf_scratch_record(s,s->n_units-1,1),2));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,V3,3));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,C1,4));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,C2,5));
  EWLF_CHECK(ewlf_scratch_meta_valid(s,C3,6));
  if(pinning) EWLF_CHECK(ewlf_raw_is_all(ewlf_raw_flag_b(&s->l,C3),s->l.flag_size,0));
  //nothing scripted: with the pin, C3 is complete for good and nothing moves;
  //without, whichever record of the chain reads as the newest half one is
  //shown, and the chain may grow again from v3, whose flag-a is solid: all
  //hold d3
  weak_script_clear();
  ewlf_scratch_boot(s,d3);
  if(pinning){
    EWLF_CHECK(ewlf_scratch_current(s) == C3);
    EWLF_CHECK(1 == counted_erases);
  }
}

//H3: a tombstone cut in its first program leaves a weak unit in the meta
//field of an aborted record, the checksum's; at a later boot the field reads
//as written again and flag-a reads programmed: the record is half, and its
//copy is numbered above every valid record, not from a re-read field.
static void ewlf_h3_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  const uint32_t write_size = s->props.write_size;
  if(!s->props.zero_overwrite) return; //no tombstone without zero overwrite
  uint8_t d2[size], d3[size];
  lftl_format(&s->ctx);
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size);
  stateful_prng_fill(d3,size);
  ewlf_scratch_cut_write(s,d3,1+EWLF_CALL_FLAG_A); //v3 at the top of the unit below, flag-a weak
  uint8_t*const V3 = ewlf_scratch_record(s,s->n_units-2,0);
  //the last unit of the meta field, the first one a tombstone programs
  uint8_t*const last_meta_unit = ewlf_raw_meta(&s->l,V3) + s->l.meta_size - write_size;
  uint8_t original[write_size];
  ewlf_raw_read(original,last_meta_unit,write_size);
  //boot 1: flag-a reads erased: aborted, tombstoned, the tombstone cut at once
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_ERASED,0);
  ewlf_scratch_cut_boot(s,0);
  EWLF_CHECK(tearing_sim_is_weak(last_meta_unit,write_size));
  //boot 2: the weak unit reads as written, flag-a reads programmed: half,
  //copied, numbered 4
  weak_script_set(last_meta_unit,WEAK_VALUE,original);
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_ZEROS,0);
  ewlf_scratch_boot(s,d3);
  uint8_t*const C1 = ewlf_scratch_record(s,s->n_units-2,1);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  EWLF_CHECK(ewlf_scratch_meta_valid(s,C1,4));
  //boot 3: nothing scripted: the copy stays, whatever v3 reads
  weak_script_clear();
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
}

//H4: an aborted write whose tombstone was cut reads as no record when the
//next write is numbered: the write path tombstones it for good as it steps
//over it, so that the new record, numbered the same, is never confused with
//it. The new write is cut in flag-b and read back at every boot. On 3 records
//per unit, so that the aborted record is stepped over rather than erased.
static void ewlf_h4_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const bool pinning = s->props.zero_overwrite;
  const uintptr_t size = s->ctx.data_size;
  const uint32_t write_size = s->props.write_size;
  if(s->records_per_unit < 3) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d2[size], d3[size], d4[size];
  lftl_format(&s->ctx);
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size); //v2, second position of the last unit
  stateful_prng_fill(d3,size);
  ewlf_scratch_cut_write(s,d3,EWLF_CALL_FLAG_A); //v3, third position, flag-a weak
  uint8_t*const S3 = ewlf_scratch_record(s,s->n_units-1,2);
  uint8_t*const last_meta_unit = ewlf_raw_meta(&s->l,S3) + s->l.meta_size - write_size;
  //boot 1: aborted; with zero overwrite the tombstone is cut at once
  weak_script_set(ewlf_raw_flag_a(&s->l,S3),WEAK_ERASED,0);
  if(pinning) ewlf_scratch_cut_boot(s,0);
  //boot 2: the meta field reads garbage, v2 is exposed; the next write steps
  //over v3 and, with zero overwrite, tombstones it for good; it rolls over
  if(pinning) weak_script_set(last_meta_unit,WEAK_ERASED,0);
  ewlf_scratch_boot(s,d2);
  stateful_prng_fill(d4,size);
  //with zero overwrite the write first tombstones v3's meta field as it
  //steps over it, one program per unit that does not read all zeros (its
  //start marker already reads zeros): the cut is counted past those programs
  unsigned int pins = 0;
  if(pinning){
    uint8_t meta[s->l.meta_size];
    ewlf_raw_read(meta,ewlf_raw_meta(&s->l,S3),s->l.meta_size);
    for(uintptr_t u=0;u<s->l.meta_size;u+=write_size){
      for(uintptr_t i=0;i<write_size;i++) if(meta[u+i]){ pins++; break; }
    }
  }
  ewlf_scratch_cut_write(s,d4,pins+1+EWLF_CALL_FLAG_B);
  uint8_t*const R = ewlf_scratch_record(s,s->n_units-2,0);
  if(pinning){
    EWLF_CHECK(ewlf_raw_is_all(ewlf_raw_meta(&s->l,S3),s->l.meta_size,0));
    EWLF_CHECK(ewlf_scratch_meta_valid(s,R,3)); //the number reused
  } else {
    EWLF_CHECK(ewlf_scratch_meta_valid(s,R,4)); //numbered above the aborted record, still valid
  }
  //boot 3: flag-b reads a bit: exposed. boot 4: it reads erased, and v3's
  //flag-a reads programmed: the new record is the one copied and shown
  weak_script_set(ewlf_raw_flag_b(&s->l,R),WEAK_INTENDED,0);
  ewlf_scratch_boot(s,d4);
  EWLF_CHECK(ewlf_scratch_current(s) == R);
  weak_script_set(ewlf_raw_flag_b(&s->l,R),WEAK_ERASED,0);
  weak_script_set(ewlf_raw_flag_a(&s->l,S3),WEAK_ZEROS,0);
  weak_script_set(last_meta_unit,WEAK_INTENDED,0);
  ewlf_scratch_boot(s,d4);
  weak_script_clear();
  ewlf_scratch_boot(s,d4);
}

//Two more sequences: a kill (tombstone) and a repair (copy) on the same record
//never disagree about what is shown, and a weak flag-a that reads all zeros is
//copied, not trusted in place.
static void ewlf_counter_examples_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  uint8_t d2[size], d3[size];
  //CE1: cut in flag-a; flag-a reads a bit: copied, the copy cut in flag-b;
  //then flag-a reads erased and the copy's flag-b too: the copy is half with
  //a solid flag-a, newest, stuck, pinned; the source, aborted, is older than
  //the copy and left alone
  lftl_format(&s->ctx);
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size);
  stateful_prng_fill(d3,size);
  ewlf_scratch_cut_write(s,d3,1+EWLF_CALL_FLAG_A);
  uint8_t*const V3 = ewlf_scratch_record(s,s->n_units-2,0);
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_INTENDED,0);
  ewlf_scratch_cut_boot(s,EWLF_CALL_FLAG_B);
  uint8_t*const C1 = ewlf_scratch_record(s,s->n_units-2,1);
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_ERASED,0);
  weak_script_set(ewlf_raw_flag_b(&s->l,C1),WEAK_ERASED,0);
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  EWLF_CHECK(ewlf_scratch_meta_valid(s,V3,3));
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_meta_valid(s,V3,3));
  weak_script_clear();
  ewlf_scratch_boot(s,d3);
  //CE2: cut in flag-a; flag-a reads all zeros: copied all the same; then it
  //reads erased: aborted, older than the copy, left alone
  lftl_format(&s->ctx);
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size);
  stateful_prng_fill(d3,size);
  ewlf_scratch_cut_write(s,d3,1+EWLF_CALL_FLAG_A);
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_ZEROS,0);
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  EWLF_CHECK(ewlf_raw_is_all(ewlf_raw_flag_b(&s->l,C1),s->l.flag_size,0));
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_ERASED,0);
  ewlf_scratch_boot(s,d3);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  EWLF_CHECK(ewlf_scratch_meta_valid(s,V3,3));
  weak_script_clear();
  ewlf_scratch_boot(s,d3);
  //lftl_erase_all cut in flag-a: the copy of the erased record reads erased
  lftl_format(&s->ctx);
  stateful_prng_fill(d2,size);
  lftl_write(&s->ctx,s->area,d2,size);
  //an erase_all programs no data: erase, start marker, meta field, flag-a
  tearing_sim_set_call_target(3,true);
  EXPECT_CUT(lftl_erase_all(&s->ctx));
  tearing_sim_clear_call_target();
  weak_script_set(ewlf_raw_flag_a(&s->l,V3),WEAK_INTENDED,0);
  uint8_t erased[size];
  memset(erased,0xFF,size);
  ewlf_scratch_boot(s,erased);
  EWLF_CHECK(ewlf_scratch_current(s) == C1);
  weak_script_clear();
  ewlf_scratch_boot(s,erased);
}

void ewlf_two_cuts_test(){
  DEBUG_PRINTLN(__func__);
  ewlf_scratch_t s;
  //2 units of 2 records, over nvmd's pages
  ewlf_scratch_init(&s,&nvmd,&nvm_props3,2);
  ewlf_h1_test(&s);
  ewlf_h3_test(&s);
  ewlf_counter_examples_test(&s);
  ewlf_scratch_end(&s);
  //2 units of 3 records
  ewlf_scratch_init(&s,&nvmd,&nvm_props3,3);
  ewlf_h4_test(&s);
  ewlf_scratch_end(&s);
  //3 units of 2 records, over ewlfa's pages
  ewlf_scratch_init(&s,&ewlfa,&nvm_props,2);
  ewlf_h2_test(&s);
  ewlf_scratch_end(&s);
  format_func(&nvmd); //resync nvm_ref with the pages used
  format_func(&ewlfa);
  SANITY_CHECK();
}

//Random writes cut at random programs, boots with random weak readings and a
//second cut at a random program of the scan: after every boot the data read
//is one of the last writes, never older than one already shown, and never
//older than the last write that completed. On 2 units of 3 records over
//nvmd's pages, with the scans cross-checked by the build. Skipped without
//zero overwrite: the stuck state is then a documented residual.
void ewlf_weak_stress_test(){
  DEBUG_PRINTLN(__func__);
  if(!nvm_props3.zero_overwrite) return;
  ewlf_scratch_t s;
  ewlf_scratch_init(&s,&nvmd,&nvm_props3,3);
  const uintptr_t size = s.ctx.data_size;
  #define STRESS_HISTORY 8
  uint8_t history[STRESS_HISTORY][size];
  unsigned int seq = 0;             //the last write, history[seq % STRESS_HISTORY]
  unsigned int last_complete = 0;   //the last write that completed
  unsigned int last_shown = 0;      //the last write a boot showed
  xs_prng_set_seed(0x57E55);
  lftl_format(&s.ctx);
  memset(history[0],0xFF,size); //a formatted area reads erased
  for(unsigned int it=0;it<300;it++){
    //a write, cut at a random program with probability 1/2
    seq++;
    uint8_t*const d = history[seq % STRESS_HISTORY];
    stateful_prng_fill(d,size);
    const uint32_t r = xs_prng_get();
    if(r & 1){
      lftl_write(&s.ctx,s.area,d,size);
      last_complete = seq;
    } else {
      tearing_sim_set_call_target((r >> 1) % 6,(r >> 4) & 1);
      bool cut;
      MAYBE_CUT(cut,lftl_write(&s.ctx,s.area,d,size));
      tearing_sim_clear_call_target();
      if(!cut) last_complete = seq; //the call did not happen: no rollover
    }
    //boots, each with its own weak readings, some with a second cut
    const unsigned int n_boots = 1 + (xs_prng_get() % 3);
    for(volatile unsigned int boot=0;boot<n_boots;boot++){
      const uint32_t b = xs_prng_get();
      tearing_sim_weak_reseed(((uint64_t)it << 8) | boot);
      tearing_sim_weak_new_boot();
      s.ctx.data = LFTL_INVALID_POINTER;
      if(0 == (b & 3)) tearing_sim_set_call_target((b >> 2) % 8,(b >> 5) & 1);
      uint8_t buf[size];
      bool cut;
      MAYBE_CUT(cut,lftl_read(&s.ctx,buf,s.area,size));
      tearing_sim_clear_call_target();
      if(cut) continue;
      unsigned int shown = 0;
      bool found = false;
      for(unsigned int back=0;(back<STRESS_HISTORY) && (back<=seq);back++){
        if(0 == memcmp(buf,history[(seq-back) % STRESS_HISTORY],size)){
          shown = seq-back;
          found = true;
          break;
        }
      }
      if(!found){
        PRINTF("ERROR: iteration %u boot %u: the data read is none of the last writes\n",it,boot);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if(shown < last_shown){
        PRINTF("ERROR: iteration %u boot %u: rollback, write %u shown after write %u\n",it,boot,shown,last_shown);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if(shown < last_complete){
        PRINTF("ERROR: iteration %u boot %u: write %u shown, write %u had completed\n",it,boot,shown,last_complete);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      last_shown = shown;
    }
    //the next write goes below the record the last boot exposed; a boot that
    //was cut exposed nothing: boot again, uncut, for it
    if(LFTL_INVALID_POINTER == s.ctx.data){
      uint8_t buf[size];
      tearing_sim_weak_new_boot();
      lftl_read(&s.ctx,buf,s.area,size);
    }
  }
  ewlf_scratch_end(&s);
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}
#endif

#ifdef HAS_TEARING_SIMULATION
//A second cut, inside the recovery from the first: the sweep arms it at one
//of the program calls the recovery's boots made when run without it (a
//tombstone, a copy, a pin), and the recovery from that one runs without arming
static int64_t secondary_call_target = -1;
static bool secondary_at_last_unit;
static unsigned int recovery_depth;
static uint64_t recovery_calls; //program calls made by the recovery's boots, run without a second cut
#endif

void exception_handler(uint32_t err_code){
  #ifdef HAS_TEARING_SIMULATION
  if(is_simulated_tearing(err_code)){
    //simulated tearing, see if we recover well, including for the next write
    const bool first = (0 == recovery_depth++);
    if(first && (secondary_call_target >= 0)) tearing_sim_set_call_target(secondary_call_target,secondary_at_last_unit);
    const uint64_t calls_before = tearing_sim_get_call_count();
    tearing_sim_check_nvm();
    tearing_sim_clear_call_target();
    if(first && (secondary_call_target < 0)) recovery_calls = tearing_sim_get_call_count() - calls_before;
    tearing_sim_write_after_recovery();
    format_func(&nvma);
    format_func(&nvmb);
    format_func(&ewlfa);
    format_func(&ewlfb);
    format_func(&nvmc);
    format_func(&nvmd);
  } else {
    //a real error, that's unexpected
    PRINTF("ERROR: test failed with error code 0x%08x\n",err_code);
    //ui_wait_button();
    fail_abort();
  }
  #else
    #ifdef HAS_PRINTF
      PRINTF("EXCEPTION: 0x%08lx\n\r",(long unsigned int)err_code);
      while(1);
    #else
      ui_wait_button();
    #endif
  #endif
}

void test_and_simulate_tearing(void (*dut)(), unsigned int target_percentage){
  static unsigned int test_cnt=0;
  uint32_t err_code=-1;
  led1(1);
  if(0 == (err_code = setjmp(exception_ctx))){
    #ifdef HAS_TEARING_SIMULATION
    PRINTLN("test_and_simulate_tearing for DUT at 0x%lx",(uintptr_t)dut);
    erase_cnt_init();
    erase_cnt_enabled = 1;
    tearing_sim_init(); // ensure any previous tearing sim is stopped at this point
    //the weak word model applies to the EWLF areas; the regular areas rely on T3
    tearing_sim_weak_scope_clear();
    tearing_sim_weak_scope_add(ewlfa.area,ewlfa.area_size);
    tearing_sim_weak_scope_add(ewlfb.area,ewlfb.area_size);
    recovery_depth = 0;
    secondary_call_target = -1;
    current_secondary_target = -1;
    #endif
    lftl_init_lib();
    // nvmc/nvmd are registered first: their nvm_props (nvm_props2/nvm_props3)
    // each cover a range nested within the shared nvm_props used below, so
    // registering them first ensures all three actually end up as distinct
    // nodes in the nvm_props linked list (see addr_to_nvm/lftl_register_nvm)
    // instead of nvm_props2/nvm_props3 being folded into the already-registered,
    // broader nvm_props.
    lftl_register_area(&nvmc);
    lftl_register_area(&nvmd);
    lftl_register_area(&nvma);
    lftl_register_area(&nvmb);
    lftl_register_area_ewlf(&ewlfa);
    lftl_register_area_ewlf(&ewlfb);
    format_func(&nvma);
    format_func(&nvmb);
    format_func(&ewlfa);
    format_func(&ewlfb);
    format_func(&nvmc);
    format_func(&nvmd);
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
    erase_cnt_enabled = 0;
    tearing_sim_check_nvm();//sanity check that model is in sync
    print_erase_cnt_stats();
    volatile unsigned int target_max = tearing_sim_get_max_target();
    const unsigned int n_targets = (target_max * target_percentage) / 100;
    volatile unsigned int target_start = target_max - n_targets;
    if(tear_at){ //one cut only, to reproduce a failure
      if(tear_at > target_max){
        PRINTLN("ERROR: --tear-at=%llu beyond the last cut, %u",(unsigned long long)tear_at,target_max);
        fail_abort();
      }
      target_start = tear_at;
      target_max = tear_at;
    }
    PRINTF("%u targets for tearing simulation, testing %u (%u%%) of them from index %u\n",target_max,target_max-target_start,target_percentage,target_start);
    led1(1);
    format_func(&nvma);
    format_func(&nvmb);
    format_func(&ewlfa);
    format_func(&ewlfb);
    format_func(&nvmc);
    format_func(&nvmd);
    for(volatile unsigned int i=target_start;i<target_max+1;i++){//volatile to remove warning about setjump.
      //if(0 == (i%1000)) PRINTF("tearing simulation target %u\n",i);
      if(0 == (i%50)) print_progress_bar(i-target_start,n_targets);
      //the cut alone first; then, when the boots that recovered from it
      //programmed anything, the same cut followed by a second one at each of
      //those programs, at its first write unit and at its last
      recovery_calls = 0;
      for(volatile int64_t k=-1;k<(int64_t)recovery_calls;k++){
        for(volatile unsigned int last=0;last<2;last++){
          if((k < 0) && last) break;
          recovery_depth = 0;
          secondary_call_target = k;
          secondary_at_last_unit = last;
          current_cut_target = i;
          current_secondary_target = k;
          tearing_sim_set_target(i);
          if(0 == (err_code = setjmp(exception_ctx))){
            dut();//simulated tearing test
          } else {
            exception_handler(err_code);
          }
        }
      }
      secondary_call_target = -1;
      current_secondary_target = -1;
    }
    print_progress_bar_done();
    err_code = 0;
    led1(0);
  #endif
  test_cnt++;
  if(0==err_code){
    DEBUG_PRINTLN("Test %d PASS",test_cnt);
  }else{
    #ifdef HAS_TEARING_SIMULATION
      exit(err_code);
    #endif
    ui_wait_button();
    led1(0);
    while(1);
  }
}

void write_nvm_to_nvm_vs_size_1()   {DEBUG_PRINTLN("write_nvm_to_nvm_vs_size_1");write_nvm_to_nvm_vs_size(1);}                    // smaller than WU
void write_nvm_to_nvm_vs_size_1wu() {DEBUG_PRINTLN("write_nvm_to_nvm_vs_size_1wu");write_nvm_to_nvm_vs_size(sizeof(lftl_wu_t));}    // single WU
void write_nvm_to_nvm_vs_size_2wu() {DEBUG_PRINTLN("write_nvm_to_nvm_vs_size_2wu");write_nvm_to_nvm_vs_size(sizeof(lftl_wu_t)*2);}  // multiple WU
void write_nvm_to_nvm_vs_size_1wu1(){DEBUG_PRINTLN("write_nvm_to_nvm_vs_size_1wu1");write_nvm_to_nvm_vs_size(sizeof(lftl_wu_t)+1);}  // larger than WU size but not multiple of it

void write_nvm_to_nvm_seq(unsigned int target_percentage){
  DEBUG_PRINTLN("write_nvm_to_nvm_seq");
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_1   ,target_percentage);
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_1wu ,target_percentage);
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_2wu ,target_percentage);
  test_and_simulate_tearing(write_nvm_to_nvm_vs_size_1wu1,target_percentage);
}
void transaction_nvm_to_nvm_seq(unsigned int target_percentage){
  DEBUG_PRINTLN("transaction_nvm_to_nvm_seq");
  write_func_t org_write_func = write_func;
  write_func = write_func_using_transaction;
  write_nvm_to_nvm_seq(target_percentage);
  write_func = org_write_func;
}

void print_lib_info(){
  PRINTLN("version: %s",lftl_version());
  PRINTLN("version timestamp: %llu",(long long unsigned int)lftl_version_timestamp());
  PRINTLN("build type: %s",lftl_build_type());
}


void test_callbacks(){
  print_lib_info();
  uint32_t status;
  do{
    if((status = nvm_erase(&nvm,sizeof(nvm)/LFTL_PAGE_SIZE))) {status|=0x100;break;}
    DEBUG_PRINTLN("Erased state:");
    DEBUG_DUMP((uintptr_t)&nvm,sizeof(nvm));
    uint8_t buf[64];
    const unsigned int nloops = sizeof(nvm) / sizeof(buf);
    xs_prng_set_seed(0);
    uint8_t*addr = (uint8_t*)&nvm;
    for(unsigned int i=0;i<nloops;i++){
      xs_prng_fill(buf,sizeof(buf));
      if((status = nvm_write(addr,buf,sizeof(buf)))) {status|=0x200;break;}
      uint8_t buf2[64];
      if((status = nvm_read(buf2,addr,sizeof(buf)))) {status|=0x300;break;}
      if(memcmp(buf,buf2,sizeof(buf))){
        PRINTLN("ERROR during writing loop %u",i);
        dump((uintptr_t)buf,sizeof(buf));
        dump((uintptr_t)buf2,sizeof(buf2));
        while(1);
      }else{
        DEBUG_PRINTLN("Writing loop %u PASS",i);
      }
      addr+=sizeof(buf);
    }
    xs_prng_set_seed(0);
    addr = (uint8_t*)&nvm;
    for(unsigned int i=0;i<nloops;i++){
      xs_prng_fill(buf,sizeof(buf));
      uint8_t buf2[64];
      if((status = nvm_read(buf2,addr,sizeof(buf)))) {status|=0x400;break;}
      if(memcmp(buf,buf2,sizeof(buf))){
        PRINTLN("ERROR during reading loop %u",i);
        dump((uintptr_t)buf,sizeof(buf));
        dump((uintptr_t)buf2,sizeof(buf2));
        while(1);
      }else{
        DEBUG_PRINTLN("Reading loop %u PASS",i);
      }
      addr+=sizeof(buf);
    }
    //try out a write with unaligned source
    if((status = nvm_erase(&nvm,sizeof(nvm)/LFTL_PAGE_SIZE))) {status|=0x500;break;}
    addr = (uint8_t*)&nvm;
    uint64_t aligned_buf[SIZE64(LFTL_WU_SIZE*2+1)];
    xs_prng_fill(aligned_buf,sizeof(aligned_buf));
    uint8_t*unaligned_buf = ((uint8_t*)&aligned_buf) + 1;
    const unsigned int unaligned_write_size = LFTL_ROUND_UP(sizeof(aligned_buf) - 1 -LFTL_WU_SIZE,LFTL_WU_SIZE);
    if((status = nvm_write(addr,unaligned_buf,unaligned_write_size))) {status|=0x600;break;}
    if((status = nvm_read(buf,addr,unaligned_write_size))) {status|=0x700;break;}
    const uint8_t*const expected8 = unaligned_buf;
    for(unsigned int i=0;i<unaligned_write_size;i++){
      if(buf[i]!=expected8[i]){
        PRINTLN("READ VALUE:");
        dump((uintptr_t)buf,unaligned_write_size);
        PRINTLN("EXPECTED:");
        dump((uintptr_t)expected8,unaligned_write_size);
        while(1);
      }
    }
    PRINTLN("Callbacks test PASS");
  }while(0);
  if(status){
    #ifdef HAS_PRINTF
      PRINTLN("ERROR during callbacks test: status = 0x%08lx",(long unsigned int)status);
      while(1);
    #else
      ui_wait_button();
    #endif
  }
}

uint64_t get_u64_core(const char*val_str, uint64_t k){
  char*end;
  uint64_t out = strtoull(val_str,&end,0);
  size_t factor=1;
  if(*end=='K' || *end=='k') factor = k;
  if(*end=='M' || *end=='m') factor = k*k;
  if(*end=='G' || *end=='g') factor = k*k*k;
  out *= factor;
  return out;
}
uint64_t get_u64(const char*val_str){
  return get_u64_core(val_str,0);
}
uint64_t get_u64_k(const char*val_str){
  return get_u64_core(val_str,1000);
}
uint64_t get_u64_kb(const char*val_str){
  return get_u64_core(val_str,1024);
}
#define U64_ARG(val,argstr,k) if(0==memcmp(argv[i],argstr,strlen(argstr))){\
    const char*arg_val_str = argv[i]+strlen(argstr);\
    val = get_u64_core(arg_val_str,k);\
    DEBUG_PRINTLN("DEBUG: %s%lu",argstr,val);\
    consumed[i]=1;\
    continue;\
  }
#define ACTION_ARG(val,argstr) if(0==memcmp(argv[i],argstr,strlen(argstr))){\
    val = 1;\
    DEBUG_PRINTLN("DEBUG: %s set",argstr);\
    consumed[i]=1;\
    continue;\
  }
#define TEAR_COV(default_coverage) do{\
    tear_cov_log[i] = tear_cov ? tear_cov : default_coverage;\
  }while(0)
int test_main(int argc, const char*argv[], bool consumed[]){
  #ifdef HAS_TEARING_SIMULATION
    format_func = tearing_sim_lftl_format;
    raw_nvm_write_func = tearing_sim_nvm_write;
    raw_nvm_erase_func = tearing_sim_nvm_erase;
    erase_all_func = tearing_sim_lftl_erase_all;
    write_func = tearing_sim_lftl_write;
    write_any_func = tearing_sim_lftl_write_any;
    transaction_start_func = tearing_sim_lftl_transaction_start;
    transaction_write_func = tearing_sim_lftl_transaction_write;
    transaction_write_any_func = tearing_sim_lftl_transaction_write_any;
    transaction_commit_func = tearing_sim_lftl_transaction_commit;
    transaction_abort_func = tearing_sim_lftl_transaction_abort;
  #else
    format_func = lftl_format;
    raw_nvm_write_func = nvm_write;
    raw_nvm_erase_func = nvm_erase;
    erase_all_func = lftl_erase_all;
    write_func = lftl_write;
    write_any_func = lftl_write_any;
    transaction_start_func = lftl_transaction_start;
    transaction_write_func = lftl_transaction_write;
    transaction_write_any_func = lftl_transaction_write_any;
    transaction_commit_func = lftl_transaction_commit;
    transaction_abort_func = lftl_transaction_abort;
  #endif
  
  #define N_TESTS 40
  #define TEAR_COV_NA 101
  unsigned int tear_cov_log[N_TESTS+1] = {0};
  uint64_t first_index=1;
  uint64_t last_index=N_TESTS;
  uint64_t test_index=0;
  uint64_t tear_cov=0;
  bool report_n_tests=0;
  bool lib_info=0;
  bool run_test_callbacks=0;
  bool no_pinning=0;
  bool weak_words=0;
  bool no_weak_words=0;
  uint64_t weak_seed=1;
  for(int i=1;i<argc;i++){
    if(consumed[i]) continue;
    ACTION_ARG(report_n_tests,"--n-tests");
    ACTION_ARG(lib_info,"--lib-info");
    ACTION_ARG(run_test_callbacks,"--test-callbacks");
    ACTION_ARG(no_pinning,"--no-pinning");
    ACTION_ARG(weak_words,"--weak-words");
    ACTION_ARG(no_weak_words,"--no-weak-words");
    U64_ARG(weak_seed,"--weak-seed=",0);
    U64_ARG(tear_at,"--tear-at=",0);
    U64_ARG(test_index,"--test=",0);
    U64_ARG(first_index,"--first=",0);
    U64_ARG(last_index,"--last=",0);
    U64_ARG(tear_cov,"--tear-cov=",0);
    if(!consumed[i]){
      printf("ERROR unsupported command line argument: '%s'\n",argv[i]);
      fail_abort();
    }
  }
  if(no_pinning){
    //a flash without the all-zero exception (N3 of the flash page): the scan
    //then never programs, and its decisions about a weak word are not durable
    nvm_props.zero_overwrite = 0;
    nvm_props2.zero_overwrite = 0;
    nvm_props3.zero_overwrite = 0;
    #ifdef HAS_TEARING_SIMULATION
    nvm_zero_overwrite = 0;
    #endif
    PRINTLN("NVM without zero overwrite: the scan programs nothing");
  }
  #ifdef HAS_TEARING_SIMULATION
  //on by default with zero overwrite: a word a cut left weak then reads
  //differently at every read, all erased included, for ever (T2 of the flash
  //page without T3), in the EWLF areas. Off without zero overwrite unless
  //asked: the stuck state of the EWLF scan is then a documented residual,
  //which the sweep would report as a rollback.
  if(!no_weak_words && (weak_words || !no_pinning)){
    nvm_weak_model = 1;
    nvm_weak_seed = weak_seed;
    n_check_boots = 2; //a word may read differently at the second boot
    PRINTLN("Weak words in the EWLF areas, seed %llu",(unsigned long long)weak_seed);
  }
  #else
  (void)weak_words;
  (void)no_weak_words;
  (void)weak_seed;
  #endif
  if(report_n_tests){
    if(lib_info){
      print_lib_info();
    }
    PRINTLN("%u",N_TESTS);
    return 0;
  }
  if(lib_info){
    print_lib_info();
    return 0;
  }
  if(run_test_callbacks){
    test_callbacks();
  }
  if(tear_cov>100){
    PRINTLN("ERROR: max value for --tear-cov is 100");
    fail_abort();
  }
  if(test_index){
    first_index=test_index;
    last_index=test_index;
  }
  if(first_index > last_index){
    PRINTLN("ERROR: '--first' must be less or equal to '--last'");
    fail_abort();
  }
  if(last_index>N_TESTS){
    PRINTLN("ERROR: '--last' max value is %u",N_TESTS);
    fail_abort();
  }
  print_lib_info();
  led1(1);
  for(unsigned int i=first_index;i<=last_index;i++){
    PRINTLN("INFO: Run test %u",i);
    switch(i){
      case 1: TEAR_COV(100);test_and_simulate_tearing(basic_test            ,tear_cov_log[i]);break;
      case 2: TEAR_COV(100);test_and_simulate_tearing(write_size_test       ,tear_cov_log[i]);break;
      case 3: TEAR_COV(100);test_and_simulate_tearing(write_offset_test     ,tear_cov_log[i]);break;
      case 4: TEAR_COV(100);test_and_simulate_tearing(transaction_basic_test,tear_cov_log[i]);break;
      case 5: TEAR_COV(100);test_and_simulate_tearing(transaction_abort_test,tear_cov_log[i]);break;
      case 6: TEAR_COV(100);test_and_simulate_tearing(erase_all_test        ,tear_cov_log[i]);break;
      case 7: TEAR_COV(100);write_nvm_to_nvm_seq(                            tear_cov_log[i]);break;
      case 8: TEAR_COV(100);transaction_nvm_to_nvm_seq(                      tear_cov_log[i]);break;
      case 9: TEAR_COV( 10);test_and_simulate_tearing(ewlf_basic_test       ,tear_cov_log[i]);break;
      case 10:TEAR_COV(100);test_and_simulate_tearing(multi_nvm_test        ,tear_cov_log[i]);break;
      case 11:TEAR_COV(100);test_and_simulate_tearing(write_any_test        ,tear_cov_log[i]);break;
      case 12:TEAR_COV(100);test_and_simulate_tearing(get_ctx_test          ,tear_cov_log[i]);break;
      case 13:TEAR_COV(100);test_and_simulate_tearing(memread_newer_test    ,tear_cov_log[i]);break;
      case 14:TEAR_COV( 10);test_and_simulate_tearing(zero_size_test        ,tear_cov_log[i]);break;
      case 15:TEAR_COV( 10);test_and_simulate_tearing(transaction_misuse_test,tear_cov_log[i]);break;
      case 16:TEAR_COV(100);test_and_simulate_tearing(untouched_transaction_read_test,tear_cov_log[i]);break;
      case 17:TEAR_COV(  0);test_and_simulate_tearing(empty_registry_test   ,tear_cov_log[i]);break;
      case 18:TEAR_COV( 10);test_and_simulate_tearing(out_of_range_test     ,tear_cov_log[i]);break;
      case 19:TEAR_COV(100);test_and_simulate_tearing(unaligned_edge_test   ,tear_cov_log[i]);break;
      case 20:TEAR_COV( 10);test_and_simulate_tearing(aligned_misuse_test   ,tear_cov_log[i]);break;
      case 21:
        //Hardcoded 0, ignoring --tear-cov: find_current_slot's own meta
        //reads happen before it can detect the deliberately-planted version
        //collision, so an injected read failure there can legitimately
        //produce a different (also genuine) error than the one this test
        //expects. Sweeping adds no coverage value here anyway -- the
        //collision comparison itself is a single, deterministic in-memory
        //check once reached.
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(version_collision_test,0);break;
      case 22:TEAR_COV( 10);test_and_simulate_tearing(ewlf_packing_test     ,tear_cov_log[i]);break;
      case 23:TEAR_COV(100);test_and_simulate_tearing(ewlf_rollover_test    ,tear_cov_log[i]);break;
      case 24:TEAR_COV(100);test_and_simulate_tearing(torn_checksum2_test   ,tear_cov_log[i]);break;
      case 25:TEAR_COV(100);test_and_simulate_tearing(transaction_after_reset_test,tear_cov_log[i]);break;
      case 26:TEAR_COV(100);test_and_simulate_tearing(large_write_unit_test ,tear_cov_log[i]);break;
      case 27:TEAR_COV(100);test_and_simulate_tearing(bad_config_test       ,tear_cov_log[i]);break;
      case 28:
        //Hardcoded 0, ignoring --tear-cov, for the same reason as test 21: an
        //injected read failure during the expected NO_VALID_VERSION scan would
        //produce a different error, and the hand-built states are what matter.
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(ewlf_grid_test,0);break;
      case 29:
        //Hardcoded 0, ignoring --tear-cov: it cuts power at chosen points
        //itself, and an injected failure would change the expected error.
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(ewlf_tearing_test,0);break;
      case 30:TEAR_COV(100);test_and_simulate_tearing(ewlf_unaligned_size_test,tear_cov_log[i]);break;
      case 31:TEAR_COV(100);test_and_simulate_tearing(memset_whole_area_test,tear_cov_log[i]);break;
      case 32:TEAR_COV(100);test_and_simulate_tearing(torn_erase_version_test,tear_cov_log[i]);break;
      case 33:TEAR_COV(100);test_and_simulate_tearing(torn_ewlf_checksum2_test,tear_cov_log[i]);break;
      case 34:
        //Hardcoded 0, ignoring --tear-cov: the test injects its own erase
        //failure, and a tear injected around it would change the expected error.
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(torn_checksum2_cut_repair_test,0);break;
      case 35:
        //Hardcoded 0, ignoring --tear-cov: the test sets the simulator's cut
        //itself. It tests the simulator, so it only exists where there is one.
        tear_cov_log[i]=TEAR_COV_NA;
        #ifdef HAS_TEARING_SIMULATION
        test_and_simulate_tearing(tearing_sim_self_test,0);
        #endif
        break;
      case 36:
        //Hardcoded 0, ignoring --tear-cov: the test cuts power at chosen points
        //itself, and an injected failure would change the expected error.
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(ewlf_many_units_test,0);break;
      case 37:TEAR_COV(100);test_and_simulate_tearing(v010_compat_test     ,tear_cov_log[i]);break;
      case 38:TEAR_COV( 10);test_and_simulate_tearing(crc64_test           ,tear_cov_log[i]);break;
      case 39:
      case 40:
        //Hardcoded 0, ignoring --tear-cov: the tests cut power at chosen
        //points themselves, under the weak word model, which only exists
        //where there is a simulator
        tear_cov_log[i]=TEAR_COV_NA;
        #ifdef HAS_TEARING_SIMULATION
        test_and_simulate_tearing((39 == i) ? ewlf_two_cuts_test : ewlf_weak_stress_test,0);
        #endif
        break;
      default:
        PRINTLN("ERROR: bad test index %u",i);
        fail_abort();
    }
  }
  
  #ifdef HAS_PRINTF
    unsigned int min_tear_coverage = 100;
    for(unsigned int i=first_index;i<=last_index;i++){
      if(tear_cov_log[i]){
        if(TEAR_COV_NA == tear_cov_log[i]){
          PRINTLN("Test %2u run (tearing coverage not applicable).",i);
        }else{
          PRINTLN("Test %2u run with tearing coverage of %3u%%.",i,tear_cov_log[i]);
          min_tear_coverage = tear_cov_log[i] < min_tear_coverage ? tear_cov_log[i] : min_tear_coverage;
        }
      }
    }
    if(min_tear_coverage < 100){
      PRINTLN("WARNING: some test ran with tearing coverage of %2u%%.",min_tear_coverage);
    }
    if((0==first_index) && (N_TESTS==last_index)){ 
      PRINTLN("All tests PASSED");
    }else{
      if(last_index > first_index + 1){
        PRINTLN("Tests %lu to %lu PASSED",(unsigned long)first_index,(unsigned long)last_index);
      }else{
        PRINTLN("Test %lu PASSED",(unsigned long)first_index);
      }
    }
  #else
    while(1){ui_led1_blink_ms(1000,DUTY_CYCLE_75);}
  #endif
  return 0;
}

