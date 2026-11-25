#pragma once

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "lean-ftl.h"

#ifdef LFTL_DEBUG
  #ifdef HAS_PRINTF
    #include <stdio.h>
    #define PRINTF(...) printf( __VA_ARGS__ );
  #else
    #define PRINTF(...)
  #endif
  #define DEBUG_PRINTLN(...) do{PRINTF( __VA_ARGS__ );PRINTF("\n\r");}while(0)
#else
  #define PRINTF(...)
  #define DEBUG_PRINTLN(...)
#endif

#define NO_TRANSACTION 0
#define TRANSACTION 1

#define UNALIGNED 0
#define ALIGNED 1

/**
 * \brief Report a fatal NVM-level error and never return.
 *
 * Calls the NVM's error handler if one is registered, then loops forever.
 * This is the last resort for the (unexpected) case where the registered
 * error handler itself returns, even though it is documented to never do so.
 *
 * \param ctx Properties of the NVM on which the error occurred
 * \param err_code The error code to report
 */
static void raise_nvm_error(lftl_nvm_props_t*ctx, uint32_t err_code){
  if((uintptr_t)(ctx->error_handler)!=(uintptr_t)LFTL_INVALID_POINTER){
    ctx->error_handler(err_code);
  }
  while(1);
}

/**
 * \brief Report a fatal error for an operation on an LFTL area and never return.
 *
 * Aborts the area's transaction, if any, before calling ::raise_nvm_error:
 * after an error the transaction may be inconsistent (tracker bits set for
 * write units never written), and the error handler may unwind the frame
 * holding the tracker. On an EWLF or a large area a low level or internal
 * error also forgets the scan, see the body.
 *
 * \param ctx Context of the LFTL area on whose behalf the operation was done,
 *   or ::LFTL_INVALID_POINTER if none
 * \param nvm_props Properties of the NVM on which the error occurred
 * \param err_code The error code to report
 */
static void raise_area_error(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, uint32_t err_code){
  if(LFTL_INVALID_POINTER != ctx){
    ctx->transaction_tracker = LFTL_INVALID_POINTER;
    //an EWLF or a large area tracks its log in volatile memory: after a
    //failed flash operation, or an internal error, that state is not to be
    //trusted, and the next access scans again
    if((lftl_is_ewlf(ctx) || lftl_is_large(ctx)) && (err_code >= LFTL_ERROR_LOW_LEVEL_ERASE)) ctx->data = LFTL_INVALID_POINTER;
  }
  raise_nvm_error(nvm_props, err_code);
}

/**
 * \brief Report a fatal error for an LFTL area and never return.
 *
 * Convenience wrapper around ::raise_area_error using the area's own NVM.
 *
 * \param ctx Context of the LFTL area on which the error occurred
 * \param err_code The error code to report
 */
static void raise_error(lftl_ctx_t*ctx, uint32_t err_code){
  raise_area_error(ctx, ctx->nvm_props, err_code);
}

/**
 * \brief Erase one or more physical pages, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_erase_t callback. Does
 * nothing if `n_pages` is 0.
 *
 * \param ctx Context of the LFTL area on whose behalf this is done, see ::raise_area_error
 * \param nvm_props Properties of the target NVM
 * \param base_address Start address of the range to erase
 * \param n_pages Number of physical pages to erase
 */
static void nvm_erase(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, void*base_address, unsigned int n_pages){
  if(0==n_pages) return;
  uint8_t status = nvm_props->erase(base_address, n_pages);
  if(status) raise_area_error(ctx,nvm_props,LFTL_ERROR_LOW_LEVEL_ERASE | status);
}

/**
 * \brief Write physical NVM, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_write_t callback. Does
 * nothing if `size` is 0.
 *
 * \param ctx Context of the LFTL area on whose behalf this is done, see ::raise_area_error
 * \param nvm_props Properties of the target NVM
 * \param dst_nvm_addr Destination address in NVM
 * \param src Source address
 * \param size Size in bytes to write
 */
static void nvm_write(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0==size) return;
  uint8_t status = nvm_props->write(dst_nvm_addr, src, size);
  if(status) raise_area_error(ctx,nvm_props,LFTL_ERROR_LOW_LEVEL_WRITE | status);
}

/**
 * \brief Read physical NVM, raising an error on failure.
 *
 * Thin wrapper around the user-provided ::nvm_read_t callback. Does
 * nothing if `size` is 0.
 *
 * \param ctx Context of the LFTL area on whose behalf this is done, see ::raise_area_error
 * \param nvm_props Properties of the target NVM
 * \param dst Destination address, in volatile memory
 * \param src_nvm_addr Source address in NVM
 * \param size Size in bytes to read
 */
static void nvm_read(lftl_ctx_t*ctx, lftl_nvm_props_t*nvm_props, void* dst, const void*const src_nvm_addr, uintptr_t size){
  if(0==size) return;
  uint8_t status = nvm_props->read(dst, src_nvm_addr, size);
  if(status) raise_area_error(ctx,nvm_props,LFTL_ERROR_LOW_LEVEL_READ | status);
}

/**
 * \brief Compute (part of) a CRC32C checksum.
 *
 * This is only the core of the CRC computation: to get the full CRC,
 * initialize `crc` to -1 and complement the result of this function.
 * Can be called repeatedly on successive chunks of a larger buffer by
 * feeding the previous return value back in as `crc`.
 *
 * \param crc Initial/accumulated CRC value
 * \param buf Start address of the buffer
 * \param len Size in bytes of the buffer
 * \returns The updated (not yet complemented) CRC value
 */
static uint32_t crc32c(uint32_t crc, const void*const buf, unsigned int len) {
  const uint8_t*buf8 = (const uint8_t*)buf;
  //Its the core of the CRC only
  //to get full CRC: init crc=-1 and complement the result of that function
  uint32_t poly = 0x05EC76F1;
  while (len != 0) {
    crc = crc ^ *buf8++;
    for (unsigned int i = 0; i<8; i++) {
        uint32_t mask = -(crc & 1);
        crc = (crc >> 1) ^ (poly & mask);
    }
    len--;
  }
  return crc;
}

/**
 * \brief Compute (part of) a CRC-64/NVME, the checksum of EWLF records and of
 * the map and the data of large areas.
 *
 * Width 64, polynomial 0xAD93D23594C93659 (0x9A6C9329AC4BC9B5 reflected),
 * reflected input and output, initial value and final XOR all ones; the
 * check value of "123456789" is 0xAE8B14860A799888. This is the core only:
 * to get the full CRC, initialize `crc` to all ones and complement the
 * result.
 *
 * Two forms, of the same result:
 * - the default takes the input four bits at a time through a table of 16
 *   entries, 128 bytes of read-only data. It reads the table at an index
 *   that depends on the input: where reads of the table take a time that
 *   depends on their address (a data cache), so does the running time;
 * - with `LFTL_CRC64_BITWISE` defined when the library is built, one bit at
 *   a time with a branch-free mask: no table, a running time that depends on
 *   the length alone, at about four times the time per byte.
 *
 * \param crc Initial/accumulated CRC value
 * \param buf Start address of the buffer
 * \param len Size in bytes of the buffer
 * \returns The updated (not yet complemented) CRC value
 */
static uint64_t crc64_update(uint64_t crc, const void*const buf, unsigned int len) {
  const uint8_t*buf8 = (const uint8_t*)buf;
#ifdef LFTL_CRC64_BITWISE
  const uint64_t poly = 0x9A6C9329AC4BC9B5ull;
  while (len != 0) {
    crc = crc ^ *buf8++;
    for (unsigned int i = 0; i<8; i++) {
        const uint64_t mask = -(crc & 1);
        crc = (crc >> 1) ^ (poly & mask);
    }
    len--;
  }
#else
  //table[i]: the register i after four steps of the bitwise form
  static const uint64_t table[16] = {
    0x0000000000000000ull, 0x78E0FF3B88BE6F81ull,
    0xF1C1FE77117CDF02ull, 0x8921014C99C2B083ull,
    0xD75ADABD7A6E2D6Full, 0xAFBA2586F2D042EEull,
    0x269B24CA6B12F26Dull, 0x5E7BDBF1E3AC9DECull,
    0x9A6C9329AC4BC9B5ull, 0xE28C6C1224F5A634ull,
    0x6BAD6D5EBD3716B7ull, 0x134D926535897936ull,
    0x4D364994D625E4DAull, 0x35D6B6AF5E9B8B5Bull,
    0xBCF7B7E3C7593BD8ull, 0xC41748D84FE75459ull,
  };
  while (len != 0) {
    crc = crc ^ *buf8++;
    crc = (crc >> 4) ^ table[crc & 0xF];
    crc = (crc >> 4) ^ table[crc & 0xF];
    len--;
  }
#endif
  return crc;
}

/**
 * \brief Multiply two polynomials modulo the CRC-64/NVME polynomial, in the
 * reflected representation ::crc64_update uses for its register: bit 63 is
 * the coefficient of x^0. Branch-free: the running time depends on nothing.
 *
 * \param a First factor
 * \param b Second factor
 * \returns a times b modulo the polynomial
 */
static uint64_t crc64_mulmod(uint64_t a, uint64_t b){
  const uint64_t poly = 0x9A6C9329AC4BC9B5ull;
  uint64_t p = 0;
  for(unsigned int i=0;i<64;i++){
    p ^= b & -((a >> (63 - i)) & 1);
    b = (b >> 1) ^ (poly & -(b & 1));
  }
  return p;
}

/**
 * \brief What ::crc64_update makes of a register followed by `n` zero bytes,
 * without the `n` steps: the register times x^(8n) modulo the polynomial,
 * by squaring. The CRC is linear, so this is how the checksum of the data of
 * a large area follows the chunks an update replaces (zlib's crc32_combine
 * works the same way).
 *
 * \param crc The register
 * \param n Number of zero bytes
 * \returns The register after them
 */
static uint64_t crc64_shift(uint64_t crc, uintptr_t n){
  uint64_t x8 = (uint64_t)1 << (63 - 8); //x^8: one zero byte
  while(n){
    if(n & 1) crc = crc64_mulmod(x8, crc);
    x8 = crc64_mulmod(x8, x8);
    n >>= 1;
  }
  return crc;
}

/**
 * \brief Return the greater of two `uintptr_t` values.
 *
 * \param a First value
 * \param b Second value
 * \returns `a` if it is greater than `b`, `b` otherwise
 */
static uintptr_t max_uintptr(uintptr_t a,uintptr_t b){
  return a > b ? a : b;
}

/**
 * \brief Check whether an address falls within a byte range.
 *
 * \param addr Address to test
 * \param base Start address of the range
 * \param size Size in bytes of the range
 * \returns true if `base <= addr < base+size`, false otherwise
 */
//Note: gcov/gcovr reports this whole function as 0 hits with one branch in
//the billions (a "suspicious hits" warning at coverage time), despite both
//outcomes of both checks below being genuinely exercised by tests -- this
//is a heavily-inlined, extremely hot static helper, in the same class as
//crc32c() and page_size() below; a known GCC/gcov counter-attribution
//quirk at -Og for such functions, not a real coverage gap.
static bool is_in_range(const void*const addr, const void*const base, uintptr_t size){
  if(addr < base) return false;
  if((uintptr_t)addr >= ((uintptr_t)base+size)) return false;
  return true;
}

/**
 * \brief Check whether a logical address is within an area's data.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to test (always between `ctx->area`
 *   and `ctx->area+ctx->data_size`, if valid)
 * \returns true if `nvm_addr` is within `ctx`'s data range
 */
static bool is_in_data(lftl_ctx_t*ctx, const void*const nvm_addr){//nvm_addr is a logical address, so always between ctx->area and ctx->area+data_size
  return is_in_range(nvm_addr, ctx->area, ctx->data_size);
}

/**
 * \brief Check that a logical range is within an area's data, raise an error otherwise.
 *
 * The end of the range is checked without computing `nvm_addr+size`, so a
 * `size` close to `UINTPTR_MAX` cannot wrap around and pass.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address of the start of the range
 * \param size Size in bytes of the range
 */
static void check_in_data(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(!is_in_data(ctx, nvm_addr)) raise_error(ctx,LFTL_ERROR_FIRST_NOT_IN_DATA);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  if(size > ctx->data_size - offset) raise_error(ctx,LFTL_ERROR_LAST_NOT_IN_DATA);
}

/**
 * \brief Search the registered areas other than `ctx` for one containing an address.
 *
 * Walks the circular linked list of registered areas (via `ctx->next`),
 * starting just after `ctx`, until it wraps back around to `ctx`.
 *
 * \param ctx Context to start the search after (excluded from the search itself)
 * \param nvm_addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*get_other_ctx(lftl_ctx_t*ctx, const void*const nvm_addr){
  const lftl_ctx_t*stop=ctx;
  while(ctx->next != stop){
    //Defensive only: lftl_register_area(_ewlf) always keeps the list
    //properly circular, so an in-list node's `next` is never
    //LFTL_INVALID_POINTER; this `break` cannot be reached by any test.
    if(LFTL_INVALID_POINTER==ctx->next) break;
    ctx = ctx->next;
    if(is_in_data(ctx,nvm_addr)) return ctx;
  }
  return LFTL_INVALID_POINTER;
}

/**
 * \brief Search `ctx` and the other registered areas for one containing an address.
 *
 * Checks `ctx` itself first, then falls back to ::get_other_ctx.
 *
 * \param ctx Context to check first (may be ::LFTL_INVALID_POINTER)
 * \param nvm_addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*get_any_ctx(lftl_ctx_t*ctx, const void*const nvm_addr){
  if((LFTL_INVALID_POINTER!=ctx) && is_in_data(ctx,nvm_addr)) return ctx;
  return get_other_ctx(ctx,nvm_addr);
}

static lftl_ctx_t*first_area;
static lftl_ctx_t*first_area_ewlf;
static lftl_ctx_t*first_area_large;
static lftl_nvm_props_t*first_nvm;

/**
 * \brief Link an area into a circular list and register its NVM.
 *
 * Shared implementation for ::lftl_register_area and ::lftl_register_area_ewlf:
 * inserts `ctx` into the circular linked list pointed to by `*first`
 * (creating a single-element self-loop if it was empty), then registers
 * `ctx->nvm_props` via ::lftl_register_nvm.
 *
 * \param ctx Context of the area to register
 * \param first Pointer to the head of the target list (::first_area, ::first_area_ewlf or ::first_area_large)
 */
static void lftl_register_area_core(lftl_ctx_t*ctx, lftl_ctx_t**first){
  if(LFTL_INVALID_POINTER==*first){
    *first = ctx;
    ctx->next = ctx;
  } else {
    lftl_ctx_t*old_second = (*first)->next;
    (*first)->next = ctx;
    ctx->next = old_second;
  }
  lftl_register_nvm(ctx->nvm_props);
}

/**
 * \brief Search a list of registered areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \param first Head of the circular area list to search (::first_area, ::first_area_ewlf or ::first_area_large)
 * \returns The matching context, or ::LFTL_INVALID_POINTER if `first` is
 *   ::LFTL_INVALID_POINTER or no area matches
 */
static lftl_ctx_t*addr_to_area_core(const void*const addr, lftl_ctx_t*first){
  if(LFTL_INVALID_POINTER == first) return LFTL_INVALID_POINTER;
  lftl_ctx_t*ctx = first;
  ctx = get_any_ctx(ctx, addr); // we search first within LFTL areas to return the right ctx if several areas use the same NVM.
  return ctx;
}

/**
 * \brief Search the registered std areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*addr_to_area_std(const void*const addr){
  return addr_to_area_core(addr, first_area);
}

/**
 * \brief Search the registered EWLF areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*addr_to_area_ewlf(const void*const addr){
  return addr_to_area_core(addr, first_area_ewlf);
}

/**
 * \brief Search the registered large areas for one containing an address.
 *
 * \param addr Logical address to search for
 * \returns The matching context, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_ctx_t*addr_to_area_large(const void*const addr){
  return addr_to_area_core(addr, first_area_large);
}

/**
 * \brief Search the registered physical NVMs for one containing an address.
 *
 * Walks the circular linked list of ::lftl_nvm_props_t built by
 * ::lftl_register_nvm.
 *
 * \param addr Physical address to search for
 * \returns The matching NVM properties, or ::LFTL_INVALID_POINTER if none matches
 */
static lftl_nvm_props_t*addr_to_nvm(const void*const addr){
  if(LFTL_INVALID_POINTER!=first_nvm){
    lftl_nvm_props_t*it = first_nvm;
    const lftl_nvm_props_t*stop=it;
    do{
      //Defensive only: lftl_register_nvm always keeps the list properly
      //circular, so an in-list node's `next` is never LFTL_INVALID_POINTER;
      //this `break` cannot be reached by any test.
      if(LFTL_INVALID_POINTER==it->next) break;
      if(is_in_range(addr, it->base, it->size)) return it;
      it = it->next;
    }while(it != stop);
  }
  return LFTL_INVALID_POINTER;
}

/**
 * \brief Resolve an address to its owning area (if any) and physical NVM.
 *
 * Searches std areas first, then EWLF areas, then large areas, so that if the same NVM is
 * shared by several areas the correct owning context is returned via
 * `*ctx`. If `addr` is not within any registered area, falls back to
 * ::addr_to_nvm to check whether it is at least within some registered
 * physical NVM.
 *
 * \param addr Address to resolve
 * \param ctx Output: set to the owning context, or ::LFTL_INVALID_POINTER
 *   if `addr` is not within any registered area
 * \returns The NVM properties covering `addr`, or ::LFTL_INVALID_POINTER
 *   if `addr` is not within any registered area or physical NVM
 */
static lftl_nvm_props_t*addr_to_area_and_nvm(const void*const addr, lftl_ctx_t**ctx){
  // we search first within LFTL areas to return the right ctx if several areas use the same NVM.
  *ctx = addr_to_area_std(addr);
  if(LFTL_INVALID_POINTER == *ctx) *ctx = addr_to_area_ewlf(addr);
  if(LFTL_INVALID_POINTER == *ctx) *ctx = addr_to_area_large(addr);
  if(LFTL_INVALID_POINTER!=*ctx) return (*ctx)->nvm_props;
  // addr is not in any LFTL areas of any kind, check other NVM addresses
  return addr_to_nvm(addr);
}

/**
 * \brief Read bytes from wherever `src` actually is.
 *
 * If `src` is a physical address within a registered NVM, reads it via
 * that NVM's ::nvm_read_t callback (needed since NVM may not be directly
 * memory-addressable). Otherwise, treats `src` as regular memory and
 * uses a plain `memcpy`.
 *
 * \param ctx Context of the LFTL area on whose behalf this read is done
 *   (its transaction is aborted if the read fails, see ::raise_area_error)
 * \param dst Destination address, in volatile memory
 * \param src Source address: an LFTL area, a physical NVM address, or regular memory
 * \param size Size in bytes to read
 */
