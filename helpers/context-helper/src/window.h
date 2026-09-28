// Foreground window discovery (Hyprland IPC) and /proc process details.
#pragma once

#include <stdbool.h>

typedef struct {
    int pid;
    char *title;
    char *cls;       // Hyprland "class" (Wayland app_id / X11 WM_CLASS)
    int x, y, w, h;
    bool visible;
    bool xwayland;
} fg_window_t;

// 1 = got a focused window, 0 = compositor reachable but nothing focused,
// -1 = Hyprland IPC unavailable.
int hypr_active_window(fg_window_t *w, int timeout_ms);
void fg_window_free(fg_window_t *w);

char *proc_exe_path(int pid);              // "" if unknown
char *proc_fallback_name(int pid);         // argv[0] basename or comm, "" if unknown
char *path_basename(const char *path);

// Builds the getFocusedAppInfo JSON (always a valid JSON object string).
char *build_app_info_json(void);
