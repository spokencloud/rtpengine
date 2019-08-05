#ifndef _stream_tracker_h_
#define _stream_tracker_h_
#include "ps_processor.h"

#if _WITH_PAUSE_RESUME_PROCESSOR
#include "../ahclient/ahclient.h"   // RTP_HEADER_SIZE

#define RTP_HEADER_SIZE (40)
#define RTP_EXTENSION_SIZE (RTP_HEADER_SIZE - 20)

typedef struct rtp_header {
    uint16_t part_1;  // Combined of : Version/Padding?Extension/CSRC count/M/Payload Type  copy from stream
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
    uint32_t csrc;
    uint16_t header_id;
    uint16_t header_length;
    unsigned char raw[RTP_EXTENSION_SIZE];
} rtp_header_t;


typedef struct stream_tracker {

    const stream_t    *   stream;

    BOOL            is_paused;
    unsigned long   timestamp;   // previous sent rtp package timestamp
    uint16_t        sequence_number;   // previous sent rtp package's sequence_number
    unsigned int    mask_beep_offset;
    pthread_t       sending_thread;
    pthread_mutex_t tracker_pack_mutex;

    unsigned int    pack_size;
    unsigned long   delta_ts;
    unsigned int    delta_time_ms;

    rtp_header_t    rtp_header;

} stream_tracker_t;

stream_tracker_t * new_stream_tracker(const stream_t * stream);
void async_delete_stream_tracker(stream_tracker_t * tracker);

BOOL track_stream(stream_tracker_t * tracker, const stream_t  * stream, const unsigned char * buf, int len);

void stream_pause(stream_tracker_t * tracker);
void async_stream_resume(stream_tracker_t * tracker);


#endif 
#endif