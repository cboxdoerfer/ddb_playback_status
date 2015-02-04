/*
    Playback Status Widget plugin for the DeaDBeeF audio player

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <fcntl.h>
#include <gtk/gtk.h>

#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>

#include "fastftoi.h"
#include "support.h"

#define REFRESH_INTERVAL 25
#define MAX_LINES 10

#define     CONFSTR_VM_REFRESH_INTERVAL       "playback_status.refresh_interval"
#define     CONFSTR_VM_COLOR_BG               "playback_status.color.background"

/* Global variables */
static DB_misc_t            plugin;
static DB_functions_t *     deadbeef = NULL;
static ddb_gtkui_t *        gtkui_plugin = NULL;

typedef struct {
    char *format;
    PangoFontDescription *desc;
    GdkColor clr;
} text_context_t;

typedef struct {
    ddb_gtkui_widget_t base;
    GtkWidget *drawarea;
    GtkWidget *popup;
    GtkWidget *popup_item;
    cairo_surface_t *surf;
    guint drawtimer;
    text_context_t **text_ctx;
    intptr_t mutex;
} w_playback_status_t;

static int CONFIG_REFRESH_INTERVAL = 100;
static GdkColor CONFIG_COLOR_BG;

static void
save_config (void)
{
    deadbeef->conf_set_int (CONFSTR_VM_REFRESH_INTERVAL,            CONFIG_REFRESH_INTERVAL);
    char color[100];
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_COLOR_BG.red, CONFIG_COLOR_BG.green, CONFIG_COLOR_BG.blue);
    deadbeef->conf_set_str (CONFSTR_VM_COLOR_BG, color);
}

static void
load_config (void)
{
    deadbeef->conf_lock ();
    CONFIG_REFRESH_INTERVAL = deadbeef->conf_get_int (CONFSTR_VM_REFRESH_INTERVAL,          100);
    const char *color;
    color = deadbeef->conf_get_str_fast (CONFSTR_VM_COLOR_BG,                   "8738 8738 8738");
    sscanf (color, "%hd %hd %hd", &CONFIG_COLOR_BG.red, &CONFIG_COLOR_BG.green, &CONFIG_COLOR_BG.blue);

    deadbeef->conf_unlock ();
}

static int
on_config_changed (gpointer user_data, uintptr_t ctx)
{
    w_playback_status_t *w = user_data;
    load_config ();
    return 0;
}

static void
on_button_config (GtkMenuItem *menuitem, gpointer user_data)
{
    GtkWidget *playback_status_properties;
    GtkWidget *config_dialog;
    GtkWidget *hbox01;
    GtkWidget *dialog_action_area13;
    GtkWidget *applybutton1;
    GtkWidget *cancelbutton1;
    GtkWidget *okbutton1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    playback_status_properties = gtk_dialog_new ();
    gtk_window_set_title (GTK_WINDOW (playback_status_properties), "Playback Status Properties");
    gtk_window_set_type_hint (GTK_WINDOW (playback_status_properties), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_resizable (GTK_WINDOW (playback_status_properties), FALSE);

    config_dialog = gtk_dialog_get_content_area (GTK_DIALOG (playback_status_properties));
    gtk_widget_show (config_dialog);

    hbox01 = gtk_hbox_new (FALSE, 8);
    gtk_widget_show (hbox01);
    gtk_box_pack_start (GTK_BOX (config_dialog), hbox01, FALSE, FALSE, 0);
    gtk_container_set_border_width (GTK_CONTAINER (hbox01), 12);

    dialog_action_area13 = gtk_dialog_get_action_area (GTK_DIALOG (playback_status_properties));
    gtk_widget_show (dialog_action_area13);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (dialog_action_area13), GTK_BUTTONBOX_END);

    applybutton1 = gtk_button_new_from_stock ("gtk-apply");
    gtk_widget_show (applybutton1);
    gtk_dialog_add_action_widget (GTK_DIALOG (playback_status_properties), applybutton1, GTK_RESPONSE_APPLY);
    gtk_widget_set_can_default (applybutton1, TRUE);

    cancelbutton1 = gtk_button_new_from_stock ("gtk-cancel");
    gtk_widget_show (cancelbutton1);
    gtk_dialog_add_action_widget (GTK_DIALOG (playback_status_properties), cancelbutton1, GTK_RESPONSE_CANCEL);
    gtk_widget_set_can_default (cancelbutton1, TRUE);

    okbutton1 = gtk_button_new_from_stock ("gtk-ok");
    gtk_widget_show (okbutton1);
    gtk_dialog_add_action_widget (GTK_DIALOG (playback_status_properties), okbutton1, GTK_RESPONSE_OK);
    gtk_widget_set_can_default (okbutton1, TRUE);

    for (;;) {
        int response = gtk_dialog_run (GTK_DIALOG (playback_status_properties));
        if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) {
            save_config ();
            deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
        }
        if (response == GTK_RESPONSE_APPLY) {
            continue;
        }
        break;
    }
    gtk_widget_destroy (playback_status_properties);
