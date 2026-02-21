/*
 * musicwidget.c
 * LightArch music widget — Cairo + Wayland + wlr-layer-shell
 *
 * Build:
 *   wayland-scanner client-header \
 *     /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
 *     wlr-layer-shell-unstable-v1-client-protocol.h
 *   wayland-scanner private-code \
 *     /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
 *     wlr-layer-shell-unstable-v1-client-protocol.c
 *   wayland-scanner client-header \
 *     /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
 *     xdg-shell-client-protocol.h
 *   wayland-scanner private-code \
 *     /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
 *     xdg-shell-client-protocol.c
 *
 *   gcc -o musicwidget musicwidget.c \
 *     wlr-layer-shell-unstable-v1-client-protocol.c \
 *     xdg-shell-client-protocol.c \
 *     $(pkg-config --cflags --libs wayland-client cairo) \
 *     -lwayland-cursor -lm -lrt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* ── Dimensions ──────────────────────────────────────────────────────── */
#define WIDTH        320
#define HEIGHT       100
#define MARGIN       20
#define ART_SIZE     72
#define ART_RADIUS   10.0
#define CARD_RADIUS  18.0
#define POLL_MS      100   /* poll playerctl every 100ms */

#define BTN_CX  (WIDTH  - MARGIN - 14)
#define BTN_CY  (MARGIN + 14)
#define BTN_R   14

/* ── Colours ─────────────────────────────────────────────────────────── */
#define COL_BG      0.059, 0.059, 0.059, 1.0
#define COL_BORDER  0.165, 0.165, 0.165, 1.0
#define COL_ART_BG  0.102, 0.102, 0.102, 1.0
#define COL_TITLE   0.941, 0.941, 0.941, 1.0
#define COL_ARTIST  0.533, 0.533, 0.533, 1.0
#define COL_ALBUM   0.314, 0.314, 0.314, 1.0
#define COL_TRACK   0.165, 0.165, 0.165, 1.0
#define COL_FILL    0.878, 0.878, 0.878, 1.0
#define COL_BTN     1.0,   1.0,   1.0,   1.0
#define COL_BTN_FG  0.059, 0.059, 0.059, 1.0
#define COL_NOTE    0.267, 0.267, 0.267, 1.0
#define FONT_FACE   "Lettera Mono LL"

/* ── Wayland globals ─────────────────────────────────────────────────── */
static struct wl_display              *display;
static struct wl_compositor           *compositor;
static struct wl_shm                  *shm;
static struct zwlr_layer_shell_v1     *layer_shell;
static struct wl_seat                 *seat;

static struct wl_surface              *surface;
static struct zwlr_layer_surface_v1   *layer_surface;
static struct wl_buffer               *buffer;
static struct wl_pointer              *pointer;

static struct wl_cursor_theme         *cursor_theme;
static struct wl_cursor               *cursor_pointer;
static struct wl_cursor               *cursor_default;
static struct wl_surface              *cursor_surface;

static void  *shm_data   = NULL;
static int    shm_fd      = -1;
static int    configured  = 0;
static int    running     = 1;

/* ── Player state ────────────────────────────────────────────────────── */
typedef struct {
    char   title[256];
    char   artist[256];
    char   album[256];
    char   art_url[512];
    double position;
    double length;
    int    playing;
} PlayerState;

static PlayerState state;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static char *run_playerctl(const char *args)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "playerctl --player=kew %s 2>/dev/null", args);
    FILE *f = popen(cmd, "r");
    if (!f) return strdup("");
    char buf[1024] = {0};
    fgets(buf, sizeof(buf), f);
    pclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return strdup(buf);
}

