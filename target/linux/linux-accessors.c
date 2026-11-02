#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>

#define SIMULATED_TEARING 0xFF

void write_file(const char*name, const void*const buf, size_t size);
void read_file(const char*name, void*const buf, size_t*size);

const uint32_t nvm_write_size = LFTL_WU_SIZE;
const uint32_t nvm_erase_size = LFTL_PAGE_SIZE;
//whether the simulated flash accepts a program of all zeros over a write
//unit that is not erased (N3 of doc/source/flash.rst): true models the
//STM32L5 and STM32U5, false a flash without that exception
bool nvm_zero_overwrite = true;

extern const void* nvm_base;
extern uintptr_t nvm_size;
extern uint32_t nvm_alignement;
extern const char*save_nvm_file_name;
extern bool trace_accessors;

//A cut at a byte count: the test harness sweeps it over every write unit an
//operation accesses, reads included
uint64_t tearing_sim_cnt=0;
uint64_t tearing_sim_target_cnt = -1;

//A cut at a program call: the k-th write or erase call after arming is cut at
//its first write unit, or at its last one. The test harness uses it for a
//second cut, inside the scan that recovers from the first.
static int64_t call_target = -1;
static bool call_target_last;
static uint64_t call_cnt;

//Weak words, T2 of doc/source/flash.rst without T3: inside the scopes the
//tests register, the write unit a cut lands in keeps the value its program
//meant to leave and is marked weak, and reads a value drawn at each boot:
//that value, all erased, all zeros, or that value under a random mask. The
//value holds until the next boot (tearing_sim_weak_new_boot, called by the
//test harness when it simulates a reset): the library reads a word once per
//decision, and the cross-check of the Linux build runs two scans per boot
//that must see the same flash. The units of the operation the cut never
//reached stay as they were (T1). A torn erase leaves every unit of its pages
//weak, meant to read erased (T4). A completed program or erase of a weak unit
//makes it solid again. Outside the scopes the older model applies: every byte
//from the cut to the end of the operation is XORed with 0x55, a fixed value
//that never reads erased unless the intended byte was 0xAA; the regular areas
//still rely on T3 and are tested with it.
bool nvm_weak_model = false;
uint64_t nvm_weak_seed = 1;
//a test-installed override of the reading of one weak write unit: `unit`
//comes filled with the value the program meant to leave; the override changes
//it as it likes and returns true, or returns false to let the model choose
bool (*nvm_weak_override)(const void*unit_nvm_addr, uint8_t*unit, uintptr_t size) = 0;
static uint8_t*weak_flag;      //one per NVM byte, set when the byte is weak
static uint8_t*weak_intended;  //what the torn program meant to leave there
static uint8_t*weak_drawn;     //one per NVM byte, set once the byte's reading was drawn this boot
static uint8_t*weak_value;     //the reading drawn this boot
#define WEAK_SCOPES_MAX 4
static const uint8_t*weak_scope_lo[WEAK_SCOPES_MAX];
static const uint8_t*weak_scope_hi[WEAK_SCOPES_MAX];
static unsigned int n_weak_scopes;
static uint64_t weak_rng = 1;