#pragma GCC diagnostic pop
    return;
}

static void
update_text_context (text_context_t *ctx, const char *format, const char *font_name, GdkColor clr)
{
    if (ctx) {
        if (ctx->format) {
            free (ctx->format);
        }
        ctx->format = strdup (format);
        if (ctx->desc) {
            pango_font_description_free(ctx->desc);
        }
        ctx->desc = pango_font_description_from_string (font_name);
        ctx->clr = clr;
    }
}

static text_context_t *
get_text_context (const char *format, const char *font_name, GdkColor clr)
{
    text_context_t *ctx = malloc (sizeof (text_context_t));
    ctx->format = strdup (format);
    ctx->desc = pango_font_description_from_string (font_name);
    ctx->clr = clr;
    return ctx;
}

static void
free_text_context (text_context_t *ctx)
{
    if (ctx) {
        if (ctx->format) {
            free (ctx->format);
        }
        if (ctx->desc) {
            pango_font_description_free(ctx->desc);
        }
        free (ctx);
    }
}

static void
w_playback_status_destroy (ddb_gtkui_widget_t *w) {
    w_playback_status_t *s = (w_playback_status_t *)w;
    deadbeef->vis_waveform_unlisten (w);
    if (s->drawtimer) {
        g_source_remove (s->drawtimer);
        s->drawtimer = 0;
    }
    if (s->surf) {
        cairo_surface_destroy (s->surf);
        s->surf = NULL;
    }
    if (s->text_ctx) {
        for (int i = 0; i < MAX_LINES; i++) {
            free_text_context (s->text_ctx[i]);
        }
        free (s->text_ctx);
    }
    if (s->mutex) {
        deadbeef->mutex_free (s->mutex);
        s->mutex = 0;
    }
}

static gboolean
playback_status_draw_cb (void *data) {
    w_playback_status_t *s = data;
    gtk_widget_queue_draw (s->drawarea);
    return TRUE;
}

static gboolean
playback_status_set_refresh_interval (gpointer user_data, int interval)
{
    w_playback_status_t *w = user_data;
    if (!w || interval <= 0) {
        return FALSE;
    }
    if (w->drawtimer) {
        g_source_remove (w->drawtimer);
        w->drawtimer = 0;
    }
    w->drawtimer = g_timeout_add (interval, playback_status_draw_cb, w);
    return TRUE;
}

static int
playback_status_draw_text (cairo_t *cr, const char *text, const char *font, int x, int y, int width) {
    PangoLayout *layout;
    PangoFontDescription *desc;

    cairo_move_to (cr, x, y);

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_width (layout, width*PANGO_SCALE);
    pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, text, -1);

    desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    pango_cairo_show_layout(cr, layout);

    int layout_width = 0;
    int layout_height = 0;
    pango_layout_get_pixel_size (layout, &layout_width, &layout_height);

    g_object_unref(layout);
    return layout_height;
}


