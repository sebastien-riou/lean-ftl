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
extern lftl_ctx_t largea;

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
//--shard=K --shards=N: a sweep runs only the cuts whose index is K modulo N,
//so that N processes share it (test/run-suite); --tear-at= has priority
static uint64_t shard_index;
static uint64_t shard_count = 1;

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
void tearing_sim_weak_clear();
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
static bool sweep_active; //false during the run without a cut that precedes a sweep
static void print_cut_context(){
  if(!sweep_active){
    PRINTF("  no cut: the run that precedes the sweep\n");
    return;
  }
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
#define N_CHECKED_AREAS 5
static bool area_seen_new[N_CHECKED_AREAS];
//Every modelled operation moves the reference: the state it is to produce
//becomes the new one, the previous new state the old one
//The old state is the new one but where the new one changed since the last
//advance: ref_touch records that range, so that an advance copies it and not
//the whole flash
static uintptr_t ref_touched_lo = 0;
static uintptr_t ref_touched_hi = sizeof(data_flash_t);
static void ref_touch(uintptr_t offset, uintptr_t size){
  if(offset < ref_touched_lo) ref_touched_lo = offset;
  if(offset + size > ref_touched_hi) ref_touched_hi = offset + size;
}
static void ref_touch_all(){
  ref_touched_lo = 0;
  ref_touched_hi = sizeof(data_flash_t);
}
static void ref_advance(){
  if(ref_touched_hi > ref_touched_lo){
    memcpy(((uint8_t*)&nvm_ref_previous_state) + ref_touched_lo,((const uint8_t*)&nvm_ref) + ref_touched_lo,ref_touched_hi - ref_touched_lo);
  }
  ref_touched_lo = sizeof(data_flash_t);
  ref_touched_hi = 0;
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
  ref_touch(offset,size);
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
  ref_touch(offset,size);
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
  ref_touch(offset,size);
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
  //compute new state, at the area's place in the reference
  uintptr_t offset = (uintptr_t)ctx->area - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  memset(dst+offset,0xFF,ctx->data_size);
  ref_touch(offset,ctx->data_size);
  //call LFTL
  lftl_erase_all(ctx);
}
void tearing_sim_lftl_write(lftl_ctx_t*ctx,void*dst_nvm_addr, const void*const src, uintptr_t size){
  ref_advance();
  //compute new state
  uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)&nvm;
  uint8_t*dst = (uint8_t*)&nvm_ref;
  lftl_memread(dst+offset,src,size);
  ref_touch(offset,size);
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
    ref_touch(offset,size);
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
  ref_touch(offset,ctx->area_size);
  //call LFTL
  lftl_transaction_commit(ctx);
}
void tearing_sim_lftl_transaction_abort(lftl_ctx_t*ctx){
  //call LFTL
  lftl_transaction_abort(ctx);
}
//What an application does with its EWLF areas after a reset, before it reads
//or writes them: the repair the scan found pending, if any
static void boot_ewlf(lftl_ctx_t*ctx){
  lftl_complete_scan(ctx);
}
bool nvm_is_equal(data_flash_t*expected){
  data_flash_t read_val;
  boot_ewlf(&ewlfa);
  boot_ewlf(&ewlfb);
  lftl_read(&nvma,&read_val.a_data,&nvm.a_data,sizeof(nvm.a_data));
  if(memcmp(&read_val.a_data,&expected->a_data,sizeof(nvm.a_data))) return 0;
  lftl_read(&nvmb,&read_val.b_data,&nvm.b_data,sizeof(nvm.b_data));
  if(memcmp(&read_val.b_data,&expected->b_data,sizeof(nvm.b_data))) return 0;
  lftl_read(&ewlfa,&read_val.ewlfa_data,&nvm.ewlfa_data,sizeof(nvm.ewlfa_data));
  if(memcmp(&read_val.ewlfa_data,&expected->ewlfa_data,sizeof(nvm.ewlfa_data))) return 0;
  lftl_read(&ewlfb,&read_val.ewlfb_data,&nvm.ewlfb_data,sizeof(nvm.ewlfb_data));
  if(memcmp(&read_val.ewlfb_data,&expected->ewlfb_data,sizeof(nvm.ewlfb_data))) return 0;
  boot_ewlf(&largea);
  lftl_read(&largea,&read_val.largea_data,&nvm.largea_data,sizeof(nvm.largea_data));
  if(memcmp(&read_val.largea_data,&expected->largea_data,sizeof(nvm.largea_data))) return 0;
  return 1;
}
//--- The state every cut of a sweep starts from. A sweep runs its test once
//per cut, from the same state: all the areas formatted. Formatting seven
//areas after each cut, and checking and writing each of them, costs far more
//than most cuts need, so the sweep takes a snapshot of that state once (the
//flash, the reference model, what a format leaves in the contexts) and:
//- after a cut, restores it instead of formatting. One cut in
//  SWEEP_FORMAT_PERIOD still formats for real, over what the cut left, and
//  checks the result;
//- after a cut, checks and writes only the areas whose flash differs from
//  the snapshot or holds a weak word: any other area is exactly as the
//  snapshot has it, and behaves as it did when the sweep started. One cut in
//  SWEEP_FULL_PERIOD checks and writes them all.
//A single cut run with --tear-at= does everything in full.
#define SWEEP_FORMAT_PERIOD 256
#define SWEEP_FULL_PERIOD 64
#define N_SWEEP_AREAS 7
static lftl_ctx_t*const sweep_areas[N_SWEEP_AREAS] = {&nvma,&nvmb,&ewlfa,&ewlfb,&nvmc,&nvmd,&largea};
static data_flash_t sweep_nvm;
static data_flash_t sweep_ref;
static data_flash_t sweep_ref_previous;
static struct {
  void*data;
  lftl_ewlf_state_t ewlf;
} sweep_ctx[N_SWEEP_AREAS];
static bool sweep_snapshot_valid; //inside the loop of a sweep
static bool sweep_full;           //this cut checks and writes every area
static void format_all(){
  for(unsigned int i=0;i<N_SWEEP_AREAS;i++) format_func(sweep_areas[i]);
}
static void sweep_snapshot(){
  memcpy(&sweep_nvm,&nvm,sizeof(nvm));
  sweep_ref = nvm_ref;
  sweep_ref_previous = nvm_ref_previous_state;
  for(unsigned int i=0;i<N_SWEEP_AREAS;i++){
    sweep_ctx[i].data = sweep_areas[i]->data;
    sweep_ctx[i].ewlf = sweep_areas[i]->ewlf;
  }
  sweep_snapshot_valid = 1;
}
static void sweep_restore(){
  memcpy(&nvm,&sweep_nvm,sizeof(nvm));
  tearing_sim_weak_clear();
  nvm_ref = sweep_ref;
  nvm_ref_previous_state = sweep_ref_previous;
  ref_touch_all();
  memset(area_seen_new,0,sizeof(area_seen_new));
  for(unsigned int i=0;i<N_SWEEP_AREAS;i++){
    sweep_areas[i]->data = sweep_ctx[i].data;
    sweep_areas[i]->ewlf = sweep_ctx[i].ewlf;
    sweep_areas[i]->transaction_tracker = LFTL_INVALID_POINTER;
  }
}
//whether an area is to be checked and written after this cut: always outside
//a sweep, and inside one unless its flash is the snapshot's, with no weak word
static bool sweep_examines(const lftl_ctx_t*ctx){
  if(!sweep_snapshot_valid || sweep_full) return 1;
  const uintptr_t offset = (uintptr_t)ctx->area - (uintptr_t)&nvm;
  if(memcmp(ctx->area,((const uint8_t*)&sweep_nvm) + offset,ctx->area_size)) return 1;
  return tearing_sim_is_weak(ctx->area,ctx->area_size);
}

