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

//Application level HAL
void init(int argc, const char*argv[]){
  nvm_alignement = get_nvm_alignement();
  for(int i=1;i<argc;i++){
    const char*test_mode_str = "--test-mode";
    const char*nvm_file_str = "--nvm-file=";
    const char*save_nvm_file_str = "--save-nvm-file=";
    if(0==memcmp(argv[i],test_mode_str,strlen(test_mode_str)+1)){
      test_mode = 1;
      continue;
    }
    if(0==memcmp(argv[i],nvm_file_str,strlen(nvm_file_str))){
      const char*nvm_file_name = argv[i]+strlen(nvm_file_str);
      size_t size = sizeof(nvm);
      read_file(nvm_file_name,&nvm,&size);
      if(size!=sizeof(nvm)) {
        printf("ERROR incorrect nvm file size: %ld bytes,  %ld bytes are expected\n",size,sizeof(nvm));
        abort();
      }
      continue;
    }
    if(0==memcmp(argv[i],save_nvm_file_str,strlen(save_nvm_file_str))){
      save_nvm_file_name = argv[i]+strlen(save_nvm_file_str);
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


const uint32_t nvm_write_size = 16;
const uint32_t nvm_erase_size = 8*1024;

uint64_t tearing_sim_cnt=0;
uint64_t tearing_sim_target_cnt = -1;

uint32_t tearing_sim_get_max_target(){
  return tearing_sim_cnt / nvm_write_size;
}

void tearing_sim_set_target(uint64_t target_write){
  tearing_sim_cnt=0;
  tearing_sim_target_cnt = target_write * nvm_write_size;
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

bool tearing_sim(void*base_address, uint32_t size){
  const uint32_t tsize = tearing_size(size);
  if(tsize){
    //corrupt the last tsize bytes to simulate a tearing
    //we prefer corrupting the data rather than exactly simulating 
    //a tearing that would keep old data because in some case
    //a tearing could go undetected (old data may match new data)
    const uint32_t offset = size - tsize;
    for(uint32_t i = offset;i<tsize;i++){
      uint8_t*dst = (uint8_t*)base_address+i;
      *dst ^= 0x55; 
    }
  } 
  return tsize ? 1:0;
}
#include "lean-ftl.h"
lftl_nvm_props_t nvm_props = {
    .base = &nvm,
    .size = sizeof(nvm),
    .write_size = nvm_write_size,
    .erase_size = nvm_erase_size,
  };

uint32_t get_alignement_requirement(uint32_t original_req){
  if(original_req > nvm_alignement) return nvm_alignement;
  return original_req;
}

uint8_t nvm_erase(void*base_address, unsigned int n_pages){
  const uintptr_t size = n_pages * nvm_erase_size;
  if(base_address < (void*)&nvm) return 1;
  if(((uintptr_t)base_address + size) > ((uintptr_t)&nvm + sizeof(nvm))) return 2;
  //Linux makes it hard to get nvm aligned to large units like 4k or 8k, so we check against the minimum between the original constraint and the alignement of nvm
  if(0 != ((uintptr_t)base_address % get_alignement_requirement(nvm_erase_size))) return 3;
  
  memset(base_address, 0xFF, size);
  bool tearing = tearing_sim(base_address,size);
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,&nvm,sizeof(nvm));
  }
  return tearing ? SIMULATED_TEARING:0;
}

uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(dst_nvm_addr < (void*)&nvm) return 1;
  if(((uintptr_t)dst_nvm_addr + size) > ((uintptr_t)&nvm + sizeof(nvm))) return 2;
  if(0 != ((uintptr_t)dst_nvm_addr % nvm_write_size)) return 3;
  if(0 != (size % nvm_write_size)) return 4;
  memcpy(dst_nvm_addr,src,size);
  bool tearing = tearing_sim(dst_nvm_addr,size);
  if(save_nvm_file_name){
    write_file(save_nvm_file_name,&nvm,sizeof(nvm));
  }
  return tearing ? SIMULATED_TEARING:0;
}

uint8_t nvm_read(void* dst, const void*const src_nvm_addr, uintptr_t size){
  memcpy(dst,src_nvm_addr,size);
  return 0;
}