static gboolean
playback_status_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    w_playback_status_t *w = user_data;
    GtkAllocation a;
    gtk_widget_get_allocation (w->drawarea, &a);

    const double width = a.width;
    const double height = a.height;

    DB_playItem_t *playing = deadbeef->streamer_get_playing_track ();
    char title[1024];
    char artist_album[1024];
    char playback_time[1024];

    if (playing) {
        deadbeef->pl_format_title (playing, -1, playback_time, sizeof (playback_time), -1, "%e / %l");
        deadbeef->pl_format_title (playing, -1, artist_album, sizeof (artist_album), -1, "%B - (%y) %b");
        deadbeef->pl_format_title (playing, -1, title, sizeof (title), -1, "%n. %t");
        deadbeef->pl_item_unref (playing);
    }
    else {
        snprintf (playback_time, sizeof (playback_time), "%s", "-- / -- (stopped)");
        snprintf (artist_album, sizeof (artist_album), "%s", "");
        snprintf (title, sizeof (title), "%s", "");
    }

    int x = 6;
    int y = 6;
    int text_width = width - x;
    cairo_set_source_rgba (cr, 0, 0, 0, 1);
    y += playback_status_draw_text (cr, playback_time, "Source Sans Pro Bold 14", x, y, text_width);
    y += playback_status_draw_text (cr, title, "Source Sans Pro Regular 12", x, y, text_width);
    y += playback_status_draw_text (cr, artist_album, "Source Sans Pro Regular 10", x, y, text_width);

    return FALSE;
}

static gboolean
playback_status_expose_event (GtkWidget *widget, GdkEventExpose *event, gpointer user_data) {
    cairo_t *cr = gdk_cairo_create (gtk_widget_get_window (widget));
    gboolean res = playback_status_draw (widget, cr, user_data);
    cairo_destroy (cr);
    return res;
}

static gboolean
playback_status_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    //w_playback_status_t *w = user_data;
    if (event->button == 3) {
      return TRUE;
    }
    return TRUE;
}

static gboolean
playback_status_button_release_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    w_playback_status_t *w = user_data;
    if (event->button == 3) {
      gtk_menu_popup (GTK_MENU (w->popup), NULL, NULL, NULL, w->drawarea, 0, gtk_get_current_event_time ());
      return TRUE;
    }
    return TRUE;
}

static int
playback_status_message (ddb_gtkui_widget_t *widget, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2)
{
    w_playback_status_t *w = (w_playback_status_t *)widget;

    switch (id) {
        case DB_EV_SONGSTARTED:
            playback_status_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
            break;
        case DB_EV_PAUSED:
            if (deadbeef->get_output ()->state () == OUTPUT_STATE_PLAYING) {
                playback_status_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
            }
            else {
                if (w->drawtimer) {
                    g_source_remove (w->drawtimer);
                    w->drawtimer = 0;
                }
            }
            break;
        case DB_EV_STOP:
            if (w->drawtimer) {
                g_source_remove (w->drawtimer);
                w->drawtimer = 0;
            }
            gtk_widget_queue_draw (w->drawarea);
            break;
        case DB_EV_CONFIGCHANGED:
            on_config_changed (w, ctx);
            playback_status_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
            break;
    }
    return 0;
}

static void
w_playback_status_init (ddb_gtkui_widget_t *w) {
    w_playback_status_t *s = (w_playback_status_t *)w;
    s->text_ctx = malloc (sizeof (text_context_t) * MAX_LINES);
    memset (s->text_ctx, 0, sizeof (text_context_t) * MAX_LINES);
    load_config ();
    deadbeef->mutex_lock (s->mutex);

    playback_status_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
    deadbeef->mutex_unlock (s->mutex);
}

