#include "lean-ftl-test.h"
void init(int argc, const char*argv[]);
int test_main();
void test_callbacks();
int main(int argc, const char*argv[]){
  init(argc,argv);
  test_callbacks();
  return test_main();
}


