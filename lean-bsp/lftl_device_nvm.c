/* The accessors of a device library (on/stm32u5a5xj and the like): lean-ftl's nvm_erase, nvm_write
 * and nvm_read on top of the NVM region of a lean-bsp BSP, whose src/bsp_nvm.c is compiled into
 * the library with them. The flash controller's sequences live in the BSP and nowhere else; the
 * bounds they accept are those the build gives (BSP_NVM0_BASE, BSP_NVM0_SIZE, see
 * cmake/device-lib.cmake).
 *
 * They are weak, so that an application can replace one of them. */
#include <stdint.h>

#include "bsp_nvm.h"
#include "lean-ftl.h"

_Static_assert(BSP_NVM_OK == 0, "lean-ftl takes 0 as success");
_Static_assert(BSP_NVM_REFUSED == LFTL_NVM_STATUS_REFUSED, "the BSP's refused status must be lean-ftl's");

__attribute__((weak)) uint8_t nvm_erase(void *base_address, unsigned int n_pages) {
    return bsp_nvm_regions[0].erase(base_address, n_pages);
}

__attribute__((weak)) uint8_t nvm_write(void *dst_nvm_addr, const void *const src, uintptr_t size) {
    return bsp_nvm_regions[0].write(dst_nvm_addr, src, size);
}

__attribute__((weak)) uint8_t nvm_read(void *dst, const void *const src_nvm_addr, uintptr_t size) {
    return bsp_nvm_regions[0].read(dst, src_nvm_addr, size);
}