//What an area reads after a simulated cut, against the reference model: the
//state the interrupted operation was to produce, the one before it, or
//neither. The operation touched one area, so the two references agree on the
//others, which then read as new.
typedef enum { AREA_NEW, AREA_OLD, AREA_CORRUPT } area_state_t;
static area_state_t area_state(lftl_ctx_t*ctx, void*data, const void*ref, const void*prev, uintptr_t size){
  uint8_t read_val[size];
  if(lftl_is_ewlf(ctx) || lftl_is_large(ctx)) boot_ewlf(ctx);
  lftl_read(ctx,read_val,data,size);
  if(0 == memcmp(read_val,ref,size)) return AREA_NEW;
  if(0 == memcmp(read_val,prev,size)) return AREA_OLD;
  return AREA_CORRUPT;
}
static const char*const checked_area_names[N_CHECKED_AREAS] = {"a","b","ewlfa","ewlfb","largea"};
static lftl_ctx_t*const checked_areas[N_CHECKED_AREAS] = {&nvma,&nvmb,&ewlfa,&ewlfb,&largea};
//`examined` says which areas were looked at, see sweep_examines
static void nvm_states(area_state_t states[N_CHECKED_AREAS], bool examined[N_CHECKED_AREAS]){
  for(unsigned int i=0;i<N_CHECKED_AREAS;i++) examined[i] = sweep_examines(checked_areas[i]);
  if(examined[0]) states[0] = area_state(&nvma,&nvm.a_data,&nvm_ref.a_data,&nvm_ref_previous_state.a_data,sizeof(nvm.a_data));
  if(examined[1]) states[1] = area_state(&nvmb,&nvm.b_data,&nvm_ref.b_data,&nvm_ref_previous_state.b_data,sizeof(nvm.b_data));
  if(examined[2]) states[2] = area_state(&ewlfa,&nvm.ewlfa_data,&nvm_ref.ewlfa_data,&nvm_ref_previous_state.ewlfa_data,sizeof(nvm.ewlfa_data));
  if(examined[3]) states[3] = area_state(&ewlfb,&nvm.ewlfb_data,&nvm_ref.ewlfb_data,&nvm_ref_previous_state.ewlfb_data,sizeof(nvm.ewlfb_data));
  if(examined[4]) states[4] = area_state(&largea,&nvm.largea_data,&nvm_ref.largea_data,&nvm_ref_previous_state.largea_data,sizeof(nvm.largea_data));
}
//One boot: every checked area reads the new state or the old one, and none
//that read the new state since the reference last moved falls back to the old
//one (the anti-tearing page's definition of a rollback)
void check_nvm(){
  area_state_t states[N_CHECKED_AREAS];
  bool examined[N_CHECKED_AREAS];
  nvm_states(states,examined);
  for(unsigned int i=0;i<N_CHECKED_AREAS;i++){
    if(!examined[i]) continue;
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
  largea.data = LFTL_INVALID_POINTER;
  largea.transaction_tracker = LFTL_INVALID_POINTER;
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
//out: some tests use them as scratch for hand-built states. Inside a sweep,
//an area the test and the cut left as the snapshot has it is left out too,
//see sweep_examines.
void tearing_sim_write_after_recovery(){
  bool examined[N_CHECKED_AREAS];
  for(unsigned int i=0;i<N_CHECKED_AREAS;i++) examined[i] = sweep_examines(checked_areas[i]);
  if(examined[0]) tearing_sim_write_area(&nvma,&nvm.a_data,sizeof(nvm.a_data));
  if(examined[1]) tearing_sim_write_area(&nvmb,&nvm.b_data,sizeof(nvm.b_data));
  if(examined[2]) tearing_sim_write_area(&ewlfa,&nvm.ewlfa_data,sizeof(nvm.ewlfa_data));
  if(examined[3]) tearing_sim_write_area(&ewlfb,&nvm.ewlfb_data,sizeof(nvm.ewlfb_data));
  if(examined[4]) tearing_sim_write_area(&largea,&nvm.largea_data,sizeof(nvm.largea_data));
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
  volatile uintptr_t tracker_size = LFTL_TRANSACTION_TRACKER_SIZE(&nvma);
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

//CRC-64/NVME, reflected, all ones in and out, bit by bit: the result of the
//CRC core of ftl_internal.h, which goes through a table unless the library is
//built with LFTL_CRC64_BITWISE.
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

#define EWLF_CHECK(cond) do{ if(!(cond)){ PRINTF("ERROR: %s, line %d\n",#cond,__LINE__); throw_exception(ERROR_VERIFICATION_FAIL); } }while(0)

//--- EWLF areas the tests build themselves, over the scratch pages: an
//unregistered context and its own NVM properties, which a test may give
//counting or failing callbacks
#define SCRATCH_UNITS (sizeof(nvm.scratch_pages)/(LFTL_PAGE_SIZE))
static void scratch_ewlf(lftl_ctx_t*ctx, lftl_nvm_props_t*props, unsigned int n_units, uintptr_t data_size){
  if(n_units > SCRATCH_UNITS) throw_exception(INTERNAL_ERROR_CORRUPT);
  *props = nvm_props;
  props->next = LFTL_INVALID_POINTER;
  memset(ctx,0,sizeof(*ctx));
  ctx->nvm_props = props;
  ctx->area = nvm.scratch_pages;
  ctx->area_size = n_units*props->erase_size;
  ctx->data = LFTL_INVALID_POINTER;
  ctx->data_size = data_size;
  ctx->transaction_tracker = LFTL_INVALID_POINTER;
  ctx->next = LFTL_INVALID_POINTER;
  ctx->area_type = LFTL_AREA_TYPE_EWLF; //not registered, so set here
}
//a reset: the next access scans the area again
static void ewlf_reset(lftl_ctx_t*ctx){
  ctx->data = LFTL_INVALID_POINTER;
}

//The layout of an EWLF area (EWLF.md), computed here from the context alone,
//independently of the library: data units of records (start marker, data,
//meta field) from their top down, then 2 log units of pairs (marker, entry,
//confirm) from their base up, regular then reserved, then certificates
typedef struct {
  uintptr_t flag_size;
  uintptr_t meta_size;
  uintptr_t entry_size;
  uintptr_t pair_size;
  uintptr_t data_phy_size;
  uintptr_t record_size;
  uintptr_t erase_size;
  unsigned int n_data_units;
  unsigned int records_per_unit;
  unsigned int n_positions;
  unsigned int n_regular;
  unsigned int n_certs;
} ewlf_layout_t;
static ewlf_layout_t ewlf_layout(const lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  ewlf_layout_t l;
  l.flag_size = write_size;
  l.meta_size = LFTL_ROUND_UP(12,write_size);
  l.entry_size = LFTL_ROUND_UP(16,write_size);
  l.pair_size = 2*l.flag_size + l.entry_size;
  l.data_phy_size = LFTL_ROUND_UP(ctx->data_size,write_size);
  l.record_size = l.flag_size + l.data_phy_size + l.meta_size;
  l.erase_size = ctx->nvm_props->erase_size;
  l.n_data_units = ctx->area_size / l.erase_size - LFTL_EWLF_LOG_UNITS;
  l.records_per_unit = l.erase_size / l.record_size;
  if(l.records_per_unit > 256) l.records_per_unit = 256;
  l.n_certs = ctx->ewlf_certificates ? ctx->ewlf_certificates : LFTL_EWLF_CERTIFICATES;
  l.n_positions = (l.erase_size - l.n_certs*l.flag_size) / l.pair_size;
  l.n_regular = l.n_positions - LFTL_EWLF_RESERVED_PAIRS;
  return l;
}
static uint8_t*ewlf_raw_record(const lftl_ctx_t*ctx, const ewlf_layout_t*l, unsigned int unit, unsigned int pos){
  return (uint8_t*)ctx->area + (unit+1)*l->erase_size - (pos+1)*l->record_size;
}
static uint8_t*ewlf_raw_data(const ewlf_layout_t*l, const uint8_t*record){ return (uint8_t*)record + l->flag_size; }
static uint8_t*ewlf_raw_meta(const ewlf_layout_t*l, const uint8_t*record){ return (uint8_t*)record + l->flag_size + l->data_phy_size; }
static uint8_t*ewlf_raw_pair(const lftl_ctx_t*ctx, const ewlf_layout_t*l, unsigned int log, unsigned int q){
  return (uint8_t*)ctx->area + (l->n_data_units+log)*l->erase_size + q*l->pair_size;
}
static uint8_t*ewlf_raw_entry(const ewlf_layout_t*l, const uint8_t*pair){ return (uint8_t*)pair + l->flag_size; }
static uint8_t*ewlf_raw_confirm(const ewlf_layout_t*l, const uint8_t*pair){ return (uint8_t*)pair + l->flag_size + l->entry_size; }
__attribute__((unused)) static uint8_t*ewlf_raw_cert(const lftl_ctx_t*ctx, const ewlf_layout_t*l, unsigned int log, unsigned int i){
  return (uint8_t*)ctx->area + (l->n_data_units+log)*l->erase_size + l->n_positions*l->pair_size + i*l->flag_size;
}
//The content of a log entry as written: generation, data unit, position,
//version, the CRC-64/NVME of those 8 bytes, zeros
static void test_ewlf_entry(uint8_t*entry, uintptr_t entry_size, uint16_t gen, uint8_t unit, uint8_t pos, uint32_t version){
  memset(entry,0,entry_size);
  entry[0] = (uint8_t)gen;
  entry[1] = (uint8_t)(gen >> 8);
  entry[2] = unit;
  entry[3] = pos;
  for(unsigned int i=0;i<4;i++) entry[4+i] = (uint8_t)(version >> (8*i));
  const uint64_t crc = ~test_crc64(~(uint64_t)0,entry,8);
  for(unsigned int i=0;i<8;i++) entry[8+i] = (uint8_t)(crc >> (8*i));
}

//Physical reads through the raw callback: lftl_memread would take an address
//inside a registered area's logical range for a logical one
static void ewlf_raw_read(void*dst, const uint8_t*addr, uintptr_t size){
  const uint8_t status = nvm_read(dst,addr,size);
  if(status) throw_exception(LFTL_ERROR_LOW_LEVEL_READ | status);
}
__attribute__((unused)) static bool ewlf_raw_is_all(const uint8_t*addr, uintptr_t size, uint8_t value){
  uint8_t buf[size];
  ewlf_raw_read(buf,addr,size);
  for(uintptr_t i=0;i<size;i++) if(value != buf[i]) return false;
  return true;
}

//How far a hand-built update got: every word an update programs, in order,
//and the two where the last word programmed is not what was meant
typedef enum {
  EWLF_RAW_MARKER,          //the record's start marker
  EWLF_RAW_DATA,            //and its data
  EWLF_RAW_META,            //and its meta field: the record is whole, nothing names it
  EWLF_RAW_PAIR_MARKER,     //and the pair's marker
  EWLF_RAW_ENTRY_GARBAGE,   //and an entry whose CRC fails
  EWLF_RAW_ENTRY,           //and the entry
  EWLF_RAW_CONFIRM_PARTIAL, //and a confirm that is not all zeros
  EWLF_RAW_CONFIRM,         //and the confirm: the update is whole
  EWLF_RAW_N_STATES,
} ewlf_raw_state_t;

//Build by hand the update that follows the current record, as a power cut
//leaves it: the record at the position after the current one, the pair at
//the position after the last started one of the live log unit. The area was
//just written or scanned, and has room for both without rollover or switch.
//Only erased cells are programmed, so this works on real flash too. The data
//is whole write units.
static void ewlf_raw_update(const lftl_ctx_t*ctx, const uint8_t*d, ewlf_raw_state_t state, bool wrong_checksum, bool wrong_entry_crc){
  const ewlf_layout_t l = ewlf_layout(ctx);
  const lftl_ewlf_state_t*const st = &ctx->ewlf;
  const unsigned int log = st->live;
  const unsigned int pos = st->d_pos + 1;
  const unsigned int q = st->unit[log].started + 1;
  const uint32_t version = st->version + 1;
  if((pos >= l.records_per_unit) || (q >= l.n_regular) || (ctx->data_size != l.data_phy_size)) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t*const record = ewlf_raw_record(ctx,&l,st->d_unit,pos);
  uint8_t zeros[l.flag_size];
  memset(zeros,0,l.flag_size);
  uint8_t partial[l.flag_size];
  memset(partial,0,l.flag_size);
  partial[l.flag_size-1] = 0xFF;
  raw_write(record,zeros,l.flag_size);
  if(EWLF_RAW_MARKER == state) return;
  raw_write(ewlf_raw_data(&l,record),d,l.data_phy_size);
  if(EWLF_RAW_DATA == state) return;
  uint8_t meta[l.meta_size];
  test_ewlf_meta(meta,l.meta_size,version,test_ewlf_checksum(version,d,ctx->data_size) ^ (wrong_checksum ? 1 : 0));
  raw_write(ewlf_raw_meta(&l,record),meta,l.meta_size);
  if(EWLF_RAW_META == state) return;
  uint8_t*const pair = ewlf_raw_pair(ctx,&l,log,q);
  raw_write(pair,zeros,l.flag_size);
  if(EWLF_RAW_PAIR_MARKER == state) return;
  uint8_t entry[l.entry_size];
  test_ewlf_entry(entry,l.entry_size,st->unit[log].gen,st->d_unit,pos,version);
  if(wrong_entry_crc || (EWLF_RAW_ENTRY_GARBAGE == state)) entry[15] ^= 1;
  raw_write(ewlf_raw_entry(&l,pair),entry,l.entry_size);
  if((EWLF_RAW_ENTRY_GARBAGE == state) || (EWLF_RAW_ENTRY == state)) return;
  raw_write(ewlf_raw_confirm(&l,pair),(EWLF_RAW_CONFIRM_PARTIAL == state) ? partial : zeros,l.flag_size);
}

//callbacks that count what an operation programs and erases
static unsigned int counted_erases = 0;
static uint8_t counting_nvm_erase(void*base_address, unsigned int n_pages){
  counted_erases++;
  return nvm_erase(base_address,n_pages);
}
static unsigned int counted_writes = 0;
static uint8_t counting_nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  counted_writes++;
  return nvm_write(dst_nvm_addr,src,size);
}

//What the scan makes of an update cut after each of its words (EWLF.md, "the
//classic failure shapes"), on an EWLF area over the scratch pages, 2 data
//units of 4 records:
//- up to the record's meta field, nothing names the record: the previous
//  version stands and nothing is pending;
//- the pair's marker alone, or an entry that does not read valid: a started
//  position above the decision, to bury under a copy of the previous version;
//- a valid entry without a whole confirm: the new version is the decision,
//  to make solid by a copy before it is shown;
//- the whole pair: the new version, nothing pending.
//The scan programs and erases nothing; while a repair is pending reads and
//writes are refused and the unproven read returns the version the repair
//then makes current; the repair is one pair; the boots after it find
//nothing to do.
static void ewlf_log_states_test_core(ewlf_raw_state_t state){
  lftl_nvm_props_t props;
  lftl_ctx_t ewlf;
  scratch_ewlf(&ewlf,&props,4,0);
  props.erase = counting_nvm_erase;
  props.write = counting_nvm_write;
  const uint32_t write_size = props.write_size;
  ewlf.data_size = ((props.erase_size/4 - write_size - LFTL_ROUND_UP(12,write_size)) / write_size) * write_size;
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  if(l.records_per_unit != 4) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t*const area = (uint8_t*)ewlf.area;
  const uintptr_t size = ewlf.data_size;
  uint8_t d1[size], d2[size], d3[size], buf[size];
  lftl_format(&ewlf);
  stateful_prng_fill(d1,size);
  lftl_write(&ewlf,area,d1,size);
  const uint32_t version1 = ewlf.ewlf.version;
  stateful_prng_fill(d2,size);
  ewlf_raw_update(&ewlf,d2,state,false,false);
  const bool new_version = state >= EWLF_RAW_ENTRY;
  const bool pending = (state >= EWLF_RAW_PAIR_MARKER) && (state < EWLF_RAW_CONFIRM);
  const uint8_t*const expected = new_version ? d2 : d1;
  const uint32_t version = new_version ? (version1 + 1) : version1;

  ewlf_reset(&ewlf);
  counted_writes = 0;
  counted_erases = 0;
  lftl_scan_status_t status;
  lftl_scan_status(&ewlf,&status);
  EWLF_CHECK(status.pending == (pending ? LFTL_REPAIR_BURY : LFTL_REPAIR_NONE));
  EWLF_CHECK(!status.stuck);
  EWLF_CHECK(status.version == version);
  EWLF_CHECK(status.remaining_cuts == l.n_certs);
  if(pending){
    EXPECT_ERROR(LFTL_ERROR_SCAN_PENDING, lftl_read(&ewlf,buf,area,size));
    EXPECT_ERROR(LFTL_ERROR_SCAN_PENDING, lftl_write(&ewlf,area,d1,size));
    EXPECT_ERROR(LFTL_ERROR_SCAN_PENDING, lftl_erase_all(&ewlf));
  }
  lftl_read_unproven(&ewlf,buf,area,size);
  EWLF_CHECK(0 == memcmp(buf,expected,size));
  EWLF_CHECK((0 == counted_writes) && (0 == counted_erases));
  lftl_complete_scan(&ewlf);
  EWLF_CHECK(counted_writes == (pending ? 3u : 0u));
  EWLF_CHECK(0 == counted_erases);
  read_and_check(&ewlf,area,expected,size);
  //the repair is done once
  for(unsigned int boot=0;boot<2;boot++){
    ewlf_reset(&ewlf);
    counted_writes = 0;
    lftl_scan_status(&ewlf,&status);
    EWLF_CHECK(LFTL_REPAIR_NONE == status.pending);
    EWLF_CHECK(status.version == version);
    read_and_check(&ewlf,area,expected,size);
    EWLF_CHECK(0 == counted_writes);
  }
  //and the area takes updates: the torn record's position is stepped over
  stateful_prng_fill(d3,size);
  lftl_write(&ewlf,area,d3,size);
  EWLF_CHECK(ewlf.ewlf.version == version + 1);
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,d3,size);
  //the unit is full: the next write erases the other data unit, once
  counted_erases = 0;
  lftl_write(&ewlf,area,d1,size);
  EWLF_CHECK(1 == counted_erases);
  lftl_write(&ewlf,area,d2,size);
  EWLF_CHECK(1 == counted_erases);
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,d2,size);
}
void ewlf_log_states_test(){
  DEBUG_PRINTLN(__func__);
  for(unsigned int state=0;state<EWLF_RAW_N_STATES;state++) ewlf_log_states_test_core((ewlf_raw_state_t)state);
  SANITY_CHECK();
}

