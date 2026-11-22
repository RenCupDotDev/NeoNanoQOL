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
/* terminal.c - a shell in a bottom pane.
 *   f4 or :terminal   open or close the terminal (starts $SHELL, or /bin/sh)
 * In the pane: typing goes to the shell; enter, backspace, tab, arrows, ctrl+c
 * and ctrl+d are sent to it; esc or F9 returns to the editor.
 *
 * Output is a simplified terminal: colour codes are removed, \r and \b work,
 * and the line-erase code is honoured. Full-screen programs (vim, top) will not
 * display correctly.
 */
#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "plugin.h"

/* ncurses key ids */
#define K_DOWN  0402
#define K_UP    0403
#define K_LEFT  0404
#define K_RIGHT 0405
#define K_HOME  0406
#define K_BACKSPACE 0407

#define MAX_LINES 1000
#define MAX_COLS  400

static const PluginAPI *api;
static int   pane_id;
static int   master = -1;
static pid_t child = -1;

static char  lines[MAX_LINES][MAX_COLS];
static int   lens[MAX_LINES];
static int   nlines = 1;               /* the last line is the one being written */
static int   col;
static bool  dirty;
static const char *shown[MAX_LINES];

enum { ST_TEXT, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC };
static int   state = ST_TEXT;
static char  csi[16];
static int   ncsi;

static void new_line(void) {
    if (nlines == MAX_LINES) {                       /* drop the oldest line */
        memmove(lines, lines + 1, sizeof lines[0] * (MAX_LINES - 1));
        memmove(lens, lens + 1, sizeof lens[0] * (MAX_LINES - 1));
        nlines--;
    }
    lines[nlines][0] = '\0';
    lens[nlines] = 0;
    nlines++;
}

static void put_byte(unsigned char c) {
    int last = nlines - 1;
    if (col >= MAX_COLS - 1) return;
    if (col < lens[last]) lines[last][col] = (char)c;
    else {
        lines[last][col] = (char)c;
        lens[last] = col + 1;
    }
    lines[last][lens[last]] = '\0';
    col++;
}

static void erase_to_end(void) {
    int last = nlines - 1;
    if (col < lens[last]) {
        lens[last] = col;
        lines[last][col] = '\0';
    }
}

static void finish_csi(char final) {
    if (final == 'K') erase_to_end();            /* ESC [ K: erase to end of line */
    /* other CSI sequences (colours, cursor moves) are dropped */
}

/* Interprets shell output. Keeps the text, drops the control codes. */
static void feed(const unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        switch (state) {
        case ST_TEXT:
            if (c == 27)       state = ST_ESC;
            else if (c == '\n') { new_line(); col = 0; }
            else if (c == '\r') col = 0;
            else if (c == '\b') { if (col > 0) col--; }
            else if (c == '\t') { int stop = (col / 8 + 1) * 8; while (col < stop && col < MAX_COLS - 1) put_byte(' '); }
            else if (c >= 32 || c >= 0x80) put_byte(c);
            break;
        case ST_ESC:
            if (c == '[')      { state = ST_CSI; ncsi = 0; }
            else if (c == ']') state = ST_OSC;
            else               state = ST_TEXT;      /* two-byte sequence */
            break;
        case ST_CSI:
            if (c >= 0x40 && c <= 0x7e) {
                finish_csi((char)c);
                state = ST_TEXT;
            } else if (ncsi < (int)sizeof csi - 1) {
                csi[ncsi++] = (char)c;
            }
            break;
        case ST_OSC:                                  /* title and similar: ends with BEL or ESC \ */
            if (c == 7) state = ST_TEXT;
            else if (c == 27) state = ST_OSC_ESC;
            break;
        case ST_OSC_ESC:
            state = ST_TEXT;
            break;
        }
    }
    dirty = true;
}

static void push_lines(void) {
    for (int i = 0; i < nlines; i++) shown[i] = lines[i];
    api->pane_set_lines(pane_id, shown, nlines);
    api->pane_set_cursor(pane_id, nlines - 1);
    dirty = false;
}

static void write_bytes(const char *s, size_t n) {
    if (master >= 0) (void)!write(master, s, n);
}

