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
/* lsp.c - LSP client plugin for neonano, with VS Code-style features.
 *
 *   diagnostics update as you type      underlines, E/W in the gutter, message on the status bar
 *   f5   hover                          popup with the type or docs under the cursor
 *   f12  go to definition               jumps, or opens the other file
 *   ^Space complete; also while typing  selectable popup; Enter inserts
 *   (    signature help                 popup after ( and ,
 *   f2   rename                         prompts for a name (this file only)
 *   f6   code actions                   popup; Enter applies the fix
 *   f7   format                         or on save: :lsp autoformat
 *   f8   problems                       popup list of all diagnostics; Enter jumps
 *
 * Commands: lsp start [cmd] | stop | hover | def | complete | rename [name] |
 *           actions | format | diag | autoformat
 *
 * Requests are asynchronous: answers are read on the editor's idle ticks.
 * Limits: columns are byte offsets, not UTF-16, so non-ASCII lines can be off.
 * Edits and rename only touch the current file. One server at a time.
 */

#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "plugin.h"

#define URI_MAX   (PATH_MAX + 16)
#define OWNER     "lsp"
#define MAX_PEND  32
#define MAX_COMP  128
#define MAX_ACT   16
#define MAX_DIAG  256

enum { K_ERROR = 1, K_WARN = 2, K_INFO = 3, K_HINT = 4 };
enum { RQ_NONE, RQ_INIT, RQ_SHUTDOWN, RQ_HOVER, RQ_DEF, RQ_COMPLETE, RQ_SIG,
       RQ_FORMAT, RQ_RENAME, RQ_ACTIONS };

static const PluginAPI *api;

/* transport and process */
static pid_t srv_pid = -1;
static int   to_srv = -1, from_srv = -1;
static char *rb;
static size_t rlen, rcap;
static int   next_id = 1;

/* document state */
static char  cur_path[PATH_MAX];
static char  cur_uri[URI_MAX];
static char  opened[URI_MAX];          /* uri the server has open */
static int   lsp_version = 1;
static bool  synced_ok;
static unsigned long synced;           /* editor version the server has */
static unsigned long seen_version;     /* version when typing was last seen */
static double seen_at;
static unsigned long auto_ver;         /* version that last triggered auto-complete */
static bool  autoformat;
static char  server_lang[32];         /* language of the running server */
static char  auto_fail[32];           /* language whose autostart failed; avoids retry loops */

/* requests waiting for an answer */
typedef struct { int id, kind; unsigned long version; size_t row, col, c0; bool silent; } Pending;
static Pending pend[MAX_PEND];
static int     npend;

/* completion items and code actions, kept until the user picks one */
typedef struct { char label[96]; char detail[64]; size_t r0, c0, r1, c1; char *text; } CompItem;
static CompItem comp[MAX_COMP];
static int      ncomp;

typedef struct { char title[96]; char *edit; char *cmd; char *args; } Action;
static Action acts[MAX_ACT];
static int    nacts;

/* diagnostics for the current file */
typedef struct { size_t line, col; int kind; char msg[200]; char *raw; } Diag;
static Diag diags[MAX_DIAG];
static int  ndiags;

/* ---------- utilities ---------- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static bool is_ident(unsigned char c) {
    return isalnum(c) || c == '_';
}

/* LSP counts columns in UTF-16 units. The editor uses UTF-8 bytes.
 * These convert one line at a time, using the line's current text. */
static size_t utf8_len_at(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static size_t u16_of_byte(const char *s, size_t len, size_t byte) {
    size_t u = 0, i = 0;
    while (i < len && i < byte) {
        size_t n = utf8_len_at((unsigned char)s[i]);
        if (i + n > len) n = len - i;
        u += n == 4 ? 2 : 1;
        i += n;
    }
    return u;
}

static size_t byte_of_u16(const char *s, size_t len, size_t u) {
    size_t cur = 0, i = 0;
    while (i < len && cur < u) {
        size_t n = utf8_len_at((unsigned char)s[i]);
        if (i + n > len) n = len - i;
        size_t units = n == 4 ? 2 : 1;
        if (cur + units > u) break;     /* u falls inside a surrogate pair */
        cur += units;
        i += n;
    }
    return i;
}

static size_t col16(Editor *e, size_t row, size_t byte) {
    size_t len;
    const char *s = api->line_text(e, row, &len);
    return u16_of_byte(s, len, byte);
}

static size_t colbyte(Editor *e, size_t row, size_t u) {
    size_t len;
    const char *s = api->line_text(e, row, &len);
    return byte_of_u16(s, len, u);
}

/* A growing buffer for building JSON. */
typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t len) {
    if (b->n + len + 1 > b->cap) {
        size_t nc = (b->n + len + 1) * 2;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}

static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bnum(Buf *b, unsigned long v) {
    char t[32];
    snprintf(t, sizeof t, "%lu", v);
    bstr(b, t);
}

/* Appends s with JSON escaping, without quotes. */
static void bjson(Buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  bstr(b, "\\\""); break;
        case '\\': bstr(b, "\\\\"); break;
        case '\n': bstr(b, "\\n");  break;
        case '\r': bstr(b, "\\r");  break;
        case '\t': bstr(b, "\\t");  break;
        default:
            if (c < 0x20) {
                char t[8];
                snprintf(t, sizeof t, "\\u%04x", c);
                bstr(b, t);
            } else {
                bput(b, (const char *)&c, 1);
            }
        }
    }
}

static void bquote(Buf *b, const char *s) {
    bstr(b, "\"");
    bjson(b, s, strlen(s));
    bstr(b, "\"");
}

