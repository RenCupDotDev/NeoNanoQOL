/*
 * Copyright (C) 2026 RenCupDotDev
 *
 * This file is part of NeoNanoQOL.
 *
 * NeoNanoQOL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 *
 * NeoNanoQOL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* dirtree.c - directory tree pane with file operations.
 *   ctrl+b or :dirtree   open or close the tree, rooted at the current file's folder
 * In the pane:
 *   up/down move    right expands a folder    left collapses it
 *   enter           open a file, or toggle a folder
 *   n               new file in the selected folder (or the file's folder)
 *   N               new folder
 *   r               rename the selected entry
 *   d               delete (files, and empty folders; asks first)
 *   .               show or hide dotfiles
 *   esc or F9       back to the editor
 */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "plugin.h"

/* ncurses key ids, matching keys.h / curses.h */
#define K_DOWN  0402
#define K_UP    0403
#define K_LEFT  0404
#define K_RIGHT 0405

#define MAX_ROWS 1024
#define MAX_EXP  256

typedef struct { char path[PATH_MAX]; char label[PATH_MAX]; bool dir; } Row;
typedef struct { char name[256]; bool dir; } Child;

static const PluginAPI *api;
static int  pane_id;
static char root[PATH_MAX];
static bool show_hidden;
static char expanded[MAX_EXP][PATH_MAX];
static int  nexp;
static Row  rows[MAX_ROWS];
static int  nrows;
static int  cursor;
static const char *labels[MAX_ROWS];

static bool is_expanded(const char *path) {
    for (int i = 0; i < nexp; i++)
        if (!strcmp(expanded[i], path)) return true;
    return false;
}

static void set_expanded(const char *path, bool on) {
    for (int i = 0; i < nexp; i++)
        if (!strcmp(expanded[i], path)) {
            if (!on) expanded[i][0] = '\0';
            return;
        }
    if (on && nexp < MAX_EXP) snprintf(expanded[nexp++], PATH_MAX, "%s", path);
}

static int cmp_child(const void *a, const void *b) {
    const Child *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;     /* folders first */
    return strcmp(x->name, y->name);
}

static void add_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    Child *kids = malloc(sizeof *kids * 512);
    if (!kids) { closedir(d); return; }
    int nk = 0;
    struct dirent *ent;
    while (nk < 512 && (ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (ent->d_name[0] == '.' && !show_hidden) continue;
        snprintf(kids[nk].name, sizeof kids[nk].name, "%s", ent->d_name);
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
        struct stat st;
        kids[nk].dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        nk++;
    }
    closedir(d);
    qsort(kids, (size_t)nk, sizeof *kids, cmp_child);

    for (int i = 0; i < nk && nrows < MAX_ROWS; i++) {
        Row *r = &rows[nrows++];
        snprintf(r->path, sizeof r->path, "%s/%s", dir, kids[i].name);
        r->dir = kids[i].dir;
        bool open = r->dir && is_expanded(r->path);
        snprintf(r->label, sizeof r->label, "%*s%s %s%s", depth * 2, "",
                 r->dir ? (open ? "-" : "+") : " ", kids[i].name, r->dir ? "/" : "");
        if (open) add_dir(r->path, depth + 1);
    }
    free(kids);
}

static void rebuild(void) {
    nrows = 0;
    add_dir(root, 0);
    for (int i = 0; i < nrows; i++) labels[i] = rows[i].label;
    if (cursor >= nrows) cursor = nrows > 0 ? nrows - 1 : 0;
    if (pane_id) {
        api->pane_set_lines(pane_id, labels, nrows);
        api->pane_set_cursor(pane_id, cursor);
    }
}

/* The folder new entries go into: the selected folder, or the selected file's folder. */
static void target_dir(char *out, size_t cap) {
    if (cursor < nrows && rows[cursor].dir) {
        snprintf(out, cap, "%s", rows[cursor].path);
        return;
    }
    if (cursor < nrows) {
        snprintf(out, cap, "%s", rows[cursor].path);
        char *slash = strrchr(out, '/');
        if (slash && slash != out) *slash = '\0';
        else if (slash) out[1] = '\0';
        return;
    }
    snprintf(out, cap, "%s", root);
}

/* Moves the cursor onto a path after a change, if it is listed. */
static void select_path(const char *path) {
    for (int i = 0; i < nrows; i++)
        if (!strcmp(rows[i].path, path)) { cursor = i; break; }
}

static void op_new(Editor *e, bool folder) {
    char dir[PATH_MAX], name[256] = "";
    target_dir(dir, sizeof dir);
    if (!api->prompt(folder ? "New folder: " : "New file: ", name, sizeof name) || !name[0]) return;
    if (strchr(name, '/')) { api->message(e, "names cannot contain /"); return; }

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int ok;
    if (folder) {
        ok = mkdir(path, 0755) == 0;
    } else {
        FILE *f = fopen(path, "wx");       /* 'x' fails if the file exists */
        ok = f != NULL;
        if (f) fclose(f);
    }
    if (!ok) { api->message(e, "could not create that name (already exists?)"); return; }
    set_expanded(dir, true);
    rebuild();
    select_path(path);
    api->pane_set_cursor(pane_id, cursor);
    api->message(e, folder ? "folder created" : "file created");
}

