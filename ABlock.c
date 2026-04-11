//#define _POSIX_C_Source 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <security/pam_appl.h>
#include <pwd.h>
#include <syslog.h>
#include "ext-session-lock-v1-client-protocol.h"

#define NUM_DOTS 18
#define DOT_RADIUS 9
#define RING_RADIUS 100
#define PULSE_SPEED 0.05
#define FPS 60
#define MAX_PWD 256
#define ERROR_FLASH_DURATION 20
#define MAX_PWD_DISPLAY 32
#define ARC_THICKNESS 4
#define LOCK_ICON_WIDTH 32
#define LOCK_ICON_HEIGHT 24
#define LOCK_ICON_BAR_WIDTH 4
#define LOCK_ICON_BAR_HEIGHT 9
#define MAX_LOCKOUT_SECS 30

#define ARGB(a,r,g,b) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|\
((uint32_t)(g)<<8)|(uint32_t)(b))

#define BG_COL   ARGB(0xFF, 0x0d, 0x12, 0x1a)
#define IDLE_COL ARGB(0xFF, 0x10, 0x4a, 0x5e)
#define ACT_COL  ARGB(0xFF, 0x7f, 0xe8, 0xff)
#define RING_COL ARGB(0xFF, 0x7c, 0x3a, 0xed)
#define ERR_COL  ARGB(0xFF, 0xe2, 0x4b, 0x4a)
#define LOCK_COL ARGB(0xFF, 0xcc, 0xd6, 0xe0)
#define WAIT_COL ARGB(0xFF, 0xef, 0x9f, 0x27)

struct ABlock;
struct output;

typedef struct output {
    struct ABlock *wl;
    struct wl_output *wl_output;
    struct ext_session_lock_surface_v1 *lock_surf;
    struct wl_surface *wl_surface;
    uint32_t width, height;
    bool configured;
    bool buffers_ok;
    struct wl_shm_pool *pool;
    struct wl_buffer *buf[2];
    uint32_t *data[2];
    int buf_idx;
    size_t buf_size;
    int shm_fd;
    struct output *next;
} Output;

typedef struct ABlock {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct ext_session_lock_manager_v1 *lock_mgr;
    struct ext_session_lock_v1 *lock;
    Output *outputs;
    int noutputs;
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_map;
    struct xkb_state *xkb_state;
    char pwd[MAX_PWD];
    int pwd_len;
    double phase;
    double dot_phase[NUM_DOTS];
    int error_flash;
    int fail_count;
    bool locked_out;
    time_t lockout_until;
    bool locked;
    bool running;
    bool unlocked;
} ABlock;

static const char *pam_password_ptr;

static int pam_conv_fn(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *data){
    (void)data;
    *resp = calloc(num_msg, sizeof(struct pam_response));
    if (!*resp) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; i++){
        (*resp)[i].resp = NULL;
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON){
            (*resp)[i].resp = strdup(pam_password_ptr ? pam_password_ptr : "");
            if (!(*resp)[i].resp){
                for (int j = 0; j < i; j++) free((*resp)[j].resp);
                free(*resp);
                return PAM_BUF_ERR;
            }
        }
    }
    return PAM_SUCCESS;
}

static bool verify_password(const char *pwd){
    pam_password_ptr = pwd;
    struct pam_conv conv = { pam_conv_fn, NULL };
    pam_handle_t *pamh = NULL;
    const char *user = getenv("USER");
    if (!user){
        struct passwd *pw = getpwuid(getuid());
        user = pw ? pw->pw_name : "root";
    }
    int r = pam_start("login", user, &conv, &pamh);
    if (r != PAM_SUCCESS) return false;
    r = pam_authenticate(pamh, PAM_SILENT);
    pam_end(pamh, r);
    return r == PAM_SUCCESS;
}

static inline uint32_t lerp_col(uint32_t a, uint32_t b, double t){
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    uint8_t ar = (a>>16)&0xff, ag = (a>>8)&0xff, ab = a&0xff;
    uint8_t br = (b>>16)&0xff, bg = (b>>8)&0xff, bb = b&0xff;
    uint8_t rr = (uint8_t)(ar + (br-ar)*t);
    uint8_t rg = (uint8_t)(ag + (bg-ag)*t);
    uint8_t rb = (uint8_t)(ab + (bb-ab)*t);
    return ARGB(0xFF, rr, rg, rb);
}

static void fill_rect(uint32_t *px, int stride, int x, int y, int w, int h, uint32_t col){
    for (int row = y; row < y+h; row++)
        for (int col_ = x; col_ < x+w; col_++)
            px[row * stride + col_] = col;
}

