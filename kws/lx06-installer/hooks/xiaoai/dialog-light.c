#define _POSIX_C_SOURCE 200809L
#include "dialog-light.h"
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    for(int i=0;i<LX06_LIGHT_COUNT;i++)
        if(!strcmp(argv[1],lx06_light_name(i))) return lx06_dialog_light(i) ? 1 : 0;
    if(!strcmp(argv[1],"idle")) return lx06_dialog_light(LX06_LIGHT_END) ? 1 : 0;
    return 2;
}
