#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
#include <string.h>
#include <gst/gst.h>

#define MIME "application/x-rtp"
#define MEDIA "audio"
#define CLOCK_RATE 48000
#define ENCODING "X-GST-OPUS-DRAFT-SPITTKA-00"

#define SIP_PORT 5060
#define RTP_PORT (SIP_PORT-50)

#define USER "lab3"

/* Set to 1 if debug prints should occur */
#define DEBUG 0

/* PJSIP stuff */
#define AF pj_AF_INET()

static pjsip_endpoint *g_endpt;
static pj_caching_pool cp;
static pjsip_inv_session *g_inv;

/* Lock so answering can only be done when it's ringing */
static gboolean is_ringing = FALSE;

static void call_on_state_changed(pjsip_inv_session *inv, pjsip_event *e);
static void call_on_forked(pjsip_inv_session *inv, pjsip_event *e);
static void call_on_media_update(pjsip_inv_session *inv, pj_status_t status);
static pj_status_t create_sdp(pj_pool_t *pool, pjmedia_sdp_session **p_sdp);
static pj_bool_t on_rx_request(pjsip_rx_data *rdata);
static pj_bool_t make_call(char *ipaddr);
static pj_bool_t answer_call(void);
static pj_bool_t hangup_call(void);

static pjsip_module user_agent = {
	NULL, NULL,
	{"user agent", 10},
	-1,
	PJSIP_MOD_PRIORITY_APPLICATION,
	NULL, NULL, NULL, NULL,
	&on_rx_request,
	NULL, NULL, NULL, NULL,
};

/* Logging functions for debugging sent and received SIP traffic */

#if DEBUG
static pj_bool_t logging_on_rx_msg(pjsip_rx_data *rdata){
	PJ_LOG(4, ("recv:", "RX %d bytes %s from %s %s:%d:\n%.*s\n--end msg--",
		rdata->msg_info.len,
		pjsip_rx_data_get_info(rdata),
		rdata->tp_info.transport->type_name,
		rdata->pkt_info.src_name,
		rdata->pkt_info.src_port,
		(int)rdata->msg_info.len,
		rdata->msg_info.msg_buf));

	return PJ_FALSE;
}

static pj_status_t logging_on_tx_msg(pjsip_tx_data *tdata){
	PJ_LOG(4, ("send:", "TX %d bytes %s to %s %s:%d:\n%.*s\n--end--msg--",
	(tdata->buf.cur - tdata->buf.start),
	pjsip_tx_data_get_info(tdata),
	tdata->tp_info.transport->type_name,
	tdata->tp_info.dst_name,
	tdata->tp_info.dst_port,
	(int)(tdata->buf.cur - tdata->buf.start),
	tdata->buf.start));

	return PJ_SUCCESS;
}

static pjsip_module msg_logger = {
	NULL, NULL,
	{"msg-log", 7},
	-1,
	PJSIP_MOD_PRIORITY_TRANSPORT_LAYER-1,
	NULL, NULL, NULL, NULL,
	&logging_on_rx_msg,
	&logging_on_rx_msg,
	&logging_on_tx_msg,
	&logging_on_tx_msg,
	NULL,
};
#endif

/* Gstreamer Structs for recieving and sending RTP as well as mainloop */
typedef struct _Receiver {
   GstElement *rpipeline;
   GstElement *rsource;
   GstElement *rjitterbuffer;
   GstElement *rdepay;
   GstElement *rdecoder;
   GstElement *rsink;
   GstCaps *caps;
} Receiver;

typedef struct _CustomData {
   GMainLoop *loop;
   GstElement *bin;

   /* Sender side elements */
   GstElement *spipeline;
   GstElement *source;
   GstElement *convert;
   GstElement *resample;
   GstElement *encoder;
   GstElement *pay;
   GstElement *sink;
} CustomData;

