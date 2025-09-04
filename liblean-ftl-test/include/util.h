#pragma once

#define SIZE64(size) (((size)+7)/8)

#define xstr(s) str(s)
#define str(s) #s

#ifdef HAS_PRINTF
  #include <stdio.h>
  #define PRINTF(...) printf( __VA_ARGS__ )
#else
  #define PRINTF(...)
#endif
#define PRINTLN(...) do{PRINTF( __VA_ARGS__ );PRINTF("\n\r");}while(0)
#ifdef LFTL_DEBUG
  #define DEBUG_PRINTF(...) PRINTF( __VA_ARGS__ )
  #define DEBUG_PRINTLN(...) PRINTLN( __VA_ARG__ )
  #define DEBUG_DUMP_CORE(addr,size,display_addr) dump(addr,size,display_addr)
  #define DEBUG_DUMP(addr,size) dump(addr,size)
#else
  #define DEBUG_PRINTF(...)
  #define DEBUG_PRINTLN(...)
  #define DEBUG_DUMP_CORE(addr,size,display_addr)
  #define DEBUG_DUMP(addr,size)
#endif