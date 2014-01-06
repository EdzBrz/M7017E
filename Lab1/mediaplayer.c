#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/interfaces/xoverlay.h>

#include <gdk/gdkkeysyms.h>

#include <gdk/gdk.h>
#if defined (GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#elif defined (GDK_WINDOWING_WIN32)
#include <gdk/gdkwin32.h>
#elif defined (GDK_WINDOWING_QUARTZ)
#include <gdk/gdkquartz.h>
#endif

typedef struct _CustomData{
   GstElement *playbin2;
   GtkWidget *slider;
   gulong slider_update_signal_id;

   GstState state;
   gint64 duration;
   gdouble rate;
} CustomData;

static void change_rate(CustomData *data);

static void realize_cb (GtkWidget *widget, CustomData *data){
   GdkWindow *window = gtk_widget_get_window (widget);
   guintptr window_handle;

   if (!gdk_window_ensure_native(window))
      g_error("Couldn't create native window needed for GstXOverlay!\n");

#if defined (GDK_WINDOWING_WIN32)
   window_handle = (guintptr)GDK_WINDOW_HWND(window);
#elif defined (GDK_WINDOWING_QUARTZ)
   window_handle = gdk_quartz_window_get_nsview(window);
#elif defined (GDK_WINDOWING_X11)
   window_handle = GDK_WINDOW_XID(window);
#endif

   gst_x_overlay_set_window_handle (GST_X_OVERLAY (data->playbin2), window_handle);
}

static void play_cb (GtkButton *button, CustomData *data){
   if (data->rate != 1.0){   
      data->rate = 1.0;
      change_rate(data);
   }
   gst_element_set_state (data->playbin2, GST_STATE_PLAYING);
}

static void pause_cb (GtkButton *button, CustomData *data){
   gst_element_set_state (data->playbin2, GST_STATE_PAUSED);
}

static void stop_cb (GtkButton *button, CustomData *data){
   gst_element_set_state(data->playbin2, GST_STATE_READY);
}

static void forward_cb(GtkButton *button, CustomData *data){
   data->rate = 2.0;
   change_rate(data);
}

static void rewind_cb(GtkButton *button, CustomData *data){
   data->rate = -2.0;
   change_rate(data);
}

static void open_cb(GtkButton *button, CustomData *data){
   GstFormat fmt = GST_FORMAT_TIME;
   GtkWidget *window = gtk_widget_get_toplevel (GTK_WIDGET(button)); 
   GtkWidget *dialog;
   char *fileuri;
   char *extension;

   dialog = gtk_file_chooser_dialog_new ("Open File",
      GTK_WINDOW(window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
      NULL);


   if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT){
      fileuri = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog));


      extension = strrchr(fileuri, '.');
      if (strcasecmp(extension, ".AVI") == 0){
         g_print("Extension: %s \n", extension);
         gst_element_set_state(data->playbin2, GST_STATE_READY);
         g_object_set(data->playbin2, "uri", fileuri, NULL);
      }
      else{
         g_printerr("Sorry the format %s is not supported. \n", extension);
      }
   }

   gtk_range_set_value(GTK_RANGE(data->slider), (gdouble)0 * GST_SECOND);
   gtk_widget_destroy (dialog);

   gst_element_set_state(data->playbin2, GST_STATE_PAUSED);

   data->duration = GST_CLOCK_TIME_NONE;
   if (!GST_CLOCK_TIME_IS_VALID(data->duration)){
      if (!gst_element_query_duration(data->playbin2, &fmt, &data->duration)){
         g_printerr("Could not query current duration.\n");
      }
      else {
         gtk_range_set_range(GTK_RANGE(data->slider), 0, (gdouble)data->duration / GST_SECOND);
      }
   }
}

static void fullscreen_cb(GtkButton *button, CustomData *data){
   GtkWidget *window = gtk_widget_get_toplevel (GTK_WIDGET(button)); 
   GtkWidget *buttons = gtk_widget_get_parent(GTK_WIDGET(button));   
   gtk_widget_hide(GTK_WIDGET(buttons));
   gtk_window_fullscreen(GTK_WINDOW(window));
}

static void unfullscreen_cb (GtkWidget *widget, GdkEventKey *event, CustomData *data){
   switch (event->keyval){
      case GDK_Escape:
         gtk_widget_show_all(GTK_WIDGET(widget));
         gtk_window_unfullscreen(GTK_WINDOW(widget));
   }
}


static void delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data){
   stop_cb (NULL, data);
   gtk_main_quit();
}