static void poll_state(void)
{
    char *v;
    v = run_playerctl("metadata title");
    strncpy(state.title,   v, 255); free(v);
    v = run_playerctl("metadata artist");
    strncpy(state.artist,  v, 255); free(v);
    v = run_playerctl("metadata album");
    strncpy(state.album,   v, 255); free(v);
    v = run_playerctl("metadata mpris:artUrl");
    strncpy(state.art_url, v, 511); free(v);
    v = run_playerctl("position");
    state.position = atof(v); free(v);
    v = run_playerctl("metadata mpris:length");
    state.length = atof(v) / 1000000.0; free(v);
    v = run_playerctl("status");
    state.playing = (strcmp(v, "Playing") == 0); free(v);
}

static char *convert_to_png(const char *url)
{
    const char *path = url;
    if (strncmp(url, "file://", 7) == 0)
        path = url + 7;
    if (strlen(path) == 0) return NULL;

    char *tmp = strdup("/tmp/musicwidget_art_XXXXXX");
    int fd = mkstemp(tmp);
    if (fd < 0) { free(tmp); return NULL; }
    close(fd);

    char png_path[512];
    snprintf(png_path, sizeof(png_path), "%s.png", tmp);
    free(tmp);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -i '%s' '%s' >/dev/null 2>&1", path, png_path);
    system(cmd);

    return strdup(png_path);
}

/* ── Cursor helpers ──────────────────────────────────────────────────── */

static void set_cursor(struct wl_pointer *ptr,
                       uint32_t serial,
                       struct wl_cursor *cur)
{
    if (!cur || !cursor_surface) return;
    struct wl_cursor_image *img = cur->images[0];
    struct wl_buffer *cur_buf = wl_cursor_image_get_buffer(img);
    wl_pointer_set_cursor(ptr, serial, cursor_surface,
                          img->hotspot_x, img->hotspot_y);
    wl_surface_attach(cursor_surface, cur_buf, 0, 0);
    wl_surface_damage(cursor_surface, 0, 0,
                      img->width, img->height);
    wl_surface_commit(cursor_surface);
}

/* ── Cairo drawing ───────────────────────────────────────────────────── */

static void rounded_rect(cairo_t *cr,
                          double x, double y,
                          double w, double h, double r)
{
    cairo_move_to(cr, x + r, y);
    cairo_line_to(cr, x + w - r, y);
    cairo_arc(cr, x+w-r, y+r,   r, -M_PI/2, 0);
    cairo_line_to(cr, x+w, y+h-r);
    cairo_arc(cr, x+w-r, y+h-r, r,  0,       M_PI/2);
    cairo_line_to(cr, x+r, y+h);
    cairo_arc(cr, x+r,   y+h-r, r,  M_PI/2,  M_PI);
    cairo_line_to(cr, x, y+r);
    cairo_arc(cr, x+r,   y+r,   r,  M_PI,    3*M_PI/2);
    cairo_close_path(cr);
}