static void draw_circle(uint32_t *px, int stride, int cx, int cy, int r, uint32_t col, int W, int H){
    int x0 = cx-r-2, x1 = cx+r+2, y0 = cy-r-2, y1 = cy+r+2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            double dx = x-cx, dy = y-cy;
            double dist = sqrt(dx*dx + dy*dy);
            double alpha = 1.0 - (dist - r);
            if (alpha <= 0.0) continue;
            if (alpha > 1.0) alpha = 1.0;
            uint32_t bg = px[y * stride + x];
            px[y * stride + x] = lerp_col(bg, col, alpha) | 0xFF000000u;
        }
    }
}

static void draw_arc(uint32_t *px, int stride, int cx, int cy, int r, int thickness, 
double start_rad, double end_rad, uint32_t col, int W, int H){
    int r_outer = r + thickness/2;
    int r_inner = r - thickness/2;
    int x0 = cx-r_outer-2, x1 = cx+r_outer+2;
    int y0 = cy-r_outer-2, y1 = cy+r_outer+2;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    double span = end_rad - start_rad;
    for (int y = y0; y < y1; y++){
        for (int x = x0; x < x1; x++){
            double dx = x-cx, dy = y-cy;
            double dist = sqrt(dx*dx + dy*dy);
            if (dist < r_inner-1 || dist > r_outer+1) continue;
            double angle = atan2(dy, dx);
            double a = fmod(angle - start_rad, 2.0*M_PI);
            if (a < 0) a += 2*M_PI;
            if (a > span) continue;
            double aa = 1.0 - fabs(dist - r);
            if (aa < 0) aa = 0;
            if (aa > 1) aa = 1;
            uint32_t bg = px[y * stride + x];
            px[y * stride + x] = lerp_col(bg, col, aa) | 0xFF000000u;
        }
    }
}

static void render_frame(Output *out){
    ABlock *wl = out->wl;
    int W = (int)out->width, H = (int)out->height;
    int stride = W, idx = out->buf_idx;
    uint32_t *px = out->data[idx];
    int cx = W/2, cy = H/2;
    double t = wl->phase;
    for (int i = 0; i < W*H; i++) px[i] = BG_COL;
    for (int i = 0; i < NUM_DOTS; i++) {
        double angle = (2.0*M_PI*i)/NUM_DOTS - M_PI/2.0;
        int dx = cx + (int)(RING_RADIUS * cos(angle));
        int dy = cy + (int)(RING_RADIUS * sin(angle));
        double bright = 0.5 + 0.5*sin(t + wl->dot_phase[i]);
        uint32_t dot_col;
        if (wl->error_flash > 0){
            double ef = (double)wl->error_flash / ERROR_FLASH_DURATION;
            dot_col = lerp_col(IDLE_COL, ERR_COL, ef * bright);
        }
        else if (wl->locked_out){
            dot_col = lerp_col(IDLE_COL, WAIT_COL, bright);
        } 
        else{
            dot_col = lerp_col(IDLE_COL, ACT_COL, bright);
        }
        uint32_t glow = lerp_col(BG_COL, dot_col, 0.25 * bright);
        draw_circle(px, stride, dx, dy, DOT_RADIUS+5, glow, W, H);
        draw_circle(px, stride, dx, dy, DOT_RADIUS, dot_col, W, H);
    }
    if (wl->pwd_len > 0){
        double frac = (double)wl->pwd_len / MAX_PWD;
        if (frac > 1.0) frac = 1.0;
        double end_angle = frac * 2.0*M_PI;
        draw_arc(px, stride, cx, cy, RING_RADIUS+22, ARC_THICKNESS, -M_PI/2.0, -M_PI/2.0 + end_angle, RING_COL, W, H);
        int show = wl->pwd_len < MAX_PWD_DISPLAY ? wl->pwd_len : MAX_PWD_DISPLAY;
        for (int i = 0; i < show; i++) {
            double a = (2.0*M_PI*i)/MAX_PWD_DISPLAY - M_PI/2.0;
            int mx = cx + (int)((RING_RADIUS+22) * cos(a));
            int my = cy + (int)((RING_RADIUS+22) * sin(a));
            draw_circle(px, stride, mx, my, 3, RING_COL, W, H);
        }
    }
    {
        int lx = cx - LOCK_ICON_WIDTH/2, ly = cy - 4;
        fill_rect(px, stride, lx, ly, LOCK_ICON_WIDTH, LOCK_ICON_HEIGHT, LOCK_COL);
        draw_arc(px, stride, cx, cy-18, 13, 5, M_PI, 2.0*M_PI, LOCK_COL, W, H);
        draw_circle(px, stride, cx, ly+10, 4, BG_COL, W, H);
        fill_rect(px, stride, cx-2, ly+10, LOCK_ICON_BAR_WIDTH, LOCK_ICON_BAR_HEIGHT, BG_COL);
    }
    wl_surface_attach(out->wl_surface, out->buf[idx], 0, 0);
    wl_surface_damage_buffer(out->wl_surface, 0, 0, W, H);
    wl_surface_commit(out->wl_surface);
    out->buf_idx ^= 1;
}