static void mem_read(lftl_ctx_t*ctx, void*dst, const void*const src, uintptr_t size){
  lftl_nvm_props_t*nvm_props = addr_to_nvm(src);
  if(LFTL_INVALID_POINTER!=nvm_props){
    nvm_read(ctx,nvm_props,dst,src,size);
    return;
  }
  memcpy(dst,src,size);
}


/**
 * \brief Update a CRC32C checksum with a (possibly non-RAM) range.
 *
 * Reads the range in chunks via ::mem_read so that `src` can be an LFTL
 * area, a physical NVM address, or regular memory.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param out Checksum so far, 0xFFFFFFFF to start one
 * \param src Start address of the range to checksum
 * \param size Size in bytes of the range
 * \returns The updated checksum
 */
static uint32_t checksum_update(lftl_ctx_t*ctx, uint32_t out, const void*const src, uintptr_t size){
  const uint8_t*src8 = (const uint8_t*)src;
  uint64_t buf[16];
  while(size){
    const uint32_t readsize = size > sizeof(buf) ? sizeof(buf) : size;
    mem_read(ctx,buf,src8,readsize);
    out = crc32c(out,buf,readsize);
    size -= readsize;
    src8 += readsize;
  }
  return out;
}

/**
 * \brief Compute a full CRC32C checksum over a (possibly non-RAM) range.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param src Start address of the range to checksum
 * \param size Size in bytes of the range
 * \returns The CRC32C checksum of the range
 */
static uint32_t checksum(lftl_ctx_t*ctx, const void*const src, uintptr_t size){
  return checksum_update(ctx,0xFFFFFFFF,src,size);
}

/// How a write unit reads, see ::read_unit_state.
typedef enum {
  UNIT_ERASED, /**< every byte reads 0xFF */
  UNIT_ZERO,   /**< every byte reads 0x00 */
  UNIT_MIXED,  /**< anything else: a value, or a marker a power cut left partially programmed */
} unit_state_t;

/**
 * \brief Classify the content of a write unit already read into memory.
 *
 * \param unit The unit's bytes
 * \param size Size in bytes of the unit
 */
static unit_state_t unit_state_of(const uint8_t*unit, uintptr_t size){
  bool erased = true;
  bool zero = true;
  for(uintptr_t i=0;i<size;i++){
    if(0xFF != unit[i]) erased = false;
    if(0x00 != unit[i]) zero = false;
  }
  return erased ? UNIT_ERASED : (zero ? UNIT_ZERO : UNIT_MIXED);
}

/**
 * \brief Read a write unit once and classify it.
 *
 * One read: a word a power cut left weak can read differently from one read
 * to the next, so every decision about it is made on the same reading.
 *
 * \param ctx Context of the LFTL area
 * \param addr Physical address of the unit
 * \param size Size in bytes of the unit, at most ::LFTL_WU_MAX_SIZE
 */
static unit_state_t read_unit_state(lftl_ctx_t*ctx, const uint8_t*addr, uintptr_t size){
  uint64_t buf[SIZE64(LFTL_WU_MAX_SIZE)];
  nvm_read(ctx,ctx->nvm_props,buf,addr,size);
  return unit_state_of((const uint8_t*)buf,size);
}

/**
 * \brief Program a write unit to zeros.
 *
 * On an erased unit this is an ordinary program. On a unit that is not
 * erased it is the one program a flash declared with `zero_overwrite`
 * accepts (N3 of the flash page): every cell gets a full pulse, so the unit
 * reads zeros for good. The scans use it to pin a decision about a word a
 * power cut left weak, never on a unit that already reads all zeros (the
 * STM32L5 and STM32U5 refuse that program).
 *
 * \param ctx Context of the LFTL area
 * \param addr Physical address of the unit
 * \param size Size in bytes of the unit, at most ::LFTL_WU_MAX_SIZE
 */
static void pin_unit(lftl_ctx_t*ctx, uint8_t*addr, uintptr_t size){
  uint64_t zeros[SIZE64(LFTL_WU_MAX_SIZE)];
  memset(zeros,0,sizeof(zeros));
  nvm_write(ctx,ctx->nvm_props,addr,zeros,size);
}

/**
 * \brief Return the physical erase page size of an area's NVM.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->nvm_props->erase_size`
 */
static uintptr_t page_size(lftl_ctx_t*ctx){
  return ctx->nvm_props->erase_size;
}

/**
 * \brief Total number of physical pages in an area.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->area_size / page_size(ctx)`
 */
static uintptr_t n_pages(lftl_ctx_t*ctx){
  return ctx->area_size / page_size(ctx);
}

typedef struct lftl_meta_struct {
  union{
    uint32_t items[LFTL_META_N_ITEMS];
    struct {
      uint32_t version;
      uint32_t checksum;
      uint32_t checksum2;
    };
  };
} lftl_meta_t;

/**
 * \brief Physical size in bytes of an area's data: `data_size` rounded up to
 * whole write units.
 *
 * Writes cover whole write units, so a slot or record always has room for
 * that much data, even when `data_size` ends in the middle of a write unit.
 *
 * \param ctx Context of the LFTL area
 */
static uintptr_t data_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(ctx->data_size, write_size) * write_size;
}

/**
 * \brief Physical size in bytes of a slot's meta-data block.
 *
 * Each of the ::LFTL_META_N_ITEMS meta items (version, checksum,
 * checksum2) occupies at least `sizeof(uint32_t)` bytes, rounded up to
 * the NVM's write unit size.
 *
 * \param ctx Context of the LFTL area
 * \returns Size in bytes of the meta-data block
 */
static uintptr_t meta_phy_size(lftl_ctx_t*ctx){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  return LFTL_META_N_ITEMS * item_size;
}

/**
 * \brief Number of physical pages occupied by one slot.
 *
 * \param ctx Context of the LFTL area
 * \returns `ceil((ctx->data_size + meta_phy_size(ctx)) / page_size(ctx))`
 */
static uintptr_t n_pages_in_slot(lftl_ctx_t*ctx){
  const uintptr_t min_size = ctx->data_size + meta_phy_size(ctx);
  const uintptr_t n_pages = (min_size + page_size(ctx)-1) / page_size(ctx);
  return n_pages;
}

/**
 * \brief Size in bytes of one slot, including its meta-data.
 *
 * \param ctx Context of the LFTL area
 * \returns `n_pages_in_slot(ctx) * page_size(ctx)`
 */
static uintptr_t slot_size(lftl_ctx_t*ctx){
  return n_pages_in_slot(ctx) * page_size(ctx);
}

/**
 * \brief Number of slots in an area.
 *
 * \param ctx Context of the LFTL area
 * \returns `ctx->area_size / slot_size(ctx)`
 */
static unsigned int n_slots(lftl_ctx_t*ctx){
  const uintptr_t size = slot_size(ctx);
  return ctx->area_size / size;
}

/**
 * \brief Physical address of the start of a slot.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot, in `[0, n_slots(ctx))`
 * \returns Address of `ctx->area + slot_index * slot_size(ctx)`
 */
static uint8_t* slot_base(lftl_ctx_t*ctx, unsigned int slot_index){
  return ((uint8_t*)ctx->area)+slot_index*slot_size(ctx);
}

/**
 * \brief Byte offset of the meta-data block within a slot.
 *
 * \param ctx Context of the LFTL area
 * \returns `slot_size(ctx) - meta_phy_size(ctx)`
 */
static uintptr_t meta_offset(lftl_ctx_t*ctx){
  return slot_size(ctx) - meta_phy_size(ctx);
}

/**
 * \brief Read one meta-data item's whole write unit.
 *
 * The items are read one at a time, so that a word left weakly programmed
 * by a tearing is read only when needed: on flash with ECC each read of it
 * can raise an NMI (THREAT-MODEL.md T1.13).
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param item Index of the item: 0 version, 1 checksum, 2 checksum2
 * \param unit Destination, `max(write_size, sizeof(uint32_t))` bytes, in volatile memory
 */
static void get_slot_item(lftl_ctx_t*ctx, unsigned int slot_index, unsigned int item, uint32_t*unit){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  const uint8_t*const phy = slot_base(ctx, slot_index) + meta_offset(ctx) + item*item_size;
  nvm_read(ctx,ctx->nvm_props,unit,phy,item_size);
}

/**
 * \brief Read the version field of a slot's meta-data.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The slot's stored version number
 */
static uint32_t get_slot_version(lftl_ctx_t*ctx, unsigned int slot_index){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,0,unit);
  return unit[0];
}

/**
 * \brief Read the stored checksum field of a slot's meta-data.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \returns The slot's stored checksum (as last written, not recomputed)
 */
static uint32_t get_slot_checksum(lftl_ctx_t*ctx, unsigned int slot_index){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,1,unit);
  return unit[0];
}

/**
 * \brief Read the checksum2 field of a slot's meta-data, and whether its
 * whole write unit reads all erased or all zeros.
 *
 * The bytes next to the value are programmed to zero with it, so the whole
 * unit tells a checksum2 that was never programmed from one whose program
 * was torn, and one that ::find_current_slot pinned, see there. One read:
 * a word a power cut left weak can read differently from one read to the
 * next, so every decision about it is made on the same reading.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param all_erased Output: true if every byte of the write unit reads 0xFF
 * \param all_zero Output: true if every byte of the write unit reads 0x00
 * \returns The slot's stored checksum2
 */
static uint32_t get_slot_checksum2(lftl_ctx_t*ctx, unsigned int slot_index, bool*all_erased, bool*all_zero){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  uint32_t unit[LFTL_DIV_CEIL(item_size,sizeof(uint32_t))];
  get_slot_item(ctx,slot_index,2,unit);
  const uint8_t*const unit8 = (const uint8_t*)unit;
  *all_erased = true;
  *all_zero = true;
  for(unsigned int i=0;i<item_size;i++){
    if(0xFF != unit8[i]) *all_erased = false;
    if(0x00 != unit8[i]) *all_zero = false;
  }
  return unit[0];
}

/**
 * \brief Program one meta-data item's whole write unit to zeros.
 *
 * The one program a flash declared with `zero_overwrite` accepts on a write
 * unit in any state (N3 of the flash page): every cell gets a full pulse, so
 * the unit reads zeros for good. ::find_current_slot uses it to pin a
 * decision about a word a power cut left weak, and never on a unit that
 * already reads all zeros (the STM32L5 and STM32U5 refuse that program).
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param item Index of the item: 0 version, 1 checksum, 2 checksum2
 */
static void pin_slot_item(lftl_ctx_t*ctx, unsigned int slot_index, unsigned int item){
  const unsigned int item_size = max_uintptr(ctx->nvm_props->write_size,sizeof(uint32_t));
  pin_unit(ctx, slot_base(ctx, slot_index) + meta_offset(ctx) + item*item_size, item_size);
}

/**
 * \brief Check whether a slot's stored checksum matches its data and a version.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot
 * \param version The slot's version, as already read by the caller
 * \returns true if the stored checksum is the checksum of the data plus `version`
 */
static bool slot_checksum_valid(lftl_ctx_t*ctx, unsigned int slot_index, uint32_t version){
  return get_slot_checksum(ctx,slot_index) == checksum(ctx,slot_base(ctx, slot_index),ctx->data_size) + version;
}

/**
 * \brief Write a fully-formed meta-data block to a slot.
 *
 * Writes every meta item except checksum2 first, then checksum2 last, so
 * that a tearing event during this call is always detectable/recoverable
 * afterwards (see ::find_current_slot).
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot to write the meta-data of
 * \param meta The meta-data to write
 */
static void write_meta_core(lftl_ctx_t*ctx, unsigned int slot_index, lftl_meta_t*meta){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int item_size = max_uintptr(write_size,sizeof(uint32_t));
  const uintptr_t meta_size = LFTL_META_N_ITEMS * item_size;
  uint8_t*const base = slot_base(ctx, slot_index);
  lftl_meta_t*meta_phy_addr = (lftl_meta_t*)(base + meta_offset(ctx));
  uint32_t buf[LFTL_DIV_CEIL(meta_size,sizeof(uint32_t))];
  memset(buf,0,sizeof(buf));
  for(unsigned int i = 0; i < LFTL_META_N_ITEMS; i++){
    buf[i*item_size/sizeof(uint32_t)] = meta->items[i];
  }
  //write everything but checksum2
  nvm_write(ctx,ctx->nvm_props,meta_phy_addr,buf,meta_size - item_size);
  //write checksum2
  const uintptr_t checksum2_offset = meta_size - item_size;
  uint32_t*const checksum2_phy_addr = (uint32_t*)(base + meta_offset(ctx) + checksum2_offset);
  uint8_t*const checksum2_src = (uint8_t*)buf + checksum2_offset;
  nvm_write(ctx,ctx->nvm_props,checksum2_phy_addr,checksum2_src,item_size);
}

/**
 * \brief Compute and write a fresh meta-data block for a slot.
 *
 * Computes the checksum from the slot's current data and the given
 * version, then writes it via ::write_meta_core.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot to write the meta-data of
 * \param version The new version number for this slot
 */
static void write_meta(lftl_ctx_t*ctx, unsigned int slot_index, uint32_t version){
  uint8_t*const base = slot_base(ctx, slot_index);
  lftl_meta_t meta;
  meta.version = version;
  meta.checksum = checksum(ctx,base,ctx->data_size) + meta.version;
  meta.checksum2 = meta.checksum;
  write_meta_core(ctx,slot_index,&meta);
}

/**
 * \brief Index of the slot currently holding `ctx->data`.
 *
 * \param ctx Context of the LFTL area
 * \returns The slot index derived from `ctx->data`'s offset within the area
 */
static unsigned int get_current_slot_index(lftl_ctx_t*ctx){
  const uintptr_t offset = (uintptr_t)ctx->data - (uintptr_t)ctx->area;
  return offset / slot_size(ctx);
}

/**
 * \brief Index of the slot to use for the next write.
 *
 * Round-robins to the slot right after the current one, wrapping back to
 * slot 0 once the area's last slot has been used.
 *
 * \param ctx Context of the LFTL area
 * \returns The index of the next slot to write
 */
static unsigned int next_slot(lftl_ctx_t*ctx){
  const uintptr_t area_limit = (uintptr_t)ctx->area+ctx->area_size;
  const uintptr_t next_slot_limit = (uintptr_t)ctx->data + 2*slot_size(ctx); // 1 slot for the current data, 1 slot for the next
  if(next_slot_limit > area_limit ){ // if equal, next slot is the last slot, we will wrap around next time
    return 0; //wrap around
  } else {
    return get_current_slot_index(ctx) + 1;
  }
}

/**
 * \brief Erase all physical pages of a slot.
 *
 * \param ctx Context of the LFTL area
 * \param slot_index Index of the slot to erase
 */
static void erase_slot(lftl_ctx_t*ctx, unsigned int slot_index){
  void*base = slot_base(ctx, slot_index);
  const uint32_t slot_pages = n_pages_in_slot(ctx);
  nvm_erase(ctx,ctx->nvm_props,base,slot_pages);
}

/**
 * \brief Whether a slot other than `except` is valid.
 *
 * Used by ::find_current_slot before it sets a torn slot aside, so that the
 * area is never left without a valid slot by its own doing. One checksum
 * per other slot, in that rare case only.
 *
 * \param ctx Context of the LFTL area
 * \param except Index of the slot to leave out
 * \returns true if another slot's checksum matches its data and version
 */
static bool other_valid_slot(lftl_ctx_t*ctx, unsigned int except){
  const unsigned int ns = n_slots(ctx);
  for(unsigned int i=0;i<ns;i++){
    if(i == except) continue;
    const uint32_t version = get_slot_version(ctx,i);
    if(version == 0xFFFFFFFF) continue;
    if(slot_checksum_valid(ctx,i,version)) return true;
  }
  return false;
}

/**
 * \brief Locate the slot holding the current data and update `ctx->data`.
 *
 * Scans every slot for the highest version number whose checksum matches
 * its data and version, raising ::LFTL_ERROR_VERSION_COLLISION if two such
 * slots share that version, or ::LFTL_ERROR_NO_VALID_VERSION if none
 * passes. A slot whose checksum does not match is skipped whatever its
 * version word: a tearing during its erase leaves the previous content with
 * random bits set, which can read as any version, the current one included.
 *
 * A write programs the data, then version and checksum, then checksum2, so
 * when the winning slot's checksum2 does not match, a tearing left it either
 * all erased (the cut came during the checksum word at the latest: that word
 * may be weakly programmed, and read differently at a later boot) or
 * partially programmed (its own program had started, so the checksum is
 * complete). Only the whole write unit tells the two apart
 * (::get_slot_checksum2), and the scan never erases, so that a power cut at
 * every boot costs no erase (ISSUES.md LFTL-010):
 * - partially programmed: the slot is taken. On a flash with
 *   `zero_overwrite` the unit is pinned to zeros first, so it can never read
 *   all erased at a later boot and bring the previous version back
 *   (LFTL-011); a unit reading all zeros is such a pinned one, or a
 *   checksum that is legitimately 0, and is taken as complete.
 * - all erased, another slot valid: the previous version is current and the
 *   scan runs again without the torn slot. With `zero_overwrite` the torn
 *   slot is tombstoned first, its version item pinned to zeros, so that its
 *   checksum can never verify again; without it the slot is left as it is,
 *   and the next write erases it anyway, as every write erases its target.
 *   The torn slot is never copied, so the previous version is never erased
 *   on the strength of a word that may read differently later (LFTL-009).
 * - all erased, no other slot valid: the slot is taken as it is. Nothing in
 *   normal operation produces that state, and taking it beats having nothing.
 *
 * \param ctx Context of the LFTL area
 */