static void draw_art(cairo_t *cr,
                     double x, double y, double size, double radius)
{
    char *png_path = convert_to_png(state.art_url);
    cairo_surface_t *img = NULL;

    if (png_path) {
        img = cairo_image_surface_create_from_png(png_path);
        unlink(png_path);
        free(png_path);
    }

    if (!img || cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_save(cr);
        rounded_rect(cr, x, y, size, size, radius);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, COL_ART_BG);
        cairo_paint(cr);
        cairo_set_source_rgba(cr, COL_NOTE);
        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 28);
        cairo_text_extents_t te;
        cairo_text_extents(cr, "\xe2\x99\xaa", &te);
        cairo_move_to(cr,
            x + (size - te.width)  / 2 - te.x_bearing,
            y + (size - te.height) / 2 - te.y_bearing);
        cairo_show_text(cr, "\xe2\x99\xaa");
        cairo_restore(cr);
        if (img) cairo_surface_destroy(img);
        return;
    }

    int iw = cairo_image_surface_get_width(img);
    int ih = cairo_image_surface_get_height(img);
    double scale = fmax(size / iw, size / ih);

    cairo_surface_t *tmp = cairo_image_surface_create(
                               CAIRO_FORMAT_ARGB32,
                               (int)size, (int)size);
    cairo_t *tc = cairo_create(tmp);
    cairo_translate(tc, (size - iw * scale) / 2,
                        (size - ih * scale) / 2);
    cairo_scale(tc, scale, scale);
    cairo_set_source_surface(tc, img, 0, 0);
    cairo_paint(tc);
    cairo_destroy(tc);
    cairo_surface_destroy(img);

    cairo_surface_flush(tmp);
    unsigned char *data = cairo_image_surface_get_data(tmp);
    int stride = cairo_image_surface_get_stride(tmp);
    int tw = cairo_image_surface_get_width(tmp);
    int th = cairo_image_surface_get_height(tmp);

    for (int row = 0; row < th; row++) {
        uint32_t *px = (uint32_t *)(data + row * stride);
        for (int col = 0; col < tw; col++) {
            uint32_t p    = px[col];
            uint8_t  a    = (p >> 24) & 0xff;
            uint8_t  r    = (p >> 16) & 0xff;
            uint8_t  g    = (p >>  8) & 0xff;
            uint8_t  b    = (p      ) & 0xff;
            uint8_t  grey = (uint8_t)(0.299*r + 0.587*g + 0.114*b);
            px[col] = ((uint32_t)a    << 24) |
                      ((uint32_t)grey << 16) |
                      ((uint32_t)grey <<  8) |
                      (uint32_t)grey;
        }
    }
    cairo_surface_mark_dirty(tmp);

    cairo_save(cr);
    rounded_rect(cr, x, y, size, size, radius);
    cairo_clip(cr);
    cairo_set_source_surface(cr, tmp, x, y);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(tmp);
}

static void draw_text_clipped(cairo_t *cr, const char *text,
                               double x, double y, double max_w)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y - 20, max_w, 30);
    cairo_clip(cr);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    cairo_restore(cr);
}

static void draw_play_pause(cairo_t *cr,
                             double cx, double cy, double r,
                             int playing)
{
    cairo_arc(cr, cx, cy, r, 0, 2*M_PI);
    cairo_set_source_rgba(cr, COL_BTN);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, COL_BTN_FG);

    if (playing) {
        double bw = r*0.22, bh = r*0.7;
        double bx = cx - r*0.28, by = cy - bh/2;
        cairo_rectangle(cr, bx,          by, bw, bh);
        cairo_rectangle(cr, bx + r*0.38, by, bw, bh);
        cairo_fill(cr);
    } else {
        cairo_move_to(cr, cx - r*0.25, cy - r*0.4);
        cairo_line_to(cr, cx + r*0.4,  cy);
        cairo_line_to(cr, cx - r*0.25, cy + r*0.4);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

static void redraw(void)
{
    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        shm_data, CAIRO_FORMAT_ARGB32, WIDTH, HEIGHT, WIDTH*4);
    cairo_t *cr = cairo_create(cs);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    rounded_rect(cr, 0, 0, WIDTH, HEIGHT, CARD_RADIUS);
    cairo_set_source_rgba(cr, COL_BG);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, COL_BORDER);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    double art_x = 14, art_y = (HEIGHT - ART_SIZE) / 2.0;
    draw_art(cr, art_x, art_y, ART_SIZE, ART_RADIUS);

    double tx       = art_x + ART_SIZE + 14;
    double text_max = BTN_CX - BTN_R - 8 - tx;

    cairo_select_font_face(cr, FONT_FACE,
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    cairo_set_source_rgba(cr, COL_TITLE);
    draw_text_clipped(cr,
        strlen(state.title) > 0 ? state.title : "Nothing playing",
        tx, 38, text_max);

    cairo_select_font_face(cr, FONT_FACE,
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, COL_ARTIST);
    draw_text_clipped(cr, state.artist, tx, 54, text_max);

    cairo_set_font_size(cr, 10);
    cairo_set_source_rgba(cr, COL_ALBUM);
    draw_text_clipped(cr, state.album, tx, 68, text_max);

    double pb_x = tx, pb_y = 80, pb_h = 2;
    double pb_w = BTN_CX - BTN_R - 8 - pb_x;
    double prog = state.length > 0
                ? fmin(1.0, state.position / state.length)
                : 0.0;
    cairo_set_source_rgba(cr, COL_TRACK);
    cairo_rectangle(cr, pb_x, pb_y, pb_w, pb_h);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, COL_FILL);
    cairo_rectangle(cr, pb_x, pb_y, pb_w * prog, pb_h);
    cairo_fill(cr);

    draw_play_pause(cr, BTN_CX, BTN_CY, BTN_R, state.playing);

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    wl_display_flush(display);
}

