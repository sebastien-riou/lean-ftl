/* lean-ftl's test firmware as a lean-bsp application: the test library's board hooks and NVM
 * accessors on top of the BSP contract (bsp_api.h), and app_main running the test suite.
 *
 * The tests use the BSP's NVM region 0. Its geometry reaches the compilation through lean-bsp
 * (LEAN_BSP_NVM0_*, read by liblean-ftl-test/include/type.h), and the test's flash data is placed
 * at the region's start (.bsp_nvm0, see nvm_state.h). The library itself is unchanged: it reaches
 * the flash only through the accessors below, which call the region's callbacks. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp_api.h"

#ifndef LEAN_BSP_NVM_REGIONS
#error "lean-ftl's tests need a BSP with an NVM region (bsp.toml [[bsp.nvm_regions]], contract 1.2)"
#endif

/* ---- the test library's board hooks ---- */

void led1(bool on) { bsp_led_set(0, on); }

bool button(void) { return bsp_button_get(0); }

void delay_ms(unsigned int ms) { bsp_delay_ms(ms); }

/* ---- the accessors the test areas are declared with: NVM region 0 ---- */

static const bsp_nvm_region_t *region0(void) { return &bsp_get_capabilities()->nvm_regions[0]; }

uint8_t nvm_erase(void *base_address, unsigned int n_pages) { return region0()->erase(base_address, n_pages); }

uint8_t nvm_write(void *dst_nvm_addr, const void *const src, uintptr_t size) {
    return region0()->write(dst_nvm_addr, src, size);
}

uint8_t nvm_read(void *dst, const void *const src_nvm_addr, uintptr_t size) {
    return region0()->read(dst, src_nvm_addr, size);
}

/* The test areas and their NVM properties: this header defines them, so it is included once, here. */
#include "lean-ftl-test.h"

/* The geometry the tests were compiled for must be the region's. */
static bool geometry_matches(void) {
    const bsp_capabilities_t *caps = bsp_get_capabilities();
    if (caps->n_nvm_regions < 1) return false;
    const bsp_nvm_region_t *r = &caps->nvm_regions[0];
    return r->page_size == LFTL_PAGE_SIZE && r->write_size == LFTL_WU_SIZE &&
           r->zero_overwrite == (bool)LFTL_ZERO_OVERWRITE && (uintptr_t)&nvm >= (uintptr_t)r->base_address &&
           (uintptr_t)&nvm + sizeof nvm <= (uintptr_t)r->base_address + r->size;
}

int app_main(void) {
    if (!geometry_matches()) {
        printf("ERROR: NVM region 0 is not what the tests were compiled for\n");
        return 1;
    }
    test_callbacks();
    return test_main(0, NULL, NULL);
}