static gboolean expose_cb (GtkWidget *widget, GdkEventExpose *event, CustomData *data){
   if (data->state < GST_STATE_PAUSED){
      GtkAllocation allocation;
      GdkWindow *window = gtk_widget_get_window(widget);
      cairo_t *cr;

      gtk_widget_get_allocation(widget, &allocation);
      cr = gdk_cairo_create(window);
      cairo_set_source_rgb(cr, 0,0,0);
      cairo_rectangle(cr,0,0, allocation.width, allocation.height);
      cairo_fill(cr);
      cairo_destroy(cr);
   }
   return FALSE;
}

static void slider_cb (GtkRange *range, CustomData *data){
   gdouble value = gtk_range_get_value(GTK_RANGE(data->slider));
   gst_element_seek_simple(data->playbin2, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, (gint64)(value * GST_SECOND));
}

static void create_ui (CustomData *data){
   GtkWidget *main_window;
   GtkWidget *video_window;
   GtkWidget *controls;
   GtkWidget *main_box;
   GtkWidget *main_hbox;
   GtkWidget *play_button, *pause_button, *stop_button, *forward_button, *rewind_button, *fullscreen_button, *open_button;

   main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
   g_signal_connect (G_OBJECT(main_window), "delete-event", G_CALLBACK(delete_event_cb), data);
   
   video_window = gtk_drawing_area_new();
   gtk_widget_set_double_buffered (video_window, FALSE);
   g_signal_connect(video_window, "realize", G_CALLBACK(realize_cb), data);
   g_signal_connect(video_window, "expose_event", G_CALLBACK(expose_cb), data);

   play_button = gtk_button_new_from_stock (GTK_STOCK_MEDIA_PLAY);
   g_signal_connect(G_OBJECT(play_button), "clicked", G_CALLBACK(play_cb), data);

   pause_button = gtk_button_new_from_stock (GTK_STOCK_MEDIA_PAUSE);
   g_signal_connect(G_OBJECT(pause_button), "clicked", G_CALLBACK(pause_cb), data);

   stop_button = gtk_button_new_from_stock (GTK_STOCK_MEDIA_STOP);
   g_signal_connect(G_OBJECT(stop_button), "clicked", G_CALLBACK(stop_cb), data);

   fullscreen_button = gtk_button_new_with_label("Fullscreen");
   g_signal_connect(G_OBJECT(fullscreen_button), "clicked", G_CALLBACK(fullscreen_cb), data);
   
   open_button = gtk_button_new_with_label("Open");
   g_signal_connect(G_OBJECT(open_button), "clicked", G_CALLBACK(open_cb), data);

   forward_button = gtk_button_new_with_label("Forward");
   g_signal_connect(G_OBJECT(forward_button), "clicked", G_CALLBACK(forward_cb), data);

   rewind_button = gtk_button_new_with_label("Rewind");
   g_signal_connect(G_OBJECT(rewind_button), "clicked", G_CALLBACK(rewind_cb), data);

   g_signal_connect (G_OBJECT(main_window), "key-press-event", G_CALLBACK(unfullscreen_cb), data);

   data->slider = gtk_hscale_new_with_range(0, 100, 1);
   gtk_scale_set_draw_value(GTK_SCALE(data->slider), 0);
   data->slider_update_signal_id = g_signal_connect(G_OBJECT(data->slider), "value-changed", G_CALLBACK(slider_cb), data);

   controls = gtk_hbox_new(FALSE, 0);
   gtk_box_pack_start(GTK_BOX(controls), data->slider, TRUE, TRUE, 2);
   gtk_box_pack_start(GTK_BOX(controls), open_button, FALSE, FALSE, 2);
   gtk_box_pack_start(GTK_BOX(controls), play_button, FALSE, FALSE, 2);
   gtk_box_pack_start(GTK_BOX(controls), stop_button, FALSE, FALSE, 2);
   gtk_box_pack_start(GTK_BOX(controls), pause_button, FALSE, FALSE, 2);
   gtk_box_pack_start(GTK_BOX(controls), fullscreen_button, FALSE, FALSE, 2);
   gtk_box_pack_start(GTK_BOX(controls), forward_button, FALSE, FALSE, 2);
   gtk_box_pack_start(GTK_BOX(controls), rewind_button, FALSE, FALSE, 2);

   main_hbox = gtk_vbox_new(FALSE, 0);
   gtk_box_pack_start(GTK_BOX(main_hbox), video_window, TRUE, TRUE, 0);

   main_box = gtk_vbox_new(FALSE, 0);
   gtk_box_pack_start(GTK_BOX(main_box), main_hbox, TRUE, TRUE, 0);
   gtk_box_pack_start(GTK_BOX(main_box), controls, FALSE, FALSE, 0);
   gtk_container_add(GTK_CONTAINER(main_window), main_box);
   gtk_window_set_default_size(GTK_WINDOW(main_window), 640, 480);
   gtk_widget_show_all(main_window);
}