/* ── Pointer events ──────────────────────────────────────────────────── */

static double   ptr_x            = 0, ptr_y = 0;
static uint32_t ptr_enter_serial = 0;
static uint32_t last_click_time  = 0;
static int      suppress_poll    = 0;

static int over_button(void)
{
    double dx = ptr_x - BTN_CX;
    double dy = ptr_y - BTN_CY;
    return sqrt(dx*dx + dy*dy) <= BTN_R;
}

static void pointer_enter(void *data, struct wl_pointer *ptr,
    uint32_t serial, struct wl_surface *surf,
    wl_fixed_t x, wl_fixed_t y)
{
    ptr_enter_serial = serial;
    ptr_x = wl_fixed_to_double(x);
    ptr_y = wl_fixed_to_double(y);
    set_cursor(ptr, serial, cursor_default);
}

static void pointer_leave(void *data, struct wl_pointer *ptr,
    uint32_t serial, struct wl_surface *surf) {}

static void pointer_motion(void *data, struct wl_pointer *ptr,
    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    ptr_x = wl_fixed_to_double(x);
    ptr_y = wl_fixed_to_double(y);
    if (over_button())
        set_cursor(ptr, ptr_enter_serial, cursor_pointer);
    else
        set_cursor(ptr, ptr_enter_serial, cursor_default);
}

static void pointer_button(void *data, struct wl_pointer *ptr,
    uint32_t serial, uint32_t time,
    uint32_t button, uint32_t btn_state)
{
    if (btn_state != WL_POINTER_BUTTON_STATE_PRESSED || button != 0x110)
        return;

    if (!over_button()) return;

    /* Debounce — ignore clicks within 300ms of the last one.
     * Because apparently some people have the trigger finger
     * of a caffeinated squirrel. */
    if (time - last_click_time < 300) return;
    last_click_time = time;

    /* Flip the icon FIRST, flush to screen immediately,
     * THEN fire playerctl. Feels instant. Is instant.
     * Playerctl can lumber along at its own pace. */
    state.playing = !state.playing;
    redraw();
    wl_display_flush(display);
    system("playerctl --player=kew play-pause");

    /* Suppress the next few polls so playerctl has time to
     * actually act before we ask it what it's doing. */
    suppress_poll = 3;
}

static void pointer_axis(void *data, struct wl_pointer *ptr,
    uint32_t time, uint32_t axis, wl_fixed_t value) {}
