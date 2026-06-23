#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "patchdl_websrv.h"

#ifndef PATCHDL_HTTP_PORT
#define PATCHDL_HTTP_PORT 12880
#endif

int
main(int argc, char** argv) {
  unsigned short port = PATCHDL_HTTP_PORT;

  if(argc > 1) {
    int requested_port = atoi(argv[1]);
    if(requested_port > 0 && requested_port <= 65535) {
      port = (unsigned short)requested_port;
    }
  }

  signal(SIGPIPE, SIG_IGN);

  printf("PatchDL starting web UI on port %u\n", port);

  if(patchdl_websrv_start(port)) {
    puts("PatchDL web server failed to start");
    return 1;
  }

  while(1) {
    sleep(60);
  }

  return 0;
}
