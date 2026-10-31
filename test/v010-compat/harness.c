//Compatibility harness: one program, built against lean-ftl v0.1.0 (with
//-DLFTL_V010, from the sources at that tag) or against the working tree, on
//the same simulated flash: programming erased write units only, bits can
//only go from 1 to 0. Regular areas only, v0.1.0 has no EWLF.
//
//  gen  ws es data_size n_slots n_writes erase_all out_prefix
//       format, then n_writes writes of the kinds in do_write(), then
//       lftl_erase_all if erase_all; read back and check; write
//       <out_prefix>.nvm (the whole NVM), .exp (the data) and .slot
//  chk  ws es data_size n_slots zero_overwrite in_prefix [out_prefix]
//       load the image, read the area as after a reset and check it, report
//       the slot found and what the scan erased or programmed; with
//       out_prefix, write 4 more versions and write that image
//  emit name ws es data_size n_slots in_prefix
//       print the image as C: the runs of programmed write units of the
//       area, the data read back, and an entry for v010_images.h
//
//gen-images.sh runs it, see there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "lean-ftl.h"

#define NVM_SIZE (256*1024)
static uint8_t nvm[NVM_SIZE] __attribute__((aligned(8192)));
static uint32_t g_ws, g_es;
static unsigned long n_erase, n_write;
static bool g_zero_overwrite;

static int in_nvm(const void*p, uintptr_t size){
  const uintptr_t a=(uintptr_t)p, b=(uintptr_t)nvm;
  return a>=b && a-b<=NVM_SIZE && size<=NVM_SIZE-(a-b);
}
static int all(const uint8_t*p, uint8_t v, uintptr_t n){
  for(uintptr_t i=0;i<n;i++) if(p[i]!=v) return 0;
  return 1;
}
static uint8_t sim_erase(void*base, unsigned int n_pages){
  if(0==n_pages) return 0;
  const uintptr_t size=(uintptr_t)n_pages*g_es;
  if(!in_nvm(base,size)) return 1;
  if(((uintptr_t)base-(uintptr_t)nvm)%g_es) return 2;
  memset(base,0xFF,size);
  n_erase++;
  return 0;
}
static uint8_t sim_write(void*dst, const void*const src, uintptr_t size){
  if(0==size) return 0;
  if(!in_nvm(dst,size)) return 1;
  if(((uintptr_t)dst-(uintptr_t)nvm)%g_ws) return 3;
  if(size%g_ws) return 4;
  uint8_t*d=dst;
  const uint8_t*s=src;
  for(uintptr_t i=0;i<size;i+=g_ws){
    //the rule of the Linux simulator: erased units, or all zeros where the
    //flash allows it
    if(!all(d+i,0xFF,g_ws) && !(g_zero_overwrite && all(s+i,0x00,g_ws))) return 5;
  }
  for(uintptr_t i=0;i<size;i++) d[i]&=s[i];
  n_write++;
  return 0;
}
static uint8_t sim_read(void*dst, const void*const src, uintptr_t size){
  if(0==size) return 0;
  if(!in_nvm(src,size)) return 1;
  memcpy(dst,src,size);
  return 0;
}
static void err(uint32_t code){
  fprintf(stderr,"LFTL ERROR 0x%08x\n",(unsigned)code);
  exit(2);
}

//deterministic content, so that gen-images.sh reproduces the same header
static void fill(uint8_t*buf, uintptr_t size, unsigned seed){
  for(uintptr_t j=0;j<size;j++) buf[j]=(uint8_t)(seed*31u + j*7u + (j>>8));
}
//the slot layout of both versions: data, then 3 meta items of one write unit
//each, at least 4 bytes, in whole pages
static uintptr_t slot_size(uintptr_t data_size){
  const uintptr_t item = g_ws>4?g_ws:4;
  const uintptr_t min = data_size + 3*item;
  return ((min+g_es-1)/g_es)*g_es;
}

static lftl_nvm_props_t props;
static lftl_ctx_t ctx;

//the area one page above the NVM base, so that offsets are not trivially 0
static void setup(uintptr_t data_size, unsigned n_slots){
  memset(&props,0,sizeof(props));
  memset(&ctx,0,sizeof(ctx));
  props.base = nvm;
  props.size = NVM_SIZE;
  props.write_size = g_ws;
  props.erase_size = g_es;
  ctx.nvm_props = &props;
  ctx.area = nvm + g_es;
  ctx.area_size = n_slots*slot_size(data_size);
  ctx.data = LFTL_INVALID_POINTER;
  ctx.data_size = data_size;
  ctx.transaction_tracker = LFTL_INVALID_POINTER;
  ctx.next = LFTL_INVALID_POINTER;
  lftl_init_lib();
#ifdef LFTL_V010
  ctx.erase = sim_erase;
  ctx.write = sim_write;
  ctx.read = sim_read;
  ctx.error_handler = err;
#else
  props.zero_overwrite = g_zero_overwrite;
  props.erase = sim_erase;
  props.write = sim_write;
  props.read = sim_read;
  props.error_handler = err;
  props.next = LFTL_INVALID_POINTER;
  lftl_register_nvm(&props);
#endif
  lftl_register_area(&ctx);
}