static void pointer_frame(void *data, struct wl_pointer *ptr) {}
static void pointer_axis_source(void *data, struct wl_pointer *ptr,
    uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *ptr,
    uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *ptr,
    uint32_t axis, int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter         = pointer_enter,
    .leave         = pointer_leave,
    .motion        = pointer_motion,
    .button        = pointer_button,
    .axis          = pointer_axis,
    .frame         = pointer_frame,
    .axis_source   = pointer_axis_source,
    .axis_stop     = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* ── Seat ────────────────────────────────────────────────────────────── */

static void seat_capabilities(void *data, struct wl_seat *s,
    uint32_t caps)
{
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *s,
    const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* ── Registry ────────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(reg, name,
                         &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(reg, name,
                  &wl_shm_interface, 1);
    else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
        layer_shell = wl_registry_bind(reg, name,
                          &zwlr_layer_shell_v1_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(reg, name,
                   &wl_seat_interface, 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_global_remove(void *data,
    struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── Layer surface ───────────────────────────────────────────────────── */

static void layer_surface_configure(void *data,
    struct zwlr_layer_surface_v1 *surf,
    uint32_t serial, uint32_t w, uint32_t h)
{
    zwlr_layer_surface_v1_ack_configure(surf, serial);
    configured = 1;
}

static void layer_surface_closed(void *data,
    struct zwlr_layer_surface_v1 *surf)
{
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* ── SHM buffer ──────────────────────────────────────────────────────── */

static struct wl_buffer *create_buffer(void)
{
    int stride = WIDTH * 4;
    int size   = stride * HEIGHT;
    char name[32];
    snprintf(name, sizeof(name), "/musicwidget-%d", getpid());
    shm_fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    shm_unlink(name);
    ftruncate(shm_fd, size);
    shm_data = mmap(NULL, size,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED, shm_fd, 0);
    struct wl_shm_pool *pool =
        wl_shm_create_pool(shm, shm_fd, size);
    struct wl_buffer *buf =
        wl_shm_pool_create_buffer(pool, 0,
            WIDTH, HEIGHT, stride,
            WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buf;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "musicwidget: cannot connect to Wayland\n");
        return 1;
    }

    struct wl_registry *registry =
        wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "musicwidget: missing Wayland globals\n");
        return 1;
    }

    cursor_theme   = wl_cursor_theme_load(NULL, 24, shm);
    cursor_pointer = wl_cursor_theme_get_cursor(cursor_theme, "pointer");
    cursor_default = wl_cursor_theme_get_cursor(cursor_theme, "default");
    cursor_surface = wl_compositor_create_surface(compositor);

    surface = wl_compositor_create_surface(compositor);

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        "musicwidget");

    zwlr_layer_surface_v1_set_size(layer_surface, WIDTH, HEIGHT);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_margin(
        layer_surface, 0, MARGIN, MARGIN, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(layer_surface,
        &layer_surface_listener, NULL);

    struct wl_region *input_region =
        wl_compositor_create_region(compositor);
    wl_region_add(input_region, 0, 0, WIDTH, HEIGHT);
    wl_surface_set_input_region(surface, input_region);
    wl_region_destroy(input_region);

    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    if (!configured) {
        fprintf(stderr, "musicwidget: layer surface not configured\n");
        return 1;
    }

    buffer = create_buffer();
    poll_state();
    redraw();

    /*
     * Main loop — use poll() on the Wayland fd so we block
     * efficiently waiting for compositor events, but wake up
     * at least every POLL_MS to refresh playerctl state.
     *
     * This keeps the button responsive AND the display current
     * without busy-looping like an absolute maniac.
     */
    int wl_fd = wl_display_get_fd(display);
    struct timespec last_poll_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_poll_ts);

    while (running) {
        /* Flush any pending outgoing requests. */
        if (wl_display_flush(display) < 0) break;

        /* Work out how long until the next playerctl poll. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec  - last_poll_ts.tv_sec)  * 1000
                        + (now.tv_nsec - last_poll_ts.tv_nsec) / 1000000;
        int timeout = (int)(POLL_MS - elapsed_ms);
        if (timeout < 0) timeout = 0;

        /* Block on the Wayland fd until an event arrives or
         * the poll timer fires — whichever comes first. */
        struct pollfd pfd = { .fd = wl_fd, .events = POLLIN };
        poll(&pfd, 1, timeout);

        /* Dispatch whatever Wayland events are waiting. */
        if (wl_display_dispatch(display) < 0) break;

        /* Poll playerctl on schedule. */
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (now.tv_sec  - last_poll_ts.tv_sec)  * 1000
                   + (now.tv_nsec - last_poll_ts.tv_nsec) / 1000000;
        if (elapsed_ms >= POLL_MS) {
            last_poll_ts = now;
            if (suppress_poll > 0) {
                suppress_poll--;
            } else {
                poll_state();
                redraw();
            }
        }
    }

    wl_cursor_theme_destroy(cursor_theme);
    return 0;
}
