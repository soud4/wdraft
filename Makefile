PREFIX = $(HOME)/.local
MANPREFIX = $(PREFIX)/share/man
BASHCOMPLETIONPREFIX = $(PREFIX)/share/bash-completion/completions/
NAME = wdraft

WL_SCANNER = $(shell pkg-config --variable=wayland_scanner wayland-scanner)
WL_PROTO_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)

# Protocols: three from wayland-protocols, plus the two wlroots ones we vendor
# in protocols/ because no distro package installs them reliably.
# vpath lets a single pattern rule find an XML wherever it lives.
vpath %.xml protocols \
	$(WL_PROTO_DIR)/stable/viewporter \
	$(WL_PROTO_DIR)/stable/xdg-shell \
	$(WL_PROTO_DIR)/staging/single-pixel-buffer \
	$(WL_PROTO_DIR)/staging/ext-data-control

PROTOCOLS = \
	viewporter \
	xdg-shell \
	single-pixel-buffer-v1 \
	ext-data-control-v1 \
	wlr-layer-shell-unstable-v1 \
	wlr-virtual-pointer-unstable-v1

PROTO_SRC = $(PROTOCOLS:%=gen/%.c)
PROTO_HDR = $(PROTOCOLS:%=gen/%.h)

DEPS_CFLAGS = `pkg-config --cflags wayland-client`
DEPS_LDLIBS = `pkg-config --libs wayland-client`

all: $(NAME)

gen:
	mkdir -p gen

gen/%.h: %.xml | gen
	$(WL_SCANNER) client-header $< $@
gen/%.c: %.xml | gen
	$(WL_SCANNER) private-code $< $@

$(NAME): wdraft.c $(PROTO_SRC) $(PROTO_HDR) Makefile
	$(CC) --std=c11 -Wall -Wextra -Wno-unused-parameter $(DEFINES) \
		-Igen wdraft.c $(PROTO_SRC) -o $(NAME) \
		$(DEPS_CFLAGS) $(CFLAGS) $(LDFLAGS) $(DEPS_LDLIBS)

install: $(NAME)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(NAME) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(NAME)
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed -e "s/wdraft/$(NAME)/g" wdraft.1 > $(DESTDIR)$(MANPREFIX)/man1/$(NAME).1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/$(NAME).1
	mkdir -p $(DESTDIR)$(BASHCOMPLETIONPREFIX)
	sed -e "s/wdraft/$(NAME)/g" bash-completion > $(DESTDIR)$(BASHCOMPLETIONPREFIX)$(NAME)
	chmod 644 $(DESTDIR)$(BASHCOMPLETIONPREFIX)$(NAME)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME) $(DESTDIR)$(MANPREFIX)/man1/$(NAME).1 $(DESTDIR)$(BASHCOMPLETIONPREFIX)$(NAME)

clean:
	rm -f $(NAME)
	rm -rf gen

.PHONY: all install uninstall clean
