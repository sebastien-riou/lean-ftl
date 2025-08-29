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
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "type.h"
#include "error.h"

static int create_virtual_com_port(){
  const int BAUDRATE = B115200;
  int fd;
  fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  if (fd == -1) {
    printf("error opening file\n");
    return -1;
  }

  grantpt(fd);
  unlockpt(fd);

  char* pts_name = ptsname(fd);
  printf("ptsname: %s\n",pts_name);
  struct termios newtio;
  memset(&newtio, 0, sizeof(newtio));
  struct termios oldtio;
  tcgetattr(fd, &oldtio);

  newtio = oldtio;
  newtio.c_cflag = CS8 | CLOCAL | CREAD;
  newtio.c_iflag = 0;
  newtio.c_oflag = 0;
  newtio.c_lflag = 0;
  newtio.c_cc[VMIN] = 1;
  newtio.c_cc[VTIME] = 0;
  tcflush(fd, TCIFLUSH);
  cfsetispeed(&newtio, BAUDRATE);
  cfsetospeed(&newtio, BAUDRATE);
  tcsetattr(fd, TCSANOW, &newtio);
  return fd;
}

void write_file(const char*name, const void*const buf, size_t size){
    FILE *fd = fopen(name,"wb");
    if(!fd){
      printf("Could not create file '%s' for write\n",name);
      abort();
    }
    fwrite(buf,size,1,fd);
    fclose(fd);
}

void read_file(const char*name, void*const buf, size_t*size){
    FILE *fd = fopen(name,"rb");
    if(!fd){
      *size=0;
      return;
    }
    int ch;
    uint8_t*buf8 = (uint8_t*)buf;
    size_t cnt=0;
    while(EOF != (ch = fgetc(fd))){
        if(*size == cnt) {
            printf("file too big: %s\n",name);
            abort();
        }
        *buf8++ = ch;
        cnt++;
    }
    *size = cnt;
    fclose(fd);
}

static int com_port = 0; 
static unsigned int button_sampling_cnt = 0;
static bool test_mode = 0;


extern data_flash_t nvm __attribute__ ((section (".data_flash")));
const char*save_nvm_file_name = 0;


uint32_t nvm_alignement;

uint32_t get_nvm_alignement(){
  const uint32_t max_alignement = 1<<31;
  uintptr_t base = ((uintptr_t)&nvm);
  if(0==base) return max_alignement;
  uintptr_t alignement = 1;
  while(0 == (base & 1)){
    alignement = alignement << 1;
    base = base >> 1;
  };
  if(alignement>max_alignement) return max_alignement;
  return (uint32_t)alignement;
}

void init_nvm_alignement(){
  nvm_alignement = get_nvm_alignement();
}

bool nvm_write64(uint64_t*nvm_addr, const uint64_t*const buf, uint32_t size){
  memcpy(nvm_addr,buf,size);
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,&nvm,sizeof(nvm));
  }
  return 0;
}
void nvm_read64(uint64_t*nvm_addr, uint64_t*buf, uint32_t size){
  memcpy(buf,nvm_addr,size);
}

const void* nvm_base = &nvm;
const uintptr_t nvm_size = sizeof(nvm);
//Application level HAL
void init(int argc, const char*argv[]){
  init_nvm_alignement();
  for(int i=1;i<argc;i++){
    const char*test_mode_str = "--test-mode";
    if(0==memcmp(argv[i],test_mode_str,strlen(test_mode_str)+1)){
      test_mode = 1;
      continue;
    }
    printf("ERROR unsupported command line argument: '%s'\n",argv[i]);
    abort();
  }
  com_port = create_virtual_com_port();
}
void led1(bool on){
  printf("led1: %d\n",on);
}
bool button(){
  static unsigned int test_cnt = 0;
  printf("button sampling %d,press 1/0: ",button_sampling_cnt++);
  uint8_t c=0;
  if(!test_mode){
    c = getchar();getchar();
    if('T' == c){
      //test mode: always answer pattern 0,1,0
      test_mode = 1;
    }
  }
  if(test_mode){
    if((test_cnt % 3) == 1) c = '1';
    else c = '0';
    test_cnt++;
  }
  bool button = '1' == c;
  printf(" --> %d\n",button);
  return button;
}
void com_tx(const void *const buf, unsigned int size){
  int status = write(com_port,buf,size);
  (void)(status);
}
void com_rx(void *const buf, unsigned int size){
  uint8_t*const buf8 = (uint8_t*const)buf;
  uint8_t inputbyte;
  for(unsigned int i=0;i<size;i++){
    while(read(com_port, &inputbyte, 1) != 1){
      i=0;//com is broken, reset the buffer
      sleep(1);
    };
    buf8[i] = inputbyte;
  }
}
void delay_ms(unsigned int ms){
  struct timespec ts;
  int res;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000;

  do {
      res = nanosleep(&ts, &ts);
  } while (res && errno == EINTR);
}
