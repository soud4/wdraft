// wdraft - drag-and-drop source/sink for Wayland, without a visible window.
//
// Copyright 2026 wdraft contributors.
// Derived in spirit from dragon (Copyright 2014 Michael Homer), whose MIME
// payload formats this reimplements without GTK.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <poll.h>
#include <time.h>
#include <wayland-client.h>

#include "ext-data-control-v1.h"
#include "single-pixel-buffer-v1.h"
#include "viewporter.h"
#include "wlr-layer-shell-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"

#define VERSION "0.1.0"

// Exit codes. Anything the drag machinery can fail at gets its own, so a
// keybind wrapper can tell "nothing stashed" from "target refused the drop".
#define EXIT_OK        0
#define EXIT_USAGE     1
#define EXIT_CANCELLED 2
#define EXIT_TIMEOUT   3
#define EXIT_NOWAYLAND 4

static const char *progname = "wdraft";
static bool verbose = false;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: error: ", progname);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(EXIT_USAGE);
}

static void info(const char *fmt, ...) {
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", progname);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p)
        die("out of memory");
    return p;
}

// ---------------------------------------------------------------- paths/URIs

// Percent-encode everything outside RFC 3986 unreserved, keeping '/' so the
// path structure survives. file:// URIs are what every drop target wants.
static char *path_to_uri(const char *path) {
    static const char *unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "abcdefghijklmnopqrstuvwxyz"
                                    "0123456789-_.~/";
    size_t len = strlen(path);
    char *uri = xmalloc(len * 3 + sizeof("file://"));
    char *w = uri;
    w += sprintf(w, "file://");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (strchr(unreserved, c) && c != '\0')
            *w++ = (char)c;
        else
            w += sprintf(w, "%%%02X", c);
    }
    *w = '\0';
    return uri;
}

// Decode a file:// URI (or a bare path) back into a filesystem path. Returns
// NULL for URIs with a scheme we cannot turn into a path.
static char *uri_to_path(const char *uri) {
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        // Skip an empty or "localhost" authority.
        if (strncmp(p, "localhost", 9) == 0 && p[9] == '/')
            p += 9;
    } else if (strstr(p, "://")) {
        return NULL;
    }
    char *out = xmalloc(strlen(p) + 1);
    char *w = out;
    while (*p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            *w++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
    return out;
}

// Resolve to an absolute path, verifying the file exists. realpath() gives us
// both at once; a stash full of paths that no longer resolve is worse than a
// loud failure at `add` time.
static char *resolve(const char *arg) {
    char *abs = realpath(arg, NULL);
    if (!abs)
        die("cannot resolve `%s': %s", arg, strerror(errno));
    return abs;
}

// -------------------------------------------------------------------- stash

struct stash {
    char **paths;
    size_t n;
    size_t cap;
};

static void stash_push(struct stash *s, char *path) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->paths = realloc(s->paths, s->cap * sizeof(char *));
        if (!s->paths)
            die("out of memory");
    }
    s->paths[s->n++] = path;
}

static void stash_free(struct stash *s) {
    for (size_t i = 0; i < s->n; i++)
        free(s->paths[i]);
    free(s->paths);
    s->paths = NULL;
    s->n = s->cap = 0;
}

// The invisible holding space is just a file: no daemon, no socket, and it
// survives across drops until `clear`.
static const char *stash_dir(void) {
    static char dir[PATH_MAX];
    if (dir[0])
        return dir;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0])
        snprintf(dir, sizeof dir, "%s/wdraft", runtime);
    else
        snprintf(dir, sizeof dir, "/tmp/wdraft-%u", (unsigned)getuid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        die("cannot create `%s': %s", dir, strerror(errno));
    return dir;
}

static const char *stash_file(void) {
    static char path[PATH_MAX];
    if (!path[0])
        snprintf(path, sizeof path, "%s/stash", stash_dir());
    return path;
}

static void stash_read(struct stash *s) {
    FILE *f = fopen(stash_file(), "r");
    if (!f) {
        if (errno == ENOENT)
            return; // An absent stash is an empty stash.
        die("cannot read `%s': %s", stash_file(), strerror(errno));
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0)
            stash_push(s, strdup(line));
    }
    free(line);
    fclose(f);
}

