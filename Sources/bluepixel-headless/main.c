#include "cpc_api.h"
#include "cpc_server.h"
#include "cpc_host.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define BLUEPIXEL_VERSION "0.2.0"
static bool executable_dir(char*out,size_t cap){char path[4096];
#if defined(__APPLE__)
 uint32_t n=sizeof path;if(_NSGetExecutablePath(path,&n)!=0)return false;
#else
 ssize_t n=readlink("/proc/self/exe",path,sizeof path-1);if(n<0)return false;path[n]=0;
#endif
 char*slash=strrchr(path,'/');if(!slash)return false;*slash=0;if(strlen(path)+1>cap)return false;strcpy(out,path);return true;}
static const char*resolve_rom_dir(const char*arg,const char*exe){if(arg)return arg;const char*env=getenv("BLUEPIXEL_ROM_DIR");if(env&&*env)return env;static char path[4096],manifest[4096];if(exe&&snprintf(path,sizeof path,"%s/roms",exe)<(int)sizeof path&&snprintf(manifest,sizeof manifest,"%s/roms/roms.json",exe)<(int)sizeof manifest&&access(manifest,R_OK)==0)return path;const char*home=getenv("HOME");if(home&&snprintf(path,sizeof path,"%s/.config/bluepixel/roms",home)<(int)sizeof path)return path;return NULL;}
int main(int argc,char**argv){
 if(argc>1&&!strcmp(argv[1],"--version")){puts("bluepixel-headless " BLUEPIXEL_VERSION);return 0;}
 const char*rom_arg=NULL;for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--rom-dir")&&i+1<argc)rom_arg=argv[++i];else if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")){puts("bluepixel-headless [--rom-dir PATH] [--version]\nReads one JSON-RPC 2.0 request per stdin line; writes one response per line.");return 0;}else{fprintf(stderr,"unknown argument: %s\n",argv[i]);return 2;}}
 char exe[4096];bool have_exe=executable_dir(exe,sizeof exe);const char*rom_dir=resolve_rom_dir(rom_arg,have_exe?exe:NULL);
 CPCApiSession*s=cpc_api_create();if(!s){fputs("failed to create CPC session\n",stderr);return 1;}
 char error[768];if(!rom_dir||!bp_roms_load(s,rom_dir,error,sizeof error)){fprintf(stderr,"%s\n",rom_dir?error:"cannot locate ROM directory; use --rom-dir or BLUEPIXEL_ROM_DIR");cpc_api_destroy(s);return 1;}
 int status=cpc_server_stdio(s,stdin,stdout);cpc_api_destroy(s);return status;
}
