#define _POSIX_C_SOURCE 200809L
#include "cpc_server.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CPC_SERVER_MAX_LINE (1024u * 1024u)

static int dispatch_line(CPCApiSession *session,const char *line,FILE *output){
    char *reply=cpc_api_dispatch(session,line);
    if(!reply){fputs("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"internal error\"}}\n",output);}
    else{fputs(reply,output);fputc('\n',output);free(reply);}
    return fflush(output)==0?0:1;
}

int cpc_server_stdio(CPCApiSession *session,FILE *input,FILE *output){
    if(!session||!input||!output)return 2;
    char *line=malloc(CPC_SERVER_MAX_LINE+1);if(!line)return 1;
    size_t n=0;bool oversized=false;int ch,status=0;
    while((ch=fgetc(input))!=EOF){
        if(ch!='\n'){
            if(!oversized){if(n<CPC_SERVER_MAX_LINE)line[n++]=(char)ch;else oversized=true;}
            continue;
        }
        while(n&&(line[n-1]=='\r'||line[n-1]=='\n'))n--;
        line[n]=0;
        if(oversized){
            fputs("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"request line exceeds 1048576 bytes\"}}\n",output);
            if(fflush(output)!=0){status=1;break;}
        }else if(n&&dispatch_line(session,line,output)){status=1;break;}
        n=0;oversized=false;
    }
    if(!status&&(n||oversized)){
        if(oversized)fputs("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"request line exceeds 1048576 bytes\"}}\n",output);
        else{while(n&&(line[n-1]=='\r'||line[n-1]=='\n'))n--;line[n]=0;if(n)status=dispatch_line(session,line,output);}
        if(!status&&fflush(output)!=0)status=1;
    }
    free(line);return status;
}
