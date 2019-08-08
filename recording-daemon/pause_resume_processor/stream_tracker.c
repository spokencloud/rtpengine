#include "ps_processor.h"

#if _WITH_PAUSE_RESUME_PROCESSOR
#include "stream_tracker.h"
#include "stream.h"
#include <unistd.h>
#include "maskbeep.h"
#include "log.h"
#include "ahclient/ahclient.h"

// default delta time stampt between 2 packets
const unsigned int DEFAULT_DELTA_TS = 160;
const unsigned int SAMPLE_RATE = 8000;

stream_tracker_t * new_stream_tracker(stream_t * stream){
    stream_tracker_t * tracker = (stream_tracker_t * )malloc(sizeof(stream_tracker_t));

    tracker->stream = stream;

    tracker->is_paused = FALSE;
    tracker->mask_beep_offset = 0;
    // The following items will be initialized on the first packet
    tracker->timestamp = 0;  
    tracker->sequence_number = 0;
    tracker->pack_size = -1;    // Use this as un-initialzied flag
    // The following delta value will be initailzied on the 2nd packet
    tracker->delta_ts = 0;
    tracker->delta_time_ms = 0;

    pthread_mutex_init(&(tracker->tracker_pack_mutex), NULL);
    return tracker;
}

// Get resume signal, reset the pause flag. The beed sending thread will quit when it got this signal
void * stream_resume(void * arg){

    stream_tracker_t * tracker = (stream_tracker_t *) arg;
    pthread_mutex_lock(& tracker->tracker_pack_mutex);
    if (tracker && tracker->is_paused == TRUE ) { 
        pthread_t sub_thread = tracker->sending_thread;
        tracker->is_paused = FALSE;
        pthread_mutex_unlock(& tracker->tracker_pack_mutex);
        // wait until child thread finished
        pthread_join(sub_thread, NULL);
        return NULL;
    }
    pthread_mutex_unlock(& tracker->tracker_pack_mutex);
    return NULL;
}

void * delete_stream_tracker(void  * arg){
    stream_tracker_t * tracker = (stream_tracker_t * )arg;
    if (tracker) {
        // make sure the child sending thread quit
        stream_resume((void *)tracker);
        pthread_mutex_destroy(&tracker->tracker_pack_mutex);
        free(tracker);
    }
    return NULL;
}

void async_delete_stream_tracker(stream_tracker_t * tracker){

    pthread_t   del_thread;
    pthread_create(&del_thread , NULL, &delete_stream_tracker, (void *)tracker);    
    return;

}

uint16_t get_sn(unsigned char * sn) {
    uint16_t v = sn[0];
    v <<= 8;
    v += sn[1];
    return v;
}
void fill_sn(uint16_t v, unsigned char * sn) {
    sn[1] = (v & 0xff); v >>= 8; 
    sn[0] = (v & 0xff);
}
uint32_t get_ts(unsigned char * ts) {
    uint32_t v = ts[0]; v <<= 8;
    v += ts[1]; v <<= 8;
    v += ts[2]; v <<= 8;
    v += ts[3]; 
    return v;
}
void fill_ts(uint32_t v, unsigned char * ts) {
    ts[3] = (v & 0xff); v >>= 8; 
    ts[2] = (v & 0xff); v >>= 8; 
    ts[1] = (v & 0xff); v >>= 8; 
    ts[0] = (v & 0xff);
}

BOOL track_stream(stream_tracker_t * tracker, stream_t  * stream, const unsigned char * buf, int len)
{
    BOOL ret = FALSE;
    if ( buf && len >= STREAM_HEADER_SIZE) {
        stream_header_t * header = (stream_header_t *)buf;
        uint16_t header_sn = get_sn(header->sequence_number);
        uint32_t header_ts = get_ts(header->timestamp);
    
        pthread_mutex_lock(& tracker->tracker_pack_mutex);

        if (tracker->is_paused) {
            // in paused state, the stream should be handled by beep_sending_thread()
            ret = TRUE;
        } else {

            if (tracker->timestamp == 0) { // uninitialized
                memcpy(&(tracker->stream_header), header, STREAM_HEADER_SIZE); 
                tracker->timestamp = header_ts; 
                tracker->sequence_number = header_sn; 
            } else  if (tracker->delta_ts == 0 && header_sn > tracker->sequence_number && header_ts  > tracker->timestamp) {      // init the delta time of two packets
                tracker->delta_ts   = ( header_ts - tracker->timestamp ) / (header_sn - tracker->sequence_number);
                tracker->delta_time_ms = tracker->delta_ts * 1000 / SAMPLE_RATE;
                tracker->pack_size = STREAM_HEADER_SIZE + tracker->delta_ts;  // 8 bits deep signal, each TS requires one byte sampling
                        
                ilog(LOG_INFO, "Init delta_ts: %d (%dms) packet size %d on ts: %u sn: %u", tracker->delta_ts, tracker->delta_time_ms,
                    tracker->pack_size, tracker->timestamp, tracker->sequence_number);
            }

            // update the timestamp and SN
            tracker->timestamp = header_ts;
            tracker->sequence_number = header_sn;       
        }
        pthread_mutex_unlock(& tracker->tracker_pack_mutex);
    }
    return ret;
}

