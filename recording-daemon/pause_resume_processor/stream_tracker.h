#ifndef _stream_tracker_h_
#define _stream_tracker_h_
#include "ps_processor.h"

#if _WITH_PAUSE_RESUME_PROCESSOR

#define STREAM_HEADER_SIZE (40)

typedef struct stream_header {

    unsigned char leading[30];
    unsigned char sequence_number[2];   // BIG_ENDIAN in the packet, use get_sn() and fill_sn() to transfer to/from uint
    unsigned char timestamp[6];         // BIG_ENDIAN in the packet, use get_ts() and fill_ts() to transfer to/from uint
    unsigned char ending[4];
} stream_header_t;


typedef struct stream_tracker {

    stream_t    *   stream;

    BOOL            is_paused;
    uint32_t        timestamp;   // previous sent rtp package timestamp
    uint16_t        sequence_number;   // previous sent rtp package's sequence_number
    unsigned int    mask_beep_offset;
    pthread_t       sending_thread;
    pthread_mutex_t tracker_pack_mutex;

    unsigned int    pack_size;
    uint32_t        delta_ts;
    unsigned int    delta_time_ms;

    stream_header_t  stream_header;

} stream_tracker_t;

stream_tracker_t * new_stream_tracker(stream_t * stream);
void async_delete_stream_tracker(stream_tracker_t * tracker);

BOOL track_stream(stream_tracker_t * tracker, stream_t  * stream, const unsigned char * buf, int len);

void stream_pause(stream_tracker_t * tracker);
void async_stream_resume(stream_tracker_t * tracker);


#endif 
#endif