static int create_shm_file(size_t size){
    char name[64];
    snprintf(name, sizeof(name), "/ABlock-shm-%d", getpid());
    int fd = shm_open(name, O_RDWR|O_CREAT|O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

static bool output_alloc_buffers(Output *out)
{
    size_t stride = out->width * 4;
    size_t buf_size = stride * out->height;
    size_t total = buf_size * 2;
    out->shm_fd = create_shm_file(total);
    if (out->shm_fd < 0) return false;
    void *data = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, out->shm_fd, 0);
    if (data == MAP_FAILED) { close(out->shm_fd); out->shm_fd = -1; return false; }
    out->buf_size = buf_size;
    out->data[0] = (uint32_t *)data;
    out->data[1] = (uint32_t *)((uint8_t *)data + buf_size);
    out->buf_idx = 0;
    out->pool = wl_shm_create_pool(out->wl->shm, out->shm_fd, (int32_t)total);
    if (!out->pool) { munmap(data, total); close(out->shm_fd); out->shm_fd = -1; return false; }
    out->buf[0] = wl_shm_pool_create_buffer(out->pool, 0,
                  (int32_t)out->width, (int32_t)out->height,
                  (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    out->buf[1] = wl_shm_pool_create_buffer(out->pool, (int32_t)buf_size,
                  (int32_t)out->width, (int32_t)out->height,
                  (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    if (!out->buf[0] || !out->buf[1]){
        if (out->buf[0]) wl_buffer_destroy(out->buf[0]);
        if (out->buf[1]) wl_buffer_destroy(out->buf[1]);
        wl_shm_pool_destroy(out->pool);
        munmap(data, total);
        close(out->shm_fd);
        out->shm_fd = -1;
        return false;
    }
    return true;
}

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *surf,
    uint32_t serial, uint32_t width, uint32_t height){
    Output *out = data;
    bool first = !out->configured;
    if (out->width != width || out->height != height){
        if (out->buf[0]) { wl_buffer_destroy(out->buf[0]); out->buf[0] = NULL; }
        if (out->buf[1]) { wl_buffer_destroy(out->buf[1]); out->buf[1] = NULL; }
        if (out->pool)   { wl_shm_pool_destroy(out->pool); out->pool   = NULL; }
        if (out->data[0]) { munmap(out->data[0], out->buf_size*2); out->data[0] = out->data[1] = NULL; }
        if (out->shm_fd >= 0) { close(out->shm_fd); out->shm_fd = -1; }
        out->width = width;
        out->height = height;
        out->buffers_ok = output_alloc_buffers(out);
    }
    out->configured = true;
    ext_session_lock_surface_v1_ack_configure(surf, serial);
    if (first && out->buffers_ok) render_frame(out);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = lock_surface_configure,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *lock){
    (void)lock;
    ABlock *wl = data;
    wl->locked = true;
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock){
    (void)lock;
    ABlock *wl = data;
    wl->running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked   = lock_locked,
    .finished = lock_finished,
};

static void output_geometry(void *d, struct wl_output *o,
    int32_t x, int32_t y, int32_t pw, int32_t ph,
    int32_t subpixel, const char *make, const char *model, int32_t transform)
{ (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)subpixel;(void)make;(void)model;(void)transform; }
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{ (void)d;(void)o;(void)flags;(void)w;(void)h;(void)refresh; }
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t factor) { (void)d;(void)o;(void)factor; }
static void output_name(void *d, struct wl_output *o, const char *name) { (void)d;(void)o;(void)name; }
static void output_description(void *d, struct wl_output *o, const char *desc) { (void)d;(void)o;(void)desc; }

static const struct wl_output_listener wl_output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

static void kb_keymap(void *data, struct wl_keyboard *kb,
                      uint32_t fmt, int32_t fd, uint32_t size)
{
    ABlock *wl = data;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) { close(fd); return; }
    if (wl->xkb_state) { xkb_state_unref(wl->xkb_state); wl->xkb_state = NULL; }
    if (wl->xkb_map)   { xkb_keymap_unref(wl->xkb_map);  wl->xkb_map   = NULL; }
    wl->xkb_map = xkb_keymap_new_from_string(wl->xkb_ctx, map_str,
                  XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    close(fd);
    if (wl->xkb_map) wl->xkb_state = xkb_state_new(wl->xkb_map);
}

static void kb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surf, struct wl_array *keys)
{ (void)d;(void)kb;(void)serial;(void)surf;(void)keys; }
static void kb_leave(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surf)
{ (void)d;(void)kb;(void)serial;(void)surf; }

static void kb_key(void *data, struct wl_keyboard *kb,
                   uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state_val)
{
    (void)kb;(void)serial;(void)time;
    ABlock *wl = data;
    if (state_val != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!wl->xkb_state) return;
    xkb_keycode_t kc  = key + 8;
    xkb_keysym_t  sym = xkb_state_key_get_one_sym(wl->xkb_state, kc);
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (wl->locked_out){
            if (time(NULL) < wl->lockout_until) return;
            wl->locked_out = false;
        }
        wl->pwd[wl->pwd_len] = '\0';
        if (verify_password(wl->pwd)){
            wl->fail_count = 0;
            wl->unlocked = true;
            wl->running  = false;
        } 
        else{
            wl->fail_count++;
            int delay = 1;
            for (int i = 1; i < wl->fail_count; i++) delay *= 2; if (delay > MAX_LOCKOUT_SECS) delay = MAX_LOCKOUT_SECS;
            if (delay > MAX_LOCKOUT_SECS) delay = MAX_LOCKOUT_SECS;
            wl->lockout_until = time(NULL) + delay;
            wl->locked_out = true;
            wl->error_flash = ERROR_FLASH_DURATION;
            syslog(LOG_AUTH|LOG_WARNING, "ABlock: failed attempt %d", wl->fail_count);
            explicit_bzero(wl->pwd, sizeof(wl->pwd));
            wl->pwd_len = 0;
        }
        return;
    }
    if (wl->locked_out){
        if (time(NULL) < wl->lockout_until) return;
        wl->locked_out = false;
    }
    if (sym == XKB_KEY_BackSpace){
        if (wl->pwd_len > 0) wl->pwd[--wl->pwd_len] = '\0';
        return;
    }
    if (sym == XKB_KEY_Escape){
        explicit_bzero(wl->pwd, sizeof(wl->pwd));
        wl->pwd_len = 0;
        return;
    }
    char buf[8] = {0};
    int n = xkb_state_key_get_utf8(wl->xkb_state, kc, buf, sizeof(buf));
    if (n > 0 && buf[0] >= 0x20 && wl->pwd_len + n < MAX_PWD){
        memcpy(wl->pwd + wl->pwd_len, buf, n);
        wl->pwd_len += n;
    }
}

static void kb_modifiers(void *data, struct wl_keyboard *kb,
                         uint32_t serial, uint32_t mods_dep, uint32_t mods_lat,
                         uint32_t mods_lock, uint32_t group){
    (void)kb;(void)serial;
    ABlock *wl = data;
    if (wl->xkb_state)
        xkb_state_update_mask(wl->xkb_state, mods_dep, mods_lat, mods_lock, 0, 0, group);
}

static void kb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{ (void)d;(void)kb;(void)rate;(void)delay; }

static const struct wl_keyboard_listener kb_listener = {
    .keymap      = kb_keymap,
    .enter       = kb_enter,
    .leave       = kb_leave,
    .key         = kb_key,
    .modifiers   = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    ABlock *wl = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->keyboard) {
        wl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl->keyboard, &kb_listener, wl);
    }
}