static void find_current_slot(lftl_ctx_t*ctx){
  const unsigned int ns = n_slots(ctx);
  const uint32_t invalid_index = 0xFFFFFFFF;
  uint32_t skip_index = invalid_index;
  //a second pass at most: it leaves out the slot the first one found torn
  for(unsigned int pass = 0; pass < 2; pass++){
    uint32_t max_version_index = invalid_index;
    uint32_t max_version = 0;
    for(unsigned int i=0;i<ns;i++){
      if(i == skip_index) continue;
      const uint32_t version = get_slot_version(ctx,i);
      if(version == 0xFFFFFFFF) continue;
      const bool found_one = invalid_index != max_version_index;
      if(found_one && (version < max_version)) continue;
      if(!slot_checksum_valid(ctx,i,version)) continue;
      if(found_one && (version == max_version)) {
        raise_error(ctx,LFTL_ERROR_VERSION_COLLISION);
      }
      max_version_index = i;
      max_version = version;
    }
    if(invalid_index == max_version_index) {
      raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
    }
    bool checksum2_erased;
    bool checksum2_zero;
    const uint32_t checksum2 = get_slot_checksum2(ctx, max_version_index, &checksum2_erased, &checksum2_zero);
    const bool complete = checksum2 == get_slot_checksum(ctx, max_version_index);
    if(complete || checksum2_zero){
      //complete, or pinned by an earlier boot
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    if(!checksum2_erased){
      //partially programmed: the checksum is complete, the slot is stable.
      //Pin the word so that it cannot read all erased at a later boot
      if(ctx->nvm_props->zero_overwrite) pin_slot_item(ctx, max_version_index, 2);
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    if(!other_valid_slot(ctx, max_version_index)){
      ctx->data = slot_base(ctx, max_version_index);
      return;
    }
    //all erased, torn during the checksum word at the latest: the previous
    //version is current. Tombstone the torn slot where the flash allows it.
    //ctx->data stays unset until the second pass, so an error here leaves
    //nothing to trust
    if(ctx->nvm_props->zero_overwrite) pin_slot_item(ctx, max_version_index, 0);
    skip_index = max_version_index;
  }
  raise_error(ctx,LFTL_INTERNAL_ERROR);
}

/*
 * EWLF areas. The design, its argument and its model check are in EWLF.md;
 * test/ewlf-model/proto_v4.py is the model this code mirrors, function by
 * function where one exists.
 *
 * area = N data units, then 2 log units, in address order
 *
 * data unit: records from its top down, position 0 at the highest address
 *   record = start marker (zeros) | data | meta (version, checksum)
 * log unit:  pairs from its base up, P regular then R reserved, then X
 *            certificate write units (zeros)
 *   pair   = marker (zeros) | entry (generation, record, version, CRC) |
 *            confirm (zeros)
 *
 * Every word is programmed only after the one before it returned, so a
 * confirm reading all zeros proves its entry solid. Nothing here decides on
 * a reading that a later boot could contradict.
 */

/// Size in bytes of the content of an EWLF record's meta field: a 32-bit
/// version followed by a 64-bit checksum, both little endian.
#define EWLF_META_SIZE 12

/// Size in bytes of the content of a log entry: a 16-bit generation, the
/// data unit and the position of the record it names (one byte each), the
/// record's 32-bit version, then the CRC-64/NVME of those 8 bytes, all
/// little endian.
#define EWLF_ENTRY_SIZE 16

/// Value of `ctx->data` between a scan that found a repair to do and
/// ::ewlf_complete: nothing is shown, the state of the scan is kept.
/// ::LFTL_INVALID_POINTER there means "not scanned since the reset".
#define EWLF_PENDING_POINTER ((void*)-2)

/// Bits of ::lftl_ewlf_log_unit_t::flags.
#define EWLF_UNIT_PROVEN    1 /**< a valid entry's confirm read all zeros */
#define EWLF_UNIT_CLOSED    2 /**< a reserved position is started, or a certificate read all zeros: no regular append */
#define EWLF_UNIT_CERTIFIED 4 /**< a certificate read all zeros: the other unit's erase returned */
#define EWLF_UNIT_CONFIRMED 8 /**< the newest valid entry's confirm read all zeros */

/**
 * \brief Physical size in bytes of every all-zeros word of an EWLF area: a
 * record's start marker, a pair's marker and confirm, a certificate. One
 * write unit.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_flag_phy_size(lftl_ctx_t*ctx){
  return ctx->nvm_props->write_size;
}

/**
 * \brief Physical size in bytes of an EWLF record's meta field:
 * ::EWLF_META_SIZE rounded up to whole write units, programmed in one call.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_meta_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(EWLF_META_SIZE, write_size) * write_size;
}

/**
 * \brief Physical size in bytes of a log entry: ::EWLF_ENTRY_SIZE rounded up
 * to whole write units, programmed in one call.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_entry_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(EWLF_ENTRY_SIZE, write_size) * write_size;
}

/*
 * Large areas (LARGE.md). Their log is the one of EWLF areas, in its
 * record-in-the-pair form (test/ewlf-model/proto_v4p.py): the data of a pair
 * is the map of the chunks of the data, and the chunks live in a pool of
 * erase units, one chunk per unit (test/ewlf-model/proto_c2.py with one slot
 * per unit, the model of the pool rules).
 *
 * area = U pool units, then 2 log units, in address order
 *
 * pool unit: one chunk, the erase unit whole: no marker, no checksum
 * log unit:  pairs from its base up, P regular then R reserved, then X
 *            certificate write units (zeros), as for an EWLF area
 *   pair   = marker (zeros) | map | meta | confirm (zeros)
 *   map    = the pool unit of each chunk (16 bits, 0xFFFF: never written),
 *            the head unit (16 bits), the CRC-64/NVME of the data (64 bits),
 *            little endian, then zeros up to whole write units
 *   meta   = generation (16 bits), zeros (16 bits), version (32 bits), the
 *            CRC-64/NVME of the map then of those 8 bytes (64 bits), little
 *            endian, then zeros up to whole write units
 *
 * Each part of a pair is programmed only after the one before it returned,
 * every chunk an update writes before its pair is started, and a pool unit
 * is erased only when the head enters it.
 */

/// Size in bytes of the content of a large area's meta field, see the layout.
#define LARGE_META_SIZE 16

/// A chunk never written, or no head unit yet.
#define LARGE_NO_UNIT 0xFFFF

/// Bytes a large area streams through RAM at a time: a multiple of any write
/// unit (::LFTL_WU_MAX_SIZE is a power of 2), even, and room for a meta field
/// and a confirm.
#define LARGE_BLOCK (2*LFTL_WU_MAX_SIZE)

/**
 * \brief Number of chunks of a large area: one per erase unit of its data.
 *
 * \param ctx Context of the large area
 */
static unsigned int large_n_chunks(lftl_ctx_t*ctx){
  return LFTL_DIV_CEIL(ctx->data_size, page_size(ctx));
}

/**
 * \brief Number of pool units of a large area: all its erase units but the
 * two log units at its end.
 *
 * \param ctx Context of the large area
 */
static unsigned int large_n_pool(lftl_ctx_t*ctx){
  return n_pages(ctx) - LFTL_EWLF_LOG_UNITS;
}

/**
 * \brief Size in bytes of the content of a map: a unit per chunk, the head
 * unit, the checksum of the data.
 *
 * \param ctx Context of the large area
 */
static uintptr_t large_map_size(lftl_ctx_t*ctx){
  return 2*large_n_chunks(ctx) + 2 + 8;
}

/**
 * \brief Physical size in bytes of a map: ::large_map_size rounded up to whole
 * write units.
 *
 * \param ctx Context of the large area
 */
static uintptr_t large_map_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(large_map_size(ctx), write_size) * write_size;
}

/**
 * \brief Physical size in bytes of a large area's meta field:
 * ::LARGE_META_SIZE rounded up to whole write units.
 *
 * \param ctx Context of the large area
 */
static uintptr_t large_meta_phy_size(lftl_ctx_t*ctx){
  const uint32_t write_size = ctx->nvm_props->write_size;
  return LFTL_DIV_CEIL(LARGE_META_SIZE, write_size) * write_size;
}

/**
 * \brief Physical size in bytes of a log pair: marker, entry, confirm for
 * an EWLF area; marker, map, meta, confirm for a large one.
 *
 * \param ctx Context of the EWLF or large area
 */
static uintptr_t ewlf_pair_phy_size(lftl_ctx_t*ctx){
  if(lftl_is_large(ctx)) return 2*ewlf_flag_phy_size(ctx) + large_map_phy_size(ctx) + large_meta_phy_size(ctx);
  return 2*ewlf_flag_phy_size(ctx) + ewlf_entry_phy_size(ctx);
}

/**
 * \brief Physical size in bytes of one whole EWLF record: start marker,
 * data rounded up to whole write units, meta field.
 *
 * \param ctx Context of the EWLF area
 */
static uintptr_t ewlf_record_phy_size(lftl_ctx_t*ctx){
  return ewlf_flag_phy_size(ctx) + data_phy_size(ctx) + ewlf_meta_phy_size(ctx);
}

/**
 * \brief Number of data units of an EWLF area: all its erase units but the
 * two log units at its end. For a large area, its pool units.
 *
 * \param ctx Context of the EWLF or large area
 */
static unsigned int ewlf_n_data_units(lftl_ctx_t*ctx){
  return n_pages(ctx) - LFTL_EWLF_LOG_UNITS;
}

/**
 * \brief Number of record positions used in a data unit: what fits, 256 at
 * most since an entry names a position on one byte.
 *
 * \param ctx Context of the EWLF area
 */
static unsigned int ewlf_records_per_unit(lftl_ctx_t*ctx){
  const uintptr_t n = page_size(ctx) / ewlf_record_phy_size(ctx);
  return n > 256 ? 256 : n;
}

/**
 * \brief Number of certificate write units of a log unit, `X` of EWLF.md.
 *
 * \param ctx Context of the EWLF area
 */
static unsigned int ewlf_n_certs(lftl_ctx_t*ctx){
  return ctx->ewlf_certificates ? ctx->ewlf_certificates : LFTL_EWLF_CERTIFICATES;
}

/**
 * \brief Number of pair positions of a log unit, regular and reserved:
 * `P` + `R` of EWLF.md. 0 when the certificates leave no room.
 *
 * \param ctx Context of the EWLF area
 */
static unsigned int ewlf_n_positions(lftl_ctx_t*ctx){
  const uintptr_t certs_size = ewlf_n_certs(ctx) * ewlf_flag_phy_size(ctx);
  if(certs_size >= page_size(ctx)) return 0;
  return (page_size(ctx) - certs_size) / ewlf_pair_phy_size(ctx);
}

/**
 * \brief Number of regular pair positions of a log unit, `P` of EWLF.md:
 * the positions before the ::LFTL_EWLF_RESERVED_PAIRS reserved ones.
 *
 * \param ctx Context of the EWLF area
 */
static unsigned int ewlf_n_regular(lftl_ctx_t*ctx){
  const unsigned int n = ewlf_n_positions(ctx);
  return n > LFTL_EWLF_RESERVED_PAIRS ? n - LFTL_EWLF_RESERVED_PAIRS : 0;
}

/**
 * \brief Physical address of a record position.
 *
 * \param ctx Context of the EWLF area
 * \param unit Index of the data unit, in `[0, ewlf_n_data_units(ctx))`
 * \param pos Position in the unit, 0 at its top
 */
static uint8_t* ewlf_record_addr(lftl_ctx_t*ctx, unsigned int unit, unsigned int pos){
  return ((uint8_t*)ctx->area) + (unit+1)*page_size(ctx) - (pos+1)*ewlf_record_phy_size(ctx);
}

/**
 * \brief Physical address of a record's data.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the record
 */
static uint8_t* ewlf_record_data(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uint8_t*)record_addr + ewlf_flag_phy_size(ctx);
}

/**
 * \brief Physical address of a record's meta field.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the record
 */
static uint8_t* ewlf_record_meta(lftl_ctx_t*ctx, const uint8_t*record_addr){
  return (uint8_t*)record_addr + ewlf_flag_phy_size(ctx) + data_phy_size(ctx);
}

/**
 * \brief Physical address of a log unit.
 *
 * \param ctx Context of the EWLF area
 * \param l Index of the log unit, 0 or 1: generation g lives in unit g mod 2
 */
static uint8_t* ewlf_log_base(lftl_ctx_t*ctx, unsigned int l){
  return ((uint8_t*)ctx->area) + (ewlf_n_data_units(ctx) + l)*page_size(ctx);
}

/**
 * \brief Physical address of a pair position: its marker.
 *
 * \param ctx Context of the EWLF area
 * \param l Index of the log unit
 * \param q Position, regular ones first
 */
static uint8_t* ewlf_pair_addr(lftl_ctx_t*ctx, unsigned int l, unsigned int q){
  return ewlf_log_base(ctx,l) + q*ewlf_pair_phy_size(ctx);
}

/**
 * \brief Physical address of a certificate write unit.
 *
 * \param ctx Context of the EWLF area
 * \param l Index of the log unit
 * \param i Index of the certificate
 */
static uint8_t* ewlf_cert_addr(lftl_ctx_t*ctx, unsigned int l, unsigned int i){
  return ewlf_log_base(ctx,l) + ewlf_n_positions(ctx)*ewlf_pair_phy_size(ctx) + i*ewlf_flag_phy_size(ctx);
}

/// The content of an EWLF record's meta field, in volatile memory.
typedef struct {
  uint32_t version;  /**< format writes 1, each update the current version plus one */
  uint64_t checksum; /**< CRC-64/NVME of the version then the data, see ::ewlf_checksum_start */
} ewlf_meta_t;

/**
 * \brief Start the checksum of an EWLF record: a CRC-64/NVME over the four
 * bytes of its version, little endian, then its data
 * (::ewlf_checksum_update), then ::ewlf_checksum_end.
 *
 * \param version The record's version
 * \returns The checksum so far
 */
static uint64_t ewlf_checksum_start(uint32_t version){
  uint8_t bytes[sizeof(version)];
  for(unsigned int i=0;i<sizeof(version);i++) bytes[i] = (uint8_t)(version >> (8*i));
  return crc64_update(~(uint64_t)0,bytes,sizeof(bytes));
}

/**
 * \brief Update the checksum of an EWLF record with a (possibly non-RAM) range.
 *
 * \param ctx Context of the LFTL area on whose behalf this checksum is computed
 * \param crc Checksum so far
 * \param src Start address of the range
 * \param size Size in bytes of the range
 * \returns The updated checksum
 */
static uint64_t ewlf_checksum_update(lftl_ctx_t*ctx, uint64_t crc, const void*const src, uintptr_t size){
  const uint8_t*src8 = (const uint8_t*)src;
  uint64_t buf[16];
  while(size){
    const uint32_t readsize = size > sizeof(buf) ? sizeof(buf) : size;
    mem_read(ctx,buf,src8,readsize);
    crc = crc64_update(crc,buf,readsize);
    size -= readsize;
    src8 += readsize;
  }
  return crc;
}

/**
 * \brief Update the checksum of an EWLF record with erased (0xFF) bytes.
 *
 * \param crc Checksum so far
 * \param size Number of erased bytes
 * \returns The updated checksum
 */
static uint64_t ewlf_checksum_update_erased(uint64_t crc, uintptr_t size){
  uint8_t erased[16];
  memset(erased,0xFF,sizeof(erased));
  while(size){
    const uintptr_t chunk = size > sizeof(erased) ? sizeof(erased) : size;
    crc = crc64_update(crc,erased,chunk);
    size -= chunk;
  }
  return crc;
}

/**
 * \brief Finish the checksum of an EWLF record.
 *
 * \param crc Checksum so far
 * \returns The value stored in the record's meta field
 */
static uint64_t ewlf_checksum_end(uint64_t crc){
  return ~crc;
}

/**
 * \brief Encode a meta field: version then checksum, little endian.
 *
 * \param meta The content
 * \param bytes Destination, ::EWLF_META_SIZE bytes at least
 */
static void ewlf_meta_to_bytes(const ewlf_meta_t*meta, uint8_t*bytes){
  for(unsigned int i=0;i<sizeof(meta->version);i++) bytes[i] = (uint8_t)(meta->version >> (8*i));
  for(unsigned int i=0;i<sizeof(meta->checksum);i++) bytes[sizeof(meta->version)+i] = (uint8_t)(meta->checksum >> (8*i));
}

/**
 * \brief Decode a meta field, see ::ewlf_meta_to_bytes.
 *
 * \param meta Destination
 * \param bytes The field as read, ::EWLF_META_SIZE bytes at least
 */
static void ewlf_meta_from_bytes(ewlf_meta_t*meta, const uint8_t*bytes){
  meta->version = 0;
  for(unsigned int i=0;i<sizeof(meta->version);i++) meta->version |= ((uint32_t)bytes[i]) << (8*i);
  meta->checksum = 0;
  for(unsigned int i=0;i<sizeof(meta->checksum);i++) meta->checksum |= ((uint64_t)bytes[sizeof(meta->version)+i]) << (8*i);
}

/**
 * \brief Program an EWLF record's meta field, in one call.
 *
 * \param ctx Context of the EWLF area
 * \param record_addr Physical address of the record
 * \param meta The content
 */
static void ewlf_write_meta(lftl_ctx_t*ctx, uint8_t*record_addr, const ewlf_meta_t*meta){
  uint64_t buf[SIZE64(LFTL_WU_MAX_SIZE)];
  memset(buf,0,sizeof(buf));
  ewlf_meta_to_bytes(meta,(uint8_t*)buf);
  nvm_write(ctx,ctx->nvm_props,ewlf_record_meta(ctx,record_addr),buf,ewlf_meta_phy_size(ctx));
}

/// What the map of a new pair of a large area holds, see ::large_program_map.
typedef struct large_map_src_struct {
  bool copy;            /**< the decision's map as it is: a bury, a carry, a reserved pair */
  bool erased;          /**< every chunk never written: a format, an erase of all the data */
  uint16_t run_first;   /**< first chunk of the run a plain write gives new units */
  uint16_t run_n;       /**< number of chunks of that run, 0 for none */
  uint16_t run_head;    /**< the head the units of the run were taken after */
  const uint8_t*slots;  /**< a transaction: the unit of each chunk, ::LARGE_NO_UNIT for none; NULL otherwise */
  uint16_t head;        /**< the head unit of the new map */
  uint64_t data_crc;    /**< the checksum of the data of the new map */
} large_map_src_t;

/// The content of a log entry, in volatile memory: which record is current,
/// or, for a large area, which map.
typedef struct {
  uint16_t gen;     /**< generation of the log unit holding the entry */
  uint8_t unit;     /**< EWLF: data unit of the record */
  uint8_t pos;      /**< EWLF: position of the record in its unit */
  uint32_t version; /**< version of the record or the map */
  const large_map_src_t*map; /**< large: how the map of a new pair is made; unused when read */
} ewlf_entry_t;

/**
 * \brief Encode a log entry: generation, unit, position, version, then the
 * CRC-64/NVME of those 8 bytes, little endian.
 *
 * \param e The content
 * \param bytes Destination, ::EWLF_ENTRY_SIZE bytes
 */
static void ewlf_entry_to_bytes(const ewlf_entry_t*e, uint8_t*bytes){
  bytes[0] = (uint8_t)e->gen;
  bytes[1] = (uint8_t)(e->gen >> 8);
  bytes[2] = e->unit;
  bytes[3] = e->pos;
  for(unsigned int i=0;i<4;i++) bytes[4+i] = (uint8_t)(e->version >> (8*i));
  const uint64_t crc = ~crc64_update(~(uint64_t)0,bytes,8);
  for(unsigned int i=0;i<8;i++) bytes[8+i] = (uint8_t)(crc >> (8*i));
}

/**
 * \brief Decode a log entry, see ::ewlf_entry_to_bytes.
 *
 * \param e Destination
 * \param bytes The entry as read, ::EWLF_ENTRY_SIZE bytes
 * \returns Whether its CRC verifies
 */