static uint8_t buf[NVM_SIZE];
static uint8_t mirror[NVM_SIZE];
static uint8_t rb[NVM_SIZE];

//write i: whole data, aligned part, transaction on the first and last write
//units, or 5 bytes at an unaligned address. mirror follows
static void do_write(uintptr_t data_size, unsigned i){
  fill(buf,data_size,i);
  uint8_t*const area=ctx.area;
  switch(i%4){
  case 0:
    lftl_write(&ctx,area,buf,data_size);
    memcpy(mirror,buf,data_size);
    break;
  case 1:
    if(data_size>=4*g_ws){
      lftl_write(&ctx,area+g_ws,buf,data_size-2*g_ws);
      memcpy(mirror+g_ws,buf,data_size-2*g_ws);
    } else {
      lftl_write(&ctx,area,buf,data_size);
      memcpy(mirror,buf,data_size);
    }
    break;
  case 2: {
    uint8_t tracker[LFTL_TRANSACTION_TRACKER_SIZE(&ctx)];
    lftl_transaction_start(&ctx,tracker);
    lftl_transaction_write(&ctx,area,buf,g_ws);
    memcpy(mirror,buf,g_ws);
    if(data_size>=2*g_ws){
      lftl_transaction_write(&ctx,area+data_size-g_ws,buf+g_ws,g_ws);
      memcpy(mirror+data_size-g_ws,buf+g_ws,g_ws);
    }
    lftl_transaction_commit(&ctx);
    } break;
  case 3: {
    const uintptr_t n = data_size>=8?5:1;
    lftl_write_any(&ctx,area+1,buf,n);
    memcpy(mirror+1,buf,n);
    } break;
  }
}

static int verify(uintptr_t data_size, const char*what){
  lftl_read(&ctx,rb,ctx.area,data_size);
  if(memcmp(rb,mirror,data_size)){
    printf("MISMATCH after %s\n",what);
    return 1;
  }
  return 0;
}
static int current_slot(uintptr_t data_size){
  return (int)(((uintptr_t)ctx.data-(uintptr_t)ctx.area)/slot_size(data_size));
}

static FILE*open_part(const char*prefix, const char*ext, const char*mode){
  char name[1024];
  snprintf(name,sizeof(name),"%s.%s",prefix,ext);
  FILE*f=fopen(name,mode);
  if(!f){
    fprintf(stderr,"cannot open %s\n",name);
    exit(1);
  }
  return f;
}
static void dump(const char*prefix, uintptr_t data_size, int slot){
  FILE*f=open_part(prefix,"nvm","wb");
  fwrite(nvm,1,NVM_SIZE,f);
  fclose(f);
  f=open_part(prefix,"exp","wb");
  fwrite(mirror,1,data_size,f);
  fclose(f);
  f=open_part(prefix,"slot","w");
  fprintf(f,"%d\n",slot);
  fclose(f);
}
static int load(const char*prefix, uintptr_t data_size){
  FILE*f=open_part(prefix,"nvm","rb");
  if(NVM_SIZE!=fread(nvm,1,NVM_SIZE,f)) return 1;
  fclose(f);
  f=open_part(prefix,"exp","rb");
  if(data_size!=fread(mirror,1,data_size,f)) return 1;
  fclose(f);
  f=open_part(prefix,"slot","r");
  int slot;
  if(1!=fscanf(f,"%d",&slot)) return 1;
  fclose(f);
  return slot;
}