ddb_gtkui_widget_t *
w_playback_status_create (void) {
    w_playback_status_t *w = malloc (sizeof (w_playback_status_t));
    memset (w, 0, sizeof (w_playback_status_t));

    w->base.widget = gtk_event_box_new ();
    w->base.init = w_playback_status_init;
    w->base.destroy  = w_playback_status_destroy;
    w->base.message = playback_status_message;
    w->drawarea = gtk_drawing_area_new ();
    w->popup = gtk_menu_new ();
    w->popup_item = gtk_menu_item_new_with_mnemonic ("Configure");
    w->mutex = deadbeef->mutex_create ();
    gtk_widget_set_size_request (w->base.widget, 16, 16);

    gtk_container_add (GTK_CONTAINER (w->base.widget), w->drawarea);
    gtk_container_add (GTK_CONTAINER (w->popup), w->popup_item);
    gtk_widget_show (w->drawarea);
    gtk_widget_show (w->popup);
    gtk_widget_show (w->popup_item);

#if !GTK_CHECK_VERSION(3,0,0)
    g_signal_connect_after ((gpointer) w->drawarea, "expose_event", G_CALLBACK (playback_status_expose_event), w);
#else
    g_signal_connect_after ((gpointer) w->drawarea, "draw", G_CALLBACK (playback_status_draw), w);
#endif
    g_signal_connect_after ((gpointer) w->base.widget, "button_press_event", G_CALLBACK (playback_status_button_press_event), w);
    g_signal_connect_after ((gpointer) w->base.widget, "button_release_event", G_CALLBACK (playback_status_button_release_event), w);
    g_signal_connect_after ((gpointer) w->popup_item, "activate", G_CALLBACK (on_button_config), w);
    gtkui_plugin->w_override_signals (w->base.widget, w);
    gtk_widget_set_events (w->base.widget, GDK_EXPOSURE_MASK
                                         | GDK_LEAVE_NOTIFY_MASK
                                         | GDK_BUTTON_PRESS_MASK
                                         | GDK_POINTER_MOTION_MASK
                                         | GDK_POINTER_MOTION_HINT_MASK);
    return (ddb_gtkui_widget_t *)w;
}

int
playback_status_connect (void)
{
    gtkui_plugin = (ddb_gtkui_t *) deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui_plugin) {
        //trace("using '%s' plugin %d.%d\n", DDB_GTKUI_PLUGIN_ID, gtkui_plugin->gui.plugin.version_major, gtkui_plugin->gui.plugin.version_minor );
        if (gtkui_plugin->gui.plugin.version_major == 2) {
            //printf ("fb api2\n");
            // 0.6+, use the new widget API
            gtkui_plugin->w_reg_widget ("Playback Status Widget", 0, w_playback_status_create, "playback_status", NULL);
            return 0;
        }
    }
    return -1;
}

int
playback_status_start (void)
{
    load_config ();
    return 0;
}

int
playback_status_stop (void)
{
    save_config ();
    return 0;
}

int
playback_status_startup (GtkWidget *cont)
{
    return 0;
}

int
playback_status_shutdown (GtkWidget *cont)
{
    return 0;
}
int
playback_status_disconnect (void)
{
    gtkui_plugin = NULL;
    return 0;
}

static const char settings_dlg[] =
    "property \"Refresh interval (ms): \"           spinbtn[10,1000,1] "      CONFSTR_VM_REFRESH_INTERVAL         " 25 ;\n"
;

static DB_misc_t plugin = {
    //DB_PLUGIN_SET_API_VERSION
    .plugin.type            = DB_PLUGIN_MISC,
    .plugin.api_vmajor      = 1,
    .plugin.api_vminor      = 5,
    .plugin.version_major   = 0,
    .plugin.version_minor   = 1,
#if GTK_CHECK_VERSION(3,0,0)
    .plugin.id              = "playback_status-gtk3",
#else
    .plugin.id              = "playback_status",
#endif
    .plugin.name            = "Playback Status Widget",
    .plugin.descr           = "Playback Status Widget",
    .plugin.copyright       =
        "Copyright (C) 2013 Christian Boxdörfer <christian.boxdoerfer@posteo.de>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website         = "https://github.com/cboxdoerfer/ddb_playback_status",
    .plugin.start           = playback_status_start,
    .plugin.stop            = playback_status_stop,
    .plugin.connect         = playback_status_connect,
    .plugin.disconnect      = playback_status_disconnect,
    .plugin.configdialog    = settings_dlg,
};

#if !GTK_CHECK_VERSION(3,0,0)
DB_plugin_t *
ddb_misc_playback_status_GTK2_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#else
DB_plugin_t *
ddb_misc_playback_status_GTK3_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#endif
