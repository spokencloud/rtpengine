#include "metafile.h"
#include <glib.h>
#include <pthread.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/timerfd.h>
#include "log.h"
#include "stream.h"
#include "garbage.h"
#include "main.h"
#include "recaux.h"
#include "packet.h"
#include "output.h"
#include "mix.h"
#include "db.h"
#include "forward.h"
#include "tag.h"
#include "decoder.h"
#include "epoll.h"
#include "maskbeep.h"

static pthread_mutex_t metafiles_lock = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *metafiles;

static long get_current_milliseconds(){
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static void meta_free(void *ptr) {
	metafile_t *mf = ptr;

	dbg("freeing metafile info for %s%s%s", FMT_M(mf->name));
	output_close(mf->mix_out);
	mix_destroy(mf->mix);
	g_string_chunk_free(mf->gsc);
	for (int i = 0; i < mf->streams->len; i++) {
		stream_t *stream = g_ptr_array_index(mf->streams, i);
		stream_close(stream); // should be closed already
		stream_free(stream);
	}
	g_ptr_array_free(mf->streams, TRUE);
	for (int i = 0; i < mf->tags->len; i++) {
		tag_t *tag = g_ptr_array_index(mf->tags, i);
		tag_free(tag);
	}
	g_ptr_array_free(mf->tags, TRUE);
	if (mf->ssrc_hash)
		g_hash_table_destroy(mf->ssrc_hash);

	g_slice_free1(sizeof(*mf), mf);
}


// mf is locked
static void meta_destroy(metafile_t *mf) {
	// close all streams
	for (int i = 0; i < mf->streams->len; i++) {
		stream_t *stream = g_ptr_array_index(mf->streams, i);
		pthread_mutex_lock(&stream->lock);
		stream_close(stream);
		pthread_mutex_unlock(&stream->lock);
	}
	//close forward socket
	if (mf->forward_fd >= 0) {
		dbg("call [%s%s%s] forwarded %d packets. %d failed sends.", FMT_M(mf->call_id),
				(int )g_atomic_int_get(&mf->forward_count),
				(int )g_atomic_int_get(&mf->forward_failed));
		close(mf->forward_fd);
		mf->forward_fd = -1;
	}
	db_close_call(mf);
}

#define CONNECTIONUID_TAG "CONNECTIONUID="

// mf is locked
static void meta_stream_interface(metafile_t *mf, unsigned long snum, char *content) {
	db_do_call(mf);
	if (output_enabled && output_mixed) {
		pthread_mutex_lock(&mf->mix_lock);
		if (!mf->mix) {
			char buf[256];
			char connectionUid[33];
			char* pUidBegin = strstr(mf->metadata, CONNECTIONUID_TAG);
			int uidlen = 0;
			if (pUidBegin != NULL){
				pUidBegin += sizeof(CONNECTIONUID_TAG)-1;
				if (*pUidBegin != '\0') {
					char* pUidEnd = strchr(pUidBegin, ';');
					if (pUidEnd != NULL)
						uidlen = pUidEnd - pUidBegin;
					else
						uidlen = strlen(pUidBegin);
					if (uidlen > sizeof(connectionUid)-1)
						uidlen = sizeof(connectionUid)-1;
					strncpy(connectionUid, pUidBegin, uidlen);
				}
			}
			connectionUid[uidlen] = '\0';
			snprintf(buf, sizeof(buf), "%s-%s-mix", mf->parent, connectionUid);
			mf->mix_out = output_new(output_dir, buf);
			mf->mix = mix_new();
			db_do_stream(mf, mf->mix_out, "mixed", NULL, 0);
		}
		pthread_mutex_unlock(&mf->mix_lock);
	}
	dbg("stream %lu interface %s%s%s", snum, FMT_M(content));
	stream_open(mf, snum, content);
}


// mf is locked
static void meta_stream_details(metafile_t *mf, unsigned long snum, char *content) {
	dbg("stream %lu details %s", snum, content);
	unsigned int tag, media, tm, cmp, flags;
	if (sscanf_match(content, "TAG %u MEDIA %u TAG-MEDIA %u COMPONENT %u FLAGS %u",
				&tag, &media, &tm, &cmp, &flags) != 5)
		return;
	stream_details(mf, snum, tag);
}


// mf is locked
static void meta_rtp_payload_type(metafile_t *mf, unsigned long mnum, unsigned int payload_num,
		char *payload_type)
{
	dbg("payload type in media %lu num %u is %s", mnum, payload_num, payload_type);

	if (payload_num >= 128) {
		ilog(LOG_ERR, "Payload type number %u is invalid", payload_num);
		return;
	}
	if (decoding_enabled) {
		pthread_mutex_lock(&mf->payloads_lock);
		mf->payload_types[payload_num] = g_string_chunk_insert(mf->gsc,
				payload_type);
		pthread_mutex_unlock(&mf->payloads_lock);
	}
}


// mf is locked
static void meta_metadata(metafile_t *mf, char *content) {
	mf->metadata = g_string_chunk_insert(mf->gsc, content);
	mf->metadata_db = mf->metadata;
	db_do_call(mf);
	if (forward_to)
		start_forwarding_capture(mf, content);
}


// mf is locked
static void meta_section(metafile_t *mf, char *section, char *content, unsigned long len) {
	unsigned long lu;
	unsigned int u;

	if (!strcmp(section, "CALL-ID"))
		mf->call_id = g_string_chunk_insert(mf->gsc, content);
	else if (!strcmp(section, "PARENT"))
		mf->parent = g_string_chunk_insert(mf->gsc, content);
	else if (!strcmp(section, "METADATA"))
		meta_metadata(mf, content);
	else if (sscanf_match(section, "STREAM %lu interface", &lu) == 1)
		meta_stream_interface(mf, lu, content);
	else if (sscanf_match(section, "STREAM %lu details", &lu) == 1)
		meta_stream_details(mf, lu, content);
	else if (sscanf_match(section, "MEDIA %lu PAYLOAD TYPE %u", &lu, &u) == 2)
		meta_rtp_payload_type(mf, lu, u, content);
	else if (sscanf_match(section, "TAG %lu", &lu) == 1)
		tag_name(mf, lu, content);
	else if (sscanf_match(section, "LABEL %lu", &lu) == 1)
		tag_label(mf, lu, content);
	else if (sscanf_match(section, "RECORDING %u", &u) == 1)
		mf->recording_on = u ? 1 : 0;
	else if (sscanf_match(section, "FORWARDING %u", &u) == 1)
		mf->forwarding_on = u ? 1 : 0;
	else if (sscanf_match(section, "STREAM %lu FORWARDING %u", &lu, &u) == 2)
		stream_forwarding_on(mf, lu, u);
}

// returns mf locked
static metafile_t *metafile_get_by_call_id(char* call_id){
	metafile_t *result = NULL;
	pthread_mutex_lock(&metafiles_lock);
	GList *mflist = g_hash_table_get_values(metafiles);
	for (GList *l = mflist; l; l = l->next) {
		metafile_t *mf = l->data;
		if (strcmp(mf->call_id, call_id) == 0) {
			result = mf;
			break;
		}
	}
	g_list_free(mflist);
	if (result != NULL)
		pthread_mutex_lock(&result->lock);
	pthread_mutex_unlock(&metafiles_lock);
	return result;
}

// returns mf locked
static metafile_t *metafile_get(char *name) {
	// get or create metafile metadata
	pthread_mutex_lock(&metafiles_lock);
	metafile_t *mf = g_hash_table_lookup(metafiles, name);
	if (mf)
		goto out;

	dbg("allocating metafile info for %s%s%s", FMT_M(name));
	mf = g_slice_alloc0(sizeof(*mf));
	mf->gsc = g_string_chunk_new(0);
	mf->name = g_string_chunk_insert(mf->gsc, name);
	pthread_mutex_init(&mf->lock, NULL);
	mf->streams = g_ptr_array_new();
	mf->tags = g_ptr_array_new();
	mf->forward_fd = -1;
	mf->forward_count = 0;
	mf->forward_failed = 0;
	mf->recording_on = 1;

	if (decoding_enabled) {
		pthread_mutex_init(&mf->payloads_lock, NULL);
		pthread_mutex_init(&mf->mix_lock, NULL);
		mf->ssrc_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, ssrc_free);
		mf->timer_fd = -1;
	}

	g_hash_table_insert(metafiles, mf->name, mf);

out:
	// switch locks
	pthread_mutex_lock(&mf->lock);
	pthread_mutex_unlock(&metafiles_lock);

	return mf;
}