/* Gstreamer struct for playing a ringtone */
typedef struct _Ringtone{
	GstElement *pipeline;
	GstElement *filesrc;
	GstElement *demux;
	GstElement *decode;
	GstElement *convert;
	GstElement *resample;
	GstElement *sink;

	guint bus_watch_id;
} Ringtone;

static Ringtone rt;
static CustomData data;
static gchar *target;
static gint t_port;

/* Gstreamer stuff */
static gboolean handle_events(void);
static gboolean start_rtp(void);
static gboolean stop_rtp(void);
static gboolean start_ringtone(void);
static gboolean stop_ringtone(void);
static void on_pad_added(GstElement *element, GstPad *pad, gpointer data);
static gboolean repeat_sound(GstBus *bus, GstMessage *msg, gpointer data);

static void print_menu(gchar *msg){
   g_print(
      "Menu: %s \n"
      " 'C <sip:USERNAME@IP:PORT>' to make call \n"
      " 'A' to answer a call \n"
      " 'H' hangup current call \n"
      " 'Q' to quit \n", msg
   );
}

static gboolean handle_keyboard(GIOChannel *source, GIOCondition cond, CustomData *data){
   gchar *str = NULL;

   if(g_io_channel_read_line(source, &str, NULL, NULL, NULL) != G_IO_STATUS_NORMAL){
      return TRUE;
   }

   switch (g_ascii_tolower (str[0])){
      case 'q':
         g_main_loop_quit(data->loop);
         break;

      case 'c':
			if(make_call(str+2)){
				target = g_strsplit(g_strsplit((str+2), ":", 3)[1], "@", 2)[1];
				t_port = atoi(g_strsplit((str+2), ":", 3)[2])-50;
			}
         break;

      case 'a':
			if(is_ringing){
				if(answer_call()){
					is_ringing = FALSE;
					stop_ringtone();
					print_menu("Call Answered!");
				}
			}
         break;

      case 'h':
			if(hangup_call()){
				print_menu("Call Hangup!");
			}
         break;

      default:
         print_menu("Select option then press enter!");
         break;
   }
   g_free(str);
   return TRUE;
}