// Write via a temp file + rename so a concurrent `drop` never sees a half
// written stash.
static void stash_write(const struct stash *s) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/stash.tmp", stash_dir());
    FILE *f = fopen(tmp, "w");
    if (!f)
        die("cannot write `%s': %s", tmp, strerror(errno));
    for (size_t i = 0; i < s->n; i++)
        fprintf(f, "%s\n", s->paths[i]);
    if (fclose(f) != 0)
        die("cannot write `%s': %s", tmp, strerror(errno));
    if (rename(tmp, stash_file()) != 0)
        die("cannot replace `%s': %s", stash_file(), strerror(errno));
}

// Collect paths from argv, or from stdin (one per line) for a lone "-".
static void collect_args(struct stash *s, int argc, char **argv, int first) {
    bool from_stdin = (argc - first == 1 && strcmp(argv[first], "-") == 0);
    if (from_stdin) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, stdin)) > 0) {
            if (line[len - 1] == '\n')
                line[--len] = '\0';
            if (len > 0)
                stash_push(s, resolve(line));
        }
        free(line);
        return;
    }
    for (int i = first; i < argc; i++)
        stash_push(s, resolve(argv[i]));
}

// ------------------------------------------------------------------- payloads

// The formats a drop target may ask for, in the order we advertise them.
// text/uri-list is what browsers, file managers and upload widgets negotiate.
static const char *const MIMES[] = {
    "text/uri-list",
    "text/plain;charset=utf-8",
    "text/plain",
    "x-special/gnome-copied-files",
};
#define N_MIMES (sizeof MIMES / sizeof MIMES[0])

// Build the bytes to hand over for a given MIME type. Caller frees.
// text/uri-list is CRLF-delimited per RFC 2483; the rest use LF.
static char *payload_for(const char *mime, const struct stash *s, size_t *out_len) {
    bool uri_list = strcmp(mime, "text/uri-list") == 0;
    bool gnome = strcmp(mime, "x-special/gnome-copied-files") == 0;
    bool uris = uri_list || gnome;
    const char *eol = uri_list ? "\r\n" : "\n";

    size_t cap = gnome ? sizeof("copy\n") : 1;
    for (size_t i = 0; i < s->n; i++)
        cap += strlen(s->paths[i]) * 3 + sizeof("file://") + 2;
    char *buf = xmalloc(cap);
    char *w = buf;
    if (gnome)
        w += sprintf(w, "copy\n");
    for (size_t i = 0; i < s->n; i++) {
        if (uris) {
            char *uri = path_to_uri(s->paths[i]);
            w += sprintf(w, "%s%s", uri, eol);
            free(uri);
        } else {
            w += sprintf(w, "%s%s", s->paths[i], eol);
        }
    }
    *out_len = (size_t)(w - buf);
    return buf;
}

// ---------------------------------------------------------------- subcommands

static int cmd_add(int argc, char **argv, int first, bool replace) {
    struct stash s = {0};
    if (!replace)
        stash_read(&s);
    size_t before = s.n;
    collect_args(&s, argc, argv, first);
    if (s.n == before && replace == false && before == 0)
        die("nothing to add");
    stash_write(&s);
    info("stash holds %zu item(s)", s.n);
    stash_free(&s);
    return EXIT_OK;
}

static int cmd_list(bool print_path) {
    struct stash s = {0};
    stash_read(&s);
    for (size_t i = 0; i < s.n; i++) {
        if (print_path) {
            puts(s.paths[i]);
        } else {
            char *uri = path_to_uri(s.paths[i]);
            puts(uri);
            free(uri);
        }
    }
    stash_free(&s);
    return EXIT_OK;
}

static int cmd_clear(void) {
    if (unlink(stash_file()) != 0 && errno != ENOENT)
        die("cannot clear `%s': %s", stash_file(), strerror(errno));
    return EXIT_OK;
}

// ------------------------------------------------------------------- wayland

// Everything the three Wayland-touching commands share. One instance, on the
// stack of each cmd_* entry point.
struct wl {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_data_device_manager *ddm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_virtual_pointer_manager_v1 *vpm;
    struct wp_single_pixel_buffer_manager_v1 *spb;
    struct wp_viewporter *viewporter;
    struct ext_data_control_manager_v1 *dcm;

    // The invisible catch-all surface.
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    struct wl_buffer *buffer;
    int32_t width, height;
    bool configured;
    bool closed;

    struct wl_pointer *pointer;
    struct wl_data_device *data_device;
    struct zwlr_virtual_pointer_v1 *vp;