//Records and entries are looked for only where the layout puts them (LF-10,
//LF-12), on an EWLF area over the scratch pages whose read callback flags
//any read outside the area.
void ewlf_grid_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props;
  lftl_ctx_t ewlf;
  scratch_ewlf(&ewlf,&props,4,0);
  props.read = area_checked_nvm_read;
  const uint32_t write_size = props.write_size;
  //the data is 4 pairs long, so a forged pair fits in it whatever the write unit
  const uintptr_t data_size = 4*(2*write_size + LFTL_ROUND_UP(16,write_size));
  ewlf.data_size = data_size;
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  uint8_t*const area = (uint8_t*)ewlf.area;
  checked_area_lo = area;
  checked_area_hi = area + ewlf.area_size;

  //LF-10: d2's write is cut before its meta field, and its data holds a
  //whole forged pair, marker, entry, confirm, naming d2's own position with
  //the next version and generation: a data unit is never read as log
  lftl_format(&ewlf);
  uint8_t d1[data_size];
  stateful_prng_fill(d1,data_size);
  lftl_write(&ewlf,area,d1,data_size);
  uint8_t d2[data_size];
  memset(d2,0xFF,data_size);
  memset(d2,0,l.flag_size);
  test_ewlf_entry(d2+l.flag_size,l.entry_size,ewlf.ewlf.unit[ewlf.ewlf.live].gen,ewlf.ewlf.d_unit,ewlf.ewlf.d_pos+1,ewlf.ewlf.version+1);
  memset(d2+l.flag_size+l.entry_size,0,l.flag_size);
  ewlf_raw_update(&ewlf,d2,EWLF_RAW_DATA,false,false);
  ewlf_reset(&ewlf);
  lftl_scan_status_t status;
  lftl_scan_status(&ewlf,&status);
  EWLF_CHECK(LFTL_REPAIR_NONE == status.pending);
  read_and_check(&ewlf,area,d1,data_size);

  //LF-12: an area without any valid entry, holding a whole pair in log unit
  //1 one write unit off the pair positions, and another in the last bytes of
  //the area: neither is an entry, and the scan never reads past the area
  raw_erase(area,ewlf.area_size/props.erase_size);
  uint8_t pair[l.pair_size];
  memset(pair,0,l.pair_size);
  test_ewlf_entry(pair+l.flag_size,l.entry_size,1,l.n_data_units-1,0,1);
  raw_write(ewlf_raw_pair(&ewlf,&l,1,0)+l.flag_size,pair,l.pair_size);
  raw_write(area+ewlf.area_size-l.pair_size,pair,l.pair_size);
  ewlf_reset(&ewlf);
  out_of_area_read = 0;
  uint8_t buf[data_size];
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_read(&ewlf,buf,area,sizeof(buf)));
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_complete_scan(&ewlf));
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_scan_status(&ewlf,&status));
  EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_read_unproven(&ewlf,buf,area,sizeof(buf)));
  if(out_of_area_read) throw_exception(ERROR_VERIFICATION_FAIL);
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
//area over the scratch pages, 2 data units. nvm_write refuses to program
//cells that are not erased, like STM32U5-class flash, so a write landing on
//a partial or complete record raises an error. A rollover erases the unit it
//enters before anything is programmed in it, and a power cut during that
//erase or during the record leaves the previous record current and the next
//write erasing again. 1000 bytes of data make records of about 1 KB: 4 per
//4 KB erase unit, 7 per 8 KB one; on 2 KB units (STM32L5) only one would
//fit, so the data is shrunk to fit two, the minimum the layout requires and
//this test assumes.
void ewlf_tearing_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props;
  lftl_ctx_t ewlf;
  scratch_ewlf(&ewlf,&props,4,0);
  props.erase = failing_once_nvm_erase;
  props.write = failing_once_nvm_write;
  const uint32_t write_size = props.write_size;
  const uintptr_t overhead = write_size + LFTL_ROUND_UP(12,write_size);
  const uintptr_t record_size_1000 = overhead + LFTL_ROUND_UP(1000,write_size);
  //whole write units, so that the data of a record is one program call
  const uintptr_t data_size = (props.erase_size / record_size_1000 >= 2) ? LFTL_ROUND_UP(1000,write_size) : props.erase_size/2 - overhead - LFTL_ROUND_UP(16,write_size);
  ewlf.data_size = data_size;
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  uint8_t*const area = (uint8_t*)ewlf.area;
  if(l.records_per_unit < 2) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d[data_size];

  //a write cut while writing data leaves a partial record right below the
  //current one, its start marker programmed: the next write must not program
  //over it
  lftl_format(&ewlf);
  stateful_prng_fill(d,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  if(ewlf.ewlf.d_pos + 1u < l.records_per_unit){
    uint8_t*const torn = ewlf_raw_record(&ewlf,&l,ewlf.ewlf.d_unit,ewlf.ewlf.d_pos+1);
    uint8_t zeros[write_size];
    memset(zeros,0,write_size);
    raw_write(torn,zeros,write_size); //start marker
    uint8_t partial[LFTL_ROUND_UP(100,write_size)];
    stateful_prng_fill(partial,sizeof(partial));
    raw_write(ewlf_raw_data(&l,torn),partial,sizeof(partial));
  }
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,d,sizeof(d));
  stateful_prng_fill(d,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  read_and_check(&ewlf,area,d,sizeof(d));
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,d,sizeof(d));

  //a wrap from the lowest data unit back to the highest one, cut during the
  //erase of the highest unit, then during the new record after that erase:
  //the reset finds the previous record, and the next write erases the unit
  //again before programming it
  lftl_format(&ewlf);
  uint8_t d_prev[data_size];
  for(unsigned int i = 1; i < l.n_data_units*l.records_per_unit; i++){ //format wrote the first record
    stateful_prng_fill(d,sizeof(d));
    lftl_write(&ewlf,area,d,sizeof(d));
  }
  EWLF_CHECK((0 == ewlf.ewlf.d_unit) && (l.records_per_unit - 1 == ewlf.ewlf.d_pos));
  memcpy(d_prev,d,sizeof(d));
  stateful_prng_fill(d,sizeof(d));
  erase_calls_before_failure = 0;
  EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_ERASE | 1, lftl_write(&ewlf,area,d,sizeof(d)));
  erase_calls_before_failure = -1;
  EWLF_CHECK(LFTL_INVALID_POINTER == ewlf.data); //a failed flash operation forgets the scan
  read_and_check(&ewlf,area,d_prev,sizeof(d));
  //the unit erased, the start marker and the data of the new record written,
  //the power cut before its meta field
  write_calls_before_failure = 2;
  EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_WRITE | 1, lftl_write(&ewlf,area,d,sizeof(d)));
  write_calls_before_failure = -1;
  read_and_check(&ewlf,area,d_prev,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,d,sizeof(d));
  EWLF_CHECK((l.n_data_units - 1 == ewlf.ewlf.d_unit) && (0 == ewlf.ewlf.d_pos));
  if(l.records_per_unit > 1){
    //only the top record of the wrapped-to unit holds anything
    if(!raw_reads_erased(ewlf_raw_record(&ewlf,&l,l.n_data_units-1,1),l.record_size)) throw_exception(ERROR_VERIFICATION_FAIL);
  }
  stateful_prng_fill(d,sizeof(d));
  lftl_write(&ewlf,area,d,sizeof(d));
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,d,sizeof(d));

  //the cut at each of the six programs of an update in turn: the record's
  //start marker, data and meta field, the pair's marker, entry and confirm.
  //Up to the entry the previous version stands; without the confirm alone
  //the entry is whole, the new version stands and the scan has it to make
  //solid first
  for(volatile unsigned int stage=0;stage<6;stage++){
    lftl_format(&ewlf);
    stateful_prng_fill(d_prev,sizeof(d));
    lftl_write(&ewlf,area,d_prev,sizeof(d));
    stateful_prng_fill(d,sizeof(d));
    write_calls_before_failure = stage;
    EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_WRITE | 1, lftl_write(&ewlf,area,d,sizeof(d)));
    write_calls_before_failure = -1;
    lftl_scan_status_t status;
    lftl_scan_status(&ewlf,&status);
    EWLF_CHECK(status.pending == ((stage >= 4) ? LFTL_REPAIR_BURY : LFTL_REPAIR_NONE));
    lftl_complete_scan(&ewlf);
    read_and_check(&ewlf,area,(5 == stage) ? d : d_prev,sizeof(d));
    stateful_prng_fill(d,sizeof(d));
    lftl_write(&ewlf,area,d,sizeof(d));
    ewlf_reset(&ewlf);
    read_and_check(&ewlf,area,d,sizeof(d));
  }
  SANITY_CHECK();
}

//EWLF areas accept any data size: each record pads the data to whole write
//units. An EWLF area over the scratch pages whose data ends 1 byte into a
//write unit: write the whole data, then only its last bytes, and read it
//back after a reset (ISSUES.md LFTL-005).
void ewlf_unaligned_size_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props;
  lftl_ctx_t ewlf;
  scratch_ewlf(&ewlf,&props,4,8*nvm_props.write_size + 1);
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
  ewlf_reset(&ewlf);
  read_and_check(&ewlf,area,expected,data_size);
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
//area: nvmd's pages, or the scratch pages for the EWLF area, through copies
//of a context and of NVM properties with 32-byte write units. The copies are
//not registered, so area_type is set here.
void large_write_unit_test(){
  DEBUG_PRINTLN(__func__);
  lftl_nvm_props_t props32 = nvm_props3;
  props32.write_size = 32;
  props32.next = LFTL_INVALID_POINTER;
  for(unsigned int ewlf=0;ewlf<2;ewlf++){
    lftl_ctx_t ctx32 = nvmd;
    lftl_nvm_props_t scratch_props;
    if(ewlf){
      //over the scratch pages: an EWLF area needs 4 erase units
      scratch_ewlf(&ctx32,&scratch_props,4,64);
      scratch_props.write_size = 32;
      //32 certificates of 32 bytes would leave a 2 KB log unit 2 regular pairs
      ctx32.ewlf_certificates = 8;
    } else {
      ctx32.nvm_props = &props32;
      ctx32.data = LFTL_INVALID_POINTER;
      ctx32.data_size = 64;
      ctx32.transaction_tracker = LFTL_INVALID_POINTER;
      ctx32.next = LFTL_INVALID_POINTER;
      ctx32.area_type = LFTL_AREA_TYPE_STD;
    }
    lftl_format(&ctx32);
    uint8_t buf[64];
    stateful_prng_fill(buf,sizeof(buf));
    lftl_write(&ctx32,ctx32.area,buf,sizeof(buf));
    //force a search of the slot or record
    ctx32.data = LFTL_INVALID_POINTER;
    read_and_check(&ctx32,ctx32.area,buf,sizeof(buf));
    if(ewlf){
      //a torn update on 32-byte write units: the entry is half a write unit
      stateful_prng_fill(buf,sizeof(buf));
      ewlf_raw_update(&ctx32,buf,EWLF_RAW_ENTRY,false,false);
      ewlf_reset(&ctx32);
      lftl_complete_scan(&ctx32);
      read_and_check(&ctx32,ctx32.area,buf,sizeof(buf));
    }
  }
  format_func(&nvmd); //resync nvm_ref with nvmd's pages
  SANITY_CHECK();
}

