#ifndef _ps_tcp_server_h
#define _ps_tcp_server_h

#include "ps_processor.h"
#if _WITH_PAUSE_RESUME_PROCESSOR

// define the tcpserver commands :

typedef enum tcp_command_id {
  STOP_RECORDING = 0,
  START_RECORDING = 1,
  HEALTH_CHECK = 2,
  UNDEFINED_COMMAND 
} tcp_command_id_t;

typedef struct ps_tcp_command {
  tcp_command_id_t   command_id;
  char *          command_str;
} ps_tcp_command_t;

const ps_tcp_command_t ps_tcp_commands[] = {
    { STOP_RECORDING,    "stopRecording" },
    { START_RECORDING,   "startRecording" },
    { HEALTH_CHECK,      "healthCheck" }
};


BOOL tcpserver_setup(int listen_port, int max_clients);
void tcpserver_close(void);

#endif
#endif
