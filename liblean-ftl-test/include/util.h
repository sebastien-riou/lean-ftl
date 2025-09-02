#pragma once

#define SIZE64(size) (((size)+7)/8)

#define xstr(s) str(s)
#define str(s) #s

#ifdef HAS_PRINTF
  #define PRINTF(...) printf( __VA_ARGS__ )
#else
  #define PRINTF(...)
#endif
