#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

#include "rtsp_server.h"
#include "common.h"
#include "rtsp.h"
#include "list.h"
#include "hash.h"
#include "thread.h"
#include "rfc.h"
#include "rtp.h"
#include "rtcp.h"
#include "bufpool.h"
#include "mime.h"

/******************************************************************************
 *              PRIVATE DEFINITIONS
 ******************************************************************************/
//static void *rtpThrFxn(void *v);
static inline int __rtp_send_h264(struct nal_rtp_t *rtp, struct list_head_t *trans_list);
static inline int __rtp_send_eachconnection_h264(struct list_t *e, void *v);
static inline int __rtp_send_audio(struct nal_rtp_t *rtp, struct list_head_t *trans_list);
static inline int __rtp_send_eachconnection_audio(struct list_t *e, void *v);
static inline int __rtp_setup_transfer(struct list_t *e, void *v);
static inline int __transfer_nal(struct list_head_t *trans_list, signed char *nalptr, size_t nalsize);
static inline int __retrieve_sprop(rtsp_handle h, signed char *buf, size_t len);

struct __transfer_set_t {
    struct list_head_t list_head;
	int cur_ch;
	int cur_dwtype;
    rtsp_handle h;
};

/******************************************************************************
 *              PRIVATE FUNCTIONS
 ******************************************************************************/

static inline int __transfer_nal(struct list_head_t *trans_list, signed char *nalptr, size_t nalsize)
{
    struct nal_rtp_t rtp;
    unsigned int nri = nalptr[0] & 0x60;
    unsigned int pt  = nalptr[0] & 0x1F;
    rtp_hdr_t *p_header = &(rtp.packet.header);
    signed char *payload = rtp.packet.payload;

    p_header->version = 2;
    p_header->p = 0;
    p_header->x = 0;
    p_header->cc = 0;
    p_header->pt = 96 & 0x7F;

    if(nalsize <= __RTP_MAXPAYLOADSIZE){
        /* single packet */
        /* SPS, PPS, SEI is not marked */
        if(pt != 7 && pt != 8 && pt != 6) { 
            p_header->m = 1;
        } else {
            p_header->m = 0;
        }

        memcpy(payload, nalptr, nalsize);

        rtp.rtpsize = nalsize + sizeof(rtp_hdr_t);

        ASSERT(__rtp_send_h264(&rtp,trans_list) == SUCCESS, return FAILURE);
    }  else  {

        nalptr += 1;
        nalsize -= 1;

        payload[0] = 28;
        payload[0] |= nri;
        payload[1] = pt;
        payload[1] |= 1 << 7;

        /* send fragmented nal */
        while(nalsize > __RTP_MAXPAYLOADSIZE - 2){

            p_header->m = 0;

            memcpy(&(payload[2]), nalptr, __RTP_MAXPAYLOADSIZE - 2);

            rtp.rtpsize = sizeof(rtp_hdr_t) + __RTP_MAXPAYLOADSIZE;

            nalptr += __RTP_MAXPAYLOADSIZE - 2;
            nalsize -= __RTP_MAXPAYLOADSIZE - 2;

            ASSERT(__rtp_send_h264(&rtp,trans_list) == SUCCESS, return FAILURE);

            /* intended xor. blame vim :( */
            payload[1] &= 0xFF ^ (1<<7); 
        }

        /* send trailing nal */
        p_header->m = 1;

        payload[1] |= 1 << 6;

        /* intended xor. blame vim :( */
        payload[1] &= 0xFF ^ (1<<7);

        rtp.rtpsize = nalsize + sizeof(rtp_hdr_t);

        memcpy(&(payload[2]), nalptr, nalsize);

        ASSERT(__rtp_send_h264(&rtp, trans_list) == SUCCESS, return FAILURE);

    }

    return SUCCESS;
}