    bool entered;
    double cx, cy;              // cursor position, surface-local == output-local
    uint32_t button_serial;     // the serial start_drag() needs
    bool button_pressed;        // our synthetic press is outstanding

    // Drag source (drop, copy).
    struct wl_data_source *source;
    const struct stash *stash;
    bool drop_performed;
    bool dnd_finished;
    bool target_responded;      // the target told us whether it wants the data
    bool target_accepted;
    bool cancelled;
    uint32_t action;

    // Drag sink (catch).
    struct wl_data_offer *offer;
    const char *offer_mime;     // best type the incoming offer provides
    uint32_t enter_serial;
    bool dropped;
    int drops_seen;
};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Dispatch until pred() holds or the deadline passes. Returns pred()'s final
// value. This is also what keeps serving wl_data_source.send while we wait for
// the drop to be acknowledged.
static bool wait_for(struct wl *w, bool (*pred)(struct wl *), int timeout_ms) {
    uint32_t deadline = now_ms() + (uint32_t)timeout_ms;
    while (!pred(w)) {
        while (wl_display_prepare_read(w->display) != 0) {
            if (wl_display_dispatch_pending(w->display) < 0)
                die("wayland connection lost");
        }
        if (wl_display_flush(w->display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(w->display);
            die("wayland connection lost");
        }
        if (pred(w)) {
            wl_display_cancel_read(w->display);
            return true;
        }
        int32_t left = (int32_t)(deadline - now_ms());
        if (left <= 0) {
            wl_display_cancel_read(w->display);
            return false;
        }
        struct pollfd pfd = {wl_display_get_fd(w->display), POLLIN, 0};
        int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR) {
            wl_display_cancel_read(w->display);
            continue;
        }
        if (r <= 0) {
            wl_display_cancel_read(w->display);
            return pred(w);
        }
        if (wl_display_read_events(w->display) < 0)
            die("wayland connection lost");
        if (wl_display_dispatch_pending(w->display) < 0)
            die("wayland connection lost");
    }
    return true;
}

// ----------------------------------------------------------- registry/globals

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    struct wl *w = data;
#define BIND(field, type, iface_var, want) \
    if (strcmp(iface, (iface_var).name) == 0) { \
        uint32_t v = version < (want) ? version : (want); \
        w->field = wl_registry_bind(reg, name, &(iface_var), v); \
        return; \
    }
    BIND(compositor, struct wl_compositor, wl_compositor_interface, 4)
    BIND(seat, struct wl_seat, wl_seat_interface, 5)
    BIND(ddm, struct wl_data_device_manager, wl_data_device_manager_interface, 3)
    BIND(layer_shell, struct zwlr_layer_shell_v1, zwlr_layer_shell_v1_interface, 4)
    BIND(vpm, struct zwlr_virtual_pointer_manager_v1,
         zwlr_virtual_pointer_manager_v1_interface, 2)
    BIND(spb, struct wp_single_pixel_buffer_manager_v1,
         wp_single_pixel_buffer_manager_v1_interface, 1)
    BIND(viewporter, struct wp_viewporter, wp_viewporter_interface, 1)
    BIND(dcm, struct ext_data_control_manager_v1,
         ext_data_control_manager_v1_interface, 1)
#undef BIND
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wl_connect(struct wl *w) {
    w->display = wl_display_connect(NULL);
    if (!w->display) {
        fprintf(stderr, "%s: error: cannot connect to a Wayland display"
                        " (is WAYLAND_DISPLAY set?)\n", progname);
        exit(EXIT_NOWAYLAND);
    }
    w->registry = wl_display_get_registry(w->display);
    wl_registry_add_listener(w->registry, &registry_listener, w);
    wl_display_roundtrip(w->display);
}

// Fail with a message that names the missing global, because "it does not
// work" is useless when the answer is "your compositor lacks layer-shell".
static void require(const void *ptr, const char *iface) {
    if (!ptr) {
        fprintf(stderr, "%s: error: compositor does not support %s\n",
                progname, iface);
        exit(EXIT_NOWAYLAND);
    }
}