/* 
int metafile_insert_decoder(metafile_t *mf, decode_t *decode){
	for (int i=0; i<MAX_DECODERS_PER_CALL; i++){
		if (mf->decoders[i] == NULL){
			mf->decoders[i] = decode;
			return i;
		}
	}
	ilog(LOG_ERR, "Call %s has too many decoders.", mf->call_id);
	return -1;
}
*/

void metafile_change(char *name) {
	metafile_t *mf = metafile_get(name);

	char fnbuf[PATH_MAX];
	snprintf(fnbuf, sizeof(fnbuf), "%s/%s", spool_dir, name);

	// open file and seek to last known position
	int fd = open(fnbuf, O_RDONLY);
	if (fd == -1) {
		ilog(LOG_ERR, "Failed to open %s%s%s: %s", FMT_M(fnbuf), strerror(errno));
		goto out;
	}
	if (lseek(fd, mf->pos, SEEK_SET) == (off_t) -1) {
		ilog(LOG_ERR, "Failed to seek to end of file %s%s%s: %s", FMT_M(fnbuf), strerror(errno));
		close(fd);
		goto out;
	}

	// read the entire file
	GString *s = g_string_new(NULL);
	char buf[1024];
	while (1) {
		int ret = read(fd, buf, sizeof(buf));
		if (ret == 0)
			break;
		if (ret == -1)
			die_errno("read on metadata file failed");
		g_string_append_len(s, buf, ret);
	}

	// save read position and close file
	mf->pos = lseek(fd, 0, SEEK_CUR);
	close(fd);

	// process contents of metadata file
	// XXX use "str" type?
	char *head = s->str;
	char *endp = s->str + s->len;
	while (head < endp) {
		// section header
		char *nl = memchr(head, '\n', endp - head);
		if (!nl || nl == head) {
			ilog(LOG_WARN, "Missing section header in %s%s%s", FMT_M(name));
			break;
		}
		if (memchr(head, '\0', nl - head)) {
			ilog(LOG_WARN, "NUL character in section header in %s%s%s", FMT_M(name));
			break;
		}
		*(nl++) = '\0';
		char *section = head;
		dbg("section %s", section);
		head = nl;

		// content length
		nl = memchr(head, ':', endp - head);
		if (!nl || nl == head) {
			ilog(LOG_WARN, "Content length for section %s missing in %s%s%s", section, FMT_M(name));
			break;
		}
		*(nl++) = '\0';
		if (*(nl++) != '\n') {
			ilog(LOG_WARN, "Unterminated content length for section %s in %s%s%s", section, FMT_M(name));
			break;
		}
		char *errp;
		unsigned long slen = strtoul(head, &errp, 10);
		if (*errp != '\0') {
			ilog(LOG_WARN, "Invalid content length for section %s in %s%s%s", section, FMT_M(name));
			break;
		}
		dbg("content length %lu", slen);
		head = nl;

		// content
		if (endp - head < slen) {
			ilog(LOG_WARN, "Content truncated in section %s in %s%s%s", section, FMT_M(name));
			break;
		}
		char *content = head;
		if (memchr(content, '\0', slen)) {
			ilog(LOG_WARN, "NUL character in content in section %s in %s%s%s", section, FMT_M(name));
			break;
		}

		// double newline separator
		head += slen;
		if (*head != '\n' || *(head + 1) != '\n') {
			ilog(LOG_WARN, "Separator missing after section %s in %s%s%s", section, FMT_M(name));
			break;
		}
		*head = '\0';
		head += 2;

		meta_section(mf, section, content, slen);
	}

	g_string_free(s, TRUE);

out:
	pthread_mutex_unlock(&mf->lock);
}