static bool ewlf_entry_from_bytes(ewlf_entry_t*e, const uint8_t*bytes){
  uint64_t crc = 0;
  for(unsigned int i=0;i<8;i++) crc |= ((uint64_t)bytes[8+i]) << (8*i);
  if(crc != ~crc64_update(~(uint64_t)0,bytes,8)) return false;
  e->gen = (uint16_t)(bytes[0] | (((uint16_t)bytes[1]) << 8));
  e->unit = bytes[2];
  e->pos = bytes[3];
  e->version = 0;
  for(unsigned int i=0;i<4;i++) e->version |= ((uint32_t)bytes[4+i]) << (8*i);
  return true;
}

/**
 * \brief Program whole write units, telling a refused program apart.
 *
 * A write unit that a power cut left weak can read erased and still be
 * refused by the flash: the protocol then steps over it. Any other failure
 * is fatal, as everywhere.
 *
 * \param ctx Context of the EWLF area
 * \param addr Physical destination
 * \param src Source, in volatile memory
 * \param size Size in bytes, whole write units
 * \returns false when the flash refused the program and programmed nothing
 */
static bool ewlf_try_write(lftl_ctx_t*ctx, uint8_t*addr, const void*src, uintptr_t size){
  const uint8_t status = ctx->nvm_props->write(addr, src, size);
  if(LFTL_NVM_STATUS_REFUSED == status) return false;
  if(status) raise_area_error(ctx,ctx->nvm_props,LFTL_ERROR_LOW_LEVEL_WRITE | status);
  return true;
}

/**
 * \brief Program one all-zeros word: a marker, a confirm or a certificate.
 *
 * \param ctx Context of the EWLF area
 * \param addr Physical address of the word
 * \returns false when the flash refused the program
 */
static bool ewlf_try_zeros(lftl_ctx_t*ctx, uint8_t*addr){
  uint64_t zeros[SIZE64(LFTL_WU_MAX_SIZE)];
  memset(zeros,0,sizeof(zeros));
  return ewlf_try_write(ctx,addr,zeros,ewlf_flag_phy_size(ctx));
}

/**
 * \brief Decode 16 bits, little endian.
 *
 * \param b The bytes
 */
static uint16_t large_get16(const uint8_t*b){
  return (uint16_t)(b[0] | (((uint16_t)b[1]) << 8));
}

/**
 * \brief Encode 16 bits, little endian.
 *
 * \param b Destination
 * \param v The value
 */
static void large_put16(uint8_t*b, uint16_t v){
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
}

/**
 * \brief Physical address of a pool unit of a large area, the chunk it holds.
 *
 * \param ctx Context of the large area
 * \param u Index of the pool unit
 */
static uint8_t*large_unit_addr(lftl_ctx_t*ctx, unsigned int u){
  return ((uint8_t*)ctx->area) + u*page_size(ctx);
}

/**
 * \brief Physical address of the map of the decision: the data of the pair
 * the scan chose, or of the last pair this session programmed.
 *
 * \param ctx Context of the large area
 */
static const uint8_t*large_map(lftl_ctx_t*ctx){
  return ewlf_pair_addr(ctx,ctx->ewlf.d_unit,ctx->ewlf.d_q) + ewlf_flag_phy_size(ctx);
}

/**
 * \brief One 16-bit value of the decision's map: the unit of chunk `i`, or the
 * head unit for `i` = the number of chunks.
 *
 * The map of a pair whose meta field reads valid is solid: every word of it
 * returned before the meta field was started.
 *
 * The scan checked every value against the pool. One that reads outside it
 * now changed since, behind the library: a write unit the read callback
 * filled after a fault (R7), or a program by someone else. It names no unit
 * of the area, and raises ::LFTL_ERROR_NO_VALID_VERSION rather than letting
 * a read or a copy go that many units past the area.
 *
 * \param ctx Context of the large area
 * \param i Index of the value
 * \returns A pool unit, or ::LARGE_NO_UNIT
 */
static uint16_t large_map_entry(lftl_ctx_t*ctx, unsigned int i){
  uint8_t b[2];
  nvm_read(ctx,ctx->nvm_props,b,large_map(ctx)+2*i,2);
  const uint16_t u = large_get16(b);
  if((LARGE_NO_UNIT != u) && (u >= large_n_pool(ctx))) raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
  return u;
}

/**
 * \brief The checksum of the data the decision's map holds.
 *
 * \param ctx Context of the large area
 */
static uint64_t large_map_data_crc(lftl_ctx_t*ctx){
  uint8_t b[8];
  nvm_read(ctx,ctx->nvm_props,b,large_map(ctx)+2*large_n_chunks(ctx)+2,8);
  uint64_t crc = 0;
  for(unsigned int i=0;i<8;i++) crc |= ((uint64_t)b[i]) << (8*i);
  return crc;
}

/**
 * \brief The units of the open transaction of a large area, one per chunk,
 * ::LARGE_NO_UNIT for a chunk it did not touch: after the bits of the
 * tracker, see ::LFTL_TRANSACTION_TRACKER_SIZE_LARGE. NULL when no
 * transaction is open.
 *
 * \param ctx Context of the large area
 */
static uint8_t*large_txn_units(lftl_ctx_t*ctx){
  if(LFTL_INVALID_POINTER == ctx->transaction_tracker) return 0;
  return ((uint8_t*)ctx->transaction_tracker) + LFTL_TRANSACTION_TRACKER_SIZE_LL(ctx->data_size,ctx->nvm_props->write_size);
}

/**
 * \brief Whether a pool unit is held: it holds a chunk of the decision's map,
 * or of the open transaction (`live` and `txn` of the model).
 *
 * \param ctx Context of the large area
 * \param u Index of the pool unit
 */
static bool large_unit_held(lftl_ctx_t*ctx, unsigned int u){
  const unsigned int n = large_n_chunks(ctx);
  const uint8_t*const map = large_map(ctx);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  for(unsigned int i=0;i<n;){
    const unsigned int k = (n - i) > LARGE_BLOCK/2 ? LARGE_BLOCK/2 : (n - i);
    nvm_read(ctx,ctx->nvm_props,buf,map+2*i,2*k);
    for(unsigned int j=0;j<k;j++) if(large_get16(buf+2*j) == u) return true;
    i += k;
  }
  const uint8_t*const units = large_txn_units(ctx);
  if(units){
    for(unsigned int i=0;i<n;i++) if(large_get16(units+2*i) == u) return true;
  }
  return false;
}

/**
 * \brief The unit the head enters after `from` (`survey` and `alloc` of the
 * model, with one slot per unit): the first free unit after it in ring
 * order, `from` itself last. Free: held by neither the decision's map nor
 * the open transaction. The units an operation took are behind the search,
 * which never goes a whole round: the operation counted its room first.
 *
 * \param ctx Context of the large area
 * \param from The head, ::LARGE_NO_UNIT before the first unit: the search
 *   then starts at unit 0
 * \returns The unit, ::LARGE_NO_UNIT when none is free
 */
static uint16_t large_next_free(lftl_ctx_t*ctx, uint16_t from){
  const unsigned int n_pool = large_n_pool(ctx);
  for(unsigned int step=0;step<n_pool;step++){
    const unsigned int u = (LARGE_NO_UNIT == from) ? step : (from + 1 + step) % n_pool;
    if(!large_unit_held(ctx,u)) return (uint16_t)u;
  }
  return LARGE_NO_UNIT;
}

/**
 * \brief Number of free pool units: all of them but those of the decision's
 * map and of the open transaction, which are distinct.
 *
 * \param ctx Context of the large area
 */
static unsigned int large_count_free(lftl_ctx_t*ctx){
  const unsigned int n = large_n_chunks(ctx);
  const uint8_t*const map = large_map(ctx);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  unsigned int used = 0;
  for(unsigned int i=0;i<n;){
    const unsigned int k = (n - i) > LARGE_BLOCK/2 ? LARGE_BLOCK/2 : (n - i);
    nvm_read(ctx,ctx->nvm_props,buf,map+2*i,2*k);
    for(unsigned int j=0;j<k;j++) if(LARGE_NO_UNIT != large_get16(buf+2*j)) used++;
    i += k;
  }
  const uint8_t*const units = large_txn_units(ctx);
  if(units){
    for(unsigned int i=0;i<n;i++) if(LARGE_NO_UNIT != large_get16(units+2*i)) used++;
  }
  const unsigned int n_pool = large_n_pool(ctx);
  return used >= n_pool ? 0 : n_pool - used;
}

/**
 * \brief Program the map of a new pair, block by block, each program after
 * the previous one returned, and accumulate its CRC.
 *
 * The values come from `src`: copied from the decision's map, or all
 * ::LARGE_NO_UNIT, or the decision's map with the units of a transaction or
 * of a plain write's run in place of the old ones. The units of a run are
 * found again as they were taken: the free units after `run_head`, in ring
 * order; the decision's map and the transaction, which say which units are
 * free, are the same as then. So the map takes no RAM that grows with the
 * data, and a pair that has to be tried again at the next position makes
 * the same map.
 *
 * \param ctx Context of the large area
 * \param dst Physical address of the map in the pair
 * \param src What the map holds
 * \param crc In/out: the CRC so far
 * \returns false when the flash refused a program
 */
static bool large_program_map(lftl_ctx_t*ctx, uint8_t*dst, const large_map_src_t*src, uint64_t*crc){
  const unsigned int n = large_n_chunks(ctx);
  const uintptr_t map_size = large_map_size(ctx);
  const uintptr_t phy_size = large_map_phy_size(ctx);
  const uint8_t*const old = large_map(ctx);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  uint16_t run_unit = src->run_head;
  for(uintptr_t off=0;off<phy_size;off+=LARGE_BLOCK){
    const uintptr_t len = (phy_size - off) > LARGE_BLOCK ? LARGE_BLOCK : (phy_size - off);
    const uintptr_t valued = (off >= map_size) ? 0 : (((map_size - off) > len) ? len : (map_size - off));
    memset(buf,0,len);
    if(!src->erased) nvm_read(ctx,ctx->nvm_props,buf,old+off,valued);
    if(!src->copy){
      //every value is 16 bits at an even offset, and a block is even
      for(uintptr_t b=0;b<valued;b+=2){
        const unsigned int h = (off + b) / 2;
        uint16_t v;
        if(h < n){
          if(src->erased){
            v = LARGE_NO_UNIT;
          } else if(src->slots && (LARGE_NO_UNIT != large_get16(src->slots+2*h))){
            v = large_get16(src->slots+2*h);
          } else if((h >= src->run_first) && (h - src->run_first < src->run_n)){
            run_unit = large_next_free(ctx,run_unit);
            v = run_unit;
          } else {
            continue;
          }
        } else if(h == n){
          v = src->head;
        } else {
          v = (uint16_t)(src->data_crc >> (16*(h - n - 1)));
        }
        large_put16(buf+b,v);
      }
    }
    *crc = crc64_update(*crc,buf,valued);
    if(!ewlf_try_write(ctx,dst+off,buf,len)) return false;
  }
  return true;
}

/**
 * \brief Program the map and the meta field of a pair of a large area, the
 * data of the pair: the map, then the meta field once it returned.
 *
 * \param ctx Context of the large area
 * \param pair Physical address of the pair, its marker programmed
 * \param gen Generation of the entry
 * \param entry Version and map of the entry
 * \returns false when the flash refused a program
 */
static bool large_try_payload(lftl_ctx_t*ctx, uint8_t*pair, uint16_t gen, const ewlf_entry_t*entry){
  uint8_t*const map = pair + ewlf_flag_phy_size(ctx);
  uint64_t crc = ~(uint64_t)0;
  if(!large_program_map(ctx,map,entry->map,&crc)) return false;
  uint64_t buf64[SIZE64(LFTL_WU_MAX_SIZE)];
  uint8_t*const buf = (uint8_t*)buf64;
  memset(buf,0,sizeof(buf64));
  large_put16(buf,gen);
  for(unsigned int i=0;i<4;i++) buf[4+i] = (uint8_t)(entry->version >> (8*i));
  crc = ~crc64_update(crc,buf,8);
  for(unsigned int i=0;i<8;i++) buf[8+i] = (uint8_t)(crc >> (8*i));
  return ewlf_try_write(ctx,map+large_map_phy_size(ctx),buf,large_meta_phy_size(ctx));
}

/**
 * \brief Read a pair of a large area once and decode it (`Unit` of
 * proto_v4p.py, for one position).
 *
 * Its entry is valid when the CRC of its meta field verifies, over the map
 * and the meta field, and every unit the map names exists. The generation's
 * parity is checked by the caller.
 *
 * \param ctx Context of the large area
 * \param l Index of the log unit
 * \param q Position
 * \param e Output: generation and version, when valid
 * \param confirmed Output: the confirm reads all zeros
 * \returns -1 when every byte of the pair reads erased, 1 when its entry is
 *   valid, 0 otherwise
 */
static int large_read_pair(lftl_ctx_t*ctx, unsigned int l, unsigned int q, ewlf_entry_t*e, bool*confirmed){
  const uintptr_t flag_size = ewlf_flag_phy_size(ctx);
  const uintptr_t map_size = large_map_size(ctx);
  const uintptr_t map_phy = large_map_phy_size(ctx);
  const uintptr_t meta_phy = large_meta_phy_size(ctx);
  const unsigned int n = large_n_chunks(ctx);
  const unsigned int n_pool = large_n_pool(ctx);
  const uint8_t*const pair = ewlf_pair_addr(ctx,l,q);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  bool in_range = true;
  uint64_t crc = ~(uint64_t)0;
  //the bytes of the map read so far, all erased, that the CRC has not taken
  //yet: a position that reads erased all along, the common case, costs no CRC
  uintptr_t erased_pending = 0;
  bool map_erased = true;
  nvm_read(ctx,ctx->nvm_props,buf,pair,flag_size);
  bool erased = UNIT_ERASED == unit_state_of(buf,flag_size);
  for(uintptr_t off=0;off<map_phy;off+=LARGE_BLOCK){
    const uintptr_t len = (map_phy - off) > LARGE_BLOCK ? LARGE_BLOCK : (map_phy - off);
    const uintptr_t valued = (off >= map_size) ? 0 : (((map_size - off) > len) ? len : (map_size - off));
    nvm_read(ctx,ctx->nvm_props,buf,pair+flag_size+off,len);
    if(UNIT_ERASED != unit_state_of(buf,len)){
      erased = false;
      map_erased = false;
    }
    if(map_erased){
      erased_pending += valued;
    } else {
      crc = ewlf_checksum_update_erased(crc,erased_pending);
      erased_pending = 0;
      crc = crc64_update(crc,buf,valued);
    }
    for(uintptr_t b=0;b<valued;b+=2){
      if((off + b)/2 > n) break; //the units and the head; then the checksum of the data
      const uint16_t v = large_get16(buf+b);
      if((LARGE_NO_UNIT != v) && (v >= n_pool)) in_range = false;
    }
  }
  nvm_read(ctx,ctx->nvm_props,buf,pair+flag_size+map_phy,meta_phy+flag_size);
  if(UNIT_ERASED != unit_state_of(buf,meta_phy+flag_size)) erased = false;
  if(erased) return -1;
  *confirmed = UNIT_ZERO == unit_state_of(buf+meta_phy,flag_size);
  crc = ewlf_checksum_update_erased(crc,erased_pending);
  crc = ~crc64_update(crc,buf,8);
  uint64_t stored = 0;
  for(unsigned int i=0;i<8;i++) stored |= ((uint64_t)buf[8+i]) << (8*i);
  if((stored != crc) || !in_range) return 0;
  e->gen = large_get16(buf);
  e->unit = 0;
  e->pos = 0;
  e->version = 0;
  for(unsigned int i=0;i<4;i++) e->version |= ((uint32_t)buf[4+i]) << (8*i);
  e->map = 0;
  return 1;
}

/**
 * \brief Whether generation `a` is newer than generation `b`.
 *
 * The two log units always hold adjacent generations, so 16 bits compared
 * as successors never wrap into a wrong order.
 */
static bool ewlf_gen_newer(uint16_t a, uint16_t b){
  return ((int16_t)(uint16_t)(a - b)) > 0;
}

/**
 * \brief Read a log unit, every word once, and derive what the protocol
 * needs from that reading (`Unit` of the model).
 *
 * A position is started when any of its bytes reads programmed. Its entry
 * is valid when its CRC verifies, its generation has the parity of the unit
 * and it names a record position that exists, or, for a large area, pool
 * units that exist (::large_read_pair). The newest valid entry is the
 * one at the highest position. Confirms and certificates count only when
 * they read all zeros: a few flipped bits in an erased word prove nothing.
 *
 * \param ctx Context of the EWLF area
 * \param l Index of the log unit
 * \param u Output: the reading
 * \param newest Output: the newest valid entry, when `u->newest` is not -1
 */
static void ewlf_read_log_unit(lftl_ctx_t*ctx, unsigned int l, lftl_ewlf_log_unit_t*u, ewlf_entry_t*newest){
  const uintptr_t flag_size = ewlf_flag_phy_size(ctx);
  const uintptr_t entry_size = ewlf_entry_phy_size(ctx);
  const uintptr_t pair_size = ewlf_pair_phy_size(ctx);
  const unsigned int n_positions = ewlf_n_positions(ctx);
  const unsigned int n_regular = ewlf_n_regular(ctx);
  const unsigned int n_data_units = ewlf_n_data_units(ctx);
  const unsigned int records_per_unit = ewlf_records_per_unit(ctx);
  uint64_t buf[SIZE64(3*LFTL_WU_MAX_SIZE)];
  const uint8_t*const bytes = (const uint8_t*)buf;
  u->started = -1;
  u->newest = -1;
  u->gen = 0;
  u->certs_free = 0;
  u->flags = 0;
  const bool large = lftl_is_large(ctx);
  for(unsigned int q=0;q<n_positions;q++){
    ewlf_entry_t e;
    bool confirmed;
    if(large){
      const int r = large_read_pair(ctx,l,q,&e,&confirmed);
      if(r < 0) continue;
      u->started = q;
      if(q >= n_regular) u->flags |= EWLF_UNIT_CLOSED;
      if(0 == r) continue;
    } else {
      nvm_read(ctx,ctx->nvm_props,buf,ewlf_pair_addr(ctx,l,q),pair_size);
      if(UNIT_ERASED == unit_state_of(bytes,pair_size)) continue;
      u->started = q;
      if(q >= n_regular) u->flags |= EWLF_UNIT_CLOSED;
      if(!ewlf_entry_from_bytes(&e,bytes+flag_size)) continue;
      if((e.unit >= n_data_units) || (e.pos >= records_per_unit)) continue;
      e.map = 0;
      confirmed = UNIT_ZERO == unit_state_of(bytes+flag_size+entry_size,flag_size);
    }
    if((e.gen & 1) != l) continue;
    if((u->newest < 0) || ewlf_gen_newer(e.gen,u->gen)) u->gen = e.gen;
    u->newest = q;
    *newest = e;
    if(confirmed){
      u->flags |= EWLF_UNIT_PROVEN | EWLF_UNIT_CONFIRMED;
    } else {
      u->flags &= ~EWLF_UNIT_CONFIRMED;
    }
  }
  const unsigned int n_certs = ewlf_n_certs(ctx);
  const unsigned int chunk_max = sizeof(buf) / flag_size;
  for(unsigned int i=0;i<n_certs;){
    const unsigned int chunk = (n_certs - i) > chunk_max ? chunk_max : (n_certs - i);
    nvm_read(ctx,ctx->nvm_props,buf,ewlf_cert_addr(ctx,l,i),chunk*flag_size);
    for(unsigned int j=0;j<chunk;j++){
      const unit_state_t state = unit_state_of(bytes + j*flag_size,flag_size);
      if(UNIT_ZERO == state) u->flags |= EWLF_UNIT_CERTIFIED | EWLF_UNIT_CLOSED;
      if(UNIT_ERASED == state) u->certs_free++;
    }
    i += chunk;
  }
}

