#define _POSIX_C_SOURCE 200809L
#include "cpc_server.h"
#include <stdlib.h>
#include <string.h>

int cpc_server_stdio(CPCApiSession *session,FILE *input,FILE *output){
    if(!session||!input||!output)return 2;
    char *line=NULL;size_t capacity=0;int status=0;
    while(getline(&line,&capacity,input)>=0){
        size_t n=strlen(line);while(n&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
        if(!n)continue;
        char *reply=cpc_api_dispatch(session,line);
        if(!reply){fputs("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"internal error\"}}\n",output);status=1;}
        else{fputs(reply,output);fputc('\n',output);free(reply);}
        if(fflush(output)!=0){status=1;break;}
    }
    free(line);return status;
}