//An EWLF area with many erase units: every page of the test NVM as one area
//(26 data units of 9 records on the Linux geometry), through three cycles of
//writes with a reset before every read, power cuts injected at every stage
//of an update and during a rollover's erase, and the log switches the writes
//bring. The test checks that every read after a reset returns the last
//complete write, or the cut one when only its confirm is missing. The
//registered areas share these pages: they are formatted again at the end.
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
  const ewlf_layout_t l = ewlf_layout(&ewlf);
  if((l.n_data_units < 8) || (l.records_per_unit < 4)) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  uint8_t d[data_size];
  uint8_t expected[data_size];
  lftl_format(&ewlf);
  memset(expected,0xFF,data_size); //a formatted EWLF area reads erased
  read_and_check(&ewlf,area,expected,data_size);
  const unsigned int n_writes = 3*l.n_data_units*l.records_per_unit;
  unsigned int stage = 0;
  unsigned int switches = 0;
  unsigned int live = ewlf.ewlf.live;
  for(unsigned int i=0;i<n_writes;i++){
    stateful_prng_fill(d,data_size);
    //whether this write rolls over: no position is left in the current record's unit
    lftl_complete_scan(&ewlf);
    unsigned int started = ewlf.ewlf.d_pos;
    for(unsigned int p=started+1;p<l.records_per_unit;p++){
      if(!raw_reads_erased(ewlf_raw_record(&ewlf,&l,ewlf.ewlf.d_unit,p),l.flag_size)) started = p;
    }
    const bool rollover = started + 1 >= l.records_per_unit;
    //whether it switches log units: its programs are then others
    const bool log_switch = (unsigned int)(ewlf.ewlf.unit[ewlf.ewlf.live].started + 1) >= l.n_regular;
    if(rollover && (0 == (i % 5))){
      //the power cut during the erase of the unit the rollover enters
      erase_calls_before_failure = 0;
      EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_ERASE | 1, lftl_write(&ewlf,area,d,data_size));
      erase_calls_before_failure = -1;
    } else if((3 == (i % 7)) && !log_switch){
      //the power cut at every program of an update in turn: the record's
      //start marker, data and meta field, the pair's marker, entry and
      //confirm. Only the cut before the confirm leaves a whole entry: that
      //write's data is the one read
      write_calls_before_failure = stage;
      if(5 == stage) memcpy(expected,d,data_size);
      stage = (stage + 1) % 6;
      EXPECT_ERROR(LFTL_ERROR_LOW_LEVEL_WRITE | 1, lftl_write(&ewlf,area,d,data_size));
      write_calls_before_failure = -1;
    } else {
      lftl_write(&ewlf,area,d,data_size);
      memcpy(expected,d,data_size);
    }
    ewlf_reset(&ewlf);
    lftl_complete_scan(&ewlf);
    read_and_check(&ewlf,area,expected,data_size);
    if(live != ewlf.ewlf.live){
      live = ewlf.ewlf.live;
      switches++;
    }
  }
  EWLF_CHECK(switches >= 2);
  //the registered areas live in these pages
  format_func(&nvma);
  format_func(&nvmb);
  format_func(&ewlfa);
  format_func(&ewlfb);
  format_func(&nvmc);
  format_func(&nvmd);
  format_func(&largea);
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
    if(LFTL_NVM_STATUS_REFUSED != nvm_write(page+k*ws,values,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
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
      if(LFTL_NVM_STATUS_REFUSED != nvm_write(page+k*ws,values,ws)) throw_exception(ERROR_VERIFICATION_FAIL);
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

//The checksum of EWLF records and entries, CRC-64/NVME, against its
//published check value, then an update the test forges with it, record and
//pair: taken by the scan; with a record checksum off by one bit the entry
//names no record and the area has no version; with an entry CRC off by one
//bit the entry is none and the previous version stands. On an EWLF area over
//the scratch pages.
void crc64_test(){
  DEBUG_PRINTLN(__func__);
  if(0xAE8B14860A799888ull != ~test_crc64(~(uint64_t)0,(const uint8_t*)"123456789",9)) throw_exception(ERROR_VERIFICATION_FAIL);
  if(0 != ~test_crc64(~(uint64_t)0,(const uint8_t*)"",0)) throw_exception(ERROR_VERIFICATION_FAIL);
  lftl_nvm_props_t props;
  lftl_ctx_t ewlf;
  scratch_ewlf(&ewlf,&props,4,64);
  uint8_t*const area = (uint8_t*)ewlf.area;
  uint8_t d1[64], d2[64], buf[64];
  for(unsigned int wrong=0;wrong<3;wrong++){
    lftl_format(&ewlf);
    stateful_prng_fill(d1,sizeof(d1));
    lftl_write(&ewlf,area,d1,sizeof(d1));
    stateful_prng_fill(d2,sizeof(d2));
    ewlf_raw_update(&ewlf,d2,EWLF_RAW_CONFIRM,1 == wrong,2 == wrong);
    ewlf_reset(&ewlf);
    if(1 == wrong){
      EXPECT_ERROR(LFTL_ERROR_NO_VALID_VERSION, lftl_read(&ewlf,buf,area,sizeof(buf)));
    } else {
      lftl_complete_scan(&ewlf);
      read_and_check(&ewlf,area,wrong ? d1 : d2,sizeof(d2));
    }
  }
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


//--- large areas (LARGE.md)

#define LARGE_CHECK(cond) EWLF_CHECK(cond)

//a large area over the scratch pages, as scratch_ewlf builds an EWLF one
static void scratch_large(lftl_ctx_t*ctx, lftl_nvm_props_t*props, unsigned int n_units, uintptr_t data_size){
  scratch_ewlf(ctx,props,n_units,data_size);
  ctx->area_type = LFTL_AREA_TYPE_LARGE; //not registered, so set here
}

//the expected content of the area under test, kept by the tests beside it
static uint8_t large_expected[LARGE_DATA_SIZE + LFTL_PAGE_SIZE];
static uint8_t large_buf[LARGE_DATA_SIZE + LFTL_PAGE_SIZE];
static uint8_t large_src[LARGE_DATA_SIZE + LFTL_PAGE_SIZE];

//the whole data reads as expected
static void large_check_whole(lftl_ctx_t*ctx){
  lftl_read(ctx,large_buf,ctx->area,ctx->data_size);
  LARGE_CHECK(0 == memcmp(large_buf,large_expected,ctx->data_size));
}

//Writes of every shape on the large area of the global sweep, which cuts
//power at every write unit they read, program or erase, and checks after
//each cut, at two boots, that the area reads the old data or the new:
//within a chunk, across two chunks, the end of the last chunk, the whole
//data, every chunk but not the whole data, from and into other areas, from
//one chunk to another of the area, and an erase of all of it.
void large_basic_test(){
  DEBUG_PRINTLN(__func__);
  uint8_t*const base = (uint8_t*)&nvm.largea_data;
  const uintptr_t size = sizeof(nvm.largea_data);
  const uintptr_t page = LFTL_PAGE_SIZE;
  randomized_test_write(&largea,base+3,5);
  randomized_test_write(&largea,base+page-7,20);
  randomized_test_write(&largea,base+size-9,9);
  randomized_test_write(&largea,base,size);
  randomized_test_write(&largea,base+1,size-2);
  //from a regular area into the large one
  lftl_read(&nvma,large_src,&nvm.a_data,sizeof(nvm.a_data));
  test_write2(&largea,base+page+5,&nvm.a_data,sizeof(nvm.a_data),large_src);
  //from the large one, across two chunks, into a regular area and an EWLF area
  lftl_read(&largea,large_src,base+page-3,sizeof(nvm.a_data));
  test_write2(&nvma,&nvm.a_data,base+page-3,sizeof(nvm.a_data),large_src);
  lftl_read(&largea,large_src,base+2*page-100,sizeof(nvm.ewlfa_data));
  test_write2(&ewlfa,&nvm.ewlfa_data,base+2*page-100,sizeof(nvm.ewlfa_data),large_src);
  //from one chunk to another of the same area
  lftl_read(&largea,large_src,base+100,50);
  test_write2(&largea,base+2*page+10,base+100,50,large_src);
  erase_all_func(&largea);
  memset(large_expected,0xFF,size);
  large_check_whole(&largea);
  SANITY_CHECK();
}

//Transactions on the large area of the global sweep: two chunks with an
//untouched one between them, an abort, every chunk in one transaction, a
//transaction that touches nothing. The newer data reads while the
//transaction is open, the committed data after.
void large_transaction_test(){
  DEBUG_PRINTLN(__func__);
  uint8_t*const base = (uint8_t*)&nvm.largea_data;
  const uintptr_t size = sizeof(nvm.largea_data);
  const uintptr_t page = LFTL_PAGE_SIZE;
  const uintptr_t wu = sizeof(lftl_wu_t);
  uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE_LARGE(sizeof(nvm.largea_data),LFTL_WU_SIZE,LFTL_PAGE_SIZE)];
  LARGE_CHECK(sizeof(tracker) == LFTL_TRANSACTION_TRACKER_SIZE(&largea));
  uint8_t d[3*sizeof(lftl_wu_t)];
  uint8_t d2[7];
  lftl_read(&largea,large_expected,base,size);
  //chunks 0 and 2
  stateful_prng_fill(d,sizeof(d));
  stateful_prng_fill(d2,sizeof(d2));
  transaction_start_func(&largea,tracker);
  transaction_write_func(&largea,base+wu,d,sizeof(d));
  transaction_write_any_func(&largea,base+2*page+3,d2,sizeof(d2));
  read_newer_and_check(&largea,base+wu,d,sizeof(d));
  read_newer_and_check(&largea,base+2*page+3,d2,sizeof(d2));
  read_and_check(&largea,base+wu,large_expected+wu,sizeof(d));
  transaction_commit_func(&largea);
  memcpy(large_expected+wu,d,sizeof(d));
  memcpy(large_expected+2*page+3,d2,sizeof(d2));
  large_check_whole(&largea);
  //an abort changes nothing
  transaction_start_func(&largea,tracker);
  transaction_write_any_func(&largea,base+page+1,d2,sizeof(d2));
  transaction_abort_func(&largea);
  large_check_whole(&largea);
  //every chunk, in two writes: the checksum is computed afresh
  stateful_prng_fill(large_src,size);
  transaction_start_func(&largea,tracker);
  transaction_write_any_func(&largea,base,large_src,page+5);
  transaction_write_any_func(&largea,base+2*page-1,large_src+2*page-1,size-2*page+1);
  transaction_commit_func(&largea);
  memcpy(large_expected,large_src,page+5);
  memcpy(large_expected+2*page-1,large_src+2*page-1,size-2*page+1);
  large_check_whole(&largea);
  largea.data = LFTL_INVALID_POINTER;
  large_check_whole(&largea);
  //nothing touched
  transaction_start_func(&largea,tracker);
  transaction_commit_func(&largea);
  large_check_whole(&largea);
  SANITY_CHECK();
}

//erases counted per unit of the scratch pages
static unsigned int scratch_unit_erases[sizeof(((data_flash_t*)0)->scratch_pages)/LFTL_PAGE_SIZE];
static uint8_t unit_counting_nvm_erase(void*base_address, unsigned int n_pages){
  const uintptr_t first = ((uintptr_t)base_address - (uintptr_t)nvm.scratch_pages) / LFTL_PAGE_SIZE;
  for(unsigned int i=0;i<n_pages;i++) if(first + i < SCRATCH_UNITS) scratch_unit_erases[first + i]++;
  counted_erases++;
  return nvm_erase(base_address,n_pages);
}

//The rules a large area adds: its configuration, the calls of the
//checksum on other types and those of the scan on regular areas, the
//registry, the room of the pool (a write or a transaction that needs more
//units than are free changes nothing), the head going round the free units
//(the erases spread evenly, a unit whose chunk never changes is never
//erased), a scan that programs nothing, the misuses of a transaction, and
//the checksum of the data following random writes and transactions.
void large_api_test(){
  DEBUG_PRINTLN(__func__);
  const uintptr_t page = LFTL_PAGE_SIZE;
  const uintptr_t wu = sizeof(lftl_wu_t);
  lftl_nvm_props_t props;
  lftl_ctx_t ctx;
  uint8_t buf[DATA_SIZE];
  uint8_t buf2[DATA_SIZE];
  lftl_scan_status_t status;
  //configuration
  scratch_large(&ctx,&props,2+2,page+1); //2 chunks in a pool of 2 units
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area_large(&ctx));
  scratch_large(&ctx,&props,3+2,0);
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area_large(&ctx));
  scratch_large(&ctx,&props,3+2,page+1);
  ctx.ewlf_certificates = (page / props.write_size) - 1; //no room for 2 regular pairs
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_register_area_large(&ctx));
  //the calls of the checksum apply to large areas only
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_check_data(&nvma));
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_read_unchecked(&ewlfa,buf,ewlfa.area,4));
  EXPECT_ERROR(LFTL_ERROR_BAD_CONFIG, lftl_repair_checksum(&nvmb));
  //the calls of the scan apply to every type: nothing is pending on a regular area
  nvma.data = LFTL_INVALID_POINTER;
  lftl_scan_status(&nvma,&status);
  LARGE_CHECK((LFTL_REPAIR_NONE == status.pending) && !status.stuck && (0xFFFF == status.remaining_cuts));
  lftl_complete_scan(&nvma);
  lftl_read_unproven(&nvma,buf,nvm.data0,sizeof(nvm.data0));
  lftl_read(&nvma,buf2,nvm.data0,sizeof(nvm.data0));
  LARGE_CHECK(0 == memcmp(buf,buf2,sizeof(nvm.data0)));
  //registry
  LARGE_CHECK(lftl_is_large(&largea) && !lftl_is_ewlf(&largea) && !lftl_is_large(&ewlfa) && !lftl_is_large(&nvma));
  LARGE_CHECK(&largea == lftl_get_ctx(&nvm.data8[1]));
  LARGE_CHECK(&largea == lftl_get_ctx_large(&nvm.data8[1]));
  LARGE_CHECK(LFTL_INVALID_POINTER == lftl_get_ctx_large(&nvm.data0[0]));
  LARGE_CHECK(LFTL_INVALID_POINTER == lftl_get_ctx_ewlf(&nvm.data8[0]));
  LARGE_CHECK(LFTL_TRANSACTION_TRACKER_SIZE(&largea) == LFTL_TRANSACTION_TRACKER_SIZE_LL(sizeof(nvm.largea_data),LFTL_WU_SIZE) + 2*3);
  //room: 2 chunks in a pool of 3 units
  scratch_large(&ctx,&props,3+2,page+page/2);
  props.erase = unit_counting_nvm_erase;
  props.write = counting_nvm_write;
  uint8_t*const area = (uint8_t*)ctx.area;
  const uintptr_t size = ctx.data_size;
  lftl_format(&ctx);
  stateful_prng_fill(large_expected,size);
  lftl_write(&ctx,area,large_expected,size);
  large_check_whole(&ctx);
  stateful_prng_fill(large_src,size);
  counted_writes = 0;
  counted_erases = 0;
  EXPECT_ERROR(LFTL_ERROR_LARGE_NO_SPACE, lftl_write(&ctx,area,large_src,size));
  LARGE_CHECK((0 == counted_writes) && (0 == counted_erases));
  large_check_whole(&ctx);
  lftl_write(&ctx,area+page,large_src,wu);
  memcpy(large_expected+page,large_src,wu);
  large_check_whole(&ctx);
  {
    uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&ctx)];
    lftl_transaction_start(&ctx,tracker);
    lftl_transaction_write(&ctx,area,large_src,wu);
    EXPECT_ERROR(LFTL_ERROR_LARGE_NO_SPACE, lftl_transaction_write(&ctx,area+page,large_src,wu));
    LARGE_CHECK(LFTL_INVALID_POINTER == ctx.transaction_tracker);
    large_check_whole(&ctx);
    //the units the aborted transaction took are free again
    lftl_write(&ctx,area+page+wu,large_src,wu);
    memcpy(large_expected+page+wu,large_src,wu);
    large_check_whole(&ctx);
  }
  //the head goes round the free units: 2 chunks in a pool of 6, chunk 0
  //never rewritten, chunk 1 written 4 rounds of the 5 other units
  scratch_large(&ctx,&props,6+2,page+page/2);
  props.erase = unit_counting_nvm_erase;
  props.write = counting_nvm_write;
  lftl_format(&ctx);
  stateful_prng_fill(large_expected,size);
  lftl_write(&ctx,area,large_expected,size);
  memset(scratch_unit_erases,0,sizeof(scratch_unit_erases));
  for(unsigned int k=0;k<4*5;k++){
    stateful_prng_fill(large_src,wu);
    lftl_write(&ctx,area+page+wu*(k%4),large_src,wu);
    memcpy(large_expected+page+wu*(k%4),large_src,wu);
  }
  large_check_whole(&ctx);
  LARGE_CHECK(0 == scratch_unit_erases[0]);
  for(unsigned int u=1;u<6;u++) LARGE_CHECK(4 == scratch_unit_erases[u]);
  //a scan, and the lazy check of the data, program nothing
  ctx.data = LFTL_INVALID_POINTER;
  counted_writes = 0;
  counted_erases = 0;
  lftl_scan_status(&ctx,&status);
  LARGE_CHECK((LFTL_REPAIR_NONE == status.pending) && !status.stuck && (status.remaining_cuts > 0));
  large_check_whole(&ctx);
  LARGE_CHECK((0 == counted_writes) && (0 == counted_erases));
  //misuses of a transaction: each error aborts it
  {
    uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&ctx)];
    lftl_transaction_start(&ctx,tracker);
    EXPECT_ERROR(LFTL_ERROR_TRANSACTION_ONGOING, lftl_transaction_start(&ctx,tracker));
    LARGE_CHECK(LFTL_INVALID_POINTER == ctx.transaction_tracker);
    lftl_transaction_start(&ctx,tracker);
    EXPECT_ERROR(LFTL_ERROR_TRANSACTION_ONGOING, lftl_basic_write(&ctx,area,large_src,wu));
    lftl_transaction_start(&ctx,tracker);
    EXPECT_ERROR(LFTL_ERROR_TRANSACTION_ONGOING, lftl_erase_all(&ctx));
    lftl_transaction_start(&ctx,tracker);
    EXPECT_ERROR(LFTL_ERROR_TRANSACTION_ONGOING, lftl_repair_checksum(&ctx));
    lftl_transaction_start(&ctx,tracker);
    lftl_transaction_write(&ctx,area,large_src,wu);
    EXPECT_ERROR(LFTL_ERROR_TRANSACTION_OVERWRITE, lftl_transaction_write_any(&ctx,area+1,large_src,1));
    LARGE_CHECK(LFTL_INVALID_POINTER == ctx.transaction_tracker);
    EXPECT_ERROR(LFTL_ERROR_NO_TRANSACTION, lftl_transaction_commit(&ctx));
    EXPECT_ERROR(LFTL_ERROR_BASE_MISALIGNED, (lftl_transaction_start(&ctx,tracker), lftl_transaction_write(&ctx,area+1,large_src,wu)));
    large_check_whole(&ctx);
  }
  //the checksum of the data follows random writes and transactions, of
  //chunks next to each other or not: 4 chunks, the last in part, in a pool
  //of 8 units. The area is checked after each of them, now and then after
  //a reset
  scratch_large(&ctx,&props,8+2,3*page+page/2);
  const uintptr_t size4 = ctx.data_size;
  lftl_format(&ctx);
  memset(large_expected,0xFF,size4);
  xs_prng_set_seed(0x1A26E);
  for(unsigned int it=0;it<64;it++){
    const uint32_t r = xs_prng_get();
    if(0 == (r % 16)){
      lftl_erase_all(&ctx);
      memset(large_expected,0xFF,size4);
    } else if(r & 1){
      const uintptr_t offset = xs_prng_get() % size4;
      const uintptr_t len = 1 + xs_prng_get() % ((r & 2) ? (size4 - offset) : (size4 - offset < 64 ? size4 - offset : 64));
      xs_prng_fill(large_src,len);
      lftl_write_any(&ctx,area+offset,large_src,len);
      memcpy(large_expected+offset,large_src,len);
    } else {
      uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&ctx)];
      lftl_transaction_start(&ctx,tracker);
      //a few bytes in some of the chunks, each chunk once: they share no write unit
      for(unsigned int i=0;i<4;i++){
        if(0 == ((xs_prng_get() >> i) & 1)) continue;
        const uintptr_t chunk_len = (i < 3) ? page : (size4 - 3*page);
        const uintptr_t offset = i*page + xs_prng_get() % chunk_len;
        const uintptr_t len = 1 + xs_prng_get() % ((i*page + chunk_len - offset) < 32 ? (i*page + chunk_len - offset) : 32);
        xs_prng_fill(large_src,len);
        lftl_transaction_write_any(&ctx,area+offset,large_src,len);
        memcpy(large_expected+offset,large_src,len);
      }
      if(r & 4){
        lftl_transaction_commit(&ctx);
      } else {
        lftl_transaction_abort(&ctx);
        lftl_read_unchecked(&ctx,large_expected,area,size4); //the committed data stays
      }
    }
    if(0 == (it % 5)) ctx.data = LFTL_INVALID_POINTER;
    LARGE_CHECK(lftl_check_data(&ctx));
    lftl_read(&ctx,large_buf,area,size4);
    LARGE_CHECK(0 == memcmp(large_buf,large_expected,size4));
  }
  SANITY_CHECK();
}

