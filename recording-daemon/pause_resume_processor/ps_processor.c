
#include "ps_processor.h"

#if _WITH_PAUSE_RESUME_PROCESSOR
#include "stream_tracker.h"
#include <pthread.h>
#include "tcpserver.h"
#include "log.h"
#include "ahclient/ahclient.h"

typedef struct stream_tracking_node 
{
    stream_tracker_t  *   tracker;
    struct stream_tracking_node * next;
} stream_tracking_node_t;

typedef struct ps_processor {
    pthread_mutex_t processor_mutex;
    stream_tracking_node_t  * tracker_node_head;
}  ps_procesor_t;

// a global sigleton pause resume processor 
ps_procesor_t * ps_processor_instance = NULL;


stream_tracking_node_t * new_stream_tracking_node_t( stream_t * stream) {
    stream_tracking_node_t * node = (stream_tracking_node_t *)malloc(sizeof(stream_tracking_node_t));
    node->next = NULL;
    node->tracker = new_stream_tracker(stream);
    return node;
}

void delete_stream_tracking_node(stream_tracking_node_t * node, BOOL recursive) {
    if (node) {
        async_delete_stream_tracker(node->tracker);
        if (recursive) { 
            delete_stream_tracking_node(node->next, recursive);
        }
        free(node);
    }
}
stream_tracking_node_t *  find_stream_tracker_node_by_uid_id( char * uid, unsigned long id) {
    stream_tracking_node_t * node = ps_processor_instance->tracker_node_head;
    while (node) {
        if (  same_uid ( uid, node->tracker->stream->metafile->call_id) && id == node->tracker->stream->id) {
            break;
        }
        node = node->next;
    }
    return node;
}

stream_tracking_node_t * find_stream_tracker_node(const stream_t * stream, BOOL create, stream_tracking_node_t ** pre_node) {

    stream_tracking_node_t * node = ps_processor_instance->tracker_node_head;
    stream_tracking_node_t * pre = NULL;
    while (node) {
        if (node->tracker->stream == stream) {
            break;
        }
        pre = node;
        node = node->next;
    }

    if (node == NULL && create) {
        // Create a new node and attach to the head of linklist
        node = new_stream_tracking_node_t((stream_t *)stream);
        node ->next = ps_processor_instance->tracker_node_head;
        ps_processor_instance->tracker_node_head = node;
        pre = NULL;
    }

    if (pre_node) *pre_node = pre;

    return node;
}

// WARN : it's not a thread safe singleton, should not be called from multiple threads
void init_ps_processor(int listen_port, int max_clients){
    ilog(LOG_INFO, "init_ps_processor ");

    if (ps_processor_instance == NULL) {
        // create tcp server first
        if ( tcpserver_setup( listen_port, max_clients)) {
            // Create instance only when tcp server created
            ps_processor_instance = (ps_procesor_t *)malloc(sizeof( ps_procesor_t));
            pthread_mutex_init(&(ps_processor_instance->processor_mutex), NULL);
            ps_processor_instance->tracker_node_head = NULL;
        }
    }
}

void destroy_ps_processor(void) {
     if (ps_processor_instance ) {
         // first close tcp server
        tcpserver_close();

        pthread_mutex_lock(&ps_processor_instance->processor_mutex);
        delete_stream_tracking_node(ps_processor_instance->tracker_node_head, TRUE);
        pthread_mutex_unlock(&ps_processor_instance->processor_mutex);
        pthread_mutex_destroy(&ps_processor_instance->processor_mutex);

        free(ps_processor_instance);
        ps_processor_instance = NULL;
     }
}
/**************************
// notify processor that a new packet is coming
// return true is this packet (with the same time stamp) has already been processed by pause - resume processor
**************************/
BOOL ps_processor_process_stream(stream_t * stream, const unsigned char * buf, int len){
    
    BOOL ret = FALSE;
    if (ps_processor_instance ) {

        if (stream->id == STREAM_ID_L_RTP || stream->id == STREAM_ID_R_RTP)  {  // RTP

            pthread_mutex_lock(&ps_processor_instance->processor_mutex);
            stream_tracking_node_t * node = find_stream_tracker_node(stream, TRUE, NULL);
            pthread_mutex_unlock(&ps_processor_instance->processor_mutex);

            if (node) {
                ret =  track_stream(node->tracker, stream, buf, len);
            }
        }  else if (stream->id == STREAM_ID_L_RTCP || stream->id == STREAM_ID_R_RTCP)  {  // RTCP should ignore if the corresponding stream is paused
            unsigned long  id = stream->id - 1; 
            pthread_mutex_lock(&ps_processor_instance->processor_mutex);
            stream_tracking_node_t * node = find_stream_tracker_node_by_uid_id(stream->metafile->call_id, id);
            if (node && node->tracker->is_paused)  ret = TRUE;   // ignore this RTCP
            pthread_mutex_unlock(&ps_processor_instance->processor_mutex);
        }
    }
    return ret;
}
/**************************
// notify processor to stop tracking this stream
// 
**************************/
void ps_processor_close_stream(stream_t * stream){
    if (ps_processor_instance ) {
        if (stream->id != STREAM_ID_L_RTP && stream->id != STREAM_ID_R_RTP) return;

        stream_tracking_node_t * pre_node = NULL;
        pthread_mutex_lock(&ps_processor_instance->processor_mutex);

        stream_tracking_node_t * node = find_stream_tracker_node(stream, FALSE, &pre_node);
        // remove this node from linked list
        if (node) {
            if (pre_node) {
                pre_node->next = node->next;
            } else {
                ps_processor_instance->tracker_node_head = node->next;
            }
        }
        pthread_mutex_unlock(&ps_processor_instance->processor_mutex);

        delete_stream_tracking_node(node, FALSE);
        
    }
}

BOOL stream_id_match_channel_id(unsigned long stream_id, channel_id_t id ) {
    if (id == ALL_CHANNELS) return TRUE;
    if (stream_id == STREAM_ID_L_RTP && id == LEFT_CHANNEL) return TRUE;
    if (stream_id == STREAM_ID_R_RTP && id == RIGHT_CHANNEL) return TRUE;
    return FALSE;
}

int find_macthed_node(char * call_id, channel_id_t id , stream_tracking_node_t ** matched) {
    int c = 0;

    pthread_mutex_lock(&ps_processor_instance->processor_mutex);
    stream_tracking_node_t * node = ps_processor_instance->tracker_node_head;
    while (node) {

        if (same_uid(node->tracker->stream->metafile->call_id, call_id) && stream_id_match_channel_id(node->tracker->stream->id, id)) {
            matched[c] = node;
            c++;
            if (id != ALL_CHANNELS || c >= CHANNEL_COUNT ) break;
        }
        node = node->next;
    }
    pthread_mutex_unlock(&ps_processor_instance->processor_mutex);
    return c;
}

void pause_stream( char * call_id, channel_id_t id ) {
    if (ps_processor_instance ) {
        stream_tracking_node_t * matched[CHANNEL_COUNT] = {NULL};
        int c = find_macthed_node(call_id, id, matched);
        int i = 0;
        for (; i < c; ++i) {
            if (matched[i] != NULL) {
                stream_pause(matched[i]->tracker);
            }
        }

    }
}

void resume_stream(char * call_id, channel_id_t id) {
     if (ps_processor_instance ) {
        stream_tracking_node_t * matched[CHANNEL_COUNT] = { NULL};
        int c = find_macthed_node(call_id, id, matched);
        int i = 0;
        for (; i < c; ++i) {
            if (matched[i] != NULL) {
                async_stream_resume(matched[i]->tracker);
            }
        }
    }
}

#endif 