/**
 * \brief Whether the other log unit may hold a carry attempt (`uncertain`
 * of the model): the live unit is certified, the other has a started
 * position and no valid entry.
 *
 * Without a certificate of the live unit nothing entered the other one
 * since its erase: what it reads is a dead life or a ghost, not consulted.
 *
 * \param st State of the EWLF area
 */
static bool ewlf_uncertain(const lftl_ewlf_state_t*st){
  const lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  const lftl_ewlf_log_unit_t*const O = &st->unit[1 - st->live];
  return (O->started >= 0) && (O->newest < 0) && (0 != (L->flags & EWLF_UNIT_CERTIFIED));
}

/**
 * \brief Whether the decision can be shown without writing (`top_solid` of
 * the model): it is proven, nothing is started above it in the live unit,
 * and the other unit is not uncertain.
 *
 * \param st State of the EWLF area
 */
static bool ewlf_top_solid(const lftl_ewlf_state_t*st){
  const lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  return (0 != (L->flags & EWLF_UNIT_CONFIRMED)) && (L->started == L->newest) && !ewlf_uncertain(st);
}

/**
 * \brief The repair the state calls for, and whether it has room: what
 * ::ewlf_bury will do, decided without programming.
 *
 * Sets `st->pending` and `st->stuck`.
 *
 * \param ctx Context of the EWLF area
 */
static void ewlf_plan_repair(lftl_ctx_t*ctx){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  const lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  const lftl_ewlf_log_unit_t*const O = &st->unit[1 - st->live];
  const int n_positions = ewlf_n_positions(ctx);
  const int n_regular = ewlf_n_regular(ctx);
  st->stuck = 0;
  if(ewlf_top_solid(st)){
    st->pending = LFTL_REPAIR_NONE;
    return;
  }
  if(ewlf_uncertain(st)){
    st->pending = LFTL_REPAIR_CARRY;
    st->stuck = (O->started + 1 >= n_positions);
    return;
  }
  const int first_reserved = (L->started + 1 > n_regular) ? (L->started + 1) : n_regular;
  if((0 == (L->flags & EWLF_UNIT_CLOSED)) && (L->started + 1 < n_regular)){
    st->pending = LFTL_REPAIR_BURY;
    return;
  }
  if(first_reserved < n_positions){
    st->pending = LFTL_REPAIR_BURY;
    return;
  }
  st->pending = LFTL_REPAIR_SWITCH;
  const bool certified = 0 != (L->flags & EWLF_UNIT_CERTIFIED);
  if(!certified && (0 == (L->flags & EWLF_UNIT_PROVEN))){
    st->stuck = 1; //step 1 needs a reserved pair and none is left
  } else if(!certified || (O->started < 0)){
    st->stuck = (0 == L->certs_free);
  } else {
    st->stuck = (O->started + 1 >= n_positions);
  }
}

/**
 * \brief The smallest number of further power cuts, each at the worst
 * instant, after which the area is stuck ("Remaining cuts" of EWLF.md).
 *
 * The minimum of the reserves a cut can burn from this state: the free
 * certificates of the live unit when its next switch needs one, the free
 * positions of the other unit when the live one is certified, and the free
 * positions of the live unit when it holds no proven entry.
 *
 * \param ctx Context of the EWLF area
 */
static uint16_t ewlf_remaining_cuts(lftl_ctx_t*ctx){
  const lftl_ewlf_state_t*const st = &ctx->ewlf;
  const lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  const lftl_ewlf_log_unit_t*const O = &st->unit[1 - st->live];
  const int n_positions = ewlf_n_positions(ctx);
  const int n_regular = ewlf_n_regular(ctx);
  if(st->stuck) return 0;
  const bool certified = 0 != (L->flags & EWLF_UNIT_CERTIFIED);
  uint32_t out = 0xFFFF;
  if(!certified || (O->started < 0)){
    if(L->certs_free < out) out = L->certs_free;
  }
  if(certified){
    const uint32_t free_positions = n_positions - 1 - O->started;
    if(free_positions < out) out = free_positions;
  }
  if(0 == (L->flags & EWLF_UNIT_PROVEN)){
    uint32_t free_positions = 0;
    if((0 == (L->flags & EWLF_UNIT_CLOSED)) && (L->started + 1 < n_regular)) free_positions += n_regular - (L->started + 1);
    const int first_reserved = (L->started + 1 > n_regular) ? (L->started + 1) : n_regular;
    if(first_reserved < n_positions) free_positions += n_positions - first_reserved;
    if(free_positions < out) out = free_positions;
  }
  return (uint16_t)out;
}

/**
 * \brief Phase 1 of the scan of an EWLF area: read only.
 *
 * Reads both log units once, finds the live unit (the one of the higher
 * generation among those holding a valid entry) and the decision (its
 * newest valid entry), checks that the record the decision names is in
 * place, and derives the pending repair. Programs and erases nothing.
 *
 * On return `ctx->data` is the current record's data when the decision can
 * be shown as it is, ::EWLF_PENDING_POINTER when a repair is pending. It is
 * left at ::LFTL_INVALID_POINTER when ::LFTL_ERROR_NO_VALID_VERSION is
 * raised: no valid entry anywhere, or a decision that names no record (a
 * cut first format, foreign content).
 *
 * \param ctx Context of the EWLF area
 */
static void ewlf_scan(lftl_ctx_t*ctx){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  ewlf_entry_t newest[2];
  memset(st,0,sizeof(*st));
  for(unsigned int l=0;l<2;l++) ewlf_read_log_unit(ctx,l,&st->unit[l],&newest[l]);
  const bool valid0 = st->unit[0].newest >= 0;
  const bool valid1 = st->unit[1].newest >= 0;
  if(!valid0 && !valid1) raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
  if(valid0 && valid1){
    st->live = ewlf_gen_newer(st->unit[1].gen,st->unit[0].gen) ? 1 : 0;
  } else {
    st->live = valid1 ? 1 : 0;
  }
  const ewlf_entry_t*const d = &newest[st->live];
  if(lftl_is_large(ctx)){
    //the map was checked with the pair: what a large area needs is in the log
    st->d_unit = st->live;
    st->d_q = st->unit[st->live].newest;
    st->version = d->version;
    st->rec_started = -2;
    st->head = large_map_entry(ctx,large_n_chunks(ctx));
    ewlf_plan_repair(ctx);
    ctx->data = (LFTL_REPAIR_NONE == st->pending) ? (void*)large_map(ctx) : EWLF_PENDING_POINTER;
    return;
  }
  //the record the decision names: always in place for an entry this
  //protocol wrote, its unit is never erased while an entry can name it
  const uint8_t*const record = ewlf_record_addr(ctx,d->unit,d->pos);
  uint64_t buf[SIZE64(LFTL_WU_MAX_SIZE)];
  nvm_read(ctx,ctx->nvm_props,buf,ewlf_record_meta(ctx,record),ewlf_meta_phy_size(ctx));
  ewlf_meta_t meta;
  ewlf_meta_from_bytes(&meta,(const uint8_t*)buf);
  if(meta.version != d->version) raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
  uint64_t crc = ewlf_checksum_start(meta.version);
  crc = ewlf_checksum_update(ctx,crc,ewlf_record_data(ctx,record),ctx->data_size);
  if(ewlf_checksum_end(crc) != meta.checksum) raise_error(ctx,LFTL_ERROR_NO_VALID_VERSION);
  st->d_unit = d->unit;
  st->d_pos = d->pos;
  st->version = d->version;
  st->rec_started = -2;
  ewlf_plan_repair(ctx);
  ctx->data = (LFTL_REPAIR_NONE == st->pending) ? (void*)ewlf_record_data(ctx,record) : EWLF_PENDING_POINTER;
}

/**
 * \brief Report that the area has no room left to make a decision solid.
 *
 * \param ctx Context of the EWLF area
 */
static void ewlf_stuck(lftl_ctx_t*ctx){
  ctx->ewlf.stuck = 1;
  raise_error(ctx,LFTL_ERROR_STUCK);
}

/**
 * \brief Program a pair at the first position of a range above the started
 * ones (`fresh_pair` of the model).
 *
 * Marker, entry, confirm, each after the previous one returned; for a large
 * area marker, map, meta, confirm. A position where the flash refuses one of
 * them counts as started and the next one is tried: nothing valued is ever
 * programmed over a word that was refused, and no word is retried.
 *
 * \param ctx Context of the EWLF area
 * \param l Index of the log unit
 * \param lo First position of the range
 * \param hi Position after the last one of the range
 * \param gen Generation of the entry
 * \param entry Record and version of the entry
 * \returns false when the range has no position left
 */
static bool ewlf_fresh_pair(lftl_ctx_t*ctx, unsigned int l, int lo, int hi, uint16_t gen, const ewlf_entry_t*entry){
  lftl_ewlf_log_unit_t*const u = &ctx->ewlf.unit[l];
  const uintptr_t flag_size = ewlf_flag_phy_size(ctx);
  const uintptr_t entry_size = ewlf_entry_phy_size(ctx);
  const uintptr_t pair_size = ewlf_pair_phy_size(ctx);
  const int n_regular = ewlf_n_regular(ctx);
  const bool large = lftl_is_large(ctx);
  uint64_t buf[SIZE64(LFTL_WU_MAX_SIZE)];
  memset(buf,0,sizeof(buf));
  if(!large){
    ewlf_entry_t e = *entry;
    e.gen = gen;
    ewlf_entry_to_bytes(&e,(uint8_t*)buf);
  }
  for(int q = (u->started + 1 > lo) ? (u->started + 1) : lo; q < hi; q++){
    uint8_t*const pair = ewlf_pair_addr(ctx,l,q);
    //started from the first program on, whatever happens next
    u->started = q;
    if(q >= n_regular) u->flags |= EWLF_UNIT_CLOSED;
    if(!ewlf_try_zeros(ctx,pair)) continue;
    if(large){
      if(!large_try_payload(ctx,pair,gen,entry)) continue;
    } else {
      if(!ewlf_try_write(ctx,pair+flag_size,buf,entry_size)) continue;
    }
    if(!ewlf_try_zeros(ctx,pair+pair_size-flag_size)) continue;
    u->newest = q;
    u->gen = gen;
    u->flags |= EWLF_UNIT_PROVEN | EWLF_UNIT_CONFIRMED;
    if(large){
      //the decision is this pair from now on: same content as before for a
      //copy, the new map for an update
      ctx->ewlf.d_unit = l;
      ctx->ewlf.d_q = q;
    }
    return true;
  }
  return false;
}

/**
 * \brief Program a fresh certificate of the live unit and see it return
 * (`fresh_cert` of the model).
 *
 * The certificate words are read again here, only to pick one that reads
 * erased: a reading that differs from the scan's costs a word and decides
 * nothing.
 *
 * \param ctx Context of the EWLF area
 * \returns false when no certificate is left
 */
static bool ewlf_fresh_cert(lftl_ctx_t*ctx){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  const unsigned int n_certs = ewlf_n_certs(ctx);
  for(unsigned int i=0;i<n_certs;i++){
    uint8_t*const cert = ewlf_cert_addr(ctx,st->live,i);
    if(UNIT_ERASED != read_unit_state(ctx,cert,ewlf_flag_phy_size(ctx))) continue;
    if(L->certs_free) L->certs_free--;
    if(ewlf_try_zeros(ctx,cert)){
      L->flags |= EWLF_UNIT_CERTIFIED | EWLF_UNIT_CLOSED;
      return true;
    }
  }
  L->certs_free = 0;
  return false;
}

/**
 * \brief The entry that copies the decision: its record for an EWLF area,
 * its map as it is for a large one.
 *
 * \param ctx Context of the EWLF or large area
 * \param d Output: the entry
 * \param copy Storage for the description of the map, which `d` points to
 */
static void ewlf_decision_entry(lftl_ctx_t*ctx, ewlf_entry_t*d, large_map_src_t*copy){
  const lftl_ewlf_state_t*const st = &ctx->ewlf;
  d->gen = st->unit[st->live].gen;
  d->unit = st->d_unit;
  d->pos = st->d_pos;
  d->version = st->version;
  d->map = 0;
  if(lftl_is_large(ctx)){
    memset(copy,0,sizeof(*copy));
    copy->copy = true;
    d->map = copy;
  }
}

/**
 * \brief Open the next generation in the other log unit, with `entry` as
 * its first entry, the carry (`switch` of the model).
 *
 * 1. No certificate of the live unit reads programmed and it holds no
 *    proven entry: a reserved pair copying the decision first, so that a
 *    solid decision exists outside the other unit before it is erased.
 * 2. No certificate of the live unit reads programmed, or the other unit
 *    has no started position: erase the other unit, then a fresh
 *    certificate, both seen returning in this session.
 * 3. The carry, above the started positions of the other unit.
 *
 * Content thus enters a unit only after an erase that returned in the same
 * session and a certificate that session saw return, or above a word that
 * entered that way: a weak entry of the next generation is never exposed to
 * a torn erase while this generation is live.
 *
 * Raises ::LFTL_ERROR_STUCK when a reserve is exhausted.
 *
 * \param ctx Context of the EWLF area
 * \param entry Record and version of the carry
 */
static void ewlf_switch(lftl_ctx_t*ctx, const ewlf_entry_t*entry){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  const unsigned int live = st->live;
  const unsigned int other = 1 - live;
  lftl_ewlf_log_unit_t*const L = &st->unit[live];
  lftl_ewlf_log_unit_t*const O = &st->unit[other];
  const int n_positions = ewlf_n_positions(ctx);
  const int n_regular = ewlf_n_regular(ctx);
  const uint16_t gen = (uint16_t)(L->gen + 1);
  if((0 == (L->flags & (EWLF_UNIT_CERTIFIED | EWLF_UNIT_PROVEN)))){
    ewlf_entry_t d;
    large_map_src_t copy;
    ewlf_decision_entry(ctx,&d,&copy);
    if(!ewlf_fresh_pair(ctx,live,n_regular,n_positions,L->gen,&d)) ewlf_stuck(ctx);
  }
  if((0 == (L->flags & EWLF_UNIT_CERTIFIED)) || (O->started < 0)){
    //nothing this session can rely on entered the other unit. Checked
    //before the erase: without a certificate the erase would be for nothing
    if(0 == L->certs_free) ewlf_stuck(ctx);
    nvm_erase(ctx,ctx->nvm_props,ewlf_log_base(ctx,other),1);
    O->started = -1;
    O->newest = -1;
    O->gen = 0;
    O->flags = 0;
    O->certs_free = ewlf_n_certs(ctx);
    if(!ewlf_fresh_cert(ctx)) ewlf_stuck(ctx);
  }
  if(!ewlf_fresh_pair(ctx,other,0,n_positions,gen,entry)) ewlf_stuck(ctx);
  st->live = other;
}

/**
 * \brief Commit an entry above everything started (`append` of the model):
 * in the live unit when it is open and has a regular position left, else by
 * a switch.
 *
 * \param ctx Context of the EWLF area
 * \param entry Record and version to commit
 */
static void ewlf_append(lftl_ctx_t*ctx, const ewlf_entry_t*entry){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  if((0 == (L->flags & EWLF_UNIT_CLOSED)) && ewlf_fresh_pair(ctx,st->live,0,ewlf_n_regular(ctx),L->gen,entry)) return;
  ewlf_switch(ctx,entry);
}

/**
 * \brief Re-commit the decision above every started position of both log
 * units (`bury` of the model).
 *
 * By the first of these that applies: a carry into the other unit when it
 * is uncertain (a copy appended in the live unit would leave a possible
 * entry of the next generation above it); a regular pair of the live unit
 * when it is open and has one; a reserved pair of the live unit; a switch.
 * Once it returns the decision is solid, and nothing a power cut left can
 * outrank it at a later boot.
 *
 * \param ctx Context of the EWLF area
 */
static void ewlf_bury(lftl_ctx_t*ctx){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  lftl_ewlf_log_unit_t*const L = &st->unit[st->live];
  const int n_positions = ewlf_n_positions(ctx);
  const int n_regular = ewlf_n_regular(ctx);
  ewlf_entry_t d;
  large_map_src_t copy;
  ewlf_decision_entry(ctx,&d,&copy);
  if(ewlf_uncertain(st)){
    ewlf_switch(ctx,&d);
    return;
  }
  if((0 == (L->flags & EWLF_UNIT_CLOSED)) && ewlf_fresh_pair(ctx,st->live,0,n_regular,L->gen,&d)) return;
  if(ewlf_fresh_pair(ctx,st->live,n_regular,n_positions,L->gen,&d)) return;
  ewlf_switch(ctx,&d);
}

/**
 * \brief Phase 2 of the scan of an EWLF area: the pending repair.
 *
 * Runs phase 1 first if nothing did since the reset. Programs only when a
 * repair is pending: the bury, from the readings of phase 1, after which
 * the decision is shown. Raises ::LFTL_ERROR_STUCK, before programming
 * anything, when phase 1 found no room for the repair.
 *
 * \param ctx Context of the EWLF area
 */
static void ewlf_complete(lftl_ctx_t*ctx){
  if(LFTL_INVALID_POINTER == ctx->data) ewlf_scan(ctx);
  if(EWLF_PENDING_POINTER != ctx->data) return;
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  if(st->stuck) raise_error(ctx,LFTL_ERROR_STUCK);
  ewlf_bury(ctx);
  st->pending = LFTL_REPAIR_NONE;
  if(lftl_is_large(ctx)){
    ctx->data = (void*)large_map(ctx);
  } else {
    ctx->data = ewlf_record_data(ctx,ewlf_record_addr(ctx,st->d_unit,st->d_pos));
  }
}

/**
 * \brief Make sure an EWLF area can be read and written: scanned, and with
 * no repair pending.
 *
 * Raises ::LFTL_ERROR_SCAN_PENDING when a repair is pending, so that the
 * application decides when it is done (::lftl_complete_scan), and
 * ::LFTL_ERROR_STUCK when that repair has no room.
 *
 * \param ctx Context of the EWLF area
 */