//The checksum of the data: a chunk the flash loses after its write (its
//unit erased behind the library's back, which any flash can do, here by
//the raw callback) is found, lazily, at the first use of the data after a
//reset; every call that uses the data then raises LFTL_ERROR_DATA_CHECKSUM,
//until a write of the whole data, an erase of all of it, or the repair of
//the checksum; the unchecked read reads it all the same; a repair is one
//commit, and none when the checksum matches.
void large_checksum_test(){
  DEBUG_PRINTLN(__func__);
  const uintptr_t page = LFTL_PAGE_SIZE;
  lftl_nvm_props_t props;
  lftl_ctx_t ctx;
  uint8_t buf[16];
  scratch_large(&ctx,&props,4+2,page+page/2);
  props.write = counting_nvm_write;
  uint8_t*const area = (uint8_t*)ctx.area;
  const uintptr_t size = ctx.data_size;
  lftl_format(&ctx);
  stateful_prng_fill(large_expected,size);
  lftl_write(&ctx,area,large_expected,size);
  large_check_whole(&ctx);
  //chunk 1 is in the unit filled last
  raw_erase(area + ctx.ewlf.head*page,1);
  memcpy(large_src,large_expected,size);
  memset(large_src+page,0xFF,size-page); //what the area reads now
  //a reset, then any use of the data
  ctx.data = LFTL_INVALID_POINTER;
  EXPECT_ERROR(LFTL_ERROR_DATA_CHECKSUM, lftl_read(&ctx,buf,area,sizeof(buf)));
  EXPECT_ERROR(LFTL_ERROR_DATA_CHECKSUM, lftl_read(&ctx,buf,area,sizeof(buf)));
  LARGE_CHECK(!lftl_check_data(&ctx));
  lftl_read_unchecked(&ctx,large_buf,area,size);
  LARGE_CHECK(0 == memcmp(large_buf,large_src,size));
  EXPECT_ERROR(LFTL_ERROR_DATA_CHECKSUM, lftl_write(&ctx,area+1,buf,4));
  {
    uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&ctx)];
    EXPECT_ERROR(LFTL_ERROR_DATA_CHECKSUM, lftl_transaction_start(&ctx,tracker));
  }
  EXPECT_ERROR(LFTL_ERROR_DATA_CHECKSUM, lftl_read_unproven(&ctx,buf,area,4));
  ctx.data = LFTL_INVALID_POINTER;
  EXPECT_ERROR(LFTL_ERROR_DATA_CHECKSUM, lftl_read(&ctx,buf,area,sizeof(buf)));
  //the repair: one commit, the data as it reads
  lftl_repair_checksum(&ctx);
  LARGE_CHECK(lftl_check_data(&ctx));
  memcpy(large_buf,large_expected,size); //what was written
  memcpy(large_expected,large_src,size);
  large_check_whole(&ctx);
  ctx.data = LFTL_INVALID_POINTER;
  large_check_whole(&ctx);
  //none when it matches
  counted_writes = 0;
  lftl_repair_checksum(&ctx);
  LARGE_CHECK(0 == counted_writes);
  //the chunk written again by an ordinary write
  memcpy(large_expected,large_buf,size);
  lftl_write(&ctx,area+page,large_expected+page,size-page);
  ctx.data = LFTL_INVALID_POINTER;
  large_check_whole(&ctx);
  //lost again, then a write of the whole data needs no check and makes the area sound
  raw_erase(area + ctx.ewlf.head*page,1);
  ctx.data = LFTL_INVALID_POINTER;
  stateful_prng_fill(large_expected,size);
  lftl_write(&ctx,area,large_expected,size);
  large_check_whole(&ctx);
  ctx.data = LFTL_INVALID_POINTER;
  LARGE_CHECK(lftl_check_data(&ctx));
  //lost again, then an erase of all of it
  raw_erase(area + ctx.ewlf.head*page,1);
  ctx.data = LFTL_INVALID_POINTER;
  LARGE_CHECK(!lftl_check_data(&ctx));
  lftl_erase_all(&ctx);
  memset(large_expected,0xFF,size);
  large_check_whole(&ctx);
  ctx.data = LFTL_INVALID_POINTER;
  LARGE_CHECK(lftl_check_data(&ctx));
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

//--- EWLF under weak words without T3, with cuts in a row: the cases the
//argument of EWLF.md rests on, as regression tests.

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
//a statement that may end in a simulated cut or in the given error; `outcome`
//is 0 when it returned, 1 on a cut, 2 on the error
#define MAYBE_CUT(outcome, allowed_err_code, stmt) do{\
    jmp_buf __saved_ctx;\
    memcpy(__saved_ctx, exception_ctx, sizeof(jmp_buf));\
    uint32_t __err_code = setjmp(exception_ctx);\
    if(0 == __err_code){\
      stmt;\
      outcome = 0;\
    } else if(is_simulated_tearing(__err_code)){\
      outcome = 1;\
    } else if(__err_code == (uint32_t)(allowed_err_code)){\
      outcome = 2;\
    } else {\
      PRINTF("ERROR: unexpected error 0x%08x, line %d\n",(unsigned)__err_code,__LINE__);\
      fail_abort();\
    }\
    memcpy(exception_ctx, __saved_ctx, sizeof(jmp_buf));\
  }while(0)

