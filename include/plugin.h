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
#ifndef PLUGIN_H
#define PLUGIN_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Plugin interface. A plugin is a shared object (.so) that exports
 *     void neonano_plugin_init(const PluginAPI *api);
 * and is built with -fPIC -shared -Isrc. It must not call neonano
 * functions directly; use the functions in PluginAPI.
 *
 * version 1: commands, messages, read-only line access.
 * version 2: file_path, get_cursor, set_cursor.
 * version 3: ticks, save hook, key bindings, edit counter, decorations,
 *            popups, text edits, open_path, prompt.
 * version 4: panes (tiled windows that plugins fill), filterable popups.
 * version 5: config_get, so plugins read their own [section] settings.
 * version 6: run_command, and PANE_OVERLAY.
 * New fields are only ever added at the end.
 */

#define PLUGIN_API_VERSION 6

/* PLUGIN_VERSION is set by the build from the VERSION file. The string is
 * exported from every plugin .so as neonano_plugin_version, so the version
 * can be read from the built file (dlsym or strings). */
#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "unknown"
#endif
const char neonano_plugin_version[] = PLUGIN_VERSION;
#define PLUGIN_ENTRY       "neonano_plugin_init"

/* Keys delivered to a pane: a typed character is PLUGIN_KEY_TEXT | codepoint.
 * Other keys use the key ids from keys.h (KEY_UP, 10 for enter, 27 for esc, ...). */
#define PLUGIN_KEY_TEXT 0x400000

/* Where a pane opens, relative to the rest of the layout. */
enum { PANE_LEFT = 0, PANE_RIGHT = 1, PANE_TOP = 2, PANE_BOTTOM = 3,
       PANE_OVERLAY = 4 };   /* covers the whole layout area; the editor is hidden while open */

typedef struct Editor Editor;
typedef void (*CmdFn)(Editor *e, const char *arg);
typedef void (*TickFn)(Editor *e);
typedef void (*SaveFn)(Editor *e);
typedef void (*PopupPick)(Editor *e, int index, void *ud);
typedef void (*PopupFilter)(Editor *e, void *ud);
/* Returns true if the pane used the key. */
typedef bool (*PaneKeyFn)(Editor *e, int key, void *ud);

/* Replaces the text from (r0,c0) to (r1,c1) with text. Lines are 0-based,
 * columns are byte offsets. */
typedef struct {
    size_t      r0, c0, r1, c1;
    const char *text;
} TextEdit;

typedef struct PluginAPI {
    int version;
    /* v1 */
    bool   (*add_command)(const char *name, const char *help, CmdFn fn);
    void   (*message)(Editor *e, const char *text);
    size_t (*line_count)(const Editor *e);
    const char *(*line_text)(const Editor *e, size_t row, size_t *len);
    /* v2 */
    const char *(*file_path)(const Editor *e);            /* NULL if untitled */
    void   (*get_cursor)(const Editor *e, size_t *row, size_t *col);
    void   (*set_cursor)(Editor *e, size_t row, size_t col);
    /* v3 */
    void   (*on_tick)(TickFn fn);                          /* about every 100 ms when idle */
    void   (*on_save)(SaveFn fn);                          /* runs before the file is written */
    bool   (*bind_key)(const char *key, const char *command);
    unsigned long (*buffer_version)(const Editor *e);      /* changes on every edit */
    void   (*decor_clear)(const char *owner);
    /* kind: 1 error, 2 warning, 3 info, 4 hint */
    void   (*decor_add)(const char *owner, size_t row, size_t c0, size_t c1,
                        int kind, const char *msg);
    void   (*popup_open)(Editor *e, const char *title, const char **lines, int n,
                         bool selectable, PopupPick pick, void *ud);
    void   (*popup_close)(void);
    void   (*apply_edits)(Editor *e, const TextEdit *edits, int n);   /* in the order given, one undo step */
    bool   (*open_path)(Editor *e, const char *path, size_t row, size_t col);
    bool   (*prompt)(const char *label, char *buf, size_t cap);
    /* v4 */
    int    (*pane_open)(const char *name, int side, int percent, PaneKeyFn on_key, void *ud);
    void   (*pane_close)(int id);
    void   (*pane_set_lines)(int id, const char **lines, int n);
    void   (*pane_set_cursor)(int id, int row);
    void   (*pane_focus)(int id);                          /* 0 focuses the editor */
    void   (*popup_filterable)(PopupFilter fn, void *ud);  /* call after popup_open */
    /* v5 */
    const char *(*config_get)(const char *section, const char *key);   /* NULL if not set */
    /* v6 */
    void   (*run_command)(Editor *e, const char *command);   /* as if typed in the F1 palette */
} PluginAPI;

typedef void (*PluginInit)(const PluginAPI *api);

void        plugins_load(Editor *e);
void        plugins_list(Editor *e);

/* Used by the core. */
void        plugin_fire_tick(Editor *e);
void        plugin_fire_save(Editor *e);
const char *plugin_key_command(int key);               /* NULL if no plugin binds the key */

#endif