int main(int argc, char *argv[]){
	/* Gstreamer and GLib init */
	GIOChannel *io_stdin;
	gst_init(&argc, &argv);
	memset(&data, 0, sizeof(data));

	data.bin = gst_bin_new("BigDaddyBin");

   data.source = gst_element_factory_make("autoaudiosrc","source");
   data.convert = gst_element_factory_make("audioconvert","convert");
   data.resample = gst_element_factory_make("audioresample","resample");
   data.encoder = gst_element_factory_make("opusenc","encoder");
   data.pay = gst_element_factory_make("rtpopuspay","pay");
   data.sink = gst_element_factory_make("multiudpsink","sink");
 
   /* Init pipeline and check that everything was made correctly */
   data.spipeline = gst_pipeline_new("SenderPipeline");

   if(!data.spipeline || !data.source || !data.convert || !data.resample || !data.encoder || !data.pay || !data.sink){
      g_printerr("Could not create all elements.\n");
      return -1;
   }

   /* Put elements into sender pipeline */
   gst_bin_add_many(GST_BIN(data.spipeline), data.source, data.convert, data.resample, data.encoder, data.pay, data.sink, NULL);

   /* Link sender side elements */

   if(!gst_element_link_many(data.source, data.convert, data.resample, data.encoder, data.pay, data.sink, NULL)){
      g_printerr("Could not link elements on sender side.\n");
   }
	gst_element_set_state(data.spipeline, GST_STATE_PLAYING);
   gst_bin_add(GST_BIN(data.bin), data.spipeline);

   gst_element_set_state(data.bin, GST_STATE_PLAYING);
	io_stdin = g_io_channel_unix_new(fileno(stdin));
	g_io_add_watch(io_stdin, G_IO_IN, (GIOFunc)handle_keyboard, &data);


	/* PJLIB init */
	pj_pool_t *pool = NULL;
	pj_status_t status;
#if DEBUG
	pj_log_set_level(5);
#else
	pj_log_set_level(0);
#endif

	status = pj_init();
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	status = pjlib_util_init();
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	pj_caching_pool_init(&cp, &pj_pool_factory_default_policy, 0);

	{
		const pj_str_t *hostname;
		const char *endpt_name;

		hostname = pj_gethostname();
		endpt_name = hostname->ptr;

		status = pjsip_endpt_create(&cp.factory, endpt_name, &g_endpt);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
	}
	
	/* Add UDP transport */
	{
		pj_sockaddr addr;
		pj_sockaddr_init(AF, &addr, NULL, (pj_uint16_t)SIP_PORT);
		if(AF == pj_AF_INET()){
			status = pjsip_udp_transport_start(g_endpt, &addr.ipv4, NULL, 1, NULL);
		}
		else{
			status = PJ_EAFNOTSUP;
		}

		if(status != PJ_SUCCESS){
			g_printerr("Unable to start UDP transport");
			return 1;
		}
	}

	/* Init transaction layer(used for SIP transactions) */
	status = pjsip_tsx_layer_init_module(g_endpt);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	/* Init UA layer module */
	status = pjsip_ua_init_module(g_endpt, NULL);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	/* Init invite session module by setting callbacks */
	{
		pjsip_inv_callback inv_cb;
		pj_bzero(&inv_cb, sizeof(inv_cb));
		inv_cb.on_state_changed = &call_on_state_changed;
		inv_cb.on_new_session = &call_on_forked;

		status = pjsip_inv_usage_init(g_endpt, &inv_cb);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
	}
	
	/* Init 100rel support */
	status = pjsip_100rel_init_module(g_endpt);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	/* Register endpoint with module to receieve incoming requests */	
	status = pjsip_endpt_register_module(g_endpt, &user_agent);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	/* Register logger module */
#if DEBUG
	status = pjsip_endpt_register_module(g_endpt, &msg_logger);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
#endif
	
	print_menu("");
	g_timeout_add_seconds(1, (GSourceFunc)handle_events, NULL);
	data.loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(data.loop);

	/* Unref everything */
	g_main_loop_unref(data.loop);

   gst_element_set_state(data.bin, GST_STATE_NULL);
   gst_object_unref(data.bin);
	if(g_endpt)
		pjsip_endpt_destroy(g_endpt);
	if(pool)
		pj_pool_release(pool);
	return 0;
}

/* Periodic callback to handle SIP events and check if ringtone shall play */
static gboolean handle_events(void){
	pj_time_val timeout = {0, 10};
	pjsip_endpt_handle_events(g_endpt, &timeout);
	return TRUE;
}

/*
	=========== PJSIP functions ===========
*/

/* Make a call */
static pj_bool_t make_call(char *ipaddr){
	pj_sockaddr hostaddr;
	char temp[80], hostip[PJ_INET6_ADDRSTRLEN+2];
	pj_str_t local_uri;
	pj_str_t dst_uri = pj_str(ipaddr);
	pjsip_dialog *dlg;
	pjmedia_sdp_session *sdp;
	pjsip_tx_data *tdata;
	pj_status_t status;

	if(pj_gethostip(AF, &hostaddr) != PJ_SUCCESS){
		g_printerr("Unable to get local host IP\n");
		return PJ_FALSE;
	}

	pj_sockaddr_print(&hostaddr, hostip, sizeof(hostip), 2);
	pj_ansi_sprintf(temp, "sip:lab3@%s:%d", hostip, SIP_PORT);
	local_uri = pj_str(temp);

	/* Make UAC dialog */
	status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local_uri, &local_uri, &dst_uri, &dst_uri, &dlg);

	if(status != PJ_SUCCESS){
	 	g_printerr("Unable to create UAC dialog: %d\n", status);
		return PJ_FALSE;
	}

	/* Make INVITE session */
	create_sdp(dlg->pool, &sdp);
	status = pjsip_inv_create_uac(dlg, sdp, 0, &g_inv);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
	
	/* Create and send INVITE request */
	status = pjsip_inv_invite(g_inv, &tdata);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	status = pjsip_inv_send_msg(g_inv, tdata);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

	return PJ_TRUE;
}

