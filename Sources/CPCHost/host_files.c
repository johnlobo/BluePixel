#include "cpc_host.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CPCDisc *dsk_load_file(const char *path) {
    FILE *f=fopen(path,"rb"); if(!f)return NULL;
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return NULL;} long len=ftell(f);
    if(len<=0||fseek(f,0,SEEK_SET)!=0){fclose(f);return NULL;}
    uint8_t *data=malloc((size_t)len);if(!data){fclose(f);return NULL;}
    bool ok=fread(data,1,(size_t)len,f)==(size_t)len;fclose(f);
    CPCDisc *d=ok?dsk_load(data,(size_t)len,path):NULL;free(data);return d;
}

bool dsk_save_file(CPCDisc *disc,const char *path) {
    if(!disc||!path)return false;size_t size=0;uint8_t *data=dsk_serialize(disc,&size);if(!data)return false;
    FILE *f=fopen(path,"wb");if(!f){free(data);return false;}bool ok=fwrite(data,1,size,f)==size;
    if(fclose(f)!=0)ok=false;free(data);
    if(ok){disc->dirty=false;if(path!=disc->path)strncpy(disc->path,path,sizeof disc->path-1);}
    return ok;
}

int dbg_symbols_load(CPC *cpc,const char *path) {
    FILE *f=fopen(path,"rb");if(!f)return -1;
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return -1;}long len=ftell(f);
    if(len<0||fseek(f,0,SEEK_SET)!=0){fclose(f);return -1;}
    char *text=malloc((size_t)len+1);if(!text){fclose(f);return -1;}
    bool ok=fread(text,1,(size_t)len,f)==(size_t)len;fclose(f);text[len]=0;
    int result=ok?dbg_symbols_parse(cpc,text,(size_t)len):-1;free(text);return result;
}