static uintptr_t nvm_offset(const void*addr){
  return (uintptr_t)addr - (uintptr_t)nvm_base;
}
static void weak_alloc(){
  if(weak_flag) return;
  weak_flag = calloc(nvm_size,1);
  weak_intended = calloc(nvm_size,1);
  weak_drawn = calloc(nvm_size,1);
  weak_value = calloc(nvm_size,1);
  if((0 == weak_flag) || (0 == weak_intended) || (0 == weak_drawn) || (0 == weak_value)){
    printf("ERROR: out of memory for the weak word model\n");
    abort();
  }
}
void tearing_sim_weak_scope_clear(){
  n_weak_scopes = 0;
}
void tearing_sim_weak_scope_add(const void*lo, uintptr_t size){
  if(WEAK_SCOPES_MAX == n_weak_scopes){
    printf("ERROR: too many weak word scopes\n");
    abort();
  }
  weak_scope_lo[n_weak_scopes] = (const uint8_t*)lo;
  weak_scope_hi[n_weak_scopes] = ((const uint8_t*)lo) + size;
  n_weak_scopes++;
}
void tearing_sim_weak_scope_pop(){
  if(n_weak_scopes) n_weak_scopes--;
}
static bool weak_active(const void*addr){
  if(!nvm_weak_model) return 0;
  const uint8_t*const a = (const uint8_t*)addr;
  for(unsigned int i=0;i<n_weak_scopes;i++){
    if((a >= weak_scope_lo[i]) && (a < weak_scope_hi[i])) return 1;
  }
  return 0;
}
void tearing_sim_weak_clear_range(const void*addr, uintptr_t size){
  if(weak_flag){
    memset(weak_flag + nvm_offset(addr),0,size);
    memset(weak_drawn + nvm_offset(addr),0,size);
  }
}
void tearing_sim_weak_clear(){
  if(weak_flag){
    memset(weak_flag,0,nvm_size);
    memset(weak_drawn,0,nvm_size);
  }
}
//a new boot: every weak unit reads a fresh value from now on
void tearing_sim_weak_new_boot(){
  if(weak_drawn) memset(weak_drawn,0,nvm_size);
}
bool tearing_sim_is_weak(const void*addr, uintptr_t size){
  if(0 == weak_flag) return 0;
  const uint8_t*const f = weak_flag + nvm_offset(addr);
  for(uintptr_t i=0;i<size;i++) if(f[i]) return 1;
  return 0;
}
static void weak_mark(const void*addr, const uint8_t*intended, uintptr_t size){
  weak_alloc();
  memset(weak_flag + nvm_offset(addr),1,size);
  memset(weak_drawn + nvm_offset(addr),0,size);
  memcpy(weak_intended + nvm_offset(addr),intended,size);
}
static void weak_mark_erased(const void*addr, uintptr_t size){
  weak_alloc();
  memset(weak_flag + nvm_offset(addr),1,size);
  memset(weak_drawn + nvm_offset(addr),0,size);
  memset(weak_intended + nvm_offset(addr),0xFF,size);
}
void tearing_sim_weak_reseed(uint64_t seed){
  weak_rng = seed ? seed : 0x9E3779B97F4A7C15ull;
}
static uint64_t weak_rand(){
  uint64_t x = weak_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  weak_rng = x;
  return x;
}
//the reading of one weak write unit this boot: drawn at its first read
static void weak_reading(const uint8_t*unit_nvm_addr, uint8_t*unit){
  const uintptr_t ws = nvm_write_size;
  const uintptr_t off = nvm_offset(unit_nvm_addr);
  if(weak_drawn[off]){
    memcpy(unit,weak_value + off,ws);
    return;
  }
  const uint8_t*const intended = weak_intended + off;
  memcpy(unit,intended,ws);
  if(!(nvm_weak_override && nvm_weak_override(unit_nvm_addr,unit,ws))){
    switch(weak_rand() % 4){
      case 0: break;
      case 1: memset(unit,0xFF,ws); break;
      case 2: memset(unit,0x00,ws); break;
      default: for(uintptr_t i=0;i<ws;i++) unit[i] = intended[i] ^ (uint8_t)weak_rand(); break;
    }
  }
  memcpy(weak_value + off,unit,ws);
  memset(weak_drawn + off,1,ws);
}
//substitute the reading of every weak write unit a read range overlaps
static void weak_read(uint8_t*dst, const uint8_t*src, uintptr_t size){
  if(!nvm_weak_model || (0 == weak_flag) || (0 == size)) return;
  const uintptr_t ws = nvm_write_size;
  const uintptr_t src_off = nvm_offset(src);
  const uintptr_t first = src_off / ws;
  const uintptr_t last = (src_off + size - 1) / ws;
  for(uintptr_t u = first; u <= last; u++){
    const uintptr_t unit_off = u*ws;
    const uint8_t*const unit_nvm_addr = ((const uint8_t*)nvm_base) + unit_off;
    if(!tearing_sim_is_weak(unit_nvm_addr,ws)) continue;
    uint8_t unit[ws];
    weak_reading(unit_nvm_addr,unit);
    //the part of the unit inside the read
    const uintptr_t lo = unit_off > src_off ? unit_off : src_off;
    const uintptr_t end = (unit_off + ws) < (src_off + size) ? (unit_off + ws) : (src_off + size);
    memcpy(dst + (lo - src_off),unit + (lo - unit_off),end - lo);
  }
}

