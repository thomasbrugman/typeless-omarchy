/* gtk_fixture.c - GTK3 window used as an AT-SPI test target (run it inside a
 * headless compositor, see test/run_headless_tests.sh).
 *   gtk_fixture entry|textview|password|button */
#include <gtk/gtk.h>
#include <string.h>

static const char *g_mode = "entry";
static GtkWidget *g_entry, *g_view, *g_pw, *g_button;

static gboolean setup_focus(gpointer data) {
    (void)data;
    if (!strcmp(g_mode, "entry")) {
        gtk_widget_grab_focus(g_entry);
        gtk_editable_select_region(GTK_EDITABLE(g_entry), 6, 11); /* "brave" */
    } else if (!strcmp(g_mode, "textview")) {
        gtk_widget_grab_focus(g_view);
        GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_view));
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(b, &it, 12);
        gtk_text_buffer_place_cursor(b, &it);
    } else if (!strcmp(g_mode, "password")) {
        gtk_widget_grab_focus(g_pw);
    } else {
        gtk_widget_grab_focus(g_button);
    }
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    if (argc > 1) g_mode = argv[1];
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Typeless AT-SPI fixture");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_entry), "Hello brave new world");
    g_view = gtk_text_view_new();
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_view)),
                             "First line\nSecond line with \"quotes\" and ümlauts\nThird", -1);
    g_pw = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(g_pw), FALSE);
    gtk_entry_set_text(GTK_ENTRY(g_pw), "secret");
    g_button = gtk_button_new_with_label("Press me");
    gtk_box_pack_start(GTK_BOX(box), g_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), g_view, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), g_pw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), g_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), box);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(win);
    g_timeout_add(300, setup_focus, NULL);
    gtk_main();
    return 0;
}