void metafile_delete(char *name) {
	// get metafile metadata
	pthread_mutex_lock(&metafiles_lock);
	metafile_t *mf = g_hash_table_lookup(metafiles, name);
	if (!mf) {
		// nothing to do
		pthread_mutex_unlock(&metafiles_lock);
		return;
	}
	// switch locks and remove entry
	pthread_mutex_lock(&mf->lock);
	g_hash_table_remove(metafiles, name);
	pthread_mutex_unlock(&metafiles_lock);

	meta_destroy(mf);

	// add to garbage
	garbage_add(mf, meta_free);
	pthread_mutex_unlock(&mf->lock);
}


void metafile_setup(void) {
	metafiles = g_hash_table_new(g_str_hash, g_str_equal);
}


void metafile_cleanup(void) {
	GList *mflist = g_hash_table_get_values(metafiles);
	for (GList *l = mflist; l; l = l->next) {
		metafile_t *mf = l->data;
		meta_destroy(mf);
		meta_free(mf);
		
	}
	g_list_free(mflist);
	g_hash_table_destroy(metafiles);
}

static void metafile_traverse_decoders(metafile_t *mf, decoder_visitor_t visitor_fun, long timediff, void* extra) {
	GList *ssrclist = g_hash_table_get_values(mf->ssrc_hash);
	for (GList *l = ssrclist; l; l = l->next) {
		ssrc_t *ssrc = l->data;
		if (ssrc != NULL){
			for (int j=0; j<128; j++){
				if (ssrc->decoders[j] != NULL){
					(*visitor_fun)(ssrc->decoders[j], ssrc, timediff, extra);
				}
			}
		}
	}
}

