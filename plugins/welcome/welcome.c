/*
 * Copyright (C) 2026 RenCupDotDev
 *
 * This file is part of NeoNanoQOL.
 *
 * NeoNanoQOL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NeoNanoQOL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* welcome.c - a start page, shown when neonano opens with no file.
 *
 *   opens by itself on an empty start (unless [welcome] show = false)
 *   :welcome          show it again
 * Keys:
 *   up/down or the letter in brackets, enter   choose an action
 *   esc                                        close it
 * Actions run the matching command: dirtree, terminal, lsp start, cfg, help, exit.
 *
 * Shows: a full-screen overlay pane (PANE_OVERLAY), run_command (API v6),
 * and config_get for a plugin setting.
 */
#define _XOPEN_SOURCE 700
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "plugin.h"

#define K_DOWN 0402
#define K_UP   0403
#define MARGIN "              "            /* left margin for the pane text */
#define MAX_LINES 64

static const PluginAPI *api;
static int  overlay_id;
static bool checked;
static const char *lines[MAX_LINES];
static char buf[MAX_LINES][160];
static int  nlines;
static int  item_row[8];
static int  selected;

static const char *logo[] = {
    "   ____  ___  ____  ____  ____ _____  ____",
    "  / __ \\/ _ \\/ __ \\/ __ \\/ __ `/ __ \\/ __ \\",
    " / / / /  __/ /_/ / / / / /_/ / / / / /_/ /",
    "/_/ /_/\\___/\\____/_/ /_/\\__,_/_/ /_/\\____/"
};

typedef struct { char key; const char *label; const char *command; } Item;
static const Item items[] = {
    { 'n', "New file",                 NULL },
    { 'f', "Find file (directory tree)", "dirtree" },
    { 't', "Open a terminal",          "terminal" },
    { 'l', "Start the language server", "lsp start" },
    { 'c', "Open the config file",     "cfg" },
    { 'h', "Command help",             "help" },
    { 'q', "Quit",                     "exit" },
};
#define NITEMS ((int)(sizeof items / sizeof items[0]))

static bool on_key(Editor *e, int key, void *ud);

static void add_line(const char *text) {
    if (nlines >= MAX_LINES) return;
    snprintf(buf[nlines], sizeof buf[nlines], "%s", text);
    lines[nlines] = buf[nlines];
    nlines++;
}

/* Builds the pane text: logo, the action list, and a footer. */
static void build(void) {
    nlines = 0;
    add_line("");
    add_line("");
    for (size_t i = 0; i < sizeof logo / sizeof logo[0]; i++) {
        char t[160];
        snprintf(t, sizeof t, MARGIN "%s", logo[i]);
        add_line(t);
    }
    add_line("");
    add_line(MARGIN "A terminal editor that stays out of your way");
    add_line("");
    for (int i = 0; i < NITEMS; i++) {
        char t[160];
        snprintf(t, sizeof t, MARGIN "  [%c]  %s", items[i].key, items[i].label);
        item_row[i] = nlines;
        add_line(t);
    }
    add_line("");
    add_line(MARGIN "up/down move   enter choose   esc close");
}

static void show_pane(void) {
    build();
    overlay_id = api->pane_open("Welcome", PANE_OVERLAY, 0, on_key, NULL);
}

static void close_pane(void) {
    if (overlay_id) {
        api->pane_close(overlay_id);
        overlay_id = 0;
    }
}

static void run_item(Editor *e, int i) {
    close_pane();
    if (items[i].command) api->run_command(e, items[i].command);
}

static void sync_pane(void) {
    if (!overlay_id) return;
    api->pane_set_lines(overlay_id, lines, nlines);
    api->pane_set_cursor(overlay_id, item_row[selected]);
}

static bool on_key(Editor *e, int key, void *ud) {
    (void)ud;
    if (key == K_UP) {
        if (selected > 0) selected--;
    } else if (key == K_DOWN) {
        if (selected + 1 < NITEMS) selected++;
    } else if (key == 10) {
        run_item(e, selected);
        return true;
    } else if (key == 27) {
        close_pane();
        return true;
    } else if (key & PLUGIN_KEY_TEXT) {
        int ch = key & ~PLUGIN_KEY_TEXT;
        for (int i = 0; i < NITEMS; i++) {
            if (ch == items[i].key) {
                run_item(e, i);
                return true;
            }
        }
        return true;                       /* other letters do nothing here */
    } else {
        return false;
    }
    sync_pane();
    return true;
}

/* Runs about every 100 ms. Opens the page once, for an empty start. */
static void on_tick(Editor *e) {
    if (checked) return;
    checked = true;
    const char *show = api->config_get("welcome", "show");
    if (show && strcmp(show, "false") == 0) return;
    size_t len = 0;
    const char *text = api->line_text(e, 0, &len);
    bool empty = api->line_count(e) == 1 && len == 0 && (text != NULL);
    if (empty && api->file_path(e) == NULL && api->buffer_version(e) == 0) {
        selected = 0;
        show_pane();
        sync_pane();
    }
}

static void cmd_welcome(Editor *e, const char *arg) {
    (void)e;
    (void)arg;
    if (overlay_id) {
        close_pane();
        return;
    }
    selected = 0;
    show_pane();
    sync_pane();
}

void neonano_plugin_init(const PluginAPI *a) {
    if (a->version < 6) return;            /* needs PANE_OVERLAY and run_command */
    api = a;
    a->add_command("welcome", "welcome  show or hide the start page", cmd_welcome);
    a->on_tick(on_tick);
}
