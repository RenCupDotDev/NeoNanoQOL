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
/* Example plugin: adds the :wc command.
 * Build with `make plugins`, then `make install-plugins`, then add
 * `enabled = ["wordcount"]` under [plugins] in ~/.config/neonano/config.toml. */
#include <ctype.h>
#include <stdio.h>
#include "plugin.h"

static const PluginAPI *api;

static void cmd_wc(Editor *e, const char *arg) {
    (void)arg;
    size_t lines = api->line_count(e);
    size_t words = 0, bytes = 0;

    for (size_t r = 0; r < lines; r++) {
        size_t n;
        const char *s = api->line_text(e, r, &n);
        bytes += n;
        bool in_word = false;
        for (size_t i = 0; i < n; i++) {
            bool space = isspace((unsigned char)s[i]);
            if (!space && !in_word) { words++; in_word = true; }
            else if (space)         { in_word = false; }
        }
    }

    char msg[128];
    snprintf(msg, sizeof msg, "%zu lines, %zu words, %zu bytes", lines, words, bytes);
    api->message(e, msg);
}

void neonano_plugin_init(const PluginAPI *a) {
    api = a;
    a->add_command("wc", "wc  count lines, words and bytes", cmd_wc);
}
