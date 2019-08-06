#ifndef _ps_processor_h_
#define _ps_processor_h_
// The following section is used to make the code enable in IDE 
// _WITH_AH_CLIENT will be injected from Makefile during compile
#ifndef _WITH_PAUSE_RESUME_PROCESSOR
#define _WITH_PAUSE_RESUME_PROCESSOR 1
#endif

#if _WITH_PAUSE_RESUME_PROCESSOR
#include "stream.h"

#ifndef BOOL
typedef unsigned int BOOL;
#endif
#ifndef TRUE 
#define TRUE    (1)
#endif  
#ifndef FALSE
#define FALSE   (0)
#endif

 typedef enum channel_id {
    LEFT_CHANNEL = 0,
    RIGHT_CHANNEL = 1,
    ALL_CHANNELS = -1
} channel_id_t;

void init_ps_processor(int listen_port, int max_clients);
void destroy_ps_processor(void);

BOOL ps_processor_process_stream(stream_t * stream, const unsigned char * buf, int len);
void ps_processor_close_stream(stream_t * stream);


// Could provide the the ability to pasuer/resume on a certain channel
void pause_stream( char * call_id, channel_id_t id);
void resume_stream(char * call_id, channel_id_t id);

#endif 

#endif