//An EWLF area over the scratch pages, with NVM properties that count
//programs and erases, under the weak word model with scripted readings. An
//update is 6 program calls: the record's start marker, data and meta field,
//the pair's marker, entry and confirm. A rollover puts the erase of the data
//unit first. A log switch replaces the pair by the erase of the other log
//unit, a certificate, then the pair.
#define EWLF_CALL_START 0
#define EWLF_CALL_DATA 1
#define EWLF_CALL_META 2
#define EWLF_CALL_MARKER 3
#define EWLF_CALL_ENTRY 4
#define EWLF_CALL_CONFIRM 5
#define EWLF_CALL_SWITCH_ERASE 3
#define EWLF_CALL_SWITCH_CERT 4
#define EWLF_CALL_SWITCH_MARKER 5
#define EWLF_CALL_SWITCH_ENTRY 6
#define EWLF_CALL_SWITCH_CONFIRM 7
typedef struct {
  lftl_ctx_t ctx;
  lftl_nvm_props_t props;
  ewlf_layout_t l;
  uint8_t*area;
  bool model_was_on;
} ewlf_scratch_t;
static void ewlf_scratch_init(ewlf_scratch_t*s, unsigned int n_units, unsigned int records_per_unit, unsigned int certificates){
  scratch_ewlf(&s->ctx,&s->props,n_units,0);
  s->props.erase = counting_nvm_erase;
  s->props.write = counting_nvm_write;
  s->ctx.ewlf_certificates = certificates;
  const uint32_t write_size = s->props.write_size;
  s->ctx.data_size = ((s->props.erase_size / records_per_unit - write_size - LFTL_ROUND_UP(12,write_size)) / write_size) * write_size;
  s->l = ewlf_layout(&s->ctx);
  s->area = (uint8_t*)s->ctx.area;
  if(s->l.records_per_unit != records_per_unit) throw_exception(INTERNAL_ERROR_CORRUPT); //test assumption
  s->model_was_on = nvm_weak_model;
  nvm_weak_model = 1;
  tearing_sim_weak_scope_add(s->ctx.area,s->ctx.area_size);
  tearing_sim_weak_clear_range(s->ctx.area,s->ctx.area_size);
  nvm_weak_override = weak_script_reading;
  weak_script_clear();
  tearing_sim_weak_reseed(0x5EED);
  counted_erases = 0;
  counted_writes = 0;
}
static void ewlf_scratch_end(ewlf_scratch_t*s){
  weak_script_clear();
  nvm_weak_override = 0;
  tearing_sim_weak_clear_range(s->ctx.area,s->ctx.area_size);
  tearing_sim_weak_scope_pop();
  nvm_weak_model = s->model_was_on;
  tearing_sim_clear_call_target();
}
//whether the next write rolls over to the other data unit: its erase is then
//the first call of the write
static bool ewlf_scratch_rolls_over(const ewlf_scratch_t*s){
  const lftl_ewlf_state_t*const st = &s->ctx.ewlf;
  unsigned int started = st->d_pos;
  for(unsigned int p=started+1;p<s->l.records_per_unit;p++){
    if(!ewlf_raw_is_all(ewlf_raw_record(&s->ctx,&s->l,st->d_unit,p),s->l.flag_size,0xFF)) started = p;
  }
  return started + 1 >= s->l.records_per_unit;
}
//a write cut at one of its program calls (EWLF_CALL_*), at its last write unit
static void ewlf_scratch_cut_write(ewlf_scratch_t*s, const uint8_t*d, unsigned int call){
  if(ewlf_scratch_rolls_over(s)) call++;
  tearing_sim_set_call_target(call,true);
  EXPECT_CUT(lftl_write(&s->ctx,s->area,d,s->ctx.data_size));
  tearing_sim_clear_call_target();
}
//a boot: reset, the scan, and what it must find pending
static void ewlf_scratch_boot(ewlf_scratch_t*s, lftl_repair_t pending, lftl_scan_status_t*status){
  tearing_sim_weak_new_boot();
  ewlf_reset(&s->ctx);
  counted_writes = 0;
  counted_erases = 0;
  lftl_scan_status(&s->ctx,status);
  EWLF_CHECK((0 == counted_writes) && (0 == counted_erases));
  EWLF_CHECK(status->pending == pending);
}
//the rest of the boot: the repair, then a read that must return `expected`
static void ewlf_scratch_show(ewlf_scratch_t*s, const uint8_t*expected){
  lftl_complete_scan(&s->ctx);
  read_and_check(&s->ctx,s->area,expected,s->ctx.data_size);
}
//writes until the live log unit has no regular position left: the next write switches
static void ewlf_scratch_fill_log(ewlf_scratch_t*s, uint8_t*d){
  while((unsigned int)(s->ctx.ewlf.unit[s->ctx.ewlf.live].started + 1) < s->l.n_regular){
    stateful_prng_fill(d,s->ctx.data_size);
    lftl_write(&s->ctx,s->area,d,s->ctx.data_size);
  }
}

//A cut in the confirm leaves the entry solid and the confirm weak: the new
//version is shown without a program at a boot that reads the confirm
//programmed, and at a boot that reads it erased it is not taken back, it is
//made solid first (EWLF.md: the decision is the newest valid entry, not the
//newest proven one).
static void ewlf_weak_confirm_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  uint8_t d1[size], d2[size], buf[size];
  lftl_scan_status_t status;
  lftl_format(&s->ctx);
  stateful_prng_fill(d1,size);
  lftl_write(&s->ctx,s->area,d1,size);
  const uint8_t*const confirm = ewlf_raw_confirm(&s->l,ewlf_raw_pair(&s->ctx,&s->l,s->ctx.ewlf.live,s->ctx.ewlf.unit[s->ctx.ewlf.live].started+1));
  stateful_prng_fill(d2,size);
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_CONFIRM);
  EWLF_CHECK(tearing_sim_is_weak(confirm,s->l.flag_size));
  weak_script_set(confirm,WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d2);
  EWLF_CHECK(0 == counted_writes);
  weak_script_set(confirm,WEAK_ERASED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_BURY,&status);
  EXPECT_ERROR(LFTL_ERROR_SCAN_PENDING, lftl_read(&s->ctx,buf,s->area,size));
  lftl_read_unproven(&s->ctx,buf,s->area,size);
  EWLF_CHECK(0 == memcmp(buf,d2,size));
  ewlf_scratch_show(s,d2);
  EWLF_CHECK(3 == counted_writes);
  //the copy is the top now: whatever the weak confirm reads
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d2);
  weak_script_set(confirm,WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d2);
  weak_script_clear();
}

//A cut in the entry leaves it weak. Read invalid first: the previous version
//is buried above it, and when the entry reads valid at a later boot it is
//below a solid decision and is not elected (no resurrection). Read valid
//first: the new version is made solid, and is not taken back when the entry
//reads invalid later (no rollback).
static void ewlf_weak_entry_test(ewlf_scratch_t*s, bool valid_first){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  uint8_t d1[size], d2[size], d3[size];
  lftl_scan_status_t status;
  lftl_format(&s->ctx);
  stateful_prng_fill(d1,size);
  lftl_write(&s->ctx,s->area,d1,size);
  const uint8_t*const pair = ewlf_raw_pair(&s->ctx,&s->l,s->ctx.ewlf.live,s->ctx.ewlf.unit[s->ctx.ewlf.live].started+1);
  //the cut is in the last write unit of the entry
  const uint8_t*const weak = ewlf_raw_entry(&s->l,pair) + s->l.entry_size - s->l.flag_size;
  stateful_prng_fill(d2,size);
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_ENTRY);
  EWLF_CHECK(tearing_sim_is_weak(weak,s->l.flag_size));
  weak_script_set(weak,valid_first ? WEAK_INTENDED : WEAK_ERASED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_BURY,&status);
  const uint8_t*const expected = valid_first ? d2 : d1;
  ewlf_scratch_show(s,expected);
  weak_script_set(weak,valid_first ? WEAK_ERASED : WEAK_INTENDED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,expected);
  weak_script_set(weak,WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,expected);
  stateful_prng_fill(d3,size);
  lftl_write(&s->ctx,s->area,d3,size);
  weak_script_set(weak,WEAK_INTENDED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d3);
  weak_script_clear();
}

//The stuck residual, as sized: on an area with 2 certificates, two log
//switch attempts in a row each cut in its window, the certificate's program
//then the carry's marker, burn one certificate each; every boot in between
//shows the last version and reports one cut less; the third attempt is
//refused before it erases anything, the area still reads, and a format
//brings it back.
static void ewlf_stuck_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  uint8_t d[size], d2[size];
  lftl_scan_status_t status;
  EWLF_CHECK(2 == s->l.n_certs);
  lftl_format(&s->ctx);
  memset(d,0xFF,size);
  ewlf_scratch_fill_log(s,d);
  const unsigned int live = s->ctx.ewlf.live;
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  EWLF_CHECK(2 == status.remaining_cuts);
  //first attempt: the cut in the certificate, which then reads programmed
  stateful_prng_fill(d2,size);
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_SWITCH_CERT);
  weak_script_set(ewlf_raw_cert(&s->ctx,&s->l,live,0),WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  EWLF_CHECK(1 == status.remaining_cuts);
  ewlf_scratch_show(s,d);
  //second attempt: the other log unit is erased again, the last certificate
  //returns, the cut is in the carry's marker, which then reads erased
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_SWITCH_MARKER);
  weak_script_set(ewlf_raw_pair(&s->ctx,&s->l,1-live,0),WEAK_ERASED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  EWLF_CHECK(0 == status.remaining_cuts);
  EWLF_CHECK(!status.stuck);
  ewlf_scratch_show(s,d);
  //third attempt: refused, nothing erased, the record it wrote is all it cost
  const unsigned int data_erases = ewlf_scratch_rolls_over(s) ? 1 : 0;
  counted_erases = 0;
  EXPECT_ERROR(LFTL_ERROR_STUCK, lftl_write(&s->ctx,s->area,d2,size));
  EWLF_CHECK(data_erases == counted_erases);
  lftl_scan_status(&s->ctx,&status);
  EWLF_CHECK(status.stuck && (0 == status.remaining_cuts) && (LFTL_REPAIR_NONE == status.pending));
  read_and_check(&s->ctx,s->area,d,size);
  EXPECT_ERROR(LFTL_ERROR_STUCK, lftl_write(&s->ctx,s->area,d2,size));
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  EWLF_CHECK(0 == status.remaining_cuts);
  ewlf_scratch_show(s,d);
  weak_script_clear();
  lftl_format(&s->ctx);
  lftl_write(&s->ctx,s->area,d2,size);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  EWLF_CHECK(2 == status.remaining_cuts);
  ewlf_scratch_show(s,d2);
}

//A carry attempt that a boot can see: the cut is in the carry's marker,
//which reads programmed. The live unit is certified and the other one has a
//started position and no valid entry: the scan carries the decision above
//it, without erasing, and the other unit is the live one afterwards, with
//all its certificates.
static void ewlf_carry_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  uint8_t d[size], d2[size];
  lftl_scan_status_t status;
  lftl_format(&s->ctx);
  memset(d,0xFF,size);
  ewlf_scratch_fill_log(s,d);
  const unsigned int live = s->ctx.ewlf.live;
  stateful_prng_fill(d2,size);
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_SWITCH_MARKER);
  weak_script_set(ewlf_raw_pair(&s->ctx,&s->l,1-live,0),WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_CARRY,&status);
  EWLF_CHECK(status.remaining_cuts == s->l.n_positions - 1);
  ewlf_scratch_show(s,d);
  EWLF_CHECK((3 == counted_writes) && (0 == counted_erases));
  EWLF_CHECK(s->ctx.ewlf.live == 1-live);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  EWLF_CHECK(status.remaining_cuts == s->l.n_certs);
  EWLF_CHECK(s->ctx.ewlf.live == 1-live);
  ewlf_scratch_show(s,d);
  lftl_write(&s->ctx,s->area,d2,size);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d2);
  weak_script_clear();
}

//The trace the certificates exist for (the checker's knock-outs "cert" and
//"empty" turn it into a resurrection). A first switch attempt is cut in its
//certificate, which reads programmed at the next boot: the other log unit is
//erased again and a second certificate programmed before the carry, whose
//entry is cut. That weak entry of the next generation reads erased at the
//next boot: the decision is carried above it. When it reads valid later, it
//is below the carried decision: the aborted write is not shown.
static void ewlf_no_resurrection_test(ewlf_scratch_t*s){
  DEBUG_PRINTLN(__func__);
  const uintptr_t size = s->ctx.data_size;
  uint8_t d[size], d2[size];
  lftl_scan_status_t status;
  lftl_format(&s->ctx);
  memset(d,0xFF,size);
  ewlf_scratch_fill_log(s,d);
  const unsigned int live = s->ctx.ewlf.live;
  const uint8_t*const cert0 = ewlf_raw_cert(&s->ctx,&s->l,live,0);
  const uint8_t*const weak_entry = ewlf_raw_entry(&s->l,ewlf_raw_pair(&s->ctx,&s->l,1-live,0)) + s->l.entry_size - s->l.flag_size;
  stateful_prng_fill(d2,size);
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_SWITCH_CERT);
  weak_script_set(cert0,WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d);
  const unsigned int data_erases = ewlf_scratch_rolls_over(s) ? 1 : 0;
  counted_erases = 0;
  ewlf_scratch_cut_write(s,d2,EWLF_CALL_SWITCH_ENTRY);
  EWLF_CHECK(data_erases + 1 == counted_erases); //the other log unit, again: its certificate may not be there tomorrow
  EWLF_CHECK(tearing_sim_is_weak(weak_entry,s->l.flag_size));
  weak_script_set(cert0,WEAK_ERASED,0);
  weak_script_set(weak_entry,WEAK_ERASED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_CARRY,&status);
  ewlf_scratch_show(s,d);
  EWLF_CHECK(0 == counted_erases);
  weak_script_set(weak_entry,WEAK_INTENDED,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d);
  weak_script_set(cert0,WEAK_ZEROS,0);
  ewlf_scratch_boot(s,LFTL_REPAIR_NONE,&status);
  ewlf_scratch_show(s,d);
  weak_script_clear();
}

void ewlf_two_cuts_test(){
  DEBUG_PRINTLN(__func__);
  ewlf_scratch_t s;
  //2 data units of 3 records, the default certificates
  ewlf_scratch_init(&s,4,3,0);
  ewlf_weak_confirm_test(&s);
  ewlf_weak_entry_test(&s,false);
  ewlf_weak_entry_test(&s,true);
  ewlf_carry_test(&s);
  ewlf_no_resurrection_test(&s);
  ewlf_scratch_end(&s);
  //the same with 2 certificates, where stuck is two cuts away
  ewlf_scratch_init(&s,4,3,2);
  ewlf_carry_test(&s);
  ewlf_no_resurrection_test(&s);
  ewlf_stuck_test(&s);
  ewlf_scratch_end(&s);
  SANITY_CHECK();
}