void tearing_sim_init(){
  tearing_sim_cnt=0;
  tearing_sim_target_cnt = -1;
  call_target = -1;
  call_cnt = 0;
  tearing_sim_weak_clear();
}

uint32_t tearing_sim_get_max_target(){
  return tearing_sim_cnt / nvm_write_size;
}

void tearing_sim_set_target(uint64_t target_write){
  tearing_sim_cnt=0;
  tearing_sim_target_cnt = target_write * nvm_write_size;
  //the weak readings of a run are reproducible from the seed and the cut
  tearing_sim_weak_reseed(nvm_weak_seed ^ (target_write * 0x9E3779B97F4A7C15ull));
  tearing_sim_weak_new_boot();
}

void tearing_sim_set_call_target(uint64_t k, bool at_last_unit){
  call_target = k;
  call_target_last = at_last_unit;
  call_cnt = 0;
}
void tearing_sim_clear_call_target(){
  call_target = -1;
}
//write and erase calls since the last arming of a call target, or since init
uint64_t tearing_sim_get_call_count(){
  return call_cnt;
}

uint32_t tearing_size(uint32_t size){
  if(tearing_sim_cnt + size > tearing_sim_target_cnt){
    uint32_t out = tearing_sim_cnt + size - tearing_sim_target_cnt;
    //reset counters
    tearing_sim_cnt=0;
    tearing_sim_target_cnt = -1;
    return out;
  }
  tearing_sim_cnt += size;
  return 0;
}

//bytes from the cut to the end of a program or erase of `size` bytes, 0 when
//the operation is not cut: by the byte count, or by the call count
static uint32_t cut_size(uint32_t size){
  uint32_t tsize = tearing_size(size);
  if(call_target >= 0){
    if((uint64_t)call_target == call_cnt){
      call_target = -1;
      tsize = call_target_last ? nvm_write_size : size;
    }
  }
  call_cnt++;
  return tsize;
}

//the older model of a cut: every byte from it to the end of the operation
//XORed with 0x55
static void corrupt_tail(void*base_address, uint32_t size, uint32_t tsize){
  //we prefer corrupting the data rather than exactly simulating
  //a tearing that would keep old data because in some case
  //a tearing could go undetected (old data may match new data)
  const uint32_t offset = size - tsize;
  for(uint32_t i = offset;i<size;i++){
    uint8_t*dst = (uint8_t*)base_address+i;
    *dst ^= 0x55;
  }
}

bool tearing_sim(void*base_address, uint32_t size){
  const uint32_t tsize = tearing_size(size);
  if(tsize){
    corrupt_tail(base_address,size,tsize);
  }
  return tsize ? 1:0;
}

uint32_t get_alignement_requirement(uint32_t original_req){
  if(original_req > nvm_alignement) return nvm_alignement;
  return original_req;
}
void dump_core(uintptr_t addr, uintptr_t size, uintptr_t display_addr);

void erase_cnt_update(void*base_address, unsigned int n_pages);
void __attribute__((weak)) erase_cnt_update(void*base_address, unsigned int n_pages){}

