#include <string.h>
#include "lean-ftl-test.h"
void init(int argc, const char*argv[], bool consumed[]);
int test_main(int argc, const char*argv[], bool consumed[]);

int main(int argc, const char*argv[]){
  bool consumed[argc];
  memset(consumed,0,sizeof(consumed));
  init(argc,argv,consumed);
  return test_main(argc,argv,consumed);
}