const unsigned char* fill_mask_data(unsigned char* mask_data, int len, const unsigned char * pMask);
static void insert_mask_beep(metafile_t *mf, long timediff) {
	int nb_samples = 160;
	int clockrate = 8000; // dec->in_format.clockrate
	uint64_t target_pts = timediff * (clockrate / 1000);
	int offset_mask_beep = mf->last_mask_pts % MASK_BEEP_LENGTH;
	int shift_ts = target_pts - mf->last_mask_pts;
	//int pkt_num = (target_pts - dec->pts) / nb_samples;

	dbg("====> metafile_insert_mask_beep: timediff=%lu,  target_pts=%llu, start_pos=%llu, shift_ts=%d",
	  timediff, 
	  (long long unsigned int)target_pts, 
	  (long long unsigned int)offset_mask_beep,
	  shift_ts);
	unsigned char mask_data[nb_samples];
	int len = nb_samples;
	const unsigned char* pMask = maskbeep + offset_mask_beep;
	dbg("====> total len = %d", shift_ts);
	while (shift_ts > 0){
		if (shift_ts<len)
			len = shift_ts;
		dbg("====> insert maskbeep[%d:%d]", (int)(pMask - maskbeep), len);
		pMask = fill_mask_data(mask_data, len, pMask);
		str pMaskData;
		pMaskData.s = (char*)mask_data;
		pMaskData.len = len;
		metafile_traverse_decoders(mf, decoder_insert_mask_beep_packet, (long)target_pts, &pMaskData);
		shift_ts -= len;
		usleep(1000);
	}	
	mf->last_mask_pts = target_pts;
}