static pj_bool_t answer_call(void){
	pjsip_tx_data *tdata;
	pj_status_t status;
	if(g_inv){
		/* Make and send 200 response */
		status = pjsip_inv_answer(g_inv, 200, NULL, NULL, &tdata);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);

		status = pjsip_inv_send_msg(g_inv, tdata);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);
		return PJ_TRUE;
	}
	return PJ_FALSE;
}

static pj_bool_t hangup_call(void){
	pjsip_tx_data *tdata;
	pj_status_t status;

	if(g_inv){
		status = pjsip_inv_end_session(g_inv, 603, NULL, &tdata);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);

		status = pjsip_inv_send_msg(g_inv, tdata);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);
		return PJ_TRUE;
	}
	return PJ_FALSE;
}

/* SIP User Agent Server callback, recieves requests and handles them */

static pj_bool_t on_rx_request(pjsip_rx_data *rdata){
	pj_sockaddr hostaddr;
	char temp[80], hostip[PJ_INET6_ADDRSTRLEN];
	pj_str_t local_uri;
	pjsip_dialog *dlg;
	pjsip_tx_data *tdata;
	pjmedia_sdp_session *sdp;
	pj_status_t status;

	/* Requests that are not supported will get a 500 response here */
	if(rdata->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD){
		if(rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD){
			pj_str_t reason = pj_str("The Clint server can't process that request");
			pjsip_endpt_respond_stateless(g_endpt, rdata, 500, &reason, NULL, NULL);
		}
	return PJ_TRUE;
	}
	
	/* Reject INVITE while a INVITE session is active with BUSY 486 */
	if (g_inv){
		pj_str_t reason = pj_str("Another call in progress");
		pjsip_endpt_respond_stateless(g_endpt, rdata, 486, &reason, NULL, NULL);
		return PJ_TRUE;
	}

	if(pj_gethostip(AF, &hostaddr) != PJ_SUCCESS){
		g_printerr("Unable to get local host IP");
		return PJ_TRUE;
	}
	pj_sockaddr_print(&hostaddr, hostip, sizeof(hostip), 2);
	
	pj_ansi_sprintf(temp, "<sip:lab3@%s:%d>", hostip, SIP_PORT);
	local_uri = pj_str(temp);

	/* Making UAS dialog */	
	status = pjsip_dlg_create_uas(pjsip_ua_instance(), rdata, &local_uri, &dlg);
	if (status != PJ_SUCCESS){
		pjsip_endpt_respond_stateless(g_endpt, rdata, 500, NULL, NULL, NULL);
		return PJ_TRUE;
	}
	
	/* Creating INVITE session */
	create_sdp(dlg->pool, &sdp);
	status = pjsip_inv_create_uas(dlg, rdata, sdp, 0, &g_inv);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);
	
	/* 180 Response */
	status = pjsip_inv_initial_answer(g_inv, rdata, 180, NULL, NULL, &tdata);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);

	status = pjsip_inv_send_msg(g_inv, tdata);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);
	
	is_ringing = TRUE;

	target = pj_inet_ntoa(rdata->pkt_info.src_addr.ipv4.sin_addr);
	t_port = (rdata->pkt_info.src_port)-50;
	return PJ_TRUE;
}

