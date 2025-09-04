#pragma once

#define CONST_OK 0
#define CONST_NOT_SET 0xFFFFFFFF

#define INTERNAL_ERROR_CORRUPT              0xFF000005
#define ERROR_BUTTON_PRESSED                0xFF000008
#define INTERNAL_ERROR_NVM_WRITE            0xFF000009
#define ERROR_VERIFICATION_FAIL             0xFF00000A
#define ERROR_NOT_IMPLEMENTED               0xFF00000B

void throw_exception(uint32_t err_code);