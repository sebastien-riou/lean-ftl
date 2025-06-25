#pragma once

#include <stdint.h>

#define CONST_OK 0
#define CONST_NOT_SET 0xFFFFFFFF

#define INTERNAL_ERROR_CORRUPT              0x00000005
#define ERROR_BUTTON_PRESSED                0x00000008
#define INTERNAL_ERROR_NVM_WRITE            0x00000009
#define ERROR_VERIFICATION_FAIL             0x0000000A
#define ERROR_NOT_IMPLEMENTED               0x0000000B

void throw_exception(uint32_t err_code);