// ------------------------------------------------------- invisible surface

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t width, uint32_t height) {
    struct wl *w = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    w->width = (int32_t)width;
    w->height = (int32_t)height;
    w->configured = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    ((struct wl *)data)->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static bool pred_configured(struct wl *w) { return w->configured || w->closed; }

// A fullscreen overlay surface that is completely transparent but does take
// pointer input. It is how we get hold of the cursor without drawing anything:
// Wayland will not let us start a drag without a button event on a surface of
// our own.
static void surface_create(struct wl *w) {
    require(w->compositor, "wl_compositor");
    require(w->layer_shell, "zwlr_layer_shell_v1 (wlr-layer-shell)");
    require(w->spb, "wp_single_pixel_buffer_v1");
    require(w->viewporter, "wp_viewporter");

    w->surface = wl_compositor_create_surface(w->compositor);
    w->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        w->layer_shell, w->surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wdraft");
    zwlr_layer_surface_v1_add_listener(w->layer_surface, &layer_listener, w);
    zwlr_layer_surface_v1_set_size(w->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(w->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(w->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(w->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(w->surface);

    if (!wait_for(w, pred_configured, 2000))
        die("compositor never configured the layer surface");
    if (w->closed)
        die("compositor closed the layer surface");
    info("surface configured %dx%d", w->width, w->height);

    // A 1x1 fully transparent pixel, stretched over the whole output by the
    // viewport. No wl_shm, no pixel buffer, nothing visible.
    w->buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        w->spb, 0, 0, 0, 0);
    w->viewport = wp_viewporter_get_viewport(w->viewporter, w->surface);
    wp_viewport_set_destination(w->viewport, w->width, w->height);

    struct wl_region *all = wl_compositor_create_region(w->compositor);
    wl_region_add(all, 0, 0, w->width, w->height);
    wl_surface_set_input_region(w->surface, all);
    wl_region_destroy(all);

    wl_surface_attach(w->surface, w->buffer, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(w->surface);
    wl_display_roundtrip(w->display);
}

// Stop taking pointer input while staying mapped. Destroying the drag origin
// mid-session is undefined in the protocol, so instead we go input-transparent
// and let the compositor hand the drag to whatever is underneath.
static void detach_origin_surface(struct wl *w) {
    struct wl_region *empty = wl_compositor_create_region(w->compositor);
    wl_surface_set_input_region(w->surface, empty);
    wl_region_destroy(empty);
    wl_surface_commit(w->surface);
    wl_display_roundtrip(w->display);
}

// ------------------------------------------------------------------- pointer

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    struct wl *w = data;
    if (surface != w->surface)
        return;
    w->entered = true;
    w->cx = wl_fixed_to_double(x);
    w->cy = wl_fixed_to_double(y);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface) {}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y) {
    struct wl *w = data;
    w->cx = wl_fixed_to_double(x);
    w->cy = wl_fixed_to_double(y);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    struct wl *w = data;
    if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
        w->button_serial = serial;
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {}
static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t s) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t a, int32_t d) {}
static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void pointer_axis_relative_direction(void *data, struct wl_pointer *p,
                                            uint32_t a, uint32_t d) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

static bool pred_entered(struct wl *w) { return w->entered; }
static bool pred_button(struct wl *w) { return w->button_serial != 0; }

// A zero-distance move: enough to make the compositor recompute what is under
// the cursor, without moving the cursor a single pixel.
static void vp_nudge(struct wl *w) {
    zwlr_virtual_pointer_v1_motion(w->vp, now_ms(), 0, 0);
    zwlr_virtual_pointer_v1_frame(w->vp);
    wl_display_flush(w->display);
}

static void vp_button(struct wl *w, uint32_t state) {
    zwlr_virtual_pointer_v1_button(w->vp, now_ms(), BTN_LEFT, state);
    zwlr_virtual_pointer_v1_frame(w->vp);
    w->button_pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    wl_display_flush(w->display);
}

// Never leave the synthetic button held down: that would look like a stuck
// mouse to the whole session.
static struct wl *cleanup_target = NULL;
static void release_button_at_exit(void) {
    struct wl *w = cleanup_target;
    if (w && w->vp && w->button_pressed) {
        vp_button(w, WL_POINTER_BUTTON_STATE_RELEASED);
        wl_display_roundtrip(w->display);
    }
}

// --------------------------------------------------------------- drag source

static void source_target(void *data, struct wl_data_source *src, const char *mime) {
    struct wl *w = data;
    w->target_responded = true;
    w->target_accepted = (mime != NULL);
    info("target accepts %s", mime ? mime : "(nothing)");
}

static void source_send(void *data, struct wl_data_source *src,
                        const char *mime, int32_t fd) {
    struct wl *w = data;
    size_t len = 0;
    char *payload = payload_for(mime, w->stash, &len);
    info("sending %zu bytes as %s", len, mime);
    // SIGPIPE is ignored; a target that hangs up early is its own problem.
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, payload + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
    free(payload);
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *src) {
    ((struct wl *)data)->cancelled = true;
}

static void source_dnd_drop_performed(void *data, struct wl_data_source *src) {
    ((struct wl *)data)->drop_performed = true;
}

static void source_dnd_finished(void *data, struct wl_data_source *src) {
    ((struct wl *)data)->dnd_finished = true;
}

static void source_action(void *data, struct wl_data_source *src, uint32_t action) {
    ((struct wl *)data)->action = action;
}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished = source_dnd_finished,
    .action = source_action,
};

static bool pred_drag_over(struct wl *w) {
    return w->dnd_finished || w->cancelled;
}

static bool pred_target_responded(struct wl *w) {
    return w->target_responded || w->cancelled;
}

static int cmd_drop(const struct stash *s, bool keep, int timeout_ms,
                    int settle_ms) {
    struct wl w = {0};
    w.stash = s;
    wl_connect(&w);
    require(w.seat, "wl_seat");
    require(w.ddm, "wl_data_device_manager");
    require(w.vpm, "zwlr_virtual_pointer_manager_v1 (wlr-virtual-pointer)");
    if (wl_data_device_manager_get_version(w.ddm) < 3)
        die("wl_data_device_manager version 3 is required for drag-and-drop");

    surface_create(&w);

    w.pointer = wl_seat_get_pointer(w.seat);
    wl_pointer_add_listener(w.pointer, &pointer_listener, &w);
    w.data_device = wl_data_device_manager_get_data_device(w.ddm, w.seat);
    w.vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(w.vpm, w.seat);
    cleanup_target = &w;
    atexit(release_button_at_exit);
    wl_display_roundtrip(w.display);

    // The compositor usually sends enter as soon as we map. If it did not,
    // a zero-distance move forces it to reconsider.
    if (!wait_for(&w, pred_entered, 300)) {
        vp_nudge(&w);
        if (!wait_for(&w, pred_entered, 700))
            die("never got pointer focus on our own surface");
    }
    info("cursor at %.0f,%.0f", w.cx, w.cy);

    // Synthesise the press that start_drag() demands a serial from.
    vp_button(&w, WL_POINTER_BUTTON_STATE_PRESSED);
    if (!wait_for(&w, pred_button, 1000)) {
        vp_button(&w, WL_POINTER_BUTTON_STATE_RELEASED);
        die("no button event arrived; is another client grabbing the pointer?");
    }
    info("button serial %u", w.button_serial);

    w.source = wl_data_device_manager_create_data_source(w.ddm);
    wl_data_source_add_listener(w.source, &source_listener, &w);
    for (size_t i = 0; i < N_MIMES; i++)
        wl_data_source_offer(w.source, MIMES[i]);
    wl_data_source_set_actions(w.source,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

    wl_data_device_start_drag(w.data_device, w.source, w.surface, NULL,
                              w.button_serial);
    wl_display_flush(w.display);

    // Step out of the way so the drag lands on the real target below us.
    detach_origin_surface(&w);
    vp_nudge(&w);

    // Releasing immediately loses a race: the compositor would see the button
    // go up before the target had a chance to answer the drag enter, and
    // cancel the session. wl_data_source.target is exactly that answer, so
    // wait for it (bounded) before letting go.
    if (!wait_for(&w, pred_target_responded, settle_ms))
        info("no target responded within %dms, releasing anyway", settle_ms);
    vp_button(&w, WL_POINTER_BUTTON_STATE_RELEASED);

    bool done = wait_for(&w, pred_drag_over, timeout_ms);
    int rc;
    if (!done) {
        fprintf(stderr, "%s: timed out waiting for the drop to complete\n", progname);
        rc = EXIT_TIMEOUT;
    } else if (w.cancelled && !w.drop_performed) {
        fprintf(stderr, "%s: nothing under the cursor accepted the drop\n", progname);
        rc = EXIT_CANCELLED;
    } else if (w.dnd_finished) {
        info("drop finished, action %u", w.action);
        rc = EXIT_OK;
    } else {
        rc = EXIT_CANCELLED;
    }

    if (rc == EXIT_OK && !keep)
        cmd_clear();

    wl_display_roundtrip(w.display);
    return rc;
}

// ----------------------------------------------------------------- drag sink

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
    struct wl *w = data;
    // Keep the best type we are offered, using the same preference order we
    // advertise as a source.
    for (size_t i = 0; i < N_MIMES; i++) {
        if (strcmp(mime, MIMES[i]) != 0)
            continue;
        for (size_t j = 0; j < N_MIMES; j++) {
            if (w->offer_mime && strcmp(w->offer_mime, MIMES[j]) == 0)
                return; // already have something at least as good
            if (j == i)
                break;
        }
        w->offer_mime = MIMES[i];
        return;
    }
}

static void offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t a) {}
static void offer_action(void *data, struct wl_data_offer *offer, uint32_t a) {}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void dd_data_offer(void *data, struct wl_data_device *dd,
                          struct wl_data_offer *offer) {
    struct wl *w = data;
    w->offer = offer;
    w->offer_mime = NULL;
    wl_data_offer_add_listener(offer, &offer_listener, w);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
                     struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *offer) {
    struct wl *w = data;
    if (!offer)
        return;
    w->offer = offer;
    w->enter_serial = serial;
    if (!w->offer_mime) {
        info("incoming drag offers nothing we can read");
        wl_data_offer_accept(offer, serial, NULL);
        return;
    }
    info("incoming drag, accepting %s", w->offer_mime);
    wl_data_offer_accept(offer, serial, w->offer_mime);
    wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
}

