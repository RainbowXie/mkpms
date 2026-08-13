/* 独立验证脚本：生成非零 base payload + 跑检查 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ghostlinker.h"
static int failures=0;
#define CHECK(c,m) do{if(!(c)){fprintf(stderr,"FAIL: %s\n",m);failures++;}else printf("ok: %s\n",m);}while(0)
int main(int argc,char**argv){
    struct gh_linker_cb cb={0}; struct gh_linker_result res;
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    void*buf=malloc(sz); fread(buf,1,sz,f); fclose(f);
    CHECK(gh_link_elf(buf,sz,&cb,&res)==0,"load offbase payload");
    CHECK(res.load_base==0x400000UL,"load_base = 0x400000 (non-zero)");
    int* gv=gh_link_find_symbol(&res,"g_value");
    int**gp=gh_link_find_symbol(&res,"g_ptr");
    int(*add)(int,int)=gh_link_find_symbol(&res,"exported_add");
    CHECK(gv&&gp&&add,"all symbols found");
    if(gv) CHECK(*gv==42,"g_value == 42");
    if(gv&&gp) CHECK(*gp==gv,"g_ptr -> g_value (RELATIVE with non-zero base)");
    if(add) CHECK(add(3,4)==7,"call exported_add(3,4)==7");
    gh_link_free(&res);
    printf("failures=%d\n",failures);
    return failures?1:0;
}
