#include "cpc_host.h"
#include "sha256.h"
#define JSMN_STATIC
#include "../CPCApi/jsmn.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int next_token(jsmntok_t*t,int count,int i){int j=i+1;while(j<count&&t[j].start<t[i].end)j++;return j;}
static int field(const char*json,jsmntok_t*t,int count,int object,const char*name){if(object<0||t[object].type!=JSMN_OBJECT)return-1;for(int i=object+1;i<count&&i<next_token(t,count,object);){int v=i+1;if(t[i].type==JSMN_STRING&&t[i].end-t[i].start==(int)strlen(name)&&!memcmp(json+t[i].start,name,strlen(name)))return v;i=next_token(t,count,v);}return-1;}
static bool token_string(const char*j,jsmntok_t*t,int i,char*out,size_t n){if(i<0||t[i].type!=JSMN_STRING)return false;size_t z=(size_t)(t[i].end-t[i].start);if(z+1>n)return false;memcpy(out,j+t[i].start,z);out[z]=0;return true;}
static bool token_number(const char*j,jsmntok_t*t,int i,long*out){if(i<0||t[i].type!=JSMN_PRIMITIVE)return false;char b[32];int n=t[i].end-t[i].start;if(n<=0||n>=(int)sizeof b)return false;memcpy(b,j+t[i].start,(size_t)n);b[n]=0;char*e;long v=strtol(b,&e,10);if(*e)return false;*out=v;return true;}
static uint8_t*read_file(const char*path,size_t*n){FILE*f=fopen(path,"rb");if(!f)return NULL;if(fseek(f,0,SEEK_END)!=0){fclose(f);return NULL;}long length=ftell(f);if(length<=0||fseek(f,0,SEEK_SET)!=0){fclose(f);return NULL;}*n=(size_t)length;uint8_t*b=malloc(*n);if(!b){fclose(f);return NULL;}bool ok=fread(b,1,*n,f)==*n;fclose(f);if(!ok){free(b);return NULL;}return b;}
static char*base64(const uint8_t*b,size_t n){static const char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";size_t cap=((n+2)/3)*4;char*r=malloc(cap+1);if(!r)return NULL;size_t j=0;for(size_t i=0;i<n;i+=3){unsigned v=(unsigned)b[i]<<16;if(i+1<n)v|=(unsigned)b[i+1]<<8;if(i+2<n)v|=b[i+2];r[j++]=a[(v>>18)&63];r[j++]=a[(v>>12)&63];r[j++]=i+1<n?a[(v>>6)&63]:'=';r[j++]=i+2<n?a[v&63]:'=';}r[j]=0;return r;}
static bool sha_matches(const uint8_t*b,size_t n,const char*expected){if(strlen(expected)!=64)return false;uint8_t digest[32];bp_sha256(b,n,digest);for(int i=0;i<32;i++){char got[3];snprintf(got,sizeof got,"%02x",digest[i]);if(tolower((unsigned char)expected[i*2])!=got[0]||tolower((unsigned char)expected[i*2+1])!=got[1])return false;}return true;}
static bool safe_filename(const char*s){return*s&&strcmp(s,".")&&strcmp(s,"..")&&!strchr(s,'/')&&!strchr(s,'\\')&&s[0]!='.';}
static bool fail(char*error,size_t n,const char*role,const char*path,const char*why){if(error&&n)snprintf(error,n,"ROM %s (%s): %s",role,path,why);return false;}
bool bp_roms_load(CPCApiSession*session,const char*rom_dir,char*error,size_t error_size){
 if(!session||!rom_dir)return fail(error,error_size,"manifest",rom_dir?rom_dir:"(null)","invalid argument");
 char manifest[4096];if(snprintf(manifest,sizeof manifest,"%s/roms.json",rom_dir)>=(int)sizeof manifest)return fail(error,error_size,"manifest",rom_dir,"path too long");
 size_t json_size=0;uint8_t*json_bytes=read_file(manifest,&json_size);if(!json_bytes)return fail(error,error_size,"manifest",manifest,"cannot read roms.json");
 jsmntok_t tokens[2048];jsmn_parser parser;jsmn_init(&parser);int count=jsmn_parse(&parser,(char*)json_bytes,json_size,tokens,2048);const char*roles[]={"os","basic","amsdos"};bool ok=count>0&&tokens[0].type==JSMN_OBJECT;
 long format_version=0;if(!ok||!token_number((char*)json_bytes,tokens,field((char*)json_bytes,tokens,count,0,"format"),&format_version)||format_version!=1){free(json_bytes);return fail(error,error_size,"manifest",manifest,"unsupported or invalid format");}
 int machines=ok?field((char*)json_bytes,tokens,count,0,"machines"):-1;int machine=machines>=0?field((char*)json_bytes,tokens,count,machines,"cpc6128"):-1;
 if(machine<0)ok=false;
 for(int r=0;r<3&&ok;r++){
  int entry=field((char*)json_bytes,tokens,count,machine,roles[r]);char filename[512],expected[80];long declared=0;
  if(entry<0||!token_string((char*)json_bytes,tokens,field((char*)json_bytes,tokens,count,entry,"file"),filename,sizeof filename)||!token_string((char*)json_bytes,tokens,field((char*)json_bytes,tokens,count,entry,"sha256"),expected,sizeof expected)||!token_number((char*)json_bytes,tokens,field((char*)json_bytes,tokens,count,entry,"size"),&declared)||declared!=16384||!safe_filename(filename)){fail(error,error_size,roles[r],manifest,"invalid manifest entry");ok=false;break;}
  char path[4096];if(snprintf(path,sizeof path,"%s/%s",rom_dir,filename)>=(int)sizeof path){fail(error,error_size,roles[r],filename,"path too long");ok=false;break;}
  size_t size=0;uint8_t*data=read_file(path,&size);if(!data){fail(error,error_size,roles[r],path,"cannot read file");ok=false;break;}
  if(size!=16384||size!=(size_t)declared||!sha_matches(data,size,expected)){free(data);fail(error,error_size,roles[r],path,"size or SHA-256 mismatch");ok=false;break;}
  char*encoded=base64(data,size);free(data);if(!encoded){fail(error,error_size,roles[r],path,"out of memory");ok=false;break;}
  size_t request_size=strlen(encoded)+128;char*request=malloc(request_size);if(!request){free(encoded);fail(error,error_size,roles[r],path,"out of memory");ok=false;break;}
  snprintf(request,request_size,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"rom.load\",\"params\":{\"role\":\"%s\",\"data_base64\":\"%s\"}}",roles[r],encoded);
  char*reply=cpc_api_dispatch(session,request);bool loaded=reply&&strstr(reply,"\"result\":true");if(!loaded)fail(error,error_size,roles[r],path,"CPCApi rejected ROM");free(reply);free(request);free(encoded);ok=loaded;
 }
 free(json_bytes);return ok;
}