static inline int __transfer_audio(struct list_head_t *trans_list, signed char *buf, size_t len)
{
    struct nal_rtp_t rtp;
    rtp_hdr_t *p_header = &(rtp.packet.header);
    signed char *payload = rtp.packet.payload;
	size_t last_total_len = len, cur_len = 0;

    p_header->version = 2;
    p_header->p = 0;
    p_header->x = 0;
    p_header->cc = 0;
    p_header->pt = 8/*nh->audio_type*/;
	p_header->m = 1;

    while (last_total_len > 0 ) {

		cur_len = (last_total_len>__RTP_MAXPAYLOADSIZE)?__RTP_MAXPAYLOADSIZE:last_total_len;

        memcpy(payload, buf, cur_len);

        rtp.rtpsize = cur_len + sizeof(rtp_hdr_t);

        ASSERT(__rtp_send_audio(&rtp,trans_list) == SUCCESS, return FAILURE);

		last_total_len -= cur_len;

    }

    return SUCCESS;
}


static inline int __rtp_send_eachconnection_h264(struct list_t *e, void *v)
{
    int send_bytes;
    struct connection_item_t *con;
    struct transfer_item_t *trans;
    struct nal_rtp_t *rtp = v;
	int track_id = 0;

    list_upcast(trans,e); 

    MUST(con = trans->con, return FAILURE);
	if (!con->trans[track_id].server_port_rtp) {
		return SUCCESS;
	}

    rtp->packet.header.seq = htons(con->trans[track_id].rtp_seq);
    rtp->packet.header.ts = htonl(con->trans[track_id].rtp_timestamp);
    rtp->packet.header.ssrc = htonl(con->ssrc);
    con->trans[track_id].rtp_seq += 1;
	if (rtp->packet.header.m) {
		con->trans[track_id].rtp_timestamp += con->trans[track_id].rtp_timeoffset;
	}

    send_bytes = send(con->trans[track_id].server_rtp_fd,&(rtp->packet),rtp->rtpsize,0);
    
    if(send_bytes == rtp->rtpsize) {
        con->trans[track_id].rtcp_packet_cnt += 1;
        con->trans[track_id].rtcp_octet += rtp->rtpsize;
        return SUCCESS;
    } 

    if(con->con_state != __CON_S_PLAYING) {
        DBG("connection state changed before send\n");
        return SUCCESS;
    }

    if(send_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)){
        ERR("EAGAIN\n");
        return FAILURE;
    } 
    
    ERR("send:%d:%s\n",send_bytes,strerror(errno));
    return FAILURE;
}

static inline int __rtp_send_h264(struct nal_rtp_t *rtp, struct list_head_t *trans_list)
{
    return list_map_inline(trans_list,(__rtp_send_eachconnection_h264), rtp);
}

static inline int __rtp_send_eachconnection_audio(struct list_t *e, void *v)
{
    int send_bytes;
    struct connection_item_t *con;
    struct transfer_item_t *trans;
    struct nal_rtp_t *rtp = v;
	int track_id = 1;

    list_upcast(trans,e); 

    MUST(con = trans->con, return FAILURE);
	if (!con->trans[track_id].server_port_rtp) {
		return SUCCESS;
	}

    rtp->packet.header.seq = htons(con->trans[track_id].rtp_seq);
    rtp->packet.header.ts = htonl(con->trans[track_id].rtp_timestamp);
    rtp->packet.header.ssrc = htonl(con->ssrc);
	rtp->packet.header.pt = con->audio_type;
    con->trans[track_id].rtp_seq += 1;
	con->trans[track_id].rtp_timestamp += con->trans[track_id].rtp_timeoffset;

    send_bytes = send(con->trans[track_id].server_rtp_fd,&(rtp->packet),rtp->rtpsize,0);
    
    if(send_bytes == rtp->rtpsize) {
        con->trans[con->track_id].rtcp_packet_cnt += 1;
        con->trans[con->track_id].rtcp_octet += rtp->rtpsize;
		//ts += (rtp->rtpsize-sizeof(rtp->packet.header));
        return SUCCESS;
    } 

    if(con->con_state != __CON_S_PLAYING) {
        DBG("connection state changed before send\n");
        return SUCCESS;
    }

    if(send_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)){
        ERR("EAGAIN\n");
        return FAILURE;
    } 
    
    ERR("send:%d:%s\n",send_bytes,strerror(errno));
    return FAILURE;
}