/* Create SDP session without using PJSIP media endpoint
	Inspired from (row 1001):
	http://www.pjsip.org/pjmedia/docs/html/page_pjmedia_samples_siprtp_c.htm
*/
static pj_status_t create_sdp(pj_pool_t *pool, pjmedia_sdp_session **p_sdp){
	pj_time_val tv;
	pjmedia_sdp_session *sdp;
	PJ_ASSERT_RETURN(pool && p_sdp, PJ_EINVAL);
	pj_sockaddr local_uri;
	char connection_url[80];
	pj_gethostip(AF, &local_uri);
	sdp = pj_pool_zalloc(pool, sizeof(pjmedia_sdp_session));

	pj_gettimeofday(&tv);
	sdp->origin.user = pj_str(USER);
	sdp->origin.version  = sdp->origin.id = tv.sec + 2208988800UL; 
	sdp->origin.net_type = pj_str("IN");
	sdp->origin.addr_type = pj_str("IP4");
	sdp->origin.addr = pj_str(pj_inet_ntoa(local_uri.ipv4.sin_addr));
	sdp->name = pj_str(USER);

	sdp->conn = pj_pool_zalloc(pool, sizeof(pjmedia_sdp_conn));
	sdp->conn->net_type = pj_str("IN");
	sdp->conn->addr_type = pj_str("IP4");

	sdp->conn->addr = pj_str(pj_inet_ntoa(local_uri.ipv4.sin_addr));

	sdp->time.start = sdp->time.stop = 0;
	sdp->attr_count = 0;
	sdp->media_count = 0;

	*p_sdp = sdp;
	
	return PJ_SUCCESS;
}

static void call_on_state_changed(pjsip_inv_session *inv, pjsip_event *e){
	PJ_UNUSED_ARG(e);

	if(inv->state == PJSIP_INV_STATE_DISCONNECTED){
		g_inv = NULL;
		is_ringing = FALSE;
		stop_ringtone();
		stop_rtp();
	}
	else if(inv->state == PJSIP_INV_STATE_NULL){
		is_ringing = FALSE;
		stop_ringtone();
	}
	else if(inv->state == PJSIP_INV_STATE_INCOMING){
		is_ringing = TRUE;
		start_ringtone();
	}
	else if(inv->state == PJSIP_INV_STATE_CONFIRMED){
		start_rtp();
	}
}

static void call_on_forked(pjsip_inv_session *inv, pjsip_event *e){
	PJ_UNUSED_ARG(inv);
	PJ_UNUSED_ARG(e);
}

static void call_on_media_changed(pjsip_inv_session *inv, pj_status_t status){
	PJ_UNUSED_ARG(inv);
	PJ_UNUSED_ARG(status);
}

/*
	=========== Gstreamer functions ===========
*/

static gboolean start_rtp(void){
	/* Start listening on port */
   Receiver rec;
   rec.rsource = gst_element_factory_make("udpsrc","rsource");
   rec.rjitterbuffer = gst_element_factory_make("gstrtpjitterbuffer","rjitterbuffer");
   rec.rdepay = gst_element_factory_make("rtpopusdepay","rdepay");
   rec.rdecoder = gst_element_factory_make("opusdec","rdecoder");
   rec.rsink = gst_element_factory_make("alsasink","rsink");
  
   rec.rpipeline = gst_pipeline_new("ReceiverPipeline");

   if(!rec.rsource || !rec.rjitterbuffer || !rec.rdepay || !rec.rdecoder || !rec.rsink){
      g_printerr("Could not create all receiver elements.\n");
      return FALSE;
   }

   gst_bin_add_many(GST_BIN(rec.rpipeline), rec.rsource, rec.rjitterbuffer, rec.rdepay, rec.rdecoder, rec.rsink, NULL);

   gst_element_link_many(rec.rsource, rec.rjitterbuffer, rec.rdepay, rec.rdecoder, rec.rsink,NULL);

   rec.caps = gst_caps_new_simple(MIME,
      "media", G_TYPE_STRING, MEDIA,
      "clock-rate", G_TYPE_INT, CLOCK_RATE,
      "encoding-name", G_TYPE_STRING, ENCODING,
   NULL);  
   g_object_set(rec.rsource, "caps", rec.caps, "port", (gint)RTP_PORT, NULL);
	gst_element_set_state(rec.rpipeline, GST_STATE_PLAYING);
   gst_bin_add(GST_BIN(data.bin), rec.rpipeline);

	/* Setup sender side */
	g_signal_emit_by_name(data.sink, "add", target, t_port, NULL);
   return TRUE;
}