static void dd_leave(void *data, struct wl_data_device *dd) {
    struct wl *w = data;
    if (w->offer) {
        wl_data_offer_destroy(w->offer);
        w->offer = NULL;
        w->offer_mime = NULL;
    }
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t time,
                      wl_fixed_t x, wl_fixed_t y) {
    struct wl *w = data;
    if (w->offer && w->offer_mime)
        wl_data_offer_accept(w->offer, w->enter_serial, w->offer_mime);
}

// Read the whole transfer off a pipe the source writes into.
static char *drain(int fd, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf)
                die("out of memory");
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    *out_len = len;
    return buf;
}

static bool print_path_mode = false;

static void dd_drop(void *data, struct wl_data_device *dd) {
    struct wl *w = data;
    if (!w->offer || !w->offer_mime) {
        info("drop with nothing readable");
        return;
    }
    int fds[2];
    if (pipe(fds) != 0)
        die("pipe: %s", strerror(errno));
    wl_data_offer_receive(w->offer, w->offer_mime, fds[1]);
    wl_display_flush(w->display);
    close(fds[1]);

    size_t len = 0;
    char *buf = drain(fds[0], &len);
    close(fds[0]);

    bool is_uris = strcmp(w->offer_mime, "text/uri-list") == 0 ||
                   strcmp(w->offer_mime, "x-special/gnome-copied-files") == 0;
    // Split on either line ending; uri-list uses CRLF, the text types LF.
    char *save = buf, *end = buf + len;
    while (save < end) {
        char *nl = memchr(save, '\n', (size_t)(end - save));
        char *stop = nl ? nl : end;
        if (stop > save && stop[-1] == '\r')
            stop--;
        size_t n = (size_t)(stop - save);
        if (n > 0 && save[0] != '#' && strncmp(save, "copy", n) != 0) {
            char *line = xmalloc(n + 1);
            memcpy(line, save, n);
            line[n] = '\0';
            if (is_uris && print_path_mode) {
                char *path = uri_to_path(line);
                if (path) {
                    puts(path);
                    free(path);
                } else {
                    puts(line);
                }
            } else {
                puts(line);
            }
            free(line);
        }
        save = nl ? nl + 1 : end;
    }
    free(buf);
    fflush(stdout);

    wl_data_offer_finish(w->offer);
    wl_data_offer_destroy(w->offer);
    w->offer = NULL;
    w->offer_mime = NULL;
    w->dropped = true;
    w->drops_seen++;
    wl_display_flush(w->display);
}

