#include <string.h>
#include <gst/gst.h>

#define MIME "application/x-rtp"
#define MEDIA "audio"
#define CLOCK_RATE 48000
#define ENCODING "X-GST-OPUS-DRAFT-SPITTKA-00"

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

static gboolean makeReceiverBin(gchar *port, CustomData *data);
static gboolean breakReceiverBin(gchar *port, CustomData *data);

static void print_menu(gchar *msg){
   g_print(
      "Menu: %s \n"
      " 'C <IP:PORT>' to connect to a client \n"
      " 'R <IP:PORT>' to disconnect a client \n"
      " 'P <PORT>' to add a port to listen to \n"
      " 'D <PORT>' to stop listening on port \n"
      " 'Q' to quit \n", msg
   );
}

static gboolean handle_keyboard(GIOChannel *source, GIOCondition cond, CustomData *data){
   gchar *str = NULL;
   gchar *clients;
   gchar **ipport;

   if(g_io_channel_read_line(source, &str, NULL, NULL, NULL) != G_IO_STATUS_NORMAL){
      return TRUE;
   }

   switch (g_ascii_tolower (str[0])){
      case 'q':
         g_main_loop_quit(data->loop);
         break;

      case 'c':
         /* Client added */
         ipport = g_strsplit((str+2), ":", 2);
         g_signal_emit_by_name(data->sink, "add", ipport[0], atoi(ipport[1]), NULL);
         g_strfreev(ipport);
         g_object_get(data->sink, "clients", &clients, NULL);
         g_print("Client list: %s \n", clients);
         g_free(clients);
         break;

      case 'r':
         ipport = g_strsplit((str+2), ":", 2);
         g_signal_emit_by_name(data->sink, "remove", ipport[0], atoi(ipport[1]), NULL);
         g_strfreev(ipport);
         break;

      case 'p':
         makeReceiverBin(str+2, data);
         break;

      case 'd':
         breakReceiverBin(str+2, data);
         break;

      default:
         print_menu("Select option then press enter!\n");
         break;
   }
   g_free(str);
   return TRUE;
}

static gboolean makeReceiverBin(gchar *port, CustomData *data){
   Receiver rec;
   gchar *bin_name;

   /* Name the pipeline: RecBin<PORT> */
   bin_name = g_strconcat("RecBin", port, NULL);

   rec.rsource = gst_element_factory_make("udpsrc","rsource");
   rec.rjitterbuffer = gst_element_factory_make("gstrtpjitterbuffer","rjitterbuffer");
   rec.rdepay = gst_element_factory_make("rtpopusdepay","rdepay");
   rec.rdecoder = gst_element_factory_make("opusdec","rdecoder");
   rec.rsink = gst_element_factory_make("alsasink","rsink");
  
   rec.rpipeline = gst_pipeline_new(bin_name);

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
   g_object_set(rec.rsource, "caps", rec.caps, "port", atoi(port), NULL);

   gst_element_set_state(rec.rpipeline, GST_STATE_PLAYING);

   gst_bin_add(GST_BIN(data->bin), rec.rpipeline);

   return TRUE;
}

static gboolean breakReceiverBin(gchar *port, CustomData *data){
   gchar *bin_name;
   GstElement *deletebin;
   bin_name = g_strconcat("RecBin", port, NULL);
   deletebin = gst_bin_get_by_name(GST_BIN(data->bin), bin_name);
   gst_bin_remove(GST_BIN(data->bin), deletebin);
   gst_element_set_state(deletebin, GST_STATE_NULL);
   gst_object_unref(deletebin);
   return TRUE;
}

int main (int argc, char *argv[]){
   CustomData data;
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

   gst_bin_add(GST_BIN(data.bin), data.spipeline);

   print_menu("");

   io_stdin = g_io_channel_unix_new(fileno(stdin));   
   g_io_add_watch(io_stdin, G_IO_IN, (GIOFunc)handle_keyboard, &data);

   gst_element_set_state(data.spipeline, GST_STATE_PLAYING);
   gst_element_set_state(data.bin, GST_STATE_PLAYING);
   data.loop = g_main_loop_new(NULL, FALSE);
   g_main_loop_run(data.loop);

   /* Free allocated stuff */
   g_main_loop_unref(data.loop);

   gst_element_set_state(data.bin, GST_STATE_NULL);
   gst_object_unref(data.bin);
   return 0;
}

