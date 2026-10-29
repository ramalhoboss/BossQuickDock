#include "native_mux.h"
#include <stdlib.h>
int main(int argc,char **argv){if(argc!=4)return 2;return native_mux(argv[1],argv[2],atoi(argv[3]))?1:0;}