static void write_str(const char *s) { write_bytes(s, strlen(s)); }

static void write_codepoint(unsigned int cp) {
    char enc[4];
    size_t n;
    if (cp < 0x80)        { enc[0] = (char)cp; n = 1; }
    else if (cp < 0x800)  { enc[0] = (char)(0xC0 | (cp >> 6));  enc[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else                  { enc[0] = (char)(0xE0 | (cp >> 12)); enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            enc[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    write_bytes(enc, n);
}

static void stop_shell(void) {
    if (child > 0) {
        kill(child, SIGHUP);
        waitpid(child, NULL, WNOHANG);
    }
    if (master >= 0) close(master);
    master = -1;
    child = -1;
}

static bool start_shell(Editor *e) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) { api->message(e, "terminal: no pseudo-terminal available"); return false; }
    if (grantpt(m) < 0 || unlockpt(m) < 0) {
        close(m);
        api->message(e, "terminal: could not open the pseudo-terminal");
        return false;
    }
    char *name = ptsname(m);
    pid_t pid = fork();
    if (pid < 0) { close(m); api->message(e, "terminal: fork failed"); return false; }
    if (pid == 0) {
        setsid();
        int s = open(name, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        struct winsize ws = { 24, 100, 0, 0 };
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(m);
        if (s > 2) close(s);
        setenv("TERM", "dumb", 1);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        execl(sh, sh, "-i", (char *)NULL);
        _exit(127);
    }
    master = m;
    child = pid;
    fcntl(master, F_SETFL, O_NONBLOCK);
    return true;
}

/* Called about every 100 ms: reads whatever the shell printed. */
static void on_tick(Editor *e) {
    if (master < 0) return;
    unsigned char buf[4096];
    for (;;) {
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0) break;
        feed(buf, (size_t)n);
    }
    if (child > 0 && waitpid(child, NULL, WNOHANG) > 0) {
        child = -1;
        stop_shell();
        feed((const unsigned char *)"\n[shell exited]", 14);
        api->message(e, "shell exited");
    }
    if (dirty && pane_id) push_lines();
}

static bool on_key(Editor *e, int key, void *ud) {
    (void)e;
    (void)ud;
    if (master < 0) return false;

    if (key & PLUGIN_KEY_TEXT) {
        write_codepoint((unsigned int)(key & ~PLUGIN_KEY_TEXT));
        return true;
    }
    switch (key) {
    case 10:           write_str("\r");     return true;
    case 127: case 8:  write_str("\x7f");   return true;
    case 9:            write_str("\t");     return true;
    case 3:            write_str("\x03");   return true;     /* ctrl+c to the shell */
    case 4:            write_str("\x04");   return true;     /* ctrl+d */
    case 12:           write_str("\x0c");   return true;     /* ctrl+l */
    case K_UP:         write_str("\x1b[A"); return true;
    case K_DOWN:       write_str("\x1b[B"); return true;
    case K_RIGHT:      write_str("\x1b[C"); return true;
    case K_LEFT:       write_str("\x1b[D"); return true;
    case K_HOME:       write_str("\x1b[H"); return true;
    default:           return false;                          /* esc, F-keys: core handles */
    }
}

static void cmd_terminal(Editor *e, const char *arg) {
    (void)arg;
    if (pane_id) {
        stop_shell();
        api->pane_close(pane_id);
        pane_id = 0;
        api->message(e, "terminal closed");
        return;
    }
    pane_id = api->pane_open("Terminal", PANE_BOTTOM, 35, on_key, NULL);
    if (!pane_id) {
        api->message(e, "no room for another pane");
        return;
    }
    nlines = 1;
    lines[0][0] = '\0';
    lens[0] = 0;
    col = 0;
    state = ST_TEXT;
    if (!start_shell(e)) {
        api->pane_close(pane_id);
        pane_id = 0;
        return;
    }
    push_lines();
    api->pane_focus(pane_id);
}

void neonano_plugin_init(const PluginAPI *a) {
    if (a->version < 5) return;                /* panes, ticks */
    api = a;
    a->add_command("terminal", "terminal  open or close a shell pane", cmd_terminal);
    a->on_tick(on_tick);
    a->bind_key("f4", "terminal");
}