static void ewlf_require_ready(lftl_ctx_t*ctx){
  if(LFTL_INVALID_POINTER == ctx->data) ewlf_scan(ctx);
  if(EWLF_PENDING_POINTER == ctx->data) raise_error(ctx,ctx->ewlf.stuck ? LFTL_ERROR_STUCK : LFTL_ERROR_SCAN_PENDING);
}

/**
 * \brief Size in bytes of the data in chunk `i` of a large area: an erase
 * unit, less for the last chunk.
 *
 * \param ctx Context of the large area
 * \param i Index of the chunk
 */
static uintptr_t large_chunk_size(lftl_ctx_t*ctx, unsigned int i){
  const uintptr_t rem = ctx->data_size - i*page_size(ctx);
  return rem < page_size(ctx) ? rem : page_size(ctx);
}

/**
 * \brief Size in bytes of what is programmed in chunk `i`: its data rounded
 * up to whole write units, which an erase unit always holds.
 *
 * \param ctx Context of the large area
 * \param i Index of the chunk
 */
static uintptr_t large_chunk_phy_size(lftl_ctx_t*ctx, unsigned int i){
  const uintptr_t rem = data_phy_size(ctx) - i*page_size(ctx);
  return rem < page_size(ctx) ? rem : page_size(ctx);
}

/**
 * \brief Read part of the data of a large area, through the decision's map:
 * a chunk never written reads erased.
 *
 * \param ctx Context of the large area, scanned
 * \param dst Destination, in volatile memory
 * \param offset Offset of the first byte in the data
 * \param size Size in bytes, within the data
 */
static void large_read_data(lftl_ctx_t*ctx, void*dst, uintptr_t offset, uintptr_t size){
  uint8_t*dst8 = (uint8_t*)dst;
  const uintptr_t chunk = page_size(ctx);
  while(size){
    const unsigned int i = offset / chunk;
    const uintptr_t in = offset % chunk;
    const uintptr_t len = (chunk - in) < size ? (chunk - in) : size;
    const uint16_t u = large_map_entry(ctx,i);
    if(LARGE_NO_UNIT == u){
      memset(dst8,0xFF,len);
    } else {
      nvm_read(ctx,ctx->nvm_props,dst8,large_unit_addr(ctx,u)+in,len);
    }
    dst8 += len;
    offset += len;
    size -= len;
  }
}

/**
 * \brief Update a CRC with a physical range of NVM, read in blocks.
 *
 * \param ctx Context of the large area
 * \param crc The CRC so far
 * \param addr Physical address
 * \param size Size in bytes
 */
static uint64_t large_crc_phys(lftl_ctx_t*ctx, uint64_t crc, const uint8_t*addr, uintptr_t size){
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  while(size){
    const uintptr_t len = size > LARGE_BLOCK ? LARGE_BLOCK : size;
    nvm_read(ctx,ctx->nvm_props,buf64,addr,len);
    crc = crc64_update(crc,buf64,len);
    addr += len;
    size -= len;
  }
  return crc;
}

/**
 * \brief Update a CRC with the data of chunk `i` as the decision's map
 * names it, or erased bytes.
 *
 * \param ctx Context of the large area
 * \param crc The CRC so far
 * \param i Index of the chunk
 */
static uint64_t large_crc_chunk(lftl_ctx_t*ctx, uint64_t crc, unsigned int i){
  const uint16_t u = large_map_entry(ctx,i);
  if(LARGE_NO_UNIT == u) return ewlf_checksum_update_erased(crc,large_chunk_size(ctx,i));
  return large_crc_phys(ctx,crc,large_unit_addr(ctx,u),large_chunk_size(ctx,i));
}

/**
 * \brief The CRC-64/NVME of the data of a large area as it reads now.
 *
 * \param ctx Context of the large area, scanned: the decision's map, pending
 *   or not
 */
static uint64_t large_data_crc(lftl_ctx_t*ctx){
  const unsigned int n = large_n_chunks(ctx);
  uint64_t crc = ~(uint64_t)0;
  for(unsigned int i=0;i<n;i++) crc = large_crc_chunk(ctx,crc,i);
  return ~crc;
}

/**
 * \brief Check the data of a large area against its checksum, and keep the
 * result until the next scan.
 *
 * \param ctx Context of the large area, scanned
 * \returns true when the data matches
 */
static bool large_check(lftl_ctx_t*ctx){
  const bool sound = large_data_crc(ctx) == large_map_data_crc(ctx);
  ctx->ewlf.data_check = sound ? 1 : 2;
  return sound;
}

/**
 * \brief The lazy check of a large area: once per scan, before the first use
 * of the data it holds; ::LFTL_ERROR_DATA_CHECKSUM when it does not match.
 *
 * \param ctx Context of the large area, scanned
 */
static void large_require_sound(lftl_ctx_t*ctx){
  if(0 == ctx->ewlf.data_check) large_check(ctx);
  if(2 == ctx->ewlf.data_check) raise_error(ctx,LFTL_ERROR_DATA_CHECKSUM);
}

/**
 * \brief Scan a large area if nothing did since the reset, and allow a
 * pending repair or a stuck area: what the reads that may see the data the
 * scan chose need (::lftl_read_unproven, ::lftl_read_unchecked,
 * ::lftl_check_data).
 *
 * \param ctx Context of the large area
 */
static void large_scanned(lftl_ctx_t*ctx){
  if(LFTL_INVALID_POINTER == ctx->data) ewlf_scan(ctx);
}

/**
 * \brief Translate a logical address into a physical one, for a std area.
 *
 * Finds the current slot first if `ctx->data` isn't known yet.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current slot
 */
