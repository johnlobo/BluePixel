#include "cpc_api.h"
#include "cpc_server.h"
#include "cpc_host.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define BLUEPIXEL_VERSION "0.3.0"
static volatile sig_atomic_t stopping;
static void stop_signal(int sig){(void)sig;stopping=1;}
static char*network_dispatch(void*context,const char*request){return cpc_api_dispatch(context,request);}
static void network_cancel(void*context){cpc_api_cancel(context);}
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
 const char*rom_arg=NULL,*unix_path=NULL,*token_file=NULL;int port=-1;char origins[2048]="";
 for(int i=1;i<argc;i++){
  const char*arg=argv[i];
  if(!strcmp(arg,"--rom-dir")&&i+1<argc)rom_arg=argv[++i];
  else if(!strcmp(arg,"--unix")&&i+1<argc)unix_path=argv[++i];
  else if(!strcmp(arg,"--token-file")&&i+1<argc)token_file=argv[++i];
  else if(!strcmp(arg,"--websocket")&&i+1<argc){char*end;errno=0;long value=strtol(argv[++i],&end,10);if(errno||!*argv[i]||*end||value<0||value>65535){fputs("invalid WebSocket port\n",stderr);return 2;}port=(int)value;}
  else if(!strcmp(arg,"--origin")&&i+1<argc){const char*value=argv[++i];size_t used=strlen(origins),n=strlen(value);if(!n||used+n+2>=sizeof origins){fputs("invalid Origin list\n",stderr);return 2;}if(used)strcat(origins,",");strcat(origins,value);}
  else if(!strcmp(arg,"--help")||!strcmp(arg,"-h")){puts("bluepixel-headless [--rom-dir PATH] [--version]\nDefault: one JSON-RPC request per stdin line. No listeners.\nOptional control server: --unix PATH and/or --websocket PORT (0: automatic loopback port)\n  --token-file PATH   owner-only token file, created if missing; required for listeners\n  --origin ORIGIN     exact browser Origin to allow; repeat for more than one\nUnix: send AUTH <token> followed by a newline; wait for OK, then JSON-RPC lines.\nWebSocket: Authorization: Bearer <token>, or subprotocols bluepixel.v1 and bluepixel.token.<token>.\nSIGINT/SIGTERM stops listeners and removes the owned Unix socket.");return 0;}
  else{fprintf(stderr,"unknown or incomplete argument: %s\n",arg);return 2;}
 }
 bool network=unix_path||port>=0;
 if(!network&&(token_file||*origins)){fputs("--token-file/--origin require --unix or --websocket\n",stderr);return 2;}
 if(network&&!token_file){fputs("listeners require --token-file\n",stderr);return 2;}
 char exe[4096];bool have_exe=executable_dir(exe,sizeof exe);const char*rom_dir=resolve_rom_dir(rom_arg,have_exe?exe:NULL);
 CPCApiSession*s=cpc_api_create();if(!s){fputs("failed to create CPC session\n",stderr);return 1;}
 char error[768];if(!rom_dir||!bp_roms_load(s,rom_dir,error,sizeof error)){fprintf(stderr,"%s\n",rom_dir?error:"cannot locate ROM directory; use --rom-dir or BLUEPIXEL_ROM_DIR");cpc_api_destroy(s);return 1;}
 int status=0;
 if(network){char token[65];if(cpc_server_token_file(token_file,token,error,sizeof error)){fprintf(stderr,"%s\n",error);cpc_api_destroy(s);return 1;}
  cpc_api_set_transports(s,(unix_path?CPC_API_UNIX:0)|(port>=0?CPC_API_WEBSOCKET:0));
  CPCServerConfig config={unix_path,port,token,origins,network_dispatch,s,network_cancel};
  struct sigaction action={0};action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);
  CPCServer*server=cpc_server_start(&config,error,sizeof error);if(!server){fprintf(stderr,"%s\n",error);cpc_api_destroy(s);return 1;}
  printf("{\"server_ready\":true,\"websocket_port\":%d}\n",cpc_server_websocket_port(server));fflush(stdout);
  while(!stopping){struct timespec interval={0,100000000};nanosleep(&interval,NULL);}cpc_server_stop(server);
 }else status=cpc_server_stdio(s,stdin,stdout);
 cpc_api_destroy(s);return status;
}