static inline int __rtp_send_audio(struct nal_rtp_t *rtp, struct list_head_t *trans_list)
{
    return list_map_inline(trans_list,(__rtp_send_eachconnection_audio), rtp);
}


static inline int __rtp_setup_transfer(struct list_t *e, void *v)
{
    struct connection_item_t *con;
    struct __transfer_set_t *trans_set = v;
    struct transfer_item_t *trans;
    unsigned int  timestamp_offset;
    int ret = FAILURE;
	int track_id = 0;

    list_upcast(con,e);

    MUST(bufpool_attach(con->pool, con) == SUCCESS,
        return FAILURE);

    if (con->con_state == __CON_S_PLAYING 
		&& con->ch == trans_set->cur_ch 
		&& ((trans_set->cur_dwtype <= RTSP_MEDIA_DWTYPE_VIDEO_EXTRA 
			&& con->dwtype == trans_set->cur_dwtype)
			|| trans_set->cur_dwtype == RTSP_MEDIA_DWTYPE_AUDIO)) {

        ASSERT(bufpool_get_free(trans_set->h->transfer_pool, &trans) == SUCCESS, ({
            ERR("transfer object resouce starvation detected. possibly connection limits are wrongfully setup\n");
            goto error;}));

        MUST(bufpool_attach(con->pool, con) == SUCCESS,
            return FAILURE);

        trans->con = con;

        MUST(list_push(&trans_set->list_head,&trans->list_entry) == SUCCESS,
            goto error);

		track_id = (trans_set->cur_dwtype==RTSP_MEDIA_DWTYPE_AUDIO)?1:0;

#if 0
        timestamp_offset = trans_set->h->stat.ts_offset;

        con->trans[track_id].rtp_timestamp = 
			((unsigned int)con->trans[track_id].rtp_timestamp + timestamp_offset);
#endif
    }

    ret = SUCCESS;

error:
    ASSERT(bufpool_detach(con->pool, con) == SUCCESS, ret = FAILURE);

    return ret;
}

static inline int __retrieve_sprop(rtsp_handle h, signed char *buf, size_t len)
{
    signed char *nalptr;
    size_t single_len;
    mime_encoded_handle base64 = NULL;
    mime_encoded_handle base16 = NULL;
    unsigned int pt;
    
    /* check SPS is set */
    if(!(h->sprop_sps_b64)){ 
        nalptr = buf;
        single_len = 0;

        while (__split_nal(buf,&nalptr,&single_len,len) == SUCCESS) {
            pt = nalptr[0] & 0x1F;
            if(pt == H264_NAL_TYPE_SPS) {
                ASSERT(base64 = mime_base64_create((char *)&(nalptr[0]),single_len), return FAILURE);
                ASSERT(base16 = mime_base16_create((char *)&(nalptr[1]),3), return FAILURE);

                DASSERT(base16->base == 16, return FAILURE);
                DASSERT(base64->base == 64, return FAILURE);

                /* optimistic lock */
                rtsp_lock(h);
                if(h->sprop_sps_b64) {
                    DBG("sps is set by another thread?\n");
                    mime_encoded_delete(base64);
                } else {
                    h->sprop_sps_b64 = base64;
                }
                
                if(h->sprop_sps_b16) {
                    DBG("sps is set by another thread?\n");
                    mime_encoded_delete(base16);
                } else {
                    h->sprop_sps_b16 = base16;
                }
                rtsp_unlock(h);
            }

        }

        base64 = NULL;
        base16 = NULL;
    }

    /* check PPS is set */
    if(!(h->sprop_pps_b64)){
        nalptr = buf;
        single_len = 0;
        while (__split_nal(buf,&nalptr,&single_len,len) == SUCCESS) {
            pt = nalptr[0] & 0x1F;

            if(pt == H264_NAL_TYPE_PPS) {
                ASSERT(single_len >= 4, return FAILURE);
                ASSERT(base64 = mime_base64_create((char *)&(nalptr[0]),single_len), return FAILURE);

                DASSERT(base64->base == 64, return FAILURE);

                /* optimistic lock */
                rtsp_lock(h);
                if(h->sprop_pps_b64) {
                    DBG("pps is set by another thread?\n");
                    mime_encoded_delete(base64);
                } else {
                    h->sprop_pps_b64 = base64;
                }
                rtsp_unlock(h);
            }
        }
        rtsp_lock(h);
        rtsp_unlock(h);
        base64 = NULL;
    }

    return SUCCESS;
}