static void*translate_addr_std(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  check_in_data(ctx, nvm_addr, size);
  if(LFTL_INVALID_POINTER == ctx->data) find_current_slot(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  return (void*)((uintptr_t)ctx->data + offset);
}
/**
 * \brief Translate a logical address into a physical one, for an EWLF area.
 *
 * Scans the area first if nothing did since the reset, and raises
 * ::LFTL_ERROR_SCAN_PENDING when the scan left a repair to do
 * (::ewlf_require_ready). The logical address space is the same shape as
 * for std areas -- `ctx->area` to `ctx->area+ctx->data_size` represents one
 * record's data -- only how `ctx->data` is located differs.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current record's data
 */
static void*translate_addr_ewlf(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  check_in_data(ctx, nvm_addr, size);
  ewlf_require_ready(ctx);
  const uintptr_t offset = (uintptr_t)nvm_addr - (uintptr_t)ctx->area;
  return (void*)((uintptr_t)ctx->data + offset);
}
/**
 * \brief Translate a logical address into a physical one.
 *
 * Dispatches to ::translate_addr_std or ::translate_addr_ewlf depending
 * on ::lftl_is_ewlf.
 *
 * \param ctx Context of the LFTL area
 * \param nvm_addr Logical address to translate
 * \param size Size in bytes of the access, used to validate the range
 * \returns The corresponding physical address within the current slot
 */
static void*translate_addr(lftl_ctx_t*ctx, const void*const nvm_addr, uintptr_t size){
  if(lftl_is_ewlf(ctx)){
    return translate_addr_ewlf(ctx,nvm_addr,size);
  }
  return translate_addr_std(ctx,nvm_addr,size);
}

/*
#include <stdio.h>
void dbg_memcpy(void*dst, const void*const src, uintptr_t size){
  printf("memcpy: dst=%p, src=%p, size=0x%08lx\n",dst,src,size);
  uint8_t*dst8 = (uint8_t*)dst;
  const uint8_t*const src8 = (uint8_t*)src;
  for(unsigned int i = 0;i<size;i++){
    dst8[i]=src8[i];
  }
}
*/

/**
 * \brief Redirect a write's source address if it is itself managed by LFTL.
 *
 * If `*p_src_phy_addr` is a logical address belonging to a registered
 * area, this translates it to the actual physical address of the current
 * data and updates `*src_ctx` to that area's context, so that
 * ::mem_read/nvm_read can be used directly on it later. A large area's
 * address stays logical: ::src_read and ::src_copy read it through its map.
 * Otherwise leaves both inputs untouched.
 *
 * \param src_ctx In/out: context to read `*p_src_phy_addr` through
 * \param p_src_phy_addr In/out: address to resolve
 * \param size Size in bytes of the access, used to validate the range
 */
static void check_src_phy_addr(lftl_ctx_t**src_ctx, const uint8_t**p_src_phy_addr, uintptr_t size){
  const uint8_t*src_phy_addr = *p_src_phy_addr;
  lftl_ctx_t* ctx = addr_to_area_std(src_phy_addr);
  if(LFTL_INVALID_POINTER!=ctx){
    if(is_in_data(ctx,src_phy_addr)){ // src is in an LFTL area
      *p_src_phy_addr = translate_addr_std(ctx, (void*)src_phy_addr, size);
      *src_ctx = ctx;
    }
  }else{
    ctx = addr_to_area_ewlf(src_phy_addr);
    if(LFTL_INVALID_POINTER!=ctx){
      if(is_in_data(ctx,src_phy_addr)){ // src is in an LFTL area
        *p_src_phy_addr = translate_addr_ewlf(ctx, (void*)src_phy_addr, size);
        *src_ctx = ctx;
      }
    }else{
      ctx = addr_to_area_large(src_phy_addr);
      if((LFTL_INVALID_POINTER!=ctx) && is_in_data(ctx,src_phy_addr)){
        //a large area's data is not contiguous: the address stays logical,
        //and the source is read through its map (::src_read, ::src_copy).
        //Reading it uses its data: it is checked first
        check_in_data(ctx, src_phy_addr, size);
        ewlf_require_ready(ctx);
        large_require_sound(ctx);
        *src_ctx = ctx;
      }
    }
  }
}

/**
 * \brief Whether a source resolved by ::check_src_phy_addr is the logical
 * address of a large area's data.
 *
 * Such an address is never also a physical one passed with that context:
 * the only ones are the large area's own pool units, which this layer never
 * hands to a source.
 *
 * \param src_ctx The context of the source
 * \param src The source address
 */
static bool src_is_large(lftl_ctx_t*src_ctx, const void*const src){
  return lftl_is_large(src_ctx) && is_in_data(src_ctx,src);
}

/**
 * \brief Read a source resolved by ::check_src_phy_addr: through its map for
 * a large area, else as ::mem_read does.
 *
 * \param src_ctx The context of the source
 * \param dst Destination, in volatile memory
 * \param src The source address
 * \param size Size in bytes
 */
static void src_read(lftl_ctx_t*src_ctx, void*dst, const void*const src, uintptr_t size){
  if(src_is_large(src_ctx,src)){
    large_read_data(src_ctx,dst,(uintptr_t)src - (uintptr_t)src_ctx->area,size);
    return;
  }
  mem_read(src_ctx,dst,src,size);
}

/**
 * \brief Program whole write units from a source resolved by
 * ::check_src_phy_addr: through RAM, block by block, for a large area, else
 * straight from the source.
 *
 * \param dst_ctx Context of the destination
 * \param dst_phy_addr Physical destination, aligned on a write unit
 * \param src_ctx The context of the source
 * \param src The source address
 * \param size Size in bytes, whole write units
 */
static void src_copy(lftl_ctx_t*dst_ctx, uint8_t*dst_phy_addr, lftl_ctx_t*src_ctx, const uint8_t*src, uintptr_t size){
  if(!src_is_large(src_ctx,src)){
    nvm_write(dst_ctx,dst_ctx->nvm_props,dst_phy_addr,src,size);
    return;
  }
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  while(size){
    const uintptr_t len = size > LARGE_BLOCK ? LARGE_BLOCK : size;
    src_read(src_ctx,buf64,src,len);
    nvm_write(dst_ctx,dst_ctx->nvm_props,dst_phy_addr,buf64,len);
    dst_phy_addr += len;
    src += len;
    size -= len;
  }
}

/**
 * \brief Update the checksum of an EWLF record with a source resolved by
 * ::check_src_phy_addr.
 *
 * \param src_ctx The context of the source
 * \param crc Checksum so far
 * \param src The source address
 * \param size Size in bytes
 */
static uint64_t src_checksum_update(lftl_ctx_t*src_ctx, uint64_t crc, const uint8_t*src, uintptr_t size){
  uint64_t buf64[16];
  while(size){
    const uintptr_t len = size > sizeof(buf64) ? sizeof(buf64) : size;
    src_read(src_ctx,buf64,src,len);
    crc = crc64_update(crc,buf64,len);
    src += len;
    size -= len;
  }
  return crc;
}

/**
 * \brief Low-level helper that assembles and writes one slot's new content.
 *
 * Copies the parts of the destination slot that are not being overwritten
 * from `current_base` (the previous slot), then writes the new data from
 * `src_phy_addr`, taking care of write-unit alignment at both ends of the
 * range so that every physical write is a whole number of write units.
 * Used by both ::write_core and ::ewlf_update.
 *
 * \param dst_ctx Context of the target LFTL area
 * \param offset Byte offset of the write within the area's data
 * \param size Size in bytes actually requested by the caller
 * \param dst_base Physical base address of the slot being written
 * \param current_base Physical base address of the slot being superseded
 * \param src_ctx Context to read `src_phy_addr` through (may differ from `dst_ctx`)
 * \param src_phy_addr Physical address of the source data
 * \param write_size The NVM's write unit size, in bytes
 * \param addr_misalignement How far `dst_nvm_addr_aligned` had to be moved down to align it
 * \param dst_nvm_addr_aligned Write-unit-aligned destination address
 * \param size_misalignement How far `size` is from a write-unit multiple
 * \param size_aligned Write-unit-aligned size of the range to write
 * \param transaction Whether this write is part of an ongoing transaction
 */
static void write_src_phy_addr_to_dst_phy_addr(
  lftl_ctx_t*dst_ctx,
  uintptr_t offset,
  uintptr_t size,
  uint8_t*const dst_base,
  const uint8_t*const current_base,
  lftl_ctx_t*src_ctx,
  const uint8_t* src_phy_addr,
  uint32_t write_size,
  uintptr_t addr_misalignement,
  uintptr_t dst_nvm_addr_aligned,
  uint32_t size_misalignement,
  uintptr_t size_aligned,
  bool transaction
){
  const uintptr_t end_offset = offset+size_aligned;
  uint64_t wu[SIZE64(write_size)];
  memset(&wu,0x55,sizeof(wu));
  uint8_t* dst_phy_addr = dst_base + offset;
  if(!transaction){
    if(offset){
      nvm_write(dst_ctx,dst_ctx->nvm_props,dst_base, current_base, offset);
    }
  }
  if(addr_misalignement){
    // fix up first WU
    uintptr_t size_consumed = write_size - addr_misalignement;
    //Note: with the "on/linux" test harness's write_size of 2, the only
    //possible addr_misalignement is 1, making size_consumed always exactly
    //1 -- this branch (size_consumed > size) would need size==0 to fire,
    //which is a separate, already-covered early-return case elsewhere. A
    //target built with a larger write unit size (e.g. 16) would be needed
    //to exercise this with a real unaligned sub-write-unit write.
    if(size_consumed > size){
      const uintptr_t tail_size = size_consumed - size;
      size_consumed = size;
      nvm_read(dst_ctx,dst_ctx->nvm_props,((uint8_t*)&wu) + addr_misalignement + size, current_base+offset+ addr_misalignement+size, tail_size);
    }
    nvm_read(dst_ctx,dst_ctx->nvm_props,&wu, current_base+offset, addr_misalignement);
    src_read(src_ctx,((uint8_t*)&wu) + addr_misalignement, src_phy_addr , size_consumed);
    nvm_write(dst_ctx,dst_ctx->nvm_props,dst_phy_addr,&wu,write_size);
    // adjust write range
    offset += write_size;
    dst_phy_addr = dst_base + offset;
    src_phy_addr += size_consumed;
    size_aligned -= write_size;
    size -= size_consumed;
  }
  if(size != size_aligned){
    //remove the last write unit from the write range as it needs special handling
    size_aligned -= write_size;
  }
  //at this point size_aligned >= size
  src_copy(dst_ctx,dst_phy_addr,src_ctx,src_phy_addr,size_aligned);
  if(size != size_aligned){
    const uintptr_t last_wu_offset = offset+size_aligned;
    // fix up last WU
    uintptr_t size_misalignement = size % write_size;//((uintptr_t)dst_nvm_addr + size) % write_size;
    const uintptr_t wu_part1_size = size_misalignement;
    const uint8_t* wu_part1_src = ((uint8_t*)src_phy_addr) + size_aligned;
    src_read(src_ctx,&wu,wu_part1_src, wu_part1_size);
    uint8_t*const wu_part2_base = ((uint8_t*)&wu) + size_misalignement;
    const uintptr_t wu_part2_size = write_size - size_misalignement;
    const uint8_t* wu_part2_src = current_base + last_wu_offset + size_misalignement;
    nvm_read(dst_ctx,dst_ctx->nvm_props,wu_part2_base, wu_part2_src, wu_part2_size);
    nvm_write(dst_ctx,dst_ctx->nvm_props,dst_base+last_wu_offset, &wu, write_size);
  }
  if(!transaction){
    //up to the data rounded up to whole write units, where end_offset may already be
    const uintptr_t remaining = data_phy_size(dst_ctx) - end_offset;
    if(remaining){
      nvm_write(dst_ctx,dst_ctx->nvm_props,dst_base+end_offset, current_base + end_offset, remaining);
    }
  }
}

/**
 * \brief Write to a std area, handling both transactional and immediate writes.
 *
 * Aligns the requested range to write units if needed, finds the next
 * slot to use, erases it first unless a transaction is ongoing, then
 * delegates the actual byte assembly to
 * ::write_src_phy_addr_to_dst_phy_addr. Outside of a transaction, also
 * bumps the version and commits the new meta-data immediately.
 *
 * \param dst_ctx Context of the target LFTL area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes to write
 * \param transaction Whether this write is part of an ongoing transaction (::TRANSACTION or ::NO_TRANSACTION)
 * \param aligned Whether the caller has already guaranteed write-unit alignment (::ALIGNED or ::UNALIGNED)
 */
static void write_core(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool transaction, bool aligned){
  DEBUG_PRINTLN("write_core(%p,%p,%p,%u,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,transaction,aligned);
  //printf("src=%p, size=0x%08lx\n",src,size);
  //check the range as requested: once extended to whole write units below, size could wrap around
  check_in_data(dst_ctx, dst_nvm_addr, size);
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  uintptr_t dst_nvm_addr_aligned;
  uintptr_t addr_misalignement;
  uintptr_t size_aligned;
  if(aligned){
    // check that the args are indeed aligned
    if(0 != ((uintptr_t)dst_nvm_addr % write_size)) raise_error(dst_ctx,LFTL_ERROR_BASE_MISALIGNED);
    if(0 != (size % write_size)) raise_error(dst_ctx,LFTL_ERROR_SIZE_MISALIGNED);
    addr_misalignement = 0;
    dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr;
    size_aligned = size;
  } else {
    // unaligned: extend the start address and end address to start of WU and end of WU respectively
    addr_misalignement = ((uintptr_t)dst_nvm_addr % write_size);
    dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr - addr_misalignement;
    size_aligned = size + addr_misalignement;
    if(0 != (size_aligned % write_size)){
      size_aligned += write_size - size_aligned % write_size;
    }
  }
  const uint32_t size_misalignement = size % write_size;

  const void*const current_phy_addr = translate_addr(dst_ctx, (void*)dst_nvm_addr_aligned, size_aligned);
  const uint8_t*const current_base = slot_base(dst_ctx,get_current_slot_index(dst_ctx));
  uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)dst_ctx->data;
  //const uintptr_t end_offset = offset+size_aligned;
  const unsigned int index = next_slot(dst_ctx);
  uint8_t*const dst_base = slot_base(dst_ctx, index);
  if(dst_base == current_base) raise_error(dst_ctx,LFTL_INTERNAL_ERROR);
  const uint8_t* src_phy_addr = src;
  lftl_ctx_t* src_ctx = dst_ctx;
  check_src_phy_addr(&src_ctx,&src_phy_addr,size);
  if(!transaction){
    if(LFTL_INVALID_POINTER != dst_ctx->transaction_tracker) raise_error(dst_ctx,LFTL_ERROR_TRANSACTION_ONGOING);
    //erase next slot
    erase_slot(dst_ctx,index);
  }
  //write new data in next slot
  write_src_phy_addr_to_dst_phy_addr(
    dst_ctx,
    offset,
    size,
    dst_base,
    current_base,
    src_ctx,
    src_phy_addr,
    write_size,
    addr_misalignement,
    dst_nvm_addr_aligned,
    size_misalignement,
    size_aligned,
    transaction
  );
  if(!transaction){
    //increment version and write new meta data in next slot
    const uint32_t version = 1 + get_slot_version(dst_ctx, get_current_slot_index(dst_ctx));
    write_meta(dst_ctx, index, version);
    //update context
    dst_ctx->data = dst_base;
  }
  DEBUG_PRINTLN("write_core exit");
}

/**
 * \brief Last started record position of a data unit: the highest one whose
 * start marker does not read erased, -1 if none.
 *
 * Read once per session for the unit of the current record; the positions
 * the session starts afterwards are tracked in the state.
 *
 * \param ctx Context of the EWLF area
 * \param unit Index of the data unit
 */
static int ewlf_last_started_record(lftl_ctx_t*ctx, unsigned int unit){
  const unsigned int records_per_unit = ewlf_records_per_unit(ctx);
  int last = -1;
  for(unsigned int p=0;p<records_per_unit;p++){
    if(UNIT_ERASED != read_unit_state(ctx,ewlf_record_addr(ctx,unit,p),ewlf_flag_phy_size(ctx))) last = p;
  }
  return last;
}

/**
 * \brief The data unit the write order moves to after `unit`: the one below
 * it, wrapping from the lowest to the highest.
 *
 * \param ctx Context of the EWLF area
 * \param unit Index of a data unit
 */
static unsigned int ewlf_next_data_unit(lftl_ctx_t*ctx, unsigned int unit){
  return (0 == unit) ? (ewlf_n_data_units(ctx) - 1) : (unit - 1);
}

/**
 * \brief One update of an EWLF area (`update` of the model): a new record,
 * then the entry that makes it current.
 *
 * 1. Room for the record: the first position of the current record's unit
 *    above the last started one. A position whose start marker the flash
 *    refuses counts as started. No position left: the next data unit, never
 *    the one of the current record, is erased, whatever it reads, right
 *    before its first record; the decision is buried first if anything
 *    started could outrank it (after a scan nothing can: the check is
 *    defensive).
 * 2. The record: start marker, data (the current data with the written
 *    range replaced; left erased when `write_data` is false), meta field.
 *    The checksum is computed from the data meant to be written, so a
 *    record that did not program as intended fails its check.
 * 3. The commit: the pair that names the record, in the live log unit or,
 *    when it has no regular position left, by a switch.
 *
 * The update is acknowledged, and `ctx->data` moved, only after the confirm
 * of its pair returned. A power cut before that leaves the next boot free
 * to make either version current, once and for all.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address (ignored when `write_data` is false)
 * \param size Size in bytes to write (ignored when `write_data` is false)
 * \param write_data Whether to write new data, or leave the record's data erased
 */
static void ewlf_update(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data){
  DEBUG_PRINTLN("ewlf_update(%p,%p,%p,%u,%u) entry",dst_ctx,dst_nvm_addr,src,size,write_data);
  //check the range as requested: once extended to whole write units, size could wrap around
  if(write_data) check_in_data(dst_ctx, dst_nvm_addr, size);
  ewlf_require_ready(dst_ctx);
  lftl_ewlf_state_t*const st = &dst_ctx->ewlf;
  const uint32_t write_size = dst_ctx->nvm_props->write_size;
  const uintptr_t addr_misalignement = write_data ? ((uintptr_t)dst_nvm_addr % write_size) : 0;
  const uintptr_t dst_nvm_addr_aligned = (uintptr_t)dst_nvm_addr - addr_misalignement;
  const uint32_t size_misalignement = write_data ? (size % write_size) : 0;
  const bool aligned = (0 == addr_misalignement) && (0 == size_misalignement);
  uintptr_t size_aligned = write_data ? (size + addr_misalignement) : dst_ctx->data_size;
  if(write_data && !aligned){
    // unaligned: extend the start address and end address to start of WU and end of WU respectively
    if(0 != (size_aligned % write_size)){
      size_aligned += write_size - size_aligned % write_size;
    }
  }
  //once extended to whole write units the range may end in the padding
  //after data_size, which every record has room for (see data_phy_size)
  const uintptr_t offset = dst_nvm_addr_aligned - (uintptr_t)dst_ctx->area;
  const uint8_t*const current_data = (const uint8_t*)dst_ctx->data;
  //the source first: resolving it can scan another area, and can fail
  const uint8_t* src_phy_addr = (const uint8_t*)src;
  lftl_ctx_t* src_ctx = dst_ctx;
  if(write_data) check_src_phy_addr(&src_ctx,&src_phy_addr,size);

  //1. room for the record
  const unsigned int records_per_unit = ewlf_records_per_unit(dst_ctx);
  const unsigned int n_data_units = ewlf_n_data_units(dst_ctx);
  if(-2 == st->rec_started){
    const int last = ewlf_last_started_record(dst_ctx,st->d_unit);
    st->rec_started = (last < (int)st->d_pos) ? (int)st->d_pos : last;
  }
  unsigned int unit = st->d_unit;
  unsigned int pos = st->rec_started + 1;
  unsigned int rollovers = 0;
  for(;;){
    if(pos < records_per_unit){
      //started from the program on, accepted or not
      if(unit == st->d_unit) st->rec_started = pos;
      if(ewlf_try_zeros(dst_ctx,ewlf_record_addr(dst_ctx,unit,pos))) break;
      pos++;
      continue;
    }
    //a flash that refuses every program must not spin
    if(rollovers++ > n_data_units) raise_error(dst_ctx,LFTL_ERROR_LOW_LEVEL_WRITE | LFTL_NVM_STATUS_REFUSED);
    //rollover: nothing a later boot reads valid may outrank the decision
    //once a record is gone
    if(!ewlf_top_solid(st)) ewlf_bury(dst_ctx);
    unit = ewlf_next_data_unit(dst_ctx,unit);
    if(unit == st->d_unit) unit = ewlf_next_data_unit(dst_ctx,unit);
    nvm_erase(dst_ctx,dst_ctx->nvm_props,((uint8_t*)dst_ctx->area) + unit*page_size(dst_ctx),1);
    pos = 0;
  }

  //2. the record
  uint8_t*const new_record = ewlf_record_addr(dst_ctx,unit,pos);
  uint8_t*const new_data = ewlf_record_data(dst_ctx,new_record);
  if(write_data){
    write_src_phy_addr_to_dst_phy_addr(
      dst_ctx,
      offset,
      size,
      new_data,
      current_data,
      src_ctx,
      src_phy_addr,
      write_size,
      addr_misalignement,
      dst_nvm_addr_aligned,
      size_misalignement,
      size_aligned,
      0 // no transaction
    );
  }
  ewlf_meta_t meta;
  meta.version = st->version + 1;
  uint64_t crc = ewlf_checksum_start(meta.version);
  if(write_data){
    const uintptr_t first = (uintptr_t)dst_nvm_addr - (uintptr_t)dst_ctx->area;
    crc = ewlf_checksum_update(dst_ctx,crc,current_data,first);
    crc = src_checksum_update(src_ctx,crc,src_phy_addr,size);
    crc = ewlf_checksum_update(dst_ctx,crc,current_data+first+size,dst_ctx->data_size-first-size);
  } else {
    crc = ewlf_checksum_update_erased(crc,dst_ctx->data_size);
  }
  meta.checksum = ewlf_checksum_end(crc);
  ewlf_write_meta(dst_ctx,new_record,&meta);

  //3. the commit
  ewlf_entry_t entry;
  entry.gen = 0; //set by the pair
  entry.unit = unit;
  entry.pos = pos;
  entry.version = meta.version;
  ewlf_append(dst_ctx,&entry);
  st->d_unit = unit;
  st->d_pos = pos;
  st->version = meta.version;
  st->rec_started = pos;
  st->stuck = 0;
  dst_ctx->data = new_data;
  DEBUG_PRINTLN("ewlf_update exit");
}

/**
 * \brief Write to an EWLF area.
 *
 * EWLF areas never support transactions, so every write is immediate.
 *
 * \param dst_ctx Context of the target EWLF area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes to write
 */
static void write_ewlf(lftl_ctx_t*dst_ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size){
  ewlf_update(dst_ctx,dst_nvm_addr,src,size,true);
}

/**
 * \brief Logically erase an EWLF area's data.
 *
 * An ordinary update whose record keeps its data in the erased state the
 * unit's erase left, so it reads back as erased.
 *
 * \param ctx Context of the target EWLF area
 */
static void erase_ewlf(lftl_ctx_t*ctx){
  ewlf_update(ctx,ctx->area,LFTL_INVALID_POINTER,0,false);
}

/**
 * \brief Format an EWLF area.
 *
 * Erases every erase unit, then writes the first record (version 1, its
 * data left erased) at the top of the highest data unit, and the pair that
 * names it at the first position of log unit 1, generation 1. Log unit 0
 * stays erased. Not anti-tearing: a cut in the erase leaves cells that a
 * later boot may read differently (T4 of the flash page), so an area whose
 * format was cut must be formatted again before use.
 *
 * \param ctx Context of the target EWLF area
 */
static void format_ewlf(lftl_ctx_t*ctx){
  DEBUG_PRINTLN("format_ewlf entry");
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  ctx->data = LFTL_INVALID_POINTER;
  nvm_erase(ctx,ctx->nvm_props,ctx->area, n_pages(ctx));
  const unsigned int unit = ewlf_n_data_units(ctx) - 1;
  uint8_t*const record_addr = ewlf_record_addr(ctx,unit,0);
  pin_unit(ctx,record_addr,ewlf_flag_phy_size(ctx));
  ewlf_meta_t meta;
  meta.version = 1;
  meta.checksum = ewlf_checksum_end(ewlf_checksum_update_erased(ewlf_checksum_start(meta.version),ctx->data_size));
  ewlf_write_meta(ctx,record_addr,&meta);
  memset(st,0,sizeof(*st));
  for(unsigned int l=0;l<2;l++){
    st->unit[l].started = -1;
    st->unit[l].newest = -1;
    st->unit[l].certs_free = ewlf_n_certs(ctx);
  }
  st->live = 1;
  st->d_unit = unit;
  st->d_pos = 0;
  st->version = meta.version;
  st->rec_started = 0;
  ewlf_entry_t entry;
  entry.gen = 1;
  entry.unit = unit;
  entry.pos = 0;
  entry.version = meta.version;
  //freshly erased cells: a refused program is a flash that does not work
  if(!ewlf_fresh_pair(ctx,1,0,1,1,&entry)) raise_error(ctx,LFTL_ERROR_LOW_LEVEL_WRITE | LFTL_NVM_STATUS_REFUSED);
  ctx->data = ewlf_record_data(ctx,record_addr);
  DEBUG_PRINTLN("format_ewlf exit");
}

/**
 * \brief The next unit of a large area's pool, ready for a chunk (`alloc` of
 * the model, with one slot per unit): the first free unit after the head,
 * erased at that instant whatever it reads, since a unit that reads erased
 * may hold the remains of an erase a power cut interrupted (T4). The erase
 * is guarded as a rollover of an EWLF area is: nothing started in the log
 * may outrank the decision once a unit is erased (after a scan nothing
 * can: the check is defensive). Nothing is erased while a repair is
 * pending: the callers need an area that is ready.
 *
 * \param ctx Context of the large area
 * \returns The unit, now the head
 */
static uint16_t large_take_unit(lftl_ctx_t*ctx){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  const uint16_t u = large_next_free(ctx,st->head);
  //the room was counted first: none free means a map naming the same unit twice
  if(LARGE_NO_UNIT == u) raise_error(ctx,LFTL_ERROR_LARGE_NO_SPACE);
  if(!ewlf_top_solid(st)) ewlf_bury(ctx);
  nvm_erase(ctx,ctx->nvm_props,large_unit_addr(ctx,u),1);
  st->head = u;
  return u;
}

/**
 * \brief Program chunk `i` of a large area into unit `u`, erased: its current
 * content with the bytes of a write in place, block by block through RAM,
 * and the CRC of what it means to program.
 *
 * Every write unit of the chunk is programmed, whatever its value, so that
 * the time does not depend on the data.
 *
 * \param ctx Context of the large area
 * \param i Index of the chunk
 * \param u Unit, erased in this session
 * \param offset Offset in the data of the bytes written
 * \param size Number of bytes written, 0 for none
 * \param src_ctx Context of the source, see ::check_src_phy_addr
 * \param src Source of the bytes written
 * \param fresh When not NULL: the CRC of the whole data, chunk after chunk,
 *   updated with the new content
 * \param delta When `fresh` is NULL: the CRC of the difference between the
 *   old and the new data, from the first chunk the update replaces, updated
 *   with this chunk: R(delta, old) ^ R(0, new)
 */
static void large_write_chunk(lftl_ctx_t*ctx, unsigned int i, uint16_t u, uintptr_t offset, uintptr_t size, lftl_ctx_t*src_ctx, const uint8_t*src, uint64_t*fresh, uint64_t*delta){
  const uintptr_t base = i*page_size(ctx);
  const uintptr_t len = large_chunk_size(ctx,i);
  const uintptr_t phy = large_chunk_phy_size(ctx,i);
  const uint16_t old_u = large_map_entry(ctx,i);
  const uint8_t*const old = (LARGE_NO_UNIT == old_u) ? 0 : large_unit_addr(ctx,old_u);
  uint8_t*const dst = large_unit_addr(ctx,u);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  uint64_t a = fresh ? 0 : *delta;
  uint64_t b = 0;
  for(uintptr_t p=0;p<phy;p+=LARGE_BLOCK){
    const uintptr_t l = (phy - p) > LARGE_BLOCK ? LARGE_BLOCK : (phy - p);
    const uintptr_t d = (p >= len) ? 0 : (((len - p) > l) ? l : (len - p)); //bytes of data, not padding
    if(old){
      nvm_read(ctx,ctx->nvm_props,buf,old+p,l);
    } else {
      memset(buf,0xFF,l);
    }
    if(!fresh) a = crc64_update(a,buf,d);
    const uintptr_t lo = (base + p > offset) ? (base + p) : offset;
    const uintptr_t hi = (base + p + l < offset + size) ? (base + p + l) : (offset + size);
    if(lo < hi) src_read(src_ctx,buf + (lo - base - p),src + (lo - offset),hi - lo);
    if(fresh){
      *fresh = crc64_update(*fresh,buf,d);
    } else {
      b = crc64_update(b,buf,d);
    }
    nvm_write(ctx,ctx->nvm_props,dst+p,buf,l);
  }
  if(!fresh) *delta = a ^ b;
}

/**
 * \brief Commit a new map of a large area: one pair, in the live log unit or
 * by a switch, after which the decision is that pair (::ewlf_fresh_pair).
 *
 * \param ctx Context of the large area
 * \param m What the map holds
 */
static void large_commit(lftl_ctx_t*ctx, const large_map_src_t*m){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  ewlf_entry_t entry;
  entry.gen = 0; //set by the pair
  entry.unit = 0;
  entry.pos = 0;
  entry.version = st->version + 1;
  entry.map = m;
  ewlf_append(ctx,&entry);
  st->version = entry.version;
  st->stuck = 0;
  ctx->data = (void*)large_map(ctx);
}

/**
 * \brief One update of a large area: a plain write, or an erase of all its
 * data.
 *
 * 1. Room: one free unit per chunk the write touches, counted before
 *    anything is programmed (::LFTL_ERROR_LARGE_NO_SPACE otherwise).
 * 2. The chunks, in increasing order, each in the next free unit, erased
 *    right before (::large_take_unit, ::large_write_chunk).
 * 3. The commit: a pair whose map names the new units and the old ones of
 *    the chunks not touched, the head, and the checksum of the new data.
 *
 * A write that replaces the whole data, and an erase of all of it, use no
 * data the area holds: no check first, and the checksum is computed afresh.
 * Any other write needs the data sound, and the checksum follows the chunks
 * it replaces (::crc64_shift), or is computed afresh when it replaces every
 * chunk.
 *
 * \param ctx Context of the target large area
 * \param dst_nvm_addr Destination logical address
 * \param src Source address (ignored when `write_data` is false)
 * \param size Size in bytes to write, not 0 (ignored when `write_data` is false)
 * \param write_data Whether to write new data, or erase all of it
 */
static void large_update(lftl_ctx_t*ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool write_data){
  if(write_data) check_in_data(ctx, dst_nvm_addr, size);
  ewlf_require_ready(ctx);
  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) raise_error(ctx,LFTL_ERROR_TRANSACTION_ONGOING);
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  const uintptr_t chunk = page_size(ctx);
  const unsigned int n = large_n_chunks(ctx);
  const uintptr_t offset = write_data ? ((uintptr_t)dst_nvm_addr - (uintptr_t)ctx->area) : 0;
  const bool whole = !write_data || ((0 == offset) && (size == ctx->data_size));
  if(!whole) large_require_sound(ctx);
  //the source: resolving it can check another area, and can fail
  const uint8_t*src_addr = (const uint8_t*)src;
  lftl_ctx_t*src_ctx = ctx;
  if(write_data) check_src_phy_addr(&src_ctx,&src_addr,size);
  //the run of chunks the write touches; extended to whole write units it
  //stays in them, an erase unit being whole write units
  unsigned int first = 0;
  unsigned int k = 0;
  if(write_data){
    first = offset / chunk;
    k = (offset + size - 1) / chunk - first + 1;
  }
  //1. room
  if(k > large_count_free(ctx)) raise_error(ctx,LFTL_ERROR_LARGE_NO_SPACE);
  //2. the chunks
  const uint16_t run_head = st->head;
  const bool fresh = whole || (k == n);
  uint64_t crc = ~(uint64_t)0;
  uint64_t delta = 0;
  const uint64_t old_crc = large_map_data_crc(ctx);
  for(unsigned int j=0;j<k;j++){
    const uint16_t u = large_take_unit(ctx);
    large_write_chunk(ctx,first+j,u,offset,size,src_ctx,src_addr,fresh ? &crc : 0,&delta);
  }
  uint64_t data_crc;
  if(!write_data){
    data_crc = ewlf_checksum_end(ewlf_checksum_update_erased(~(uint64_t)0,ctx->data_size));
  } else if(fresh){
    data_crc = ewlf_checksum_end(crc);
  } else {
    const uintptr_t end = first*chunk;
    uintptr_t run_end = end;
    for(unsigned int j=0;j<k;j++) run_end += large_chunk_size(ctx,first+j);
    data_crc = old_crc ^ crc64_shift(delta,ctx->data_size - run_end);
  }
  //3. the commit
  large_map_src_t m;
  memset(&m,0,sizeof(m));
  m.erased = !write_data;
  m.run_first = first;
  m.run_n = k;
  m.run_head = run_head;
  m.head = st->head;
  m.data_crc = data_crc;
  large_commit(ctx,&m);
  st->data_check = 1;
}