static void seat_name(void *d, struct wl_seat *seat, const char *name)
{ (void)d;(void)seat;(void)name; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

static void registry_global(void *data, struct wl_registry *reg,
                             uint32_t name, const char *iface, uint32_t ver){
    ABlock *wl = data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        wl->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        wl->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        wl->seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(wl->seat, &seat_listener, wl);
    } else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name)) {
        wl->lock_mgr = wl_registry_bind(reg, name,
                       &ext_session_lock_manager_v1_interface, (ver < 1 ? ver : 1));
    } else if (!strcmp(iface, wl_output_interface.name)) {
        Output *out = calloc(1, sizeof(*out));
        out->wl = wl;
        out->shm_fd = -1;
        out->wl_output = wl_registry_bind(reg, name, &wl_output_interface, (ver < 4 ? ver : 4));
        wl_output_add_listener(out->wl_output, &wl_output_listener, out);
        out->next = wl->outputs;
        wl->outputs = out;
        wl->noutputs++;
    }
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d;(void)r;(void)n; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static void sleep_ms(long ms){
    struct timespec ts = { ms/1000, (ms%1000)*1000000L };
    nanosleep(&ts, NULL);
}

int main(void){
    ABlock wl;
    memset(&wl, 0, sizeof(wl));
    wl.running = true;
    srand((unsigned)time(NULL));
    for (int i = 0; i < NUM_DOTS; i++)
        wl.dot_phase[i] = ((double)rand() / RAND_MAX) * 2.0*M_PI;
    mlock(wl.pwd, sizeof(wl.pwd));
    openlog("ABlock", LOG_PID, LOG_AUTH);
    wl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!wl.xkb_ctx) { fprintf(stderr, "ABlock: xkb_context failed\n"); return 1; }
    wl.display = wl_display_connect(NULL);
    if (!wl.display) { fprintf(stderr, "ABlock: cannot connect to Wayland display\n"); return 1; }
    wl.registry = wl_display_get_registry(wl.display);
    wl_registry_add_listener(wl.registry, &registry_listener, &wl);
    wl_display_roundtrip(wl.display);
    wl_display_roundtrip(wl.display);
    if (!wl.compositor || !wl.shm || !wl.lock_mgr) {
        fprintf(stderr,
            "ABlock: missing required globals. compositor=%p shm=%p lock_mgr=%p\n"
            "  Compositor must support ext-session-lock-v1\n",
            wl.compositor, wl.shm, wl.lock_mgr);
        return 1;
    }
    wl.lock = ext_session_lock_manager_v1_lock(wl.lock_mgr);
    ext_session_lock_v1_add_listener(wl.lock, &lock_listener, &wl);
    for (Output *out = wl.outputs; out; out = out->next) {
        out->wl_surface = wl_compositor_create_surface(wl.compositor);
        out->lock_surf = ext_session_lock_v1_get_lock_surface(
                         wl.lock, out->wl_surface, out->wl_output);
        ext_session_lock_surface_v1_add_listener(out->lock_surf, &lock_surface_listener, out);
    }
    wl_display_roundtrip(wl.display);
    while (!wl.locked && wl.running)
        wl_display_dispatch(wl.display);
    long frame_ms = 1000/FPS;
    while (wl.running){
        while (wl_display_prepare_read(wl.display) != 0)
            wl_display_dispatch_pending(wl.display);
        wl_display_flush(wl.display);
        wl_display_cancel_read(wl.display);
        wl_display_dispatch_pending(wl.display);
        if (wl.locked_out && time(NULL) >= wl.lockout_until)
            wl.locked_out = false;
        wl.phase += PULSE_SPEED;
        if (wl.error_flash > 0) wl.error_flash--;
        for (Output *out = wl.outputs; out; out = out->next)
            if (out->configured && out->buffers_ok)
                render_frame(out);
        wl_display_flush(wl.display);
        sleep_ms(frame_ms);
    }
    if (wl.unlocked)
        ext_session_lock_v1_unlock_and_destroy(wl.lock);
    else
        ext_session_lock_v1_destroy(wl.lock);
    explicit_bzero(wl.pwd, sizeof(wl.pwd));
    closelog();
    for (Output *out = wl.outputs; out;){
        if (out->buf[0]) wl_buffer_destroy(out->buf[0]);
        if (out->buf[1]) wl_buffer_destroy(out->buf[1]);
        if (out->pool)   wl_shm_pool_destroy(out->pool);
        if (out->data[0]) munmap(out->data[0], out->buf_size*2);
        if (out->shm_fd >= 0) close(out->shm_fd);
        if (out->lock_surf)  ext_session_lock_surface_v1_destroy(out->lock_surf);
        if (out->wl_surface) wl_surface_destroy(out->wl_surface);
        if (out->wl_output)  wl_output_release(out->wl_output);
        Output *next = out->next;
        free(out);
        out = next;
    }
    if (wl.keyboard) wl_keyboard_release(wl.keyboard);
    if (wl.seat) wl_seat_release(wl.seat);
    if (wl.lock_mgr) ext_session_lock_manager_v1_destroy(wl.lock_mgr);
    if (wl.shm) wl_shm_destroy(wl.shm);
    if (wl.compositor) wl_compositor_destroy(wl.compositor);
    if (wl.registry) wl_registry_destroy(wl.registry);
    if (wl.xkb_state) xkb_state_unref(wl.xkb_state);
    if (wl.xkb_map) xkb_keymap_unref(wl.xkb_map);
    if (wl.xkb_ctx) xkb_context_unref(wl.xkb_ctx);
    wl_display_disconnect(wl.display);
    return wl.unlocked ? 0 : 1;
}