static gboolean refresh_ui (CustomData *data){
   GstFormat fmt = GST_FORMAT_TIME;
   gint64 current = -1;

   if (data->state < GST_STATE_PAUSED){
      return TRUE;
   }

   if (!GST_CLOCK_TIME_IS_VALID(data->duration)){
      if (!gst_element_query_duration(data->playbin2, &fmt, &data->duration)){
         g_printerr("Could not query current duration.\n");
      }
      else {
         gtk_range_set_range(GTK_RANGE(data->slider), 0, (gdouble)data->duration / GST_SECOND);
      }
   }

   if (gst_element_query_position(data->playbin2, &fmt, &current)){
      g_signal_handler_block(data->slider, data->slider_update_signal_id);
      gtk_range_set_value(GTK_RANGE(data->slider), (gdouble)current / GST_SECOND);
      g_signal_handler_unblock(data->slider, data->slider_update_signal_id);
   }
   return TRUE;
}

static void tags_cb (GstElement *playbin2, gint stream, CustomData *data){
   gst_element_post_message(playbin2, gst_message_new_application(GST_OBJECT(playbin2), gst_structure_new("tags-changed", NULL)));   
}

static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data){
   GError *err;
   gchar *debug_info;
   
   gst_message_parse_error(msg, &err, &debug_info);
   g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
   g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
   g_clear_error(&err);
   g_free(debug_info);

   gst_element_set_state(data->playbin2, GST_STATE_READY);
}

static void eos_cb(GstBus *bus, GstMessage *msg, CustomData *data){
   g_print("End-of-stream reached.\n");
   gst_element_set_state(data->playbin2, GST_STATE_READY);
}

static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data){
   GstState old_state, new_state, pending_state;
   gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
   if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->playbin2)){
      data->state = new_state;
      g_print("State set to %s\n", gst_element_state_get_name(new_state));
      if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED){
         refresh_ui(data);
      }
   }
}

/* http://docs.gstreamer.com/display/GstSDK/Basic+tutorial+13%3A+Playback+speed */

static void change_rate(CustomData *data){
   GstFormat fmt = GST_FORMAT_TIME;
   gint64 current;
   GstEvent *seek_event;

   if (!gst_element_query_position(data->playbin2, &fmt, &current)){
      g_printerr("Couldn't query current position.\n");
      return;
   }

   if (data->rate > 0){
      seek_event = gst_event_new_seek(data->rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
        GST_SEEK_TYPE_SET, current, GST_SEEK_TYPE_NONE, 0);
   }
   else{
      seek_event = gst_event_new_seek(data->rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
        GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, current);   
   }

   gst_element_send_event(data->playbin2, seek_event);
}

int main(int argc, char *argv[]){
   CustomData data;
   GstStateChangeReturn ret;
   GstBus *bus;

   gtk_init(&argc, &argv);
   gst_init(&argc, &argv);

   memset (&data, 0, sizeof(data));
   data.duration = GST_CLOCK_TIME_NONE;
   data.rate = 1.0;

   data.playbin2 = gst_element_factory_make("playbin2", "playbin2");

   if (!data.playbin2){
      g_printerr("Not all elements could be created.\n");
      return -1;
   }

   g_object_set(data.playbin2, "uri", "http://docs.gstreamer.com/media/sintel_trailer-480p.webm", NULL);

   g_signal_connect(G_OBJECT(data.playbin2), "video-tags-changed", (GCallback)tags_cb, &data);
   g_signal_connect(G_OBJECT(data.playbin2), "audio-tags-changed", (GCallback)tags_cb, &data);
   g_signal_connect(G_OBJECT(data.playbin2), "text-tags-changed", (GCallback)tags_cb, &data);

   create_ui(&data);

   bus = gst_element_get_bus(data.playbin2);
   gst_bus_add_signal_watch(bus);
   g_signal_connect(G_OBJECT(bus), "message::error", (GCallback)error_cb, &data);
   g_signal_connect(G_OBJECT(bus), "message::eos", (GCallback)eos_cb, &data);
   g_signal_connect(G_OBJECT(bus), "message::state-changed", (GCallback)state_changed_cb, &data);
   gst_object_unref(bus);

   ret = gst_element_set_state(data.playbin2, GST_STATE_PLAYING);
   if (ret == GST_STATE_CHANGE_FAILURE){
      g_printerr("Unable to set the pipeline to the playing state.\n");
      gst_object_unref(data.playbin2);
      return -1;
   }

   g_timeout_add_seconds(1, (GSourceFunc)refresh_ui, &data);
   gtk_main();

   gst_element_set_state(data.playbin2, GST_STATE_NULL);
   gst_object_unref(data.playbin2);
   return 0;
}