static gboolean stop_rtp(void){
	/* Teardown the receiver pipeline for the RTP stream */
   GstElement *deletebin;
   deletebin = gst_bin_get_by_name(GST_BIN(data.bin), "ReceiverPipeline");
   gst_element_set_state(deletebin, GST_STATE_NULL);
   gst_bin_remove(GST_BIN(data.bin), deletebin);
   gst_object_unref(deletebin);
	
	/* Disable sending RTP */
	g_signal_emit_by_name(data.sink, "remove", target, t_port, NULL);
   return TRUE;
}

static gboolean start_ringtone(void){
	GstBus *bus;
	rt.filesrc = gst_element_factory_make("filesrc","fs");
	rt.demux = gst_element_factory_make("oggdemux","dmux");
	rt.decode = gst_element_factory_make("vorbisdec","dec");
	rt.convert = gst_element_factory_make("audioconvert","ac");
	rt.resample = gst_element_factory_make("audioresample","ar");
	rt.sink = gst_element_factory_make("autoaudiosink","aas");

	rt.pipeline = gst_pipeline_new("RingTone");

	if(!rt.pipeline || !rt.filesrc || !rt.demux || !rt.decode || !rt.convert || !rt.resample || !rt.sink){
      g_printerr("Could not create all ringtone elements.\n");
      return -1;
   }

   gst_bin_add_many(GST_BIN(rt.pipeline), rt.filesrc, rt.demux, rt.decode, rt.convert, rt.resample, rt.sink, NULL);

   if(!gst_element_link(rt.filesrc, rt.demux) || !gst_element_link_many(rt.decode, rt.convert, rt.resample, rt.sink, NULL)){
      g_printerr("Could not link ringtone elements.\n");
   }

	g_signal_connect(rt.demux, "pad-added", G_CALLBACK(on_pad_added), rt.decode);

	g_object_set(rt.filesrc, "location", "ringtone.ogg", NULL);

	bus = gst_pipeline_get_bus(GST_PIPELINE(rt.pipeline));
	rt.bus_watch_id = gst_bus_add_watch(bus, repeat_sound, rt.pipeline);

	gst_element_set_state (rt.pipeline, GST_STATE_PLAYING);
	gst_object_unref(bus);

	return TRUE;
}

static void on_pad_added(GstElement *element, GstPad *pad, gpointer data){
	GstPad *sinkpad;
	GstElement *decoder = (GstElement *) data;
	sinkpad = gst_element_get_static_pad(decoder, "sink");
	gst_pad_link(pad, sinkpad);
	gst_object_unref(sinkpad);
}

/* Looping playback in Gstreamer inspired from:
	http://stackoverflow.com/questions/6833147/looping-a-video-with-gstreamer-and-gst-launch
*/

static gboolean repeat_sound(GstBus *bus, GstMessage *msg, gpointer data){
	GstElement *pipe = GST_ELEMENT(data);
	switch(GST_MESSAGE_TYPE(msg)){
		case GST_MESSAGE_EOS:
			gst_element_seek(pipe, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
			break;
		default:
			break;
	}
	return TRUE;
}

static gboolean stop_ringtone(void){
	if(rt.bus_watch_id){
		gst_element_set_state(rt.pipeline, GST_STATE_NULL);
		g_source_remove(rt.bus_watch_id);
		rt.bus_watch_id = 0;
		gst_object_unref(rt.pipeline);
	}
	return TRUE;
}