static inline int __rtcp_poll(struct list_t *e, void *v)
{
    struct connection_item_t *con;
    struct transfer_item_t *trans;
	int *pthrack_id = v;

    list_upcast(trans, e);
    MUST(con = trans->con, return FAILURE);
    
    if((con->trans[*pthrack_id].rtcp_tick)-- == 0) {
		con->track_id = *pthrack_id;
        ASSERT(__rtcp_send_sr(con) == SUCCESS, return FAILURE);

        /* postcondition check */
        DASSERT(con->trans[*pthrack_id].rtcp_tick == con->trans[*pthrack_id].rtcp_tick_org, return FAILURE);
        DASSERT(con->trans[*pthrack_id].rtcp_packet_cnt == 0, return FAILURE);
        DASSERT(con->trans[*pthrack_id].rtcp_octet == 0, return FAILURE);
    }
    return SUCCESS;
}
/******************************************************************************
 *              PUBLIC FUNCTIONS
 ******************************************************************************/

int rtp_send_media(rtsp_handle h, int ch,  int type, signed char *buf, size_t len, struct timeval *p_tv)
{
    signed char *nalptr = buf;
    size_t single_len = 0;
    int ret = FAILURE;
    struct __transfer_set_t trans = {};
	int track_id = 0;

    /* checkout RTP packet */
    DASSERT(h, return FAILURE);
    DASSERT(p_tv, return FAILURE);


    if(gbl_get_quit(h->pool->sharedp->gbl)) {
        ERR("server threads have gone already. call rtsp_finish()\n");
        return FAILURE;
    }
    
    //__get_timestamp_offset(&h->stat, p_tv);
    
    ASSERT(__retrieve_sprop(h,buf,len) == SUCCESS, goto error);

	trans.cur_ch = ch;
	trans.cur_dwtype = type;
	track_id = (type==RTSP_MEDIA_DWTYPE_AUDIO)?1:0;
	
    trans.h = h;

    /* setup transmission objecl t*/
    ASSERT(list_map_inline(&h->con_list,(__rtp_setup_transfer),&trans) == SUCCESS, goto error);
    
	if (trans.list_head.list) {
		if (type < RTSP_MEDIA_DWTYPE_AUDIO) {
			while (__split_nal(buf,&nalptr,&single_len,len) == SUCCESS) {
				ASSERT(__transfer_nal(&(trans.list_head),nalptr,single_len) == SUCCESS, goto error);
			}
		} else {
			ASSERT(__transfer_audio(&(trans.list_head), buf, len) == SUCCESS, goto error);
		}
		ASSERT(list_map_inline(&(trans.list_head),(__rtcp_poll), &track_id) == SUCCESS, goto error);
	} 


    ret = SUCCESS;

error:
    list_destroy(&(trans.list_head));

    return ret;
}