//Random writes cut at random programs, boots with random weak readings and a
//cut at a random program of the scan's repair: after every boot the data
//read is one of the last writes, never older than one already shown, never
//older than the last write that completed, and never a write that was cut
//and that a boot has since shown an older version than (no rollback, no
//lost acknowledgement, no resurrection). The unproven read of a boot is the
//version its repair then shows. A stuck area still reads what it showed
//last, by the unproven read when it is the scan that is stuck, and is
//formatted to go on. Over the scratch pages, 2 data units of 3 records, on
//two geometries: 4 regular pairs per log unit, so that the log switches
//every few writes (the certificates are then far too many to run out), and
//1 certificate behind the default number of pairs, kept nearly full by uncut
//writes, where a switch runs out of certificates after one cut.
static void ewlf_weak_stress_core(unsigned int certificates, unsigned int n_regular, bool prefill, unsigned int n_iterations, uint32_t seed){
  ewlf_scratch_t s;
  ewlf_scratch_init(&s,4,3,certificates);
  EWLF_CHECK(s.l.n_regular == n_regular);
  const uintptr_t size = s.ctx.data_size;
  #define STRESS_HISTORY 64
  static uint8_t history[STRESS_HISTORY][LFTL_PAGE_SIZE/3];
  if(size > sizeof(history[0])) throw_exception(INTERNAL_ERROR_CORRUPT);
  unsigned int seq = 0;             //the last write, history[seq % STRESS_HISTORY]
  unsigned int last_complete = 0;   //the last write that completed
  unsigned int last_shown = 0;      //the last write a boot showed
  unsigned int seq_at_last_show = 0;//the last write when it was shown
  unsigned int current = 0;         //the write the area shows now
  unsigned int n_stuck = 0;
  unsigned int n_switches = 0;
  xs_prng_set_seed(seed);
  nvm_weak_override = 0; //the model draws the readings
  bool format = true;
  for(volatile unsigned int it=0;it<n_iterations;it++){
    if(format){
      format = false;
      weak_script_clear();
      lftl_format(&s.ctx);
      seq++;
      memset(history[seq % STRESS_HISTORY],0xFF,size); //a formatted area reads erased
      last_complete = last_shown = seq_at_last_show = current = seq;
    }
    if(prefill){
      //uncut writes until the live log unit is nearly full: the switches are what this geometry is for
      while((unsigned int)(s.ctx.ewlf.unit[s.ctx.ewlf.live].started + 4) < s.l.n_regular){
        seq++;
        uint8_t*const fill = history[seq % STRESS_HISTORY];
        stateful_prng_fill(fill,size);
        lftl_write(&s.ctx,s.area,fill,size);
        last_complete = current = seq;
      }
    }
    //a write, cut at a random program with probability 1/2
    const unsigned int live = s.ctx.ewlf.live;
    seq++;
    uint8_t*const d = history[seq % STRESS_HISTORY];
    stateful_prng_fill(d,size);
    const uint32_t r = xs_prng_get();
    //with the log nearly full, around the programs of a switch
    if(0 == (r & 1)) tearing_sim_set_call_target(prefill ? (3 + (r >> 1) % 4) : ((r >> 1) % 10),(r >> 5) & 1);
    volatile int outcome;
    MAYBE_CUT(outcome,LFTL_ERROR_STUCK,lftl_write(&s.ctx,s.area,d,size));
    tearing_sim_clear_call_target();
    if(0 == outcome){
      last_complete = current = seq;
      if(live != s.ctx.ewlf.live) n_switches++;
    }
    if(2 == outcome){
      //stuck at a write: the area still shows what it showed
      uint8_t buf[size];
      lftl_read(&s.ctx,buf,s.area,size);
      EWLF_CHECK(0 == memcmp(buf,history[current % STRESS_HISTORY],size));
      n_stuck++;
      format = true;
      continue;
    }
    //boots, each with its own weak readings, some with a cut in the repair
    const unsigned int n_boots = 1 + (xs_prng_get() % 3);
    for(volatile unsigned int boot=0;boot<=n_boots;boot++){
      const uint32_t b = xs_prng_get();
      tearing_sim_weak_reseed(((uint64_t)seed << 32) | ((uint64_t)it << 8) | boot);
      tearing_sim_weak_new_boot();
      ewlf_reset(&s.ctx);
      lftl_scan_status_t status;
      lftl_scan_status(&s.ctx,&status);
      uint8_t unproven[size];
      lftl_read_unproven(&s.ctx,unproven,s.area,size);
      //the last boot is not cut: the next write needs the area
      if((boot < n_boots) && (0 == (b & 3))) tearing_sim_set_call_target((b >> 2) % 8,(b >> 5) & 1);
      MAYBE_CUT(outcome,LFTL_ERROR_STUCK,lftl_complete_scan(&s.ctx));
      tearing_sim_clear_call_target();
      if(1 == outcome) continue;
      const uint8_t*shown_data = unproven;
      uint8_t buf[size];
      if(2 == outcome){
        //stuck at the scan: nothing is shown but by the unproven read
        EWLF_CHECK(status.stuck || (LFTL_REPAIR_NONE != status.pending));
        n_stuck++;
        format = true;
      } else {
        lftl_read(&s.ctx,buf,s.area,size);
        EWLF_CHECK(0 == memcmp(buf,unproven,size));
        shown_data = buf;
      }
      unsigned int shown = 0;
      bool found = false;
      for(unsigned int back=0;(back<STRESS_HISTORY) && (back<=seq);back++){
        if(0 == memcmp(shown_data,history[(seq-back) % STRESS_HISTORY],size)){
          shown = seq-back;
          found = true;
          break;
        }
      }
      if(!found){
        PRINTF("ERROR: iteration %u boot %u: the data read is none of the last writes\n",it,boot);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if(format) break; //the unproven read of a stuck area is one of the last writes, no more is promised
      if(shown < last_shown){
        PRINTF("ERROR: iteration %u boot %u: rollback, write %u shown after write %u\n",it,boot,shown,last_shown);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if(shown < last_complete){
        PRINTF("ERROR: iteration %u boot %u: write %u shown, write %u had completed\n",it,boot,shown,last_complete);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if((shown != last_shown) && (shown <= seq_at_last_show)){
        PRINTF("ERROR: iteration %u boot %u: resurrection, write %u shown after a boot showed write %u\n",it,boot,shown,last_shown);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      last_shown = current = shown;
      seq_at_last_show = seq;
    }
  }
  PRINTLN("EWLF weak stress, %u certificates, %u regular pairs: %u iterations, %u log switches, %u times stuck",s.l.n_certs,s.l.n_regular,n_iterations,n_switches,n_stuck);
  if(prefill){
    EWLF_CHECK(n_stuck > 0);
  } else {
    EWLF_CHECK(n_switches > n_iterations/16);
  }
  ewlf_scratch_end(&s);
}
void ewlf_weak_stress_test(){
  DEBUG_PRINTLN(__func__);
  //the certificates take what 4 regular and the reserved pairs leave
  const uint32_t write_size = nvm_props.write_size;
  const uintptr_t pair_size = 2*write_size + LFTL_ROUND_UP(16,write_size);
  const unsigned int many_certificates = (LFTL_PAGE_SIZE - (4 + LFTL_EWLF_RESERVED_PAIRS)*pair_size) / write_size;
  ewlf_weak_stress_core(many_certificates,4,false,2000,0x57E55);
  //1 certificate
  const unsigned int default_regular = (LFTL_PAGE_SIZE - write_size)/pair_size - LFTL_EWLF_RESERVED_PAIRS;
  ewlf_weak_stress_core(1,default_regular,true,400,0x57E56);
  SANITY_CHECK();
}
#endif

#ifdef HAS_TEARING_SIMULATION
//--- large areas under weak words without T3, with cuts in a row

typedef struct {
  lftl_ctx_t ctx;
  lftl_nvm_props_t props;
  uint8_t*area;
  bool model_was_on;
} large_scratch_t;
static void large_scratch_init(large_scratch_t*s, unsigned int n_units, uintptr_t data_size, unsigned int certificates){
  scratch_large(&s->ctx,&s->props,n_units,data_size);
  s->props.erase = counting_nvm_erase;
  s->props.write = counting_nvm_write;
  s->ctx.ewlf_certificates = certificates;
  s->area = (uint8_t*)s->ctx.area;
  s->model_was_on = nvm_weak_model;
  nvm_weak_model = 1;
  tearing_sim_weak_scope_add(s->ctx.area,s->ctx.area_size);
  tearing_sim_weak_clear_range(s->ctx.area,s->ctx.area_size);
  nvm_weak_override = 0; //the model draws the readings
  weak_script_clear();
  tearing_sim_weak_reseed(0x1A26E);
  counted_erases = 0;
  counted_writes = 0;
}
static void large_scratch_end(large_scratch_t*s){
  tearing_sim_weak_clear_range(s->ctx.area,s->ctx.area_size);
  tearing_sim_weak_scope_pop();
  nvm_weak_model = s->model_was_on;
  tearing_sim_clear_call_target();
}

//one operation of the stress, from parameters drawn before it: a write, an
//erase of all the data, or a transaction of one or two writes, each in its
//own chunk, committed
#define LARGE_STRESS_DATA (LFTL_PAGE_SIZE + LFTL_PAGE_SIZE/2)
typedef struct {
  unsigned int kind;
  unsigned int n;
  uintptr_t off[2];
  uintptr_t len[2];
} large_op_t;
static uint8_t large_tracker[LFTL_TRANSACTION_TRACKER_SIZE_LARGE(LARGE_STRESS_DATA,LFTL_WU_SIZE,LFTL_PAGE_SIZE)];
static void large_stress_apply(large_scratch_t*s, const large_op_t*op){
  if(0 == op->kind){
    lftl_write_any(&s->ctx,s->area+op->off[0],large_src,op->len[0]);
  } else if(1 == op->kind){
    lftl_erase_all(&s->ctx);
  } else {
    lftl_transaction_start(&s->ctx,large_tracker);
    for(unsigned int i=0;i<op->n;i++) lftl_transaction_write_any(&s->ctx,s->area+op->off[i],large_src+op->off[i],op->len[i]);
    lftl_transaction_commit(&s->ctx);
  }
}

//Random operations cut at random programs or erases, boots with random weak
//readings and a cut at a random program of the scan's repair, as
//ewlf_weak_stress_core does for EWLF areas: after every boot the data read
//is one of the last operations' results, never older than one already
//shown, never older than the last operation that completed, and never an
//operation that was cut and that a boot has since shown an older result
//than. The data always matches its checksum, the scan programs nothing,
//and a stuck area still shows what it showed. 2 chunks, the last in part,
//over the scratch pages, on two geometries: 4 regular pairs per log unit,
//so that the log switches every few operations, and 1 certificate behind
//the default number of pairs, kept nearly full, where a switch runs out of
//certificates after one cut.
#define LARGE_STRESS_HISTORY 32
static uint8_t large_history[LARGE_STRESS_HISTORY][LARGE_STRESS_DATA];
static void large_weak_stress_core(unsigned int n_pool, unsigned int certificates_arg, unsigned int n_regular, bool prefill, unsigned int n_iterations, uint32_t seed){
  large_scratch_t s;
  //copies that live across the setjmp of MAYBE_CUT
  volatile unsigned int certificates = certificates_arg;
  large_scratch_init(&s,n_pool+2,LARGE_STRESS_DATA,certificates);
  const uintptr_t size = s.ctx.data_size;
  const uintptr_t page = LFTL_PAGE_SIZE;
  const uintptr_t pair_size = 2*s.props.write_size + LFTL_ROUND_UP(2*2+10,s.props.write_size) + LFTL_ROUND_UP(16,s.props.write_size);
  const unsigned int n_certs = certificates ? certificates : LFTL_EWLF_CERTIFICATES;
  LARGE_CHECK((page - n_certs*s.props.write_size)/pair_size - LFTL_EWLF_RESERVED_PAIRS == n_regular);
  #define H(k) large_history[(k) % LARGE_STRESS_HISTORY]
  unsigned int seq = 0;              //the last operation that changed the data, H(seq)
  unsigned int last_complete = 0;    //the last one that completed
  unsigned int last_shown = 0;       //the last one a boot showed
  unsigned int seq_at_last_show = 0; //the last one when it was shown
  unsigned int current = 0;          //the one the area shows now
  unsigned int n_stuck = 0;
  unsigned int n_switches = 0;
  unsigned int n_cut = 0;
  lftl_scan_status_t status;
  xs_prng_set_seed(seed);
  bool format = true;
  for(volatile unsigned int it=0;it<n_iterations;it++){
    if(format){
      format = false;
      lftl_format(&s.ctx);
      seq++;
      memset(H(seq),0xFF,size); //a formatted area reads erased
      last_complete = last_shown = seq_at_last_show = current = seq;
    }
    if(prefill){
      //uncut writes until the live log unit is nearly full
      while((unsigned int)(s.ctx.ewlf.unit[s.ctx.ewlf.live].started + 4) < n_regular){
        seq++;
        memcpy(H(seq),H(current),size);
        const uintptr_t off = xs_prng_get() % (size - 8);
        xs_prng_fill(large_src,8);
        memcpy(H(seq)+off,large_src,8);
        lftl_write_any(&s.ctx,s.area+off,large_src,8);
        last_complete = current = seq;
      }
    }
    //the operation and its result
    large_op_t op;
    const uint32_t r = xs_prng_get();
    op.kind = (0 == (r % 16)) ? 1 : ((r & 2) ? 2 : 0);
    //with the log nearly full: erases of all the data and small writes, so
    //that the cuts can be aimed at the programs of the commit and its switch
    if(prefill) op.kind = (r & 2) ? 1 : 0;
    op.n = 0;
    seq++;
    uint8_t*const d = H(seq);
    memcpy(d,H(current),size);
    xs_prng_fill(large_src,size);
    if(0 == op.kind){
      op.off[0] = xs_prng_get() % size;
      op.len[0] = 1 + xs_prng_get() % (((r & 4) && !prefill) ? (size - op.off[0]) : ((size - op.off[0]) < 40 ? (size - op.off[0]) : 40));
      memcpy(d+op.off[0],large_src,op.len[0]);
    } else if(1 == op.kind){
      memset(d,0xFF,size);
    } else {
      for(unsigned int i=0;i<2;i++){
        //each chunk with probability 1/2, at least one of them
        const bool pick = ((1 == i) && (0 == op.n)) || (0 != ((r >> (3+i)) & 1));
        if(!pick) continue;
        const uintptr_t chunk_len = (0 == i) ? page : (size - page);
        const uintptr_t off = i*page + xs_prng_get() % chunk_len;
        const uintptr_t rest = i*page + chunk_len - off;
        op.off[op.n] = off;
        op.len[op.n] = 1 + xs_prng_get() % (rest < 40 ? rest : 40);
        memcpy(d+off,large_src+off,op.len[op.n]);
        op.n++;
      }
    }
    //cut at a random call with probability 1/2; with the log nearly full,
    //at a call of the commit: after the erase and the programs of each chunk
    //written, one per block of the library (LARGE_BLOCK)
    unsigned int before_pair = 0;
    if(prefill && (0 == op.kind)){
      const uint32_t ws = s.props.write_size;
      for(unsigned int i=op.off[0]/page;i<=(op.off[0]+op.len[0]-1)/page;i++){
        const uintptr_t phy = (0 == i) ? page : LFTL_ROUND_UP(size - page,ws);
        before_pair += 1 + LFTL_DIV_CEIL(phy,2*LFTL_WU_MAX_SIZE);
      }
    }
    const unsigned int live = s.ctx.ewlf.live;
    if(0 == ((r >> 6) & 1)) tearing_sim_set_call_target(prefill ? (before_pair + (r >> 7) % 6) : ((r >> 7) % 48),(r >> 13) & 1);
    volatile int outcome;
    MAYBE_CUT(outcome,LFTL_ERROR_STUCK,large_stress_apply(&s,&op));
    tearing_sim_clear_call_target();
    if(0 == outcome){
      last_complete = current = seq;
      if(live != s.ctx.ewlf.live) n_switches++;
    }
    if(1 == outcome) n_cut++;
    if(2 == outcome){
      //stuck at an operation: the area still shows what it showed
      s.ctx.transaction_tracker = LFTL_INVALID_POINTER;
      lftl_read(&s.ctx,large_buf,s.area,size);
      LARGE_CHECK(0 == memcmp(large_buf,H(current),size));
      n_stuck++;
      format = true;
      continue;
    }
    //boots, each with its own weak readings, some with a cut in the repair
    const unsigned int n_boots = 1 + (xs_prng_get() % 3);
    for(volatile unsigned int boot=0;boot<=n_boots;boot++){
      const uint32_t b = xs_prng_get();
      tearing_sim_weak_reseed(((uint64_t)seed << 32) | ((uint64_t)it << 8) | boot);
      tearing_sim_weak_new_boot();
      s.ctx.data = LFTL_INVALID_POINTER;
      s.ctx.transaction_tracker = LFTL_INVALID_POINTER;
      counted_writes = 0;
      counted_erases = 0;
      lftl_scan_status(&s.ctx,&status);
      //the data the scan chose, checked against its checksum: a mismatch is an error
      lftl_read_unproven(&s.ctx,large_buf,s.area,size);
      LARGE_CHECK((0 == counted_writes) && (0 == counted_erases));
      //the last boot is not cut: the next operation needs the area
      if((boot < n_boots) && (0 == (b & 3))) tearing_sim_set_call_target((b >> 2) % 8,(b >> 5) & 1);
      MAYBE_CUT(outcome,LFTL_ERROR_STUCK,lftl_complete_scan(&s.ctx));
      tearing_sim_clear_call_target();
      if(1 == outcome) continue;
      if(2 == outcome){
        //stuck at the scan: nothing is shown but by the unproven read
        LARGE_CHECK(status.stuck || (LFTL_REPAIR_NONE != status.pending));
        n_stuck++;
        format = true;
      } else {
        lftl_read(&s.ctx,large_expected,s.area,size);
        LARGE_CHECK(0 == memcmp(large_expected,large_buf,size));
        LARGE_CHECK(lftl_check_data(&s.ctx));
      }
      unsigned int shown = 0;
      bool found = false;
      for(unsigned int back=0;(back<LARGE_STRESS_HISTORY) && (back<=seq);back++){
        if(0 == memcmp(large_buf,H(seq-back),size)){
          shown = seq-back;
          found = true;
          break;
        }
      }
      if(!found){
        PRINTF("ERROR: iteration %u boot %u: the data read is none of the last results\n",it,boot);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if(format) break; //the unproven read of a stuck area is one of the last results, no more is promised
      if(shown < last_shown){
        PRINTF("ERROR: iteration %u boot %u: rollback, result %u shown after result %u\n",it,boot,shown,last_shown);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if(shown < last_complete){
        PRINTF("ERROR: iteration %u boot %u: result %u shown, operation %u had completed\n",it,boot,shown,last_complete);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      if((shown != last_shown) && (shown <= seq_at_last_show)){
        PRINTF("ERROR: iteration %u boot %u: resurrection, result %u shown after a boot showed result %u\n",it,boot,shown,last_shown);
        throw_exception(ERROR_VERIFICATION_FAIL);
      }
      last_shown = current = shown;
      seq_at_last_show = seq;
    }
  }
  #undef H
  PRINTLN("Large weak stress, %u pool units, %u certificates, %u regular pairs: %u iterations, %u cut, %u log switches, %u times stuck",n_pool,n_certs,n_regular,n_iterations,n_cut,n_switches,n_stuck);
  if(prefill){
    LARGE_CHECK(n_stuck > 0);
  } else {
    LARGE_CHECK(n_switches > n_iterations/16);
  }
  large_scratch_end(&s);
}
void large_weak_stress_test(){
  DEBUG_PRINTLN(__func__);
  const uint32_t write_size = nvm_props.write_size;
  const uintptr_t pair_size = 2*write_size + LFTL_ROUND_UP(2*2+10,write_size) + LFTL_ROUND_UP(16,write_size);
  //the certificates take what 4 regular and the reserved pairs leave
  const unsigned int many_certificates = (LFTL_PAGE_SIZE - (4 + LFTL_EWLF_RESERVED_PAIRS)*pair_size) / write_size;
  large_weak_stress_core(4,many_certificates,4,false,1500,0x1A265);
  //1 certificate, a pool of 5
  const unsigned int default_regular = (LFTL_PAGE_SIZE - write_size)/pair_size - LFTL_EWLF_RESERVED_PAIRS;
  large_weak_stress_core(5,1,default_regular,true,300,0x1A266);
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
static bool sweep_format; //this cut ends with a format for real, see SWEEP_FORMAT_PERIOD
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
    //back to the state every cut starts from
    if(!sweep_snapshot_valid){
      format_all();
    } else {
      if(sweep_format){
        //a format for real over what the cut left: every area then reads formatted
        format_all();
        sweep_full = 1;
        tearing_sim_check_nvm();
      }
      sweep_restore();
    }
  } else {
    //a real error, that's unexpected
    PRINTF("ERROR: test failed with error code 0x%08x\n",err_code);
    print_cut_context();
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
  #ifdef HAS_TEARING_SIMULATION
  volatile unsigned int dut_targets = 0; //write units the test accesses: the cut points of its sweep
  #endif
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
    tearing_sim_weak_scope_add(largea.area,largea.area_size);
    recovery_depth = 0;
    secondary_call_target = -1;
    current_secondary_target = -1;
    sweep_active = 0;
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
    lftl_register_area_large(&largea);
    format_func(&nvma);
    format_func(&nvmb);
    format_func(&ewlfa);
    format_func(&ewlfb);
    format_func(&nvmc);
    format_func(&nvmd);
    format_func(&largea);
    #ifdef HAS_TEARING_SIMULATION
    sweep_snapshot_valid = 0;
    tearing_sim_init();
    #endif
    dut();//functional test
    #ifdef HAS_TEARING_SIMULATION
    dut_targets = tearing_sim_get_max_target();
    #endif
    err_code = 0;
    led1(0);
  } else {
    exception_handler(err_code);
  }
  #ifdef HAS_TEARING_SIMULATION
    erase_cnt_enabled = 0;
    tearing_sim_check_nvm();//sanity check that model is in sync
    print_erase_cnt_stats();
    //the cut points of the sweep are the accesses of the test alone. Counted
    //after the check above, as they once were, they included its accesses:
    //cut points past the end of the test, where a run cuts nothing
    const unsigned int targets_with_check = tearing_sim_get_max_target();
    volatile unsigned int target_max = dut_targets;
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
    PRINTF("  (%u with the accesses of the check that follows the test, which no cut reaches)\n",targets_with_check);
    volatile unsigned int shard_cuts = 0;
    sweep_active = 1;
    led1(1);
    format_all();
    sweep_snapshot();
    for(volatile unsigned int i=target_start;i<target_max+1;i++){//volatile to remove warning about setjump.
      //if(0 == (i%1000)) PRINTF("tearing simulation target %u\n",i);
      if(0 == (i%50)) print_progress_bar(i-target_start,n_targets);
      if(!tear_at && ((i % shard_count) != shard_index)) continue;
      //a single cut asked for does everything in full
      const bool full = tear_at || (0 == (shard_cuts % SWEEP_FULL_PERIOD));
      sweep_format = tear_at || (0 == (shard_cuts % SWEEP_FORMAT_PERIOD));
      shard_cuts++;
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
          sweep_full = full; //the format that ends a cut sets it
          tearing_sim_set_target(i);
          if(0 == (err_code = setjmp(exception_ctx))){
            dut();//simulated tearing test
            //not cut: the last cut point is the end of the test. Back to the
            //state every cut starts from all the same
            sweep_restore();
          } else {
            exception_handler(err_code);
          }
        }
      }
      secondary_call_target = -1;
      current_secondary_target = -1;
    }
    print_progress_bar_done();
    sweep_active = 0;
    sweep_snapshot_valid = 0;
    if(shard_count > 1) PRINTF("shard %llu of %llu: %u cuts run\n",(unsigned long long)shard_index,(unsigned long long)shard_count,shard_cuts);
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
  
  #define N_TESTS 45
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
    U64_ARG(shard_index,"--shard=",0);
    U64_ARG(shard_count,"--shards=",0);
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
  //on by default: a word a cut left weak then reads differently at every
  //boot, all erased included, for ever (T2 of the flash page without T3), in
  //the EWLF areas, with or without zero overwrite
  (void)weak_words;
  if(!no_weak_words){
    nvm_weak_model = 1;
    nvm_weak_seed = weak_seed;
    n_check_boots = 2; //a word may read differently at the second boot
    PRINTLN("Weak words in the EWLF and large areas, seed %llu",(unsigned long long)weak_seed);
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
  if((0 == shard_count) || (shard_index >= shard_count)){
    PRINTLN("ERROR: '--shard' must be less than '--shards'");
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
      case 33:TEAR_COV(100);test_and_simulate_tearing(ewlf_log_states_test,tear_cov_log[i]);break;
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
      case 41:TEAR_COV(100);test_and_simulate_tearing(large_basic_test      ,tear_cov_log[i]);break;
      case 42:TEAR_COV(100);test_and_simulate_tearing(large_transaction_test,tear_cov_log[i]);break;
      case 43:
        //Hardcoded 0, ignoring --tear-cov: the test expects errors, which a
        //cut injected inside them would replace
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(large_api_test,0);break;
      case 44:
        tear_cov_log[i]=TEAR_COV_NA;test_and_simulate_tearing(large_checksum_test,0);break;
      case 45:
        //Hardcoded 0, ignoring --tear-cov: the test cuts power at chosen
        //points itself, under the weak word model
        tear_cov_log[i]=TEAR_COV_NA;
        #ifdef HAS_TEARING_SIMULATION
        test_and_simulate_tearing(large_weak_stress_test,0);
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