void * beep_sending_thread(void * arg) {
    stream_tracker_t * tracker = (stream_tracker_t *)arg; 
    BOOL resumed = FALSE;
    struct timeval tv;
    unsigned long ut1, ut2;

    while(TRUE) {

        gettimeofday(&tv,NULL);
        // ms on starting a new loop
        ut1 = 1000000 * tv.tv_sec + tv.tv_usec;

        pthread_mutex_lock(& tracker->tracker_pack_mutex);
        if (tracker->is_paused ) 
        {
            // update the sequence number and timestamp
            tracker->timestamp += tracker->delta_ts;
            tracker->sequence_number ++;     
            fill_ts(tracker->timestamp, tracker->stream_header.timestamp);
            fill_sn(tracker->sequence_number, tracker->stream_header.sequence_number);

        } else {
            // got resume signal
            resumed = TRUE;
        }
        pthread_mutex_unlock(& tracker->tracker_pack_mutex);

        if (resumed) {
            break; // return
        } else {
            // gen new pack and sent
            unsigned char * buf = malloc(tracker->pack_size);
            memcpy(buf,&(tracker->stream_header), STREAM_HEADER_SIZE);
            int raw_data_size = tracker->pack_size - STREAM_HEADER_SIZE;
            // here the logic didn't handle the case if raw_data_size is bigger than MASK_BEEP_LENGTH 
            if (tracker->mask_beep_offset + raw_data_size <= MASK_BEEP_LENGTH) {
                memcpy(buf + STREAM_HEADER_SIZE, maskbeep + tracker->mask_beep_offset , raw_data_size );
                tracker->mask_beep_offset += raw_data_size;
                if (tracker->mask_beep_offset == MASK_BEEP_LENGTH) tracker->mask_beep_offset = 0;
            } else {
                memcpy(buf + STREAM_HEADER_SIZE, maskbeep + tracker->mask_beep_offset , MASK_BEEP_LENGTH -  tracker->mask_beep_offset);
                raw_data_size -= MASK_BEEP_LENGTH -  tracker->mask_beep_offset;
                memcpy(buf + STREAM_HEADER_SIZE + MASK_BEEP_LENGTH -  tracker->mask_beep_offset, maskbeep, raw_data_size);
                tracker->mask_beep_offset = raw_data_size;
            }

            // buf hand off to process_stream, not need to free here
            process_stream((stream_t *)tracker->stream, buf, tracker->pack_size); 

            gettimeofday(&tv,NULL);
            // ms on ending a new loop
            ut2 = 1000000 * tv.tv_sec + tv.tv_usec;

            // sleep ms before send next packet
            long sleep_time_ms = (long)tracker->delta_time_ms - (ut2 - ut1);
            if (sleep_time_ms > 0) usleep(sleep_time_ms);
        }
    }

    return NULL;
}


// Get pause signal, set pause  flag and start the beep sending thread
void stream_pause(stream_tracker_t * tracker){
    pthread_mutex_lock(& tracker->tracker_pack_mutex);
    if (tracker && tracker->is_paused == FALSE ) {
        if (tracker->delta_ts == 0 ) {
            ilog(LOG_ERROR,"Get pause signal after the 1st package, don't know the delta time of 2 packets, will use default setting : %d ", DEFAULT_DELTA_TS);
            tracker->delta_ts = DEFAULT_DELTA_TS;
            tracker->delta_time_ms = tracker->delta_ts * 1000 / SAMPLE_RATE;
            tracker->pack_size = STREAM_HEADER_SIZE + DEFAULT_DELTA_TS;  // 8 bits deep signal, each TS requires one byte sampling
        }
        tracker->is_paused = TRUE;
        tracker->mask_beep_offset = 0;
        pthread_create(&tracker->sending_thread , NULL, &beep_sending_thread, (void *)tracker);    
    }
    pthread_mutex_unlock(& tracker->tracker_pack_mutex);

}

void async_stream_resume(stream_tracker_t * tracker){

    pthread_t   resume_thread;
    pthread_create(&resume_thread , NULL, &stream_resume, (void *)tracker);    
    return;

}

#endif