static void emit_bytes(const uint8_t*p, uintptr_t n){
  for(uintptr_t i=0;i<n;i++){
    printf("%s0x%02X,",(0==i%16)?"\n  ":"",p[i]);
  }
  printf("\n");
}
//the area as runs of consecutive programmed write units: everything else
//reads erased, and a test rebuilds the image with an erase and programs of
//erased units only, as real flash allows
static void emit(const char*name, uintptr_t data_size, unsigned n_slots, int slot){
  const uint8_t*const area=ctx.area;
  const uintptr_t area_size=n_slots*slot_size(data_size);
  unsigned n_runs=0;
  uintptr_t start=0;
  bool in_run=false;
  for(uintptr_t off=0;off<=area_size;off+=g_ws){
    const bool programmed = (off<area_size) && !all(area+off,0xFF,g_ws);
    if(programmed && !in_run){
      start=off;
      in_run=true;
    } else if(!programmed && in_run){
      printf("static const uint8_t v010_%s_run%u[] = {",name,n_runs);
      emit_bytes(area+start,off-start);
      printf("};\n");
      n_runs++;
      in_run=false;
    }
  }
  printf("static const v010_run_t v010_%s_runs[] = {\n",name);
  unsigned k=0;
  in_run=false;
  for(uintptr_t off=0;off<=area_size;off+=g_ws){
    const bool programmed = (off<area_size) && !all(area+off,0xFF,g_ws);
    if(programmed && !in_run){
      start=off;
      in_run=true;
    } else if(!programmed && in_run){
      printf("  { %lu, %lu, v010_%s_run%u },\n",(unsigned long)start,(unsigned long)(off-start),name,k);
      k++;
      in_run=false;
    }
  }
  printf("};\n");
  printf("static const uint8_t v010_%s_expected[] = {",name);
  emit_bytes(mirror,data_size);
  printf("};\n");
  printf("#define V010_ENTRY_%s { \"%s\", %u, %u, %lu, %lu, %d, %u, v010_%s_runs, v010_%s_expected }\n\n",
    name,name,(unsigned)g_ws,(unsigned)g_es,(unsigned long)data_size,(unsigned long)area_size,slot,n_runs,name,name);
}

int main(int argc, char**argv){
  if(argc<7){
    fprintf(stderr,"usage: see the comment at the top of harness.c\n");
    return 1;
  }
  const char*mode=argv[1];
  const int a = strcmp(mode,"emit") ? 2 : 3;
  g_ws=strtoul(argv[a],0,0);
  g_es=strtoul(argv[a+1],0,0);
  const uintptr_t data_size=strtoul(argv[a+2],0,0);
  const unsigned n_slots=strtoul(argv[a+3],0,0);
  if(!strcmp(mode,"gen")){
    if(argc<9){
      fprintf(stderr,"usage: gen ws es data_size n_slots n_writes erase_all out_prefix\n");
      return 1;
    }
    const unsigned n_writes=strtoul(argv[6],0,0);
    const int erase_all=atoi(argv[7]);
    g_zero_overwrite=false;
    memset(nvm,0xA5,NVM_SIZE); //never erased: a format must not depend on it
    setup(data_size,n_slots);
    lftl_format(&ctx);
    memset(mirror,0xFF,data_size);
    if(verify(data_size,"format")) return 3;
    for(unsigned i=1;i<=n_writes;i++){
      do_write(data_size,i);
      if(verify(data_size,"write")) return 3;
    }
    if(erase_all){
      lftl_erase_all(&ctx);
      memset(mirror,0xFF,data_size);
      if(verify(data_size,"erase_all")) return 3;
    }
    dump(argv[8],data_size,current_slot(data_size));
    printf("gen ok slot=%d erases=%lu writes=%lu\n",current_slot(data_size),n_erase,n_write);
    return 0;
  }
  if(!strcmp(mode,"chk")){
    if(argc<8){
      fprintf(stderr,"usage: chk ws es data_size n_slots zero_overwrite in_prefix [out_prefix]\n");
      return 1;
    }
    g_zero_overwrite=atoi(argv[6])!=0;
    const int slot=load(argv[7],data_size);
    if(slot<0) return 1;
    setup(data_size,n_slots);
    n_erase=0;
    n_write=0;
    if(verify(data_size,"load")) return 3;
    if(slot!=current_slot(data_size)){
      printf("SLOT MISMATCH: image %d, found %d\n",slot,current_slot(data_size));
      return 3;
    }
    printf("chk ok slot=%d scan_erases=%lu scan_writes=%lu",current_slot(data_size),n_erase,n_write);
    if(argc>8){
      for(unsigned i=100;i<104;i++){
        do_write(data_size,i);
        if(verify(data_size,"write")) return 3;
      }
      dump(argv[8],data_size,current_slot(data_size));
      printf(" then wrote 4 versions, slot=%d",current_slot(data_size));
    }
    printf("\n");
    return 0;
  }
  if(!strcmp(mode,"emit")){
    if(argc<8){
      fprintf(stderr,"usage: emit name ws es data_size n_slots in_prefix\n");
      return 1;
    }
    const int slot=load(argv[7],data_size);
    if(slot<0) return 1;
    setup(data_size,n_slots);
    emit(argv[2],data_size,n_slots,slot);
    return 0;
  }
  return 1;
}