static void metafile_timer_handler(handler_t *handler) {
    uint64_t exp = 0;
    metafile_t *mf = handler->ptr;
    read(mf->timer_fd, &exp, sizeof(uint64_t)); 
	long now = get_current_milliseconds();
	//metafile_traverse_decoders(mf, decoder_insert_mask_beep, now - mf->pause_start_time);
	insert_mask_beep(mf, now - mf->pause_start_time);
}

#define CHECK_MASK_BEEP_INTERVAL 200  // miliseconds
int timerfd_init(metafile_t *mf)
{
	if (mf->timer_fd != -1)
		return 0;

    int tmfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tmfd < 0) {
        ilog(LOG_ERR, "timerfd_create error, Error:[%d:%s]", errno, strerror(errno));
        return -1;
    }
	
	struct itimerspec its;
	its.it_value.tv_sec = CHECK_MASK_BEEP_INTERVAL/1000;
	its.it_value.tv_nsec = CHECK_MASK_BEEP_INTERVAL%1000 * 1000000;
	its.it_interval.tv_sec = its.it_value.tv_sec;
	its.it_interval.tv_nsec = its.it_value.tv_nsec;

    int ret = timerfd_settime(tmfd, 0, &its, NULL);
    if (ret < 0) {
        ilog(LOG_ERR, "timerfd_settime error, Error:[%d:%s]", errno, strerror(errno));
        close(tmfd);
        return -1;
    }

	mf->timer_handler.ptr = mf;
	mf->timer_handler.func = metafile_timer_handler;
	if (epoll_add(tmfd, EPOLLIN, &mf->timer_handler)) {
        ilog(LOG_ERR, "epoll_add error, Error:[%d:%s]", errno, strerror(errno));		
		close(tmfd);
		return -1;
	}
	mf->timer_fd = tmfd;

    return 0;
}

int timerfd_destroy(metafile_t *mf)
{
	if (mf->timer_fd == -1)
		return 0;
	int tmfd = mf->timer_fd;
	epoll_del(tmfd);
	close(tmfd);
	mf->timer_fd = -1;
	return 0;
}


// only insert mask_beep when the time of a pause is greater than PAUSE_RECORDING_THRESHOLD
#define PAUSE_RECORDING_THRESHOLD 1000	

const unsigned char* fill_mask_data(unsigned char* mask_data, int len, const unsigned char * pMask){
	if (pMaskBeepEnd - pMask >= len)
	{
		memcpy(mask_data, pMask, len);
		pMask += len;
		if (pMask == pMaskBeepEnd)
			pMask = maskbeep;
	}
	else{
		int firstPartLen = pMaskBeepEnd - pMask;
		memcpy(mask_data, pMask, firstPartLen);
		int secondPartLen = len - firstPartLen;
		pMask = maskbeep;
		memcpy(mask_data + firstPartLen, pMask, secondPartLen);
		pMask += secondPartLen;
	}
	return pMask;
}

int metafile_stop_recording(char *call_id){
	metafile_t *mf = metafile_get_by_call_id(call_id);
	if (mf == NULL){
		ilog(LOG_WARN, "Call %s does not exist", call_id);
		pthread_mutex_unlock(&mf->lock);
		return -1;
	}
	timerfd_init(mf);
	mf->pause_start_time = get_current_milliseconds();
	mf->last_mask_pts = 0;

	metafile_traverse_decoders(mf, decoder_start_mask_beep, 0, NULL);
	pthread_mutex_unlock(&mf->lock);
	return 0;
}

int metafile_start_recording(char *call_id){
	metafile_t *mf = metafile_get_by_call_id(call_id);
	if (mf == NULL){
		ilog(LOG_WARN, "Call %s does not exist", call_id);
		pthread_mutex_unlock(&mf->lock);
		return -1;
	}
	timerfd_destroy(mf);
	mf->pause_start_time = -1;

	metafile_traverse_decoders(mf, decoder_stop_mask_beep, 0, NULL);
	pthread_mutex_unlock(&mf->lock);
	return 0;
}

