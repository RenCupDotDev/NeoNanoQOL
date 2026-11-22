# Every plugin is plugins/<name>/<name>.c and becomes build/<name>.so.
# Add a name to NAMES when you add a plugin.
CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
# The version is read from the VERSION file and compiled into every plugin
# as PLUGIN_VERSION (see include/plugin.h).
VERSION := $(strip $(shell cat VERSION))
CPPFLAGS += -DPLUGIN_VERSION='"$(VERSION)"'
NAMES   = dirtree lsp terminal welcome wordcount
PLUGINS = $(addprefix build/,$(addsuffix .so,$(NAMES)))

all: $(PLUGINS)

build:
	mkdir -p build

define plugin_rule
build/$(1).so: plugins/$(1)/$(1).c include/plugin.h VERSION | build
	$$(CC) $$(CFLAGS) $$(CPPFLAGS) -fPIC -shared -Iinclude -o $$@ $$<
endef
$(foreach p,$(NAMES),$(eval $(call plugin_rule,$(p))))

# Copies the plugins where neonano looks for them.
install: all
	mkdir -p $(HOME)/.nnanoplugins
	cp $(PLUGINS) $(HOME)/.nnanoplugins/

clean:
	rm -rf build

.PHONY: all install clean