static void dd_selection(void *data, struct wl_data_device *dd,
                         struct wl_data_offer *offer) {
    // We never read the clipboard; drop the offer so it is not leaked.
    struct wl *w = data;
    if (offer) {
        if (offer == w->offer) {
            w->offer = NULL;
            w->offer_mime = NULL;
        }
        wl_data_offer_destroy(offer);
    }
}

static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

static bool pred_dropped(struct wl *w) { return w->dropped || w->closed; }
static bool pred_never(struct wl *w) { return w->closed; }

static int cmd_catch(bool and_exit, int timeout_ms) {
    struct wl w = {0};
    wl_connect(&w);
    require(w.seat, "wl_seat");
    require(w.ddm, "wl_data_device_manager");

    surface_create(&w);
    w.data_device = wl_data_device_manager_get_data_device(w.ddm, w.seat);
    wl_data_device_add_listener(w.data_device, &dd_listener, &w);
    wl_display_roundtrip(w.display);

    info("catching anywhere on screen; drop something");
    if (and_exit) {
        if (!wait_for(&w, pred_dropped, timeout_ms))
            return EXIT_TIMEOUT;
        return w.drops_seen ? EXIT_OK : EXIT_CANCELLED;
    }
    // Run until killed.
    wait_for(&w, pred_never, INT32_MAX);
    return EXIT_OK;
}