/* Request header. The caller appends the params object and the closing brace. */
static Buf req(int id, const char *method) {
    Buf b = {0};
    bstr(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    bnum(&b, (unsigned long)id);
    bstr(&b, ",\"method\":");
    bquote(&b, method);
    bstr(&b, ",\"params\":");
    return b;
}

static Buf note(const char *method) {
    Buf b = {0};
    bstr(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    bquote(&b, method);
    bstr(&b, ",\"params\":");
    return b;
}

static void params_pos(Buf *b, Editor *e, size_t row, size_t col) {
    bstr(b, "{\"textDocument\":{\"uri\":");
    bquote(b, cur_uri);
    bstr(b, "},\"position\":{\"line\":");
    bnum(b, row);
    bstr(b, ",\"character\":");
    bnum(b, col16(e, row, col));
    bstr(b, "}}");
}

/* The whole buffer as one JSON string, without quotes around the lines. */
static void bdoc_text(Buf *b, const Editor *e) {
    bstr(b, "\"");
    size_t n = api->line_count(e);
    for (size_t r = 0; r < n; r++) {
        size_t len;
        const char *s = api->line_text(e, r, &len);
        bjson(b, s, len);
        if (r + 1 < n) bstr(b, "\\n");
    }
    bstr(b, "\"");
}

static const char *lang_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "plaintext";
    if (!strcmp(dot, ".c") || !strcmp(dot, ".h"))                            return "c";
    if (!strcmp(dot, ".cc") || !strcmp(dot, ".cpp") || !strcmp(dot, ".hpp")) return "cpp";
    if (!strcmp(dot, ".py"))  return "python";
    if (!strcmp(dot, ".rs"))  return "rust";
    if (!strcmp(dot, ".go"))  return "go";
    if (!strcmp(dot, ".js"))  return "javascript";
    if (!strcmp(dot, ".ts"))  return "typescript";
    if (!strcmp(dot, ".sh"))  return "shellscript";
    return "plaintext";
}

/* ---------- reading JSON (enough for the messages we get) ---------- */

/* Returns a pointer just past the JSON value that starts at p. */
static const char *jskip(const char *p) {
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = jskip(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* For a JSON array: returns the pointer just past '[', or NULL. */
static const char *arr_begin(const char *p) {
    while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return (p && *p == '[') ? p + 1 : NULL;
}

/* Returns the next element of an array, and its length. NULL at the end. */
static const char *arr_next(const char **cur, size_t *len) {
    const char *p = *cur;
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (!*p || *p == ']') { *cur = p; return NULL; }
    const char *s = p;
    p = jskip(p);
    *len = (size_t)(p - s);
    *cur = p;
    return s;
}

/* Finds "key": in s and returns a pointer to its value, or NULL. */
static const char *find_key(const char *s, const char *key) {
    char pat[URI_MAX + 16];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(s, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    return p;
}

/* Copies a JSON string, keeping newlines. */
static void read_str_ml(const char *p, char *out, size_t cap) {
    size_t o = 0;
    out[0] = '\0';
    if (!p || *p != '"' || cap == 0) return;
    p++;
    while (*p && *p != '"' && o + 1 < cap) {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') continue;
            else if (e == 'u') { for (int k = 0; k < 4 && *p; k++) p++; c = '?'; }
            else c = e;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

/* Copies a JSON string onto one line. */
static void read_str(const char *p, char *out, size_t cap) {
    read_str_ml(p, out, cap);
    for (char *q = out; *q; q++)
        if (*q == '\n' || *q == '\t') *q = ' ';
}

/* Returns a malloc'd, unescaped copy of a JSON string (empty if not a string). */
static char *json_str(const char *p) {
    if (!p || *p != '"') return strdup("");
    size_t span = (size_t)(jskip(p) - p);
    char *o = malloc(span + 1);
    read_str_ml(p, o, span + 1);
    return o;
}

/* Returns a malloc'd copy of any JSON value. */
static char *json_value(const char *p) {
    return strndup(p, (size_t)(jskip(p) - p));
}

static unsigned long num_key(const char *s, const char *key) {
    const char *p = find_key(s, key);
    return p ? strtoul(p, NULL, 10) : 0;
}

/* Reads the "range" of an object such as a TextEdit, Location or Diagnostic. */
static bool parse_range(const char *s, size_t *r0, size_t *c0, size_t *r1, size_t *c1) {
    const char *rng = find_key(s, "range");
    if (!rng) return false;
    const char *st = find_key(rng, "start");
    const char *en = find_key(rng, "end");
    if (!st || !en) return false;
    *r0 = num_key(st, "line");
    *c0 = num_key(st, "character");
    *r1 = num_key(en, "line");
    *c1 = num_key(en, "character");
    return true;
}

/* ---------- transport ---------- */

static bool write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Frames the body with Content-Length, sends it, and frees b. */
static bool send_buf(Buf *b) {
    if (to_srv < 0) { free(b->p); b->p = NULL; b->n = b->cap = 0; return false; }
    char hdr[64];
    int h = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", b->n);
    bool ok = write_all(to_srv, hdr, (size_t)h) && write_all(to_srv, b->p, b->n);
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
    return ok;
}

static bool fill(int ms) {
    struct pollfd pf = { from_srv, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0) return false;
    if (rcap - rlen < 4096) {
        size_t nc = rcap ? rcap * 2 : 8192;
        char *nb = realloc(rb, nc);
        if (!nb) return false;
        rb = nb;
        rcap = nc;
    }
    ssize_t n = read(from_srv, rb + rlen, rcap - rlen);
    if (n <= 0) return false;
    rlen += (size_t)n;
    return true;
}

/* Returns the next complete message body (malloc'd), or NULL if none is ready in ms. */
static char *read_msg(int ms) {
    for (;;) {
        size_t hend = 0;
        bool found = false;
        for (size_t i = 0; i + 3 < rlen; i++) {
            if (rb[i] == '\r' && rb[i + 1] == '\n' && rb[i + 2] == '\r' && rb[i + 3] == '\n') {
                hend = i;
                found = true;
                break;
            }
        }
        if (found) {
            long clen = -1;
            char *hdr = strndup(rb, hend);
            char *cl = strstr(hdr, "Content-Length:");
            if (cl) clen = strtol(cl + 15, NULL, 10);
            free(hdr);

            if (clen < 0) {                         /* malformed: drop the header */
                memmove(rb, rb + hend + 4, rlen - hend - 4);
                rlen -= hend + 4;
                continue;
            }
            size_t total = hend + 4 + (size_t)clen;
            if (rlen >= total) {
                char *body = malloc((size_t)clen + 1);
                memcpy(body, rb + hend + 4, (size_t)clen);
                body[clen] = '\0';
                memmove(rb, rb + total, rlen - total);
                rlen -= total;
                return body;
            }
        }
        if (!fill(ms)) return NULL;
    }
}

/* ---------- requests ---------- */

static int pend_add(int kind, unsigned long ver, size_t row, size_t col, size_t c0, bool silent) {
    int id = next_id++;
    if (npend < MAX_PEND)
        pend[npend++] = (Pending){ id, kind, ver, row, col, c0, silent };
    return id;
}

static Pending *pend_find(int id) {
    for (int i = 0; i < npend; i++)
        if (pend[i].id == id) return &pend[i];
    return NULL;
}

static void pend_remove(int id) {
    for (int i = 0; i < npend; i++)
        if (pend[i].id == id) { pend[i] = pend[--npend]; return; }
}

static void diags_clear(void) {
    for (int i = 0; i < ndiags; i++) free(diags[i].raw);
    ndiags = 0;
}

static void comp_clear(void) {
    for (int i = 0; i < ncomp; i++) free(comp[i].text);
    ncomp = 0;
}

static void acts_clear(void) {
    for (int i = 0; i < nacts; i++) {
        free(acts[i].edit);
        free(acts[i].cmd);
        free(acts[i].args);
        acts[i].edit = acts[i].cmd = acts[i].args = NULL;
    }
    nacts = 0;
}

static bool need_server(Editor *e) {
    if (srv_pid > 0) return true;
    api->message(e, "no language server: use :lsp start [cmd]");
    return false;
}

/* Tells the server about the current text, if it has changed. */
static bool doc_sync(Editor *e) {
    const char *path = api->file_path(e);
    if (!path || !*path) {
        api->message(e, "save the file first: LSP needs a path");
        return false;
    }
    unsigned long v = api->buffer_version(e);
    if (!realpath(path, cur_path)) snprintf(cur_path, sizeof cur_path, "%s", path);
    snprintf(cur_uri, sizeof cur_uri, "file://%s", cur_path);

    if (synced_ok && !strcmp(opened, cur_uri) && v == synced) return true;

    Buf b = {0};
    if (strcmp(opened, cur_uri) != 0) {          /* a different file: open it */
        api->decor_clear(OWNER);
        diags_clear();
        lsp_version = 1;
        bstr(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":");
        bquote(&b, cur_uri);
        bstr(&b, ",\"languageId\":");
        bquote(&b, lang_for(cur_path));
        bstr(&b, ",\"version\":1,\"text\":");
        bdoc_text(&b, e);
        bstr(&b, "}}}");
        snprintf(opened, sizeof opened, "%s", cur_uri);
    } else {
        lsp_version++;
        bstr(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":");
        bquote(&b, cur_uri);
        bstr(&b, ",\"version\":");
        bnum(&b, (unsigned long)lsp_version);
        bstr(&b, "},\"contentChanges\":[{\"text\":");
        bdoc_text(&b, e);
        bstr(&b, "}]}}");
    }
    synced = v;
    synced_ok = true;
    return send_buf(&b);
}

/* Sends a request whose params are a position in the current file. */
static int req_pos(Editor *e, int kind, const char *method, bool silent, size_t c0) {
    if (!doc_sync(e)) return 0;
    size_t row, col;
    api->get_cursor(e, &row, &col);
    int id = pend_add(kind, api->buffer_version(e), row, col, c0, silent);
    Buf b = req(id, method);
    params_pos(&b, e, row, col);
    bstr(&b, "}");
    send_buf(&b);
    return id;
}

static int req_format(Editor *e, bool silent) {
    if (!doc_sync(e)) return 0;
    int id = pend_add(RQ_FORMAT, api->buffer_version(e), 0, 0, 0, silent);
    Buf b = req(id, "textDocument/formatting");
    bstr(&b, "{\"textDocument\":{\"uri\":");
    bquote(&b, cur_uri);
    bstr(&b, "},\"options\":{\"tabSize\":4,\"insertSpaces\":true}}}");
    send_buf(&b);
    return id;
}

static void req_rename(Editor *e, const char *name) {
    if (!doc_sync(e)) return;
    size_t row, col;
    api->get_cursor(e, &row, &col);
    int id = pend_add(RQ_RENAME, api->buffer_version(e), row, col, 0, false);
    Buf b = req(id, "textDocument/rename");
    bstr(&b, "{\"textDocument\":{\"uri\":");
    bquote(&b, cur_uri);
    bstr(&b, "},\"position\":{\"line\":");
    bnum(&b, row);
    bstr(&b, ",\"character\":");
    bnum(&b, col16(e, row, col));
    bstr(&b, "},\"newName\":");
    bquote(&b, name);
    bstr(&b, "}}");
    send_buf(&b);
}

static void req_actions(Editor *e) {
    if (!doc_sync(e)) return;
    size_t row, col;
    api->get_cursor(e, &row, &col);
    int id = pend_add(RQ_ACTIONS, api->buffer_version(e), row, col, 0, false);

    Buf b = req(id, "textDocument/codeAction");
    bstr(&b, "{\"textDocument\":{\"uri\":");
    bquote(&b, cur_uri);
    bstr(&b, "},\"range\":{\"start\":{\"line\":");
    bnum(&b, row);
    bstr(&b, ",\"character\":");
    bnum(&b, col16(e, row, col));
    bstr(&b, "},\"end\":{\"line\":");
    bnum(&b, row);
    bstr(&b, ",\"character\":");
    bnum(&b, col16(e, row, col));
    bstr(&b, "}},\"context\":{\"diagnostics\":[");
    bool first = true;
    for (int i = 0; i < ndiags; i++) {
        if (diags[i].line != row) continue;
        if (!first) bstr(&b, ",");
        bstr(&b, diags[i].raw);
        first = false;
    }
    bstr(&b, "]}}}");
    send_buf(&b);
}

/* ---------- incoming messages ---------- */

static void on_diag(Editor *e, const char *body) {
    (void)e;
    if (!cur_uri[0] || !strstr(body, cur_uri)) return;

    api->decor_clear(OWNER);
    diags_clear();

    const char *a = find_key(body, "diagnostics");
    a = a ? arr_begin(a) : NULL;
    size_t len;
    const char *it;
    while (a && (it = arr_next(&a, &len)) != NULL) {
        char *o = strndup(it, len);
        size_t l0, c0, l1, c1;
        if (parse_range(o, &l0, &c0, &l1, &c1) && ndiags < MAX_DIAG) {
            c0 = colbyte(e, l0, c0);
            c1 = colbyte(e, l1, c1);
            int kind = (int)num_key(o, "severity");
            if (kind < 1 || kind > 4) kind = K_ERROR;
            char msg[200];
            read_str(find_key(o, "message"), msg, sizeof msg);
            api->decor_add(OWNER, l0, c0, l1 == l0 ? c1 : c0 + 1, kind, msg);

            Diag *d = &diags[ndiags++];
            d->line = l0;
            d->col = c0;
            d->kind = kind;
            snprintf(d->msg, sizeof d->msg, "%s", msg);
            d->raw = strdup(o);
        }
        free(o);
    }
}

static void apply_text_edits(Editor *e, const char *arr, const char *ok, bool silent);

/* Notifications from the server, and requests the server sends us. */
static void handle_incoming(Editor *e, const char *body) {
    if (strstr(body, "\"method\":\"textDocument/publishDiagnostics\"")) {
        on_diag(e, body);
        return;
    }
    const char *idp = find_key(body, "id");
    if (!idp || !strstr(body, "\"method\":")) return;

    /* The server applies a fix by sending us its edit (after executeCommand). */
    bool apply = strstr(body, "\"method\":\"workspace/applyEdit\"") != NULL;
    if (apply) {
        const char *ed = find_key(body, "edit");
        const char *changes = ed ? find_key(ed, "changes") : NULL;
        const char *mine = changes ? find_key(changes, cur_uri) : NULL;
        if (mine) apply_text_edits(e, mine, "applied the server's fix", false);
    }

    /* Answer the request: applied for applyEdit, null for anything else. */
    Buf r = {0};
    bstr(&r, "{\"jsonrpc\":\"2.0\",\"id\":");
    bnum(&r, strtoul(idp, NULL, 10));
    bstr(&r, apply ? ",\"result\":{\"applied\":true}}" : ",\"result\":null}");
    send_buf(&r);
}

static void show_text(Editor *e, const char *title, char *txt) {
    const char *lines[40];
    int n = 0;
    char *p = txt;
    while (p && n < 40) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }
    api->popup_open(e, title, lines, n, false, NULL, NULL);
}

/* Hover contents can be a string, a MarkupContent, or an array. */
static char *hover_text(const char *r) {
    const char *c = find_key(r, "contents");
    if (!c) return NULL;
    if (*c == '"') return json_str(c);
    if (*c == '{') return json_str(find_key(c, "value"));
    if (*c == '[') {
        const char *a = arr_begin(c);
        size_t len = 0;
        const char *it = a ? arr_next(&a, &len) : NULL;
        if (!it) return NULL;
        if (*it == '"') return json_str(it);
        char *o = strndup(it, len);
        char *out = json_str(find_key(o, "value"));
        free(o);
        return out;
    }
    return NULL;
}

static void apply_text_edits(Editor *e, const char *arr, const char *ok, bool silent);

static void diag_pick(Editor *e, int idx, void *ud) {
    (void)ud;
    if (idx >= 0 && idx < ndiags) api->set_cursor(e, diags[idx].line, diags[idx].col);
}

static size_t ident_start(Editor *e);
static void complete_filter(Editor *e, void *ud);

/* Completions currently shown: shown[i] indexes comp[]. */
static int shown[MAX_COMP];
static int nshown;

/* Case-insensitive prefix test. */
static bool starts_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static void complete_pick(Editor *e, int idx, void *ud) {
    (void)ud;
    if (idx < 0 || idx >= nshown) return;
    CompItem *ci = &comp[shown[idx]];
    /* Replace from where the item started up to the cursor, which may have
     * moved on while the popup was open (the user kept typing). */
    size_t row, col;
    api->get_cursor(e, &row, &col);
    TextEdit te = { ci->r0, ci->c0, row, col, ci->text ? ci->text : "" };
    api->apply_edits(e, &te, 1);
}

/* Shows the completions whose labels start with the identifier before the
 * cursor. With close_if_none_typed, closes the popup if no identifier is
 * being typed. Returns true if a popup is open. */
static bool comp_show_cur(Editor *e, bool close_if_none_typed) {
    size_t row, col, len;
    api->get_cursor(e, &row, &col);
    const char *s = api->line_text(e, row, &len);
    size_t st = ident_start(e);
    if (close_if_none_typed && col == st) {
        api->popup_close();
        return false;
    }

    char prefix[128] = "";
    size_t n = col - st;
    if (n >= sizeof prefix) n = sizeof prefix - 1;
    if (n) memcpy(prefix, s + st, n);
    prefix[n] = '\0';

    static char disp[MAX_COMP][160];
    static const char *lines[MAX_COMP];
    nshown = 0;
    for (int i = 0; i < ncomp && nshown < MAX_COMP; i++) {
        if (!starts_ci(comp[i].label, prefix)) continue;
        snprintf(disp[nshown], sizeof disp[nshown], "%s  %s", comp[i].label, comp[i].detail);
        lines[nshown] = disp[nshown];
        shown[nshown] = i;
        nshown++;
    }
    if (nshown == 0) {
        api->popup_close();
        return false;
    }
    api->popup_open(e, "Completions", lines, nshown, true, complete_pick, NULL);
    api->popup_filterable(complete_filter, NULL);
    return true;
}

/* Runs after each typed character or backspace while the popup is open. */
static void complete_filter(Editor *e, void *ud) {
    (void)ud;
    comp_show_cur(e, true);
}

static void action_pick(Editor *e, int idx, void *ud) {
    (void)ud;
    if (idx < 0 || idx >= nacts) return;
    Action *ac = &acts[idx];
    if (ac->edit) {
        const char *changes = find_key(ac->edit, "changes");
        const char *mine = changes ? find_key(changes, cur_uri) : NULL;
        if (mine) apply_text_edits(e, mine, ac->title, false);
        else api->message(e, "that action changes other files only");
        return;
    }
    if (ac->args) {                      /* clangd keeps the edit in the arguments */
        const char *changes = find_key(ac->args, "changes");
        const char *mine = changes ? find_key(changes, cur_uri) : NULL;
        if (mine) {
            apply_text_edits(e, mine, ac->title, false);
            return;
        }
    }
    if (ac->cmd) {                       /* otherwise ask the server to run it */
        int id = next_id++;
        Buf b = req(id, "workspace/executeCommand");
        bstr(&b, "{\"command\":");
        bquote(&b, ac->cmd);
        bstr(&b, ",\"arguments\":");
        bstr(&b, ac->args ? ac->args : "[]");
        bstr(&b, "}}");
        send_buf(&b);
        api->message(e, "applying fix...");
        return;
    }
    api->message(e, "that action has nothing to apply");
}

static int cmp_desc(const void *a, const void *b) {
    const TextEdit *x = a, *y = b;
    if (x->r0 != y->r0) return x->r0 < y->r0 ? 1 : -1;
    if (x->c0 != y->c0) return x->c0 < y->c0 ? 1 : -1;
    return 0;
}

/* Parses an array of TextEdits and applies them, bottom to top. */
static void apply_text_edits(Editor *e, const char *arr, const char *ok, bool silent) {
    const char *c = arr_begin(arr);
    TextEdit te[256];
    int n = 0;
    size_t len;
    const char *it;
    while (c && n < 256 && (it = arr_next(&c, &len)) != NULL) {
        char *o = strndup(it, len);
        size_t r0, c0, r1, c1;
        if (parse_range(o, &r0, &c0, &r1, &c1)) {
            te[n].r0 = r0; te[n].c0 = colbyte(e, r0, c0);
            te[n].r1 = r1; te[n].c1 = colbyte(e, r1, c1);
            te[n].text = json_str(find_key(o, "newText"));
            n++;
        }
        free(o);
    }
    if (n == 0) {
        if (!silent) api->message(e, "nothing to change");
    } else {
        qsort(te, (size_t)n, sizeof te[0], cmp_desc);
        api->apply_edits(e, te, n);
        if (!silent) api->message(e, ok);
    }
    for (int i = 0; i < n; i++) free((void *)te[i].text);
}

static void dispatch(Editor *e, char *body) {
    const char *idp = find_key(body, "id");
    if (!idp || strstr(body, "\"method\":")) {
        handle_incoming(e, body);
        return;
    }
    int id = (int)strtol(idp, NULL, 10);
    Pending *p = pend_find(id);
    if (!p) return;
    Pending pc = *p;
    pend_remove(id);

    const char *r = find_key(body, "result");
    if (!r) {                                       /* error response */
        if (!pc.silent) {
            char t[200];
            read_str(find_key(body, "message"), t, sizeof t);
            if (t[0]) api->message(e, t);
        }
        return;
    }
    bool stale = pc.version != api->buffer_version(e);

    switch (pc.kind) {
    case RQ_HOVER: {
        if (stale) break;
        char *txt = (*r == '{') ? hover_text(r) : NULL;
        if (txt && txt[0]) show_text(e, "Hover", txt);
        else if (!pc.silent) api->message(e, "no hover info here");
        free(txt);
        break;
    }

    case RQ_DEF: {
        if (stale) break;
        if (*r != '{' && *r != '[') { api->message(e, "no definition found"); break; }
        const char *u = find_key(r, "uri");
        if (!u) u = find_key(r, "targetUri");
        char uri[URI_MAX] = "";
        read_str(u, uri, sizeof uri);
        size_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
        parse_range(r, &r0, &c0, &r1, &c1);
        if (!uri[0] || strncmp(uri, "file://", 7) != 0) {
            api->message(e, "no definition found");
            break;
        }
        const char *path = uri + 7;
        if (!strcmp(path, cur_path)) {
            api->set_cursor(e, r0, colbyte(e, r0, c0));
        } else if (api->open_path(e, path, r0, 0)) {
            api->set_cursor(e, r0, colbyte(e, r0, c0));   /* the new file's text */
        }
        break;
    }

    case RQ_COMPLETE: {
        comp_clear();
        if (stale) break;
        const char *arr = NULL;
        if (*r == '{') arr = find_key(r, "items");
        else if (*r == '[') arr = r;
        const char *a = arr ? arr_begin(arr) : NULL;

        size_t len;
        const char *it;
        while (a && ncomp < MAX_COMP && (it = arr_next(&a, &len)) != NULL) {
            char *o = strndup(it, len);
            CompItem *ci = &comp[ncomp];
            read_str(find_key(o, "label"), ci->label, sizeof ci->label);
            {   /* clangd sometimes labels with a leading space */
                char *lab = ci->label;
                while (*lab == ' ') lab++;
                if (lab != ci->label) memmove(ci->label, lab, strlen(lab) + 1);
            }
            /* clangd sometimes puts leading spaces in labels; trim them so the
             * prefix match and the fallback insert text are right. */
            size_t lead = strspn(ci->label, " \t");
            if (lead) memmove(ci->label, ci->label + lead, strlen(ci->label + lead) + 1);
            read_str(find_key(o, "detail"), ci->detail, sizeof ci->detail);
            if (parse_range(o, &ci->r0, &ci->c0, &ci->r1, &ci->c1)) {
                ci->c0 = colbyte(e, ci->r0, ci->c0);
                ci->c1 = colbyte(e, ci->r1, ci->c1);
                ci->text = json_str(find_key(o, "newText"));
            } else {
                ci->r0 = pc.row; ci->c0 = pc.c0; ci->r1 = pc.row; ci->c1 = pc.col;
                const char *ins = find_key(o, "insertText");
                ci->text = ins ? json_str(ins) : strdup(ci->label);
            }
            ncomp++;
            free(o);
        }
        if (ncomp == 0) {
            if (!pc.silent) api->message(e, "no completions");
            break;
        }
        if (!comp_show_cur(e, false) && !pc.silent) api->message(e, "no completions");
        break;
    }

    case RQ_SIG: {
        if (stale || *r != '{') break;
        const char *sg = find_key(r, "signatures");
        const char *a = sg ? arr_begin(sg) : NULL;
        size_t len;
        const char *it = a ? arr_next(&a, &len) : NULL;
        if (!it) break;
        char *o = strndup(it, len);
        char label[240];
        read_str(find_key(o, "label"), label, sizeof label);
        free(o);
        if (label[0]) {
            const char *lines[1] = { label };
            api->popup_open(e, "Signature", lines, 1, false, NULL, NULL);
        }
        break;
    }

    case RQ_FORMAT:
        if (stale) break;
        if (*r != '[') { if (!pc.silent) api->message(e, "nothing to format"); break; }
        apply_text_edits(e, r, "formatted", pc.silent);
        break;

    case RQ_RENAME: {
        if (stale) break;
        const char *changes = (*r == '{') ? find_key(r, "changes") : NULL;
        const char *mine = changes ? find_key(changes, cur_uri) : NULL;
        if (!mine) { api->message(e, "rename: the server returned no edits for this file"); break; }
        apply_text_edits(e, mine, "renamed", false);
        break;
    }

    case RQ_ACTIONS: {
        acts_clear();
        if (stale) break;
        const char *a = (*r == '[') ? arr_begin(r) : NULL;
        size_t len;
        const char *it;
        static const char *lines[MAX_ACT];
        static char titles[MAX_ACT][96];
        while (a && nacts < MAX_ACT && (it = arr_next(&a, &len)) != NULL) {
            char *o = strndup(it, len);
            Action *ac = &acts[nacts];
            read_str(find_key(o, "title"), ac->title, sizeof ac->title);
            const char *ev = find_key(o, "edit");
            ac->edit = (ev && *ev == '{') ? json_value(ev) : NULL;
            const char *cv = find_key(o, "command");
            if (cv && *cv == '{') {
                ac->cmd = json_str(find_key(cv, "command"));
                const char *av = find_key(cv, "arguments");
                ac->args = (av && *av == '[') ? json_value(av) : NULL;
            } else if (cv && *cv == '"') {          /* Command form: arguments sit beside it */
                ac->cmd = json_str(cv);
                const char *av = find_key(o, "arguments");
                ac->args = (av && *av == '[') ? json_value(av) : NULL;
            }
            free(o);
            snprintf(titles[nacts], sizeof titles[nacts], "%s", ac->title);
            lines[nacts] = titles[nacts];
            nacts++;
        }
        if (nacts == 0) {
            api->message(e, "no code actions here");
            break;
        }
        api->popup_open(e, "Code actions", lines, nacts, true, action_pick, NULL);
        break;
    }

    default:
        break;
    }
}

/* Waits until the answer to request id has been handled, or ms passes. */
static bool wait_for(Editor *e, int id, int ms) {
    double end = now_ms() + ms;
    while (pend_find(id)) {
        double left = end - now_ms();
        if (left <= 0) return false;
        char *body = read_msg((int)left);
        if (!body) {
            if (srv_pid > 0 && waitpid(srv_pid, NULL, WNOHANG) > 0) return false;
            continue;
        }
        dispatch(e, body);
        free(body);
    }
    return true;
}

/* ---------- server lifecycle ---------- */

static void reset_state(void) {
    npend = 0;
    comp_clear();
    acts_clear();
    diags_clear();
    synced_ok = false;
    opened[0] = '\0';
    rlen = 0;
}

static void kill_server(Editor *e) {
    server_lang[0] = '\0';
    if (to_srv >= 0) close(to_srv);
    if (from_srv >= 0) close(from_srv);
    if (srv_pid > 0) {
        kill(srv_pid, SIGTERM);
        waitpid(srv_pid, NULL, 0);
    }
    srv_pid = -1;
    to_srv = from_srv = -1;
    reset_state();
    api->decor_clear(OWNER);
    (void)e;
}

static void server_gone(Editor *e) {
    if (to_srv >= 0) close(to_srv);
    if (from_srv >= 0) close(from_srv);
    srv_pid = -1;
    to_srv = from_srv = -1;
    reset_state();
    api->decor_clear(OWNER);
    api->message(e, "language server exited");
}

/* Built-in servers. A [lsp] entry in config.toml overrides them. */
static const char *default_server(const char *lang) {
    if (!strcmp(lang, "c") || !strcmp(lang, "cpp")) return "clangd";
    if (!strcmp(lang, "python"))                    return "pyright-langserver --stdio";
    if (!strcmp(lang, "rust"))                      return "rust-analyzer";
    if (!strcmp(lang, "go"))                        return "gopls";
    if (!strcmp(lang, "javascript") || !strcmp(lang, "typescript"))
        return "typescript-language-server --stdio";
    if (!strcmp(lang, "shellscript"))               return "bash-language-server start";
    return NULL;
}

/* The server command for the current file's language, or NULL. */
static const char *server_for(Editor *e) {
    const char *path = api->file_path(e);
    if (!path) return NULL;
    const char *lang = lang_for(path);
    const char *cfgcmd = api->config_get("lsp", lang);
    if (cfgcmd && *cfgcmd) return cfgcmd;
    return default_server(lang);
}


static void start(Editor *e, const char *cmd) {
    if (srv_pid > 0) {
        api->message(e, "server already running (:lsp stop first)");
        return;
    }
    int in[2], out[2];
    if (pipe(in) < 0) { api->message(e, "lsp: pipe failed"); return; }
    if (pipe(out) < 0) {
        close(in[0]); close(in[1]);
        api->message(e, "lsp: pipe failed");
        return;
    }

    signal(SIGPIPE, SIG_IGN);        /* a dead server must not kill the editor */

    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        api->message(e, "lsp: fork failed");
        return;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);   /* server logs must not draw over the editor */
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    srv_pid = pid;
    to_srv = in[1];
    from_srv = out[0];
    reset_state();

    const char *path = api->file_path(e);
    snprintf(server_lang, sizeof server_lang, "%s", path ? lang_for(path) : "plaintext");

    /* rootUri is the working directory, so servers like pyright find the project. */
    char cwd[PATH_MAX], root[PATH_MAX + 8] = "";
    if (getcwd(cwd, sizeof cwd)) snprintf(root, sizeof root, "file://%s", cwd);

    int id = pend_add(RQ_INIT, api->buffer_version(e), 0, 0, 0, false);
    Buf b = req(id, "initialize");
    bstr(&b, "{\"processId\":null,\"rootUri\":");
    if (root[0]) bquote(&b, root);
    else         bstr(&b, "null");
    bstr(&b, ",\"capabilities\":{}}}");
    send_buf(&b);
    if (!wait_for(e, id, 8000)) {
        snprintf(auto_fail, sizeof auto_fail, "%s", server_lang);
        kill_server(e);
        api->message(e, "lsp: server did not answer initialize (is it installed?)");
        return;
    }

    Buf n = note("initialized");
    bstr(&n, "{}}");
    send_buf(&n);

    if (api->file_path(e)) doc_sync(e);
    api->message(e, "LSP started");
}

static void stop(Editor *e) {
    if (!need_server(e)) return;
    int id = pend_add(RQ_SHUTDOWN, api->buffer_version(e), 0, 0, 0, true);
    Buf b = req(id, "shutdown");
    bstr(&b, "null}");
    send_buf(&b);
    wait_for(e, id, 1000);

    Buf n = note("exit");
    bstr(&n, "null}");
    send_buf(&n);

    kill_server(e);
    api->message(e, "LSP stopped");
}

/* ---------- ticks, save hook ---------- */

static bool is_ident_before_cursor(Editor *e, size_t *col_out) {
    size_t row, col, len;
    api->get_cursor(e, &row, &col);
    const char *s = api->line_text(e, row, &len);
    *col_out = col;
    return col > 0 && col <= len && is_ident((unsigned char)s[col - 1]);
}

static size_t ident_start(Editor *e) {
    size_t row, col, len;
    api->get_cursor(e, &row, &col);
    const char *s = api->line_text(e, row, &len);
    size_t c = col;
    while (c > 0 && c <= len && is_ident((unsigned char)s[c - 1])) c--;
    return c;
}

/* With [lsp] autostart = true, starts the right server for the current file,
 * and switches servers when the language changes. */
static void maybe_autostart(Editor *e) {
    const char *on = api->config_get("lsp", "autostart");
    if (!on || strcmp(on, "true") != 0) return;
    const char *path = api->file_path(e);
    if (!path) return;
    const char *lang = lang_for(path);
    if (srv_pid > 0 && !strcmp(server_lang, lang)) return;
    if (!strcmp(auto_fail, lang)) return;
    const char *cmd = server_for(e);
    if (!cmd) return;
    if (srv_pid > 0) kill_server(e);
    start(e, cmd);
}

static void on_tick(Editor *e) {
    if (srv_pid > 0 && waitpid(srv_pid, NULL, WNOHANG) > 0) {
        snprintf(auto_fail, sizeof auto_fail, "%s", server_lang);   /* do not restart-loop */
        server_gone(e);
    }
    maybe_autostart(e);
    if (srv_pid <= 0) return;

    char *body;
    while ((body = read_msg(0)) != NULL) {
        dispatch(e, body);
        free(body);
    }
    if (srv_pid <= 0 || !api->file_path(e)) return;

    unsigned long v = api->buffer_version(e);
    double t = now_ms();
    if (v != seen_version) { seen_version = v; seen_at = t; }

    /* Send changes once typing pauses, so the server can refresh diagnostics. */
    if (synced_ok && v != synced && t - seen_at >= 300) doc_sync(e);

    /* After a pause in typing: complete an identifier, or signature help after ( and ,. */
    if (v != auto_ver && t - seen_at >= 200) {
        auto_ver = v;
        size_t col;
        size_t row, cur_col, len;
        api->get_cursor(e, &row, &cur_col);
        const char *s = api->line_text(e, row, &len);
        unsigned char prev = (cur_col > 0 && cur_col <= len) ? (unsigned char)s[cur_col - 1] : 0;
        (void)col;
        if (is_ident_before_cursor(e, &col))
            req_pos(e, RQ_COMPLETE, "textDocument/completion", true, ident_start(e));
        else if (prev == '(' || prev == ',')
            req_pos(e, RQ_SIG, "textDocument/signatureHelp", true, 0);
    }
}

static void on_save(Editor *e) {
    if (!autoformat || srv_pid <= 0 || !api->file_path(e)) return;
    int id = req_format(e, true);
    if (id) wait_for(e, id, 1500);
}

/* ---------- commands ---------- */

static void cmd_diag(Editor *e) {
    if (ndiags == 0) {
        api->message(e, "no problems in this file");
        return;
    }
    static char disp[MAX_DIAG][240];
    const char *lines[MAX_DIAG];
    for (int i = 0; i < ndiags; i++) {
        snprintf(disp[i], sizeof disp[i], "%zu:%zu  %s",
                 diags[i].line + 1, diags[i].col + 1, diags[i].msg);
        lines[i] = disp[i];
    }
    api->popup_open(e, "Problems", lines, ndiags, true, diag_pick, NULL);
}

static void cmd_lsp(Editor *e, const char *arg) {
    if (!strncmp(arg, "start", 5) && (arg[5] == '\0' || arg[5] == ' ')) {
        const char *c = arg + 5;
        while (*c == ' ') c++;
        const char *cmd = *c ? c : server_for(e);
        if (!cmd) {
            api->message(e, "no language server for this file: set [lsp] in config.toml");
            return;
        }
        start(e, cmd);
    } else if (!strcmp(arg, "stop")) {
        stop(e);
    } else if (!strcmp(arg, "hover")) {
        if (need_server(e)) req_pos(e, RQ_HOVER, "textDocument/hover", false, 0);
    } else if (!strcmp(arg, "def")) {
        if (need_server(e)) req_pos(e, RQ_DEF, "textDocument/definition", false, 0);
    } else if (!strcmp(arg, "complete")) {
        if (need_server(e)) req_pos(e, RQ_COMPLETE, "textDocument/completion", false, ident_start(e));
    } else if (!strncmp(arg, "rename", 6) && (arg[6] == '\0' || arg[6] == ' ')) {
        if (!need_server(e)) return;
        char name[128] = "";
        const char *a = arg + 6;
        while (*a == ' ') a++;
        if (*a) snprintf(name, sizeof name, "%s", a);
        else if (!api->prompt("New name: ", name, sizeof name)) return;
        if (name[0]) req_rename(e, name);
    } else if (!strcmp(arg, "actions")) {
        if (need_server(e)) req_actions(e);
    } else if (!strcmp(arg, "format")) {
        if (need_server(e)) req_format(e, false);
    } else if (!strcmp(arg, "diag")) {
        if (need_server(e)) cmd_diag(e);
    } else if (!strcmp(arg, "autoformat")) {
        autoformat = !autoformat;
        api->message(e, autoformat ? "format on save: on" : "format on save: off");
    } else {
        api->message(e, "usage: lsp start|stop|hover|def|complete|rename|actions|format|diag|autoformat");
    }
}

void neonano_plugin_init(const PluginAPI *a) {
    if (a->version < 3) return;           /* needs ticks, popups, decorations */
    api = a;
    a->add_command("lsp", "lsp start|stop|hover|def|complete|rename|actions|format|diag|autoformat", cmd_lsp);
    a->on_tick(on_tick);
    a->on_save(on_save);
    a->bind_key("f5", "lsp hover");
    a->bind_key("f12", "lsp def");
    a->bind_key("ctrl+space", "lsp complete");
    a->bind_key("f2", "lsp rename");
    a->bind_key("f6", "lsp actions");
    a->bind_key("f7", "lsp format");
    a->bind_key("f8", "lsp diag");
}