uint8_t nvm_erase(void*base_address, unsigned int n_pages){
  const uintptr_t size = n_pages * nvm_erase_size;
  if(trace_accessors){
    printf("nvm_erase @0x%08lx, %lu bytes\n\r",(uintptr_t)base_address,size);
  }
  if(base_address < nvm_base) return 1;
  if(((uintptr_t)base_address + size) > ((uintptr_t)nvm_base + nvm_size)) return 2;
  //Linux makes it hard to get nvm aligned to large units like 4k or 8k, so we check against the minimum between the original constraint and the alignement of nvm
  if(0 != ((uintptr_t)base_address % get_alignement_requirement(nvm_erase_size))) return 3;
  //count only erases that happen: an out of range base_address would index erase_cnt out of bounds
  erase_cnt_update(base_address, n_pages);

  const uint32_t tsize = cut_size(size);
  memset(base_address, 0xFF, size);
  if(tsize && weak_active(base_address)){
    //T4: the cells of the pages are indeterminate: every unit of them is
    //weak, meant to read erased
    weak_mark_erased(base_address,size);
  } else {
    tearing_sim_weak_clear_range(base_address,size);
    if(tsize) corrupt_tail(base_address,size,tsize);
  }
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,nvm_base,nvm_size);
  }
  return tsize ? SIMULATED_TEARING:0;
}

static bool is_all(const uint8_t*buf, uint8_t val, uintptr_t size){
  for(uintptr_t i=0;i<size;i++){
    if(buf[i] != val) return 0;
  }
  return 1;
}

uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(trace_accessors){
    printf("nvm_write ");
    dump_core((uintptr_t)src,size,(uintptr_t)dst_nvm_addr);
  }
  if(dst_nvm_addr < nvm_base) return 1;
  if(((uintptr_t)dst_nvm_addr + size) > ((uintptr_t)nvm_base + nvm_size)) return 2;
  if(0 != ((uintptr_t)dst_nvm_addr % nvm_write_size)) return 3;
  if(0 != (size % nvm_write_size)) return 4;
  //like STM32U5-class flash: a write unit can be programmed only while it is
  //erased, unless the data to write is all 0 and the flash accepts that (R3
  //and N3 of doc/source/flash.rst: the library needs the exception only to
  //pin a word a power cut left weak, and only on a flash that declares it).
  //A weak unit is not erased, whatever it reads.
  for(uintptr_t i=0;i<size;i+=nvm_write_size){
    const uint8_t*const dst8 = (const uint8_t*)dst_nvm_addr + i;
    const uint8_t*const src8 = (const uint8_t*)src + i;
    const bool erased = is_all(dst8,0xFF,nvm_write_size) && !tearing_sim_is_weak(dst8,nvm_write_size);
    if(!erased && !(nvm_zero_overwrite && is_all(src8,0x00,nvm_write_size))) return 5;
  }
  const uint32_t tsize = cut_size(size);
  if(tsize && weak_active(dst_nvm_addr)){
    //the units before the cut are programmed, the unit the cut lands in is
    //weak, the units after it are untouched
    const uintptr_t cut = ((size - tsize) / nvm_write_size) * nvm_write_size;
    uint8_t*const dst8 = (uint8_t*)dst_nvm_addr;
    const uint8_t*const src8 = (const uint8_t*)src;
    memcpy(dst8,src8,cut);
    tearing_sim_weak_clear_range(dst8,cut);
    memcpy(dst8+cut,src8+cut,nvm_write_size);
    weak_mark(dst8+cut,src8+cut,nvm_write_size);
  } else {
    memcpy(dst_nvm_addr,src,size);
    tearing_sim_weak_clear_range(dst_nvm_addr,size);
    if(tsize) corrupt_tail(dst_nvm_addr,size,tsize);
  }
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,nvm_base,nvm_size);
  }
  return tsize ? SIMULATED_TEARING:0;
}

uint8_t nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  memcpy(dst,src_nvm_addr,size);
  weak_read((uint8_t*)dst,(const uint8_t*)src_nvm_addr,size);
  if(trace_accessors){
    printf("nvm_read ");
    dump_core((uintptr_t)dst,size,(uintptr_t)src_nvm_addr);
  }
  //corrupting dst (a transient, caller-owned buffer) rather than src_nvm_addr:
  //a real NVM read glitch does not alter the stored data, and the caller
  //discards dst entirely on a nonzero return, so this has no lasting effect
  //on NVM state or the test harness's reference model.
  bool tearing = tearing_sim(dst,size);
  return tearing ? SIMULATED_TEARING:0;
}