/**
 * \brief Start a transaction on a large area: it uses the data the area
 * holds, so the data is checked first; nothing is erased.
 *
 * \param ctx Context of the large area
 * \param tracker The caller's tracker, ::LFTL_TRANSACTION_TRACKER_SIZE bytes
 */
static void large_txn_start(lftl_ctx_t*ctx, void*const tracker){
  ewlf_require_ready(ctx);
  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) raise_error(ctx,LFTL_ERROR_TRANSACTION_ONGOING);
  large_require_sound(ctx);
  //start tracking only once nothing can fail anymore
  const uintptr_t bits = LFTL_TRANSACTION_TRACKER_SIZE_LL(ctx->data_size,ctx->nvm_props->write_size);
  memset(tracker,0,bits);
  memset(((uint8_t*)tracker) + bits,0xFF,2*large_n_chunks(ctx));
  ctx->transaction_tracker = tracker;
}

/**
 * \brief Whether the transaction wrote the write unit at an offset of the data.
 *
 * \param ctx Context of the area, with a transaction open
 * \param offset Offset in the data, in the write unit
 */
static bool txn_marked(lftl_ctx_t*ctx, uintptr_t offset){
  const uintptr_t wu = offset / ctx->nvm_props->write_size;
  return 0 != (((const uint8_t*)ctx->transaction_tracker)[wu / BITS_PER_BYTE] & (1 << (wu % BITS_PER_BYTE)));
}

/**
 * \brief A write of a transaction on a large area, its write units already
 * marked in the tracker: each chunk it touches for the first time takes a
 * free unit, and its write units are programmed straight there, the bytes
 * of a write unit the write covers only in part completed from the
 * committed data.
 *
 * \param ctx Context of the large area, with a transaction open
 * \param dst_nvm_addr Destination logical address
 * \param src Source address
 * \param size Size in bytes, not 0
 * \param aligned Whether the write is to be whole write units (::ALIGNED)
 */
static void large_txn_write(lftl_ctx_t*ctx, void*const dst_nvm_addr, const void*const src, uintptr_t size, bool aligned){
  const uint32_t write_size = ctx->nvm_props->write_size;
  if(aligned){
    if(0 != ((uintptr_t)dst_nvm_addr % write_size)) raise_error(ctx,LFTL_ERROR_BASE_MISALIGNED);
    if(0 != (size % write_size)) raise_error(ctx,LFTL_ERROR_SIZE_MISALIGNED);
  }
  ewlf_require_ready(ctx);
  const uint8_t*src_addr = (const uint8_t*)src;
  lftl_ctx_t*src_ctx = ctx;
  check_src_phy_addr(&src_ctx,&src_addr,size);
  uint8_t*const units = large_txn_units(ctx);
  const uintptr_t chunk = page_size(ctx);
  const uintptr_t offset = (uintptr_t)dst_nvm_addr - (uintptr_t)ctx->area;
  const uintptr_t lo = offset - offset % write_size;
  const uintptr_t hi = LFTL_DIV_CEIL(offset + size, write_size) * write_size;
  const unsigned int first = lo / chunk;
  const unsigned int last = (hi - 1) / chunk;
  //room: a unit for each chunk the transaction did not touch yet
  unsigned int k = 0;
  for(unsigned int i=first;i<=last;i++) if(LARGE_NO_UNIT == large_get16(units+2*i)) k++;
  if(k > large_count_free(ctx)) raise_error(ctx,LFTL_ERROR_LARGE_NO_SPACE);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  for(unsigned int i=first;i<=last;i++){
    uint16_t u = large_get16(units+2*i);
    if(LARGE_NO_UNIT == u){
      u = large_take_unit(ctx);
      large_put16(units+2*i,u);
    }
    const uintptr_t base = i*chunk;
    const uintptr_t c_lo = (lo > base) ? lo - base : 0;
    const uintptr_t c_hi = ((hi - base) < chunk) ? (hi - base) : chunk;
    for(uintptr_t p=c_lo;p<c_hi;p+=LARGE_BLOCK){
      const uintptr_t l = (c_hi - p) > LARGE_BLOCK ? LARGE_BLOCK : (c_hi - p);
      //the committed bytes, for a write unit the write covers in part; the
      //bytes past the data, in its last write unit, read erased
      memset(buf,0xFF,l);
      if(base + p < ctx->data_size) large_read_data(ctx,buf,base+p,((base+p+l) <= ctx->data_size) ? l : (ctx->data_size - base - p));
      const uintptr_t w_lo = (base + p > offset) ? (base + p) : offset;
      const uintptr_t w_hi = (base + p + l < offset + size) ? (base + p + l) : (offset + size);
      src_read(src_ctx,buf + (w_lo - base - p),src_addr + (w_lo - offset),w_hi - w_lo);
      nvm_write(ctx,ctx->nvm_props,large_unit_addr(ctx,u)+p,buf,l);
    }
  }
}

/**
 * \brief The newer data of a large area during a transaction: the write
 * units it wrote from its units, the others from the committed data.
 *
 * \param ctx Context of the large area, with a transaction open
 * \param dst Destination, in volatile memory
 * \param offset Offset in the data
 * \param size Size in bytes, within the data
 */
static void large_txn_read(lftl_ctx_t*ctx, void*dst, uintptr_t offset, uintptr_t size){
  const uint32_t write_size = ctx->nvm_props->write_size;
  const uintptr_t chunk = page_size(ctx);
  const uint8_t*const units = large_txn_units(ctx);
  uint8_t*dst8 = (uint8_t*)dst;
  const uintptr_t end = offset + size;
  while(offset < end){
    //up to the end of the write unit holding offset
    const uintptr_t wu_end = (offset / write_size + 1) * write_size;
    const uintptr_t len = (wu_end < end ? wu_end : end) - offset;
    if(txn_marked(ctx,offset)){
      const uint16_t u = large_get16(units+2*(offset / chunk));
      nvm_read(ctx,ctx->nvm_props,dst8,large_unit_addr(ctx,u)+(offset % chunk),len);
    } else {
      large_read_data(ctx,dst8,offset,len);
    }
    dst8 += len;
    offset += len;
  }
}

/**
 * \brief Commit a transaction on a large area.
 *
 * Completes each chunk the transaction touched (the write units it did not
 * write, from the committed data), computes the checksum of the new data
 * (afresh when every chunk was touched, else from the chunks replaced and
 * the gaps between them, ::crc64_shift), then commits the map. A
 * transaction that touched no chunk changes nothing and commits nothing.
 *
 * \param ctx Context of the large area, with a transaction open
 */
static void large_txn_commit(lftl_ctx_t*ctx){
  ewlf_require_ready(ctx);
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  const uint32_t write_size = ctx->nvm_props->write_size;
  const unsigned int n = large_n_chunks(ctx);
  const uintptr_t chunk = page_size(ctx);
  const uint8_t*const units = large_txn_units(ctx);
  uint64_t buf64[SIZE64(LARGE_BLOCK)];
  uint8_t*const buf = (uint8_t*)buf64;
  unsigned int touched = 0;
  for(unsigned int i=0;i<n;i++){
    const uint16_t u = large_get16(units+2*i);
    if(LARGE_NO_UNIT == u) continue;
    touched++;
    const uintptr_t base = i*chunk;
    const uintptr_t phy = large_chunk_phy_size(ctx,i);
    for(uintptr_t p=0;p<phy;){
      if(txn_marked(ctx,base+p)){
        p += write_size;
        continue;
      }
      uintptr_t l = 0;
      while((p + l < phy) && (l < LARGE_BLOCK) && !txn_marked(ctx,base+p+l)) l += write_size;
      memset(buf,0xFF,l);
      if(base + p < ctx->data_size) large_read_data(ctx,buf,base+p,(base + p + l <= ctx->data_size) ? l : (ctx->data_size - base - p));
      nvm_write(ctx,ctx->nvm_props,large_unit_addr(ctx,u)+p,buf,l);
      p += l;
    }
  }
  if(0 == touched){
    ctx->transaction_tracker = LFTL_INVALID_POINTER;
    return;
  }
  uint64_t data_crc;
  if(touched == n){
    uint64_t crc = ~(uint64_t)0;
    for(unsigned int i=0;i<n;i++) crc = large_crc_phys(ctx,crc,large_unit_addr(ctx,large_get16(units+2*i)),large_chunk_size(ctx,i));
    data_crc = ewlf_checksum_end(crc);
  } else {
    uint64_t delta = 0;
    uintptr_t prev_end = 0;
    for(unsigned int i=0;i<n;i++){
      const uint16_t u = large_get16(units+2*i);
      if(LARGE_NO_UNIT == u) continue;
      const uintptr_t base = i*chunk;
      delta = crc64_shift(delta,base - prev_end);
      const uint64_t a = large_crc_chunk(ctx,delta,i);
      const uint64_t b = large_crc_phys(ctx,0,large_unit_addr(ctx,u),large_chunk_size(ctx,i));
      delta = a ^ b;
      prev_end = base + large_chunk_size(ctx,i);
    }
    data_crc = large_map_data_crc(ctx) ^ crc64_shift(delta,ctx->data_size - prev_end);
  }
  large_map_src_t m;
  memset(&m,0,sizeof(m));
  m.slots = units;
  m.head = st->head;
  m.data_crc = data_crc;
  large_commit(ctx,&m);
  ctx->transaction_tracker = LFTL_INVALID_POINTER;
}

/**
 * \brief Make the checksum of a large area that of its data as it reads now:
 * a commit of the same map with that checksum, when it differs.
 *
 * \param ctx Context of the large area
 */
static void large_repair_checksum(lftl_ctx_t*ctx){
  ewlf_require_ready(ctx);
  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) raise_error(ctx,LFTL_ERROR_TRANSACTION_ONGOING);
  const uint64_t crc = large_data_crc(ctx);
  if(crc != large_map_data_crc(ctx)){
    large_map_src_t m;
    memset(&m,0,sizeof(m));
    m.head = ctx->ewlf.head;
    m.data_crc = crc;
    large_commit(ctx,&m);
  }
  ctx->ewlf.data_check = 1;
}

/**
 * \brief Format a large area.
 *
 * Erases every erase unit, then writes the first pair at the first position
 * of log unit 1, generation 1, version 1: a map that names no unit, no head,
 * the checksum of erased data. Not anti-tearing, as for an EWLF area.
 *
 * \param ctx Context of the target large area
 */
static void format_large(lftl_ctx_t*ctx){
  lftl_ewlf_state_t*const st = &ctx->ewlf;
  ctx->data = LFTL_INVALID_POINTER;
  ctx->transaction_tracker = LFTL_INVALID_POINTER;
  nvm_erase(ctx,ctx->nvm_props,ctx->area,n_pages(ctx));
  memset(st,0,sizeof(*st));
  for(unsigned int l=0;l<2;l++){
    st->unit[l].started = -1;
    st->unit[l].newest = -1;
    st->unit[l].certs_free = ewlf_n_certs(ctx);
  }
  st->live = 1;
  st->version = 1;
  st->rec_started = -2;
  st->head = LARGE_NO_UNIT;
  large_map_src_t m;
  memset(&m,0,sizeof(m));
  m.erased = true;
  m.head = LARGE_NO_UNIT;
  m.data_crc = ewlf_checksum_end(ewlf_checksum_update_erased(~(uint64_t)0,ctx->data_size));
  ewlf_entry_t entry;
  entry.gen = 1;
  entry.unit = 0;
  entry.pos = 0;
  entry.version = 1;
  entry.map = &m;
  //freshly erased cells: a refused program is a flash that does not work
  if(!ewlf_fresh_pair(ctx,1,0,1,1,&entry)) raise_error(ctx,LFTL_ERROR_LOW_LEVEL_WRITE | LFTL_NVM_STATUS_REFUSED);
  st->data_check = 1;
  ctx->data = (void*)large_map(ctx);
}

/**
 * \brief Logically erase a range of an area's data.
 *
 * Used by ::lftl_erase_all. Like ::write_core's non-transactional
 * branch, erases the next slot and copies over the parts of the range
 * outside `[dst_nvm_addr, dst_nvm_addr+size)`, but writes nothing new
 * inside that range, so it reads back as erased.
 *
 * \param ctx Context of the target LFTL area
 * \param dst_nvm_addr Start of the logical range to erase
 * \param size Size in bytes of the range to erase
 */
//Note: erase()'s only caller is lftl_erase_all, which always passes the
//full, page-aligned area range (offset==0 and remaining==0 always hold).
//The misalignment checks below and the offset/remaining copy branches are
//therefore unreachable via the current public API; erase() keeps them
//because it is written as a general sub-range primitive, ready for a future
//caller that erases a narrower range.
static void erase(lftl_ctx_t*ctx, void*const dst_nvm_addr, uintptr_t size){
  DEBUG_PRINTLN("erase entry");
  const uint32_t write_size = ctx->nvm_props->write_size;
  if(0 != ((uintptr_t)dst_nvm_addr % write_size)) raise_error(ctx,LFTL_ERROR_BASE_MISALIGNED);
  if(0 != (size % write_size)) raise_error(ctx,LFTL_ERROR_SIZE_MISALIGNED);
  const void*const current_phy_addr = translate_addr(ctx, dst_nvm_addr, size);
  const uint8_t*const current_base = slot_base(ctx,get_current_slot_index(ctx));
  const uintptr_t offset = (uintptr_t)current_phy_addr - (uintptr_t)ctx->data;
  const unsigned int index = next_slot(ctx);
  uint8_t*const base = slot_base(ctx, index);

  if(LFTL_INVALID_POINTER != ctx->transaction_tracker) raise_error(ctx,LFTL_ERROR_TRANSACTION_ONGOING);
  //erase next slot
  erase_slot(ctx,index);
  //write new data in next slot
  if(offset){
    nvm_write(ctx,ctx->nvm_props,base, current_base, offset);
  }

  const uintptr_t end_offset = offset+size;
  const uintptr_t remaining = ctx->data_size - end_offset;
  if(remaining){
    nvm_write(ctx,ctx->nvm_props,base+end_offset, current_base + end_offset, remaining);
  }
  //increment version and write new meta data in next slot
  const uint32_t version = 1 + get_slot_version(ctx, get_current_slot_index(ctx));
  write_meta(ctx, index, version);
  //update context
  ctx->data = base;
  DEBUG_PRINTLN("erase exit");
}

/**
 * \brief Check an area's configuration, raise an error if it is bad.
 *
 * Called when an area is registered, before it is linked, so that a bad
 * configuration fails there instead of corrupting memory or flash later.
 * Raises ::LFTL_ERROR_WU_SIZE_TOO_LARGE if the write unit is larger than
 * ::LFTL_WU_MAX_SIZE (which bounds stack buffers), ::LFTL_ERROR_BAD_CONFIG if:
 * - the write unit is not a power of 2 dividing the erase unit,
 * - the area is not whole erase units within its NVM,
 * - a regular area holds less than 2 slots, or its data is not whole write
 *   units (::lftl_transaction_commit copies whole write units only),
 * - an EWLF area has less than 2 data units on top of its 2 log units, or
 *   more than 256 (an entry names a data unit on one byte), less than 2
 *   records fit in a data unit (a constraint the layout macro
 *   ::LFTL_AREA_EWLF asserts too), or a log unit has no room for its
 *   certificates, its reserved pairs and 2 regular pairs;
 * - a large area has no data, a pool smaller than its chunks plus one unit
 *   (a constraint the layout macro ::LFTL_AREA_LARGE asserts too) or larger
 *   than 65535 units (a map names a unit on 16 bits), or a log unit has no
 *   room for its certificates, its reserved pairs and 2 regular pairs of
 *   its map.
 *
 * \param ctx Context of the LFTL area, with `area_type` already set
 */
static void check_config(lftl_ctx_t*ctx){
  const lftl_nvm_props_t*const nvm_props = ctx->nvm_props;
  const uint32_t write_size = nvm_props->write_size;
  const uint32_t erase_size = nvm_props->erase_size;
  if(write_size > LFTL_WU_MAX_SIZE) raise_error(ctx,LFTL_ERROR_WU_SIZE_TOO_LARGE);
  if((0 == write_size) || (0 != (write_size & (write_size-1)))) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  if((0 == erase_size) || (0 != (erase_size % write_size))) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  const uintptr_t area = (uintptr_t)ctx->area;
  const uintptr_t base = (uintptr_t)nvm_props->base;
  if((area < base) || (area - base > nvm_props->size) || (ctx->area_size > nvm_props->size - (area - base))) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  if((0 != (area - base) % erase_size) || (0 != ctx->area_size % erase_size)) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  if(lftl_is_ewlf(ctx)){
    if(n_pages(ctx) < LFTL_EWLF_LOG_UNITS + 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(ewlf_n_data_units(ctx) > 256) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(erase_size / ewlf_record_phy_size(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(ewlf_n_regular(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  } else if(lftl_is_large(ctx)){
    if(0 == ctx->data_size) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(n_pages(ctx) < LFTL_EWLF_LOG_UNITS + large_n_chunks(ctx) + 1) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(large_n_pool(ctx) > LARGE_NO_UNIT) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(ewlf_n_regular(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  } else {
    if(n_slots(ctx) < 2) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
    if(0 != (ctx->data_size % write_size)) raise_error(ctx,LFTL_ERROR_BAD_CONFIG);
  }
}

#define xstr(s) str(s)
#define str(s) #s
static const char*version = xstr(GIT_VERSION);
static const uint64_t version_timestamp = VERSION_TIMESTAMP;
static const char*build_type = xstr(BUILD_TYPE);