// ------------------------------------------------------ clipboard (no DnD)

static void dc_source_send(void *data, struct ext_data_control_source_v1 *src,
                           const char *mime, int32_t fd) {
    struct wl *w = data;
    size_t len = 0;
    char *payload = payload_for(mime, w->stash, &len);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, payload + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
    free(payload);
    close(fd);
}

static void dc_source_cancelled(void *data, struct ext_data_control_source_v1 *src) {
    ((struct wl *)data)->cancelled = true;
}

static const struct ext_data_control_source_v1_listener dc_source_listener = {
    .send = dc_source_send,
    .cancelled = dc_source_cancelled,
};

static void dc_dev_data_offer(void *data, struct ext_data_control_device_v1 *d,
                              struct ext_data_control_offer_v1 *o) {
    if (o)
        ext_data_control_offer_v1_destroy(o);
}
static void dc_dev_selection(void *data, struct ext_data_control_device_v1 *d,
                             struct ext_data_control_offer_v1 *o) {}
static void dc_dev_finished(void *data, struct ext_data_control_device_v1 *d) {
    ((struct wl *)data)->cancelled = true;
}
static void dc_dev_primary(void *data, struct ext_data_control_device_v1 *d,
                           struct ext_data_control_offer_v1 *o) {}

static const struct ext_data_control_device_v1_listener dc_dev_listener = {
    .data_offer = dc_dev_data_offer,
    .selection = dc_dev_selection,
    .finished = dc_dev_finished,
    .primary_selection = dc_dev_primary,
};

static bool pred_cancelled(struct wl *w) { return w->cancelled; }

// Offering the stash on the clipboard needs no pointer and no surface, so it
// is the reliable fallback for targets that take a paste but not a drop.
static int cmd_copy(const struct stash *s, bool foreground) {
    struct wl w = {0};
    w.stash = s;
    wl_connect(&w);
    require(w.seat, "wl_seat");
    require(w.dcm, "ext_data_control_manager_v1 (ext-data-control)");

    if (!foreground) {
        // Clipboard ownership lives as long as this process does, so detach
        // like wl-copy does and let the caller's keybind return.
        pid_t pid = fork();
        if (pid < 0)
            die("fork: %s", strerror(errno));
        if (pid > 0)
            return EXIT_OK;
        setsid();
        if (!verbose) {
            freopen("/dev/null", "r", stdin);
            freopen("/dev/null", "w", stdout);
        }
    }

    struct ext_data_control_device_v1 *dev =
        ext_data_control_manager_v1_get_data_device(w.dcm, w.seat);
    ext_data_control_device_v1_add_listener(dev, &dc_dev_listener, &w);
    struct ext_data_control_source_v1 *src =
        ext_data_control_manager_v1_create_data_source(w.dcm);
    ext_data_control_source_v1_add_listener(src, &dc_source_listener, &w);
    for (size_t i = 0; i < N_MIMES; i++)
        ext_data_control_source_v1_offer(src, MIMES[i]);
    ext_data_control_device_v1_set_selection(dev, src);
    wl_display_flush(w.display);

    // Serve transfers until someone else takes the clipboard.
    wait_for(&w, pred_cancelled, INT32_MAX);
    return EXIT_OK;
}

// ---------------------------------------------------------------------- main