static void op_rename(Editor *e) {
    if (cursor >= nrows) return;
    Row *r = &rows[cursor];
    char name[256];
    const char *base = strrchr(r->path, '/');
    snprintf(name, sizeof name, "%s", base ? base + 1 : r->path);
    if (!api->prompt("Rename to: ", name, sizeof name) || !name[0]) return;
    if (strchr(name, '/')) { api->message(e, "names cannot contain /"); return; }

    char dir[PATH_MAX], to[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", r->path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = '\0';
    snprintf(to, sizeof to, "%s/%s", dir, name);

    if (rename(r->path, to) != 0) {
        api->message(e, "rename failed");
        return;
    }
    if (r->dir) set_expanded(r->path, false);
    rebuild();
    select_path(to);
    api->pane_set_cursor(pane_id, cursor);
    api->message(e, "renamed");
}

static void op_delete(Editor *e) {
    if (cursor >= nrows) return;
    Row *r = &rows[cursor];
    const char *base = strrchr(r->path, '/');
    char q[300], answer[16] = "";
    snprintf(q, sizeof q, "delete %s? type yes: ", base ? base + 1 : r->path);
    if (!api->prompt(q, answer, sizeof answer) || strcmp(answer, "yes") != 0) {
        api->message(e, "not deleted");
        return;
    }
    int rc = r->dir ? rmdir(r->path) : unlink(r->path);
    if (rc != 0) {
        api->message(e, r->dir ? "folder is not empty: delete its contents first" : "delete failed");
        return;
    }
    if (r->dir) set_expanded(r->path, false);
    rebuild();
    api->message(e, "deleted");
}

static bool on_key(Editor *e, int key, void *ud) {
    (void)ud;
    if (key == K_UP) {
        if (cursor > 0) cursor--;
    } else if (key == K_DOWN) {
        if (cursor + 1 < nrows) cursor++;
    } else if (key == K_RIGHT) {
        if (cursor < nrows && rows[cursor].dir && !is_expanded(rows[cursor].path)) {
            set_expanded(rows[cursor].path, true);
            rebuild();
        }
    } else if (key == K_LEFT) {
        if (cursor < nrows && rows[cursor].dir && is_expanded(rows[cursor].path)) {
            set_expanded(rows[cursor].path, false);
            rebuild();
        }
    } else if (key == 10) {                                   /* enter */
        if (cursor >= nrows) return true;
        Row *r = &rows[cursor];
        if (r->dir) {
            set_expanded(r->path, !is_expanded(r->path));
            rebuild();
        } else if (api->open_path(e, r->path, 0, 0)) {
            api->pane_focus(0);
        }
    } else if (key == (PLUGIN_KEY_TEXT | 'n')) {
        op_new(e, false);
    } else if (key == (PLUGIN_KEY_TEXT | 'N')) {
        op_new(e, true);
    } else if (key == (PLUGIN_KEY_TEXT | 'r')) {
        op_rename(e);
    } else if (key == (PLUGIN_KEY_TEXT | 'd')) {
        op_delete(e);
    } else if (key == (PLUGIN_KEY_TEXT | '.')) {
        show_hidden = !show_hidden;
        rebuild();
    } else {
        return false;                                         /* esc and others: core handles */
    }
    if (pane_id) api->pane_set_cursor(pane_id, cursor);
    return true;
}

static void cmd_dirtree(Editor *e, const char *arg) {
    (void)arg;
    if (pane_id) {
        api->pane_close(pane_id);
        pane_id = 0;
        api->message(e, "dirtree closed");
        return;
    }

    char abs[PATH_MAX] = "";
    const char *f = api->file_path(e);
    if (f && realpath(f, abs)) {
        char *slash = strrchr(abs, '/');
        if (slash == abs) abs[1] = '\0';
        else if (slash) *slash = '\0';
        snprintf(root, sizeof root, "%s", abs);
    } else if (!getcwd(root, sizeof root)) {
        snprintf(root, sizeof root, "/");
    }

    pane_id = api->pane_open("Dirtree", PANE_LEFT, 22, on_key, NULL);
    if (!pane_id) {
        api->message(e, "no room for another pane");
        return;
    }
    cursor = 0;
    rebuild();
    api->pane_focus(pane_id);
}

void neonano_plugin_init(const PluginAPI *a) {
    if (a->version < 4) return;               /* needs panes */
    api = a;
    a->add_command("dirtree", "dirtree  open or close the directory tree", cmd_dirtree);
    a->add_command("tree", "tree  same as dirtree", cmd_dirtree);
    a->bind_key("ctrl+b", "dirtree");
}
