#include <string.h>
#include <gst/gst.h>

#define IS_HOST 0

//gst-launch-0.10 -v filesrc location="/home/d7020e/m7017e/One.mp3" ! decodebin2 location="/home/d7020e/m7017e/One.mp3" ! audioconvert ! opusenc ! oggmux ! filesink location="/home/d7020e/m7017e/one.opus"


typedef struct _CustomData {
   GMainLoop *loop;
   GstElement *pipeline;

   /* Sender side elements */
   GstElement *source;
   GstElement *convert;
   GstElement *resample;
   GstElement *encoder;
   GstElement *pay;
   GstElement *sink;

   /* Receiver side elements */
   GstElement *rsource;
   GstElement *rjitterbuffer;
   GstElement *rdepay;
   GstElement *rdecoder;
   GstElement *rsink;
   
   GstState state;
} CustomData;

static gboolean handle_keyboard(GIOChannel *source, GIOCondition cond, CustomData *data){
   gchar *str = NULL;

   if(g_io_channel_read_line(source, &str, NULL, NULL, NULL) != G_IO_STATUS_NORMAL){
      return TRUE;
   }

   switch (g_ascii_tolower (str[0])){

      case 'q':
         g_main_loop_quit(data->loop);
         break;

   }
   g_free(str);
   return TRUE;
}

static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data){
   GstState old_state, new_state, pending_state;
   gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
   if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->pipeline)){
      data->state = new_state;
      g_print("State set to %s\n", gst_element_state_get_name(new_state));
   }
}

static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data){
   GError *err;
   gchar *debug_info;
   
   gst_message_parse_error(msg, &err, &debug_info);
   g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
   g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
   g_clear_error(&err);
   g_free(debug_info);

   gst_element_set_state(data->pipeline, GST_STATE_READY);
}


int main (int argc, char *argv[]){
   CustomData data;
   GIOChannel *io_stdin;
   gst_init(&argc, &argv);
   memset(&data, 0, sizeof(data));
   GstStateChangeReturn ret;
   GstBus *bus;

   data.source = gst_element_factory_make("autoaudiosrc","source");
   data.convert = gst_element_factory_make("audioconvert","convert");
   data.resample = gst_element_factory_make("audioresample","resample");
   data.encoder = gst_element_factory_make("opusenc","encoder");
   data.pay = gst_element_factory_make("rtpopuspay","pay");
   data.sink = gst_element_factory_make("udpsink","sink");

   data.rsource = gst_element_factory_make("udpsrc","rsource");
   data.rjitterbuffer = gst_element_factory_make("gstrtpjitterbuffer","rjitterbuffer");
   data.rdepay = gst_element_factory_make("rtpopusdepay","rdepay");
   data.rdecoder = gst_element_factory_make("opusdec","rdecoder");
   data.rsink = gst_element_factory_make("alsasink","rsink");

   
   /* Init pipeline and check that everything was made correctly */
   data.pipeline = gst_pipeline_new("Audio-conference-pipeline");

   if(!data.pipeline || !data.source || !data.convert || !data.resample || !data.encoder || !data.pay || !data.sink || !data.rsource || !data.rjitterbuffer || !data.rdepay || !data.rdecoder || !data.rsink){
      g_printerr("Could not create all elements.\n");
      return -1;
   }

   /* Put elements into pipeline */

   gst_bin_add_many(GST_BIN(data.pipeline), data.source, data.convert, data.resample, data.encoder, data.pay, data.sink, data.rsource, data.rjitterbuffer, data.rdepay, data.rdecoder, data.rsink, NULL);

   /* Link sender side elements */

   gst_element_link_many(data.source, data.convert, data.resample, data.encoder, data.pay, data.sink, NULL);

   /* Link receiver side elements */

   gst_element_link_many(data.rsource, data.rjitterbuffer, data.rdepay, data.rdecoder, data.rsink, NULL);

   GstCaps *dstcaps;
   dstcaps = gst_caps_from_string("application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)X-GST-OPUS-DRAFT-SPITTKA-00");
   g_object_set(data.rsource, "caps", dstcaps, NULL);
   g_object_set(data.rsource, "port", 8080, NULL);

   g_print(
      "Menu: \n"
      " 'H' to host, host status: %d \n"
      " 'S' to toggle conversation on/off \n"
      " 'Q' to quit \n", IS_HOST
   );


   io_stdin = g_io_channel_unix_new(fileno(stdin));   
   g_io_add_watch(io_stdin, G_IO_IN, (GIOFunc)handle_keyboard, &data);

   ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
   if (ret == GST_STATE_CHANGE_FAILURE) {
      g_printerr ("Unable to set the pipeline to the playing state.\n");
      gst_object_unref (data.pipeline);
      return -1;
}


   data.loop = g_main_loop_new(NULL, FALSE);
   g_main_loop_run(data.loop);

   /* Free allocated stuff */
   g_main_loop_unref(data.loop);
   gst_element_set_state(data.pipeline, GST_STATE_NULL);
   gst_object_unref(data.pipeline);


   return 0;
}








