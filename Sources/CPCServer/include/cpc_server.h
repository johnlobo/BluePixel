#ifndef BLUEPIXEL_CPC_SERVER_H
#define BLUEPIXEL_CPC_SERVER_H
#include <stdio.h>
#include "cpc_api.h"
int cpc_server_stdio(CPCApiSession *session, FILE *input, FILE *output);
#endif