static void usage(FILE *out) {
    fprintf(out, "wdraft - drag-and-drop for Wayland, with no visible window\n");
    fprintf(out, "Usage: %s COMMAND [OPTION]... [FILE]...\n\n", progname);
    fprintf(out, "Commands:\n");
    fprintf(out, "  add FILE...|-   add files to the stash (- reads stdin)\n");
    fprintf(out, "  set FILE...|-   replace the stash\n");
    fprintf(out, "  list            print the stash as URIs\n");
    fprintf(out, "  clear           empty the stash\n");
    fprintf(out, "  drop [FILE]...  drop the stash onto whatever is under the cursor\n");
    fprintf(out, "  catch           print whatever is dropped anywhere on screen\n");
    fprintf(out, "  copy            put the stash on the clipboard instead\n\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -k, --keep        with drop, do not clear the stash afterwards\n");
    fprintf(out, "  -p, --print-path  print paths instead of URIs\n");
    fprintf(out, "  -x, --and-exit    with catch, exit after one drop\n");
    fprintf(out, "  -t, --timeout MS  with drop, give up after MS (default 5000)\n");
    fprintf(out, "  -s, --settle MS   with drop, wait up to MS for the target to\n");
    fprintf(out, "                    answer before releasing (default 600)\n");
    fprintf(out, "  -v, --verbose     be verbose\n");
    fprintf(out, "      --help        show this help\n");
    fprintf(out, "      --version     show version details\n\n");
    fprintf(out, "Put the pointer over the drop target, then run `drop' from a\n");
    fprintf(out, "compositor keybind -- running it from a terminal aims at the\n");
    fprintf(out, "terminal. See wdraft(1).\n");
}

int main(int argc, char **argv) {
    progname = argv[0];
    bool keep = false, print_path = false, and_exit = false;
    int timeout_ms = 5000;
    int settle_ms = 600;
    const char *cmd = NULL;
    int first_file = argc;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            return EXIT_OK;
        } else if (strcmp(a, "--version") == 0) {
            puts("wdraft " VERSION);
            puts("Copyright (C) 2026 wdraft contributors");
            puts("This program comes with ABSOLUTELY NO WARRANTY.");
            puts("See the source for copying conditions.");
            return EXIT_OK;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(a, "-k") == 0 || strcmp(a, "--keep") == 0) {
            keep = true;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--print-path") == 0) {
            print_path = true;
        } else if (strcmp(a, "-x") == 0 || strcmp(a, "--and-exit") == 0) {
            and_exit = true;
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--timeout") == 0) {
            if (i + 1 >= argc || (timeout_ms = atoi(argv[++i])) <= 0)
                die("bad argument for %s", a);
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--settle") == 0) {
            if (i + 1 >= argc || (settle_ms = atoi(argv[++i])) <= 0)
                die("bad argument for %s", a);
        } else if (a[0] == '-' && a[1] != '\0' && strcmp(a, "-") != 0) {
            die("unknown option `%s'", a);
        } else if (!cmd) {
            cmd = a;
        } else {
            first_file = i;
            break;
        }
    }

    if (!cmd) {
        usage(stderr);
        return EXIT_USAGE;
    }

    signal(SIGPIPE, SIG_IGN);

    if (strcmp(cmd, "add") == 0) {
        if (first_file == argc)
            die("add needs at least one file (or `-')");
        return cmd_add(argc, argv, first_file, false);
    }
    if (strcmp(cmd, "set") == 0) {
        if (first_file == argc)
            die("set needs at least one file (or `-')");
        return cmd_add(argc, argv, first_file, true);
    }
    if (strcmp(cmd, "list") == 0)
        return cmd_list(print_path);
    if (strcmp(cmd, "clear") == 0)
        return cmd_clear();
    if (strcmp(cmd, "catch") == 0) {
        print_path_mode = print_path;
        return cmd_catch(and_exit, and_exit ? timeout_ms : INT32_MAX);
    }
    if (strcmp(cmd, "drop") == 0 || strcmp(cmd, "copy") == 0) {
        struct stash s = {0};
        if (first_file < argc)
            collect_args(&s, argc, argv, first_file); // explicit files win
        else
            stash_read(&s);
        if (s.n == 0)
            die("nothing to %s: the stash is empty (try `%s add FILE')",
                cmd, progname);
        int rc = strcmp(cmd, "drop") == 0
                     ? cmd_drop(&s, keep || first_file < argc, timeout_ms,
                                settle_ms)
                     : cmd_copy(&s, verbose);
        stash_free(&s);
        return rc;
    }

    die("unknown command `%s' (try --help)", cmd);
    return EXIT_USAGE;
}
