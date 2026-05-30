// TubeStatus — London Underground status board for ESP32-4848S040 (480×480 LVGL)
// Network: TfL Unified API + NTP clock, WiFi credentials persisted in NVS

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ESP32_4848S040.h>
#include <lvgl.h>
#include "touch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ── Hardware ────────────────────────────────────────────────────────────────
#define GFX_BL         38
#define TFT_HOR_RES    480
#define TFT_VER_RES    480
#define TFT_ROTATION   LV_DISPLAY_ROTATION_90
#define TFT_BRIGHTNESS 255
#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

static Arduino_ESP32SPI   *bus;
static Arduino_RGB_Display *gfx;
static uint8_t *draw_buf = nullptr;  // PSRAM-allocated in setup()

// ── Line data ────────────────────────────────────────────────────────────────
#define NUM_LINES      20
#define LINES_PER_PAGE 12  // 4 cols × 3 rows

typedef enum { STATUS_GOOD = 0, STATUS_MINOR, STATUS_SEVERE } StatusLevel;

struct LineInfo {
    const char *id;
    const char *name;
    const char *short_name;
    uint32_t    color;
    bool        text_dark;
};

struct StatusInfo {
    StatusLevel level;
    char        headline[80];
    char        detail[320];
};

struct StatusTone {
    uint32_t bg, fg, dot;
};

static const LineInfo LINES[NUM_LINES] = {
    { "bakerloo",        "Bakerloo",           nullptr,   0xB36305, false },
    { "central",         "Central",            nullptr,   0xE32017, false },
    { "circle",          "Circle",             nullptr,   0xFFD300, true  },
    { "district",        "District",           nullptr,   0x00782A, false },
    { "elizabeth",       "Elizabeth",          nullptr,   0x6950A1, false },
    { "hammersmith-city","Hammersmith & City", "H&C",     0xF3A9BB, true  },
    { "jubilee",         "Jubilee",            nullptr,   0xA0A5A9, true  },
    { "metropolitan",    "Metropolitan",       "Met",     0x9B0056, false },
    { "northern",        "Northern",           nullptr,   0x525860, false },
    { "piccadilly",      "Piccadilly",         nullptr,   0x003688, false },
    { "victoria",        "Victoria",           nullptr,   0x0098D4, false },
    { "waterloo-city",   "Waterloo & City",    "W&C",     0x95CDBA, true  },
    { "dlr",             "DLR",                nullptr,   0x009BBB, false },
    { "lioness",         "Lioness",            nullptr,   0xF882B8, true  },
    { "mildmay",         "Mildmay",            nullptr,   0x0098A4, false },
    { "windrush",        "Windrush",           nullptr,   0xDB261B, false },
    { "weaver",          "Weaver",             nullptr,   0x92298D, false },
    { "suffragette",     "Suffragette",        "Suffrgt", 0x507326, false },
    { "liberty",         "Liberty",            nullptr,   0x751056, false },
    { "tram",            "Tram",               nullptr,   0x84B817, true  },
};

// PSRAM-allocated in setup(); populated by fetch_tfl_status()
static StatusInfo *line_statuses = nullptr;

static const StatusTone STATUS_TONES[3] = {
    { 0x1A1F26, 0xE6E8EB, 0x39B57C }, // good
    { 0x5A3D14, 0xFFE3B0, 0xF2A93B }, // minor
    { 0x6B1721, 0xFFD9DD, 0xFF5168 }, // severe
};

// ── App state ─────────────────────────────────────────────────────────────
static bool     line_enabled[NUM_LINES];
static char     disp_names[NUM_LINES][20];
static uint32_t g_last_fetch_ms   = 0;
static bool     g_ever_fetched    = false;
static char     g_last_fetch_time[6] = "--:--";

// ── Screen/navigation state ───────────────────────────────────────────────
typedef enum { SCREEN_HOME, SCREEN_DETAIL, SCREEN_SETTINGS, SCREEN_SETTINGS_LINES, SCREEN_WIFI_CONFIG } ScreenType;

static ScreenType g_current_screen  = SCREEN_HOME;

// ── Pagination state ──────────────────────────────────────────────────────
static int        g_home_page       = 0;
static int        g_home_page_count = 1;
static int16_t    g_swipe_start_x   = 0;
static bool       g_was_swipe       = false;
static lv_obj_t  *g_tile_cont       = nullptr;
static int16_t    g_cont_start_x    = 0;
static lv_obj_t  *g_page_dots[8]    = {};

// ── UI refs ────────────────────────────────────────────────────────────────
static lv_obj_t *g_time_lbl               = nullptr;
static lv_obj_t *g_wifi_icon_cont         = nullptr;
static lv_obj_t *g_wifi_arcs[3]           = {};
static lv_obj_t *g_toggle_track[NUM_LINES] = {};
static lv_obj_t *g_toggle_knob[NUM_LINES]  = {};

// WiFi config state
static char      g_wifi_ssid[64]     = "";
static char      g_wifi_password[64] = "";
static lv_obj_t *g_wifi_keyboard     = nullptr;
static lv_obj_t *g_ssid_ta           = nullptr;
static lv_obj_t *g_pwd_ta            = nullptr;
static lv_obj_t *g_wifi_status_lbl   = nullptr;
static bool      g_wifi_was_connected = false;

// ── Forward declarations ────────────────────────────────────────────────
static void navigate_to(ScreenType type, int line_idx = 0);
static void fetch_tfl_status();

// ── Helpers ────────────────────────────────────────────────────────────────
static inline lv_color_t C(uint32_t rgb) { return lv_color_hex(rgb); }

static const lv_font_t *badge_font(int name_len, bool large) {
    if (large) {
        if (name_len <= 7) return &lv_font_montserrat_22;
        if (name_len <= 9) return &lv_font_montserrat_20;
        return &lv_font_montserrat_18;
    }
    if (name_len <= 7) return &lv_font_montserrat_14;
    if (name_len <= 9) return &lv_font_montserrat_12;
    return &lv_font_montserrat_10;
}

// ── NTP / Clock ───────────────────────────────────────────────────────────
static void configure_ntp() {
    // Europe/London: GMT winter, BST (UTC+1) last Sun Mar → last Sun Oct
    configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.google.com");
    Serial.println("[NTP] Sync requested");
}

static String clock_str() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
        return String(buf);
    }
    // NTP not yet synced — count up from 12:00 as a fallback
    int total_m = 720 + (int)(millis() / 60000);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", (total_m / 60) % 24, total_m % 60);
    return String(buf);
}

static void updated_str(char *buf, size_t n) {
    if (!g_ever_fetched) {
        snprintf(buf, n, "Updating...");
        return;
    }
    snprintf(buf, n, "Updated %s", g_last_fetch_time);
}

// ── WiFi icon helpers ─────────────────────────────────────────────────────
static int wifi_bars() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    int rssi = WiFi.RSSI();
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    return 1;
}

static void update_wifi_icon() {
    if (!g_wifi_icon_cont) return;
    int bars = wifi_bars();
    for (int i = 0; i < 3; i++) {
        if (!g_wifi_arcs[i]) continue;
        uint32_t clr = (bars == 0) ? 0x5A2828        // disconnected: dim red
                     : (i < bars)  ? 0xE6E8EB         // active bar: bright
                                   : 0x3A4048;         // inactive bar: dim
        lv_obj_set_style_arc_color(g_wifi_arcs[i], C(clr), LV_PART_INDICATOR);
    }
}

// Three curved arcs (r=5,9,13) fan upward from a common origin at container bottom.
// Arc angles 225°→315° (90° sweep through 270°=top) match the standard WiFi roundel shape.
static void create_wifi_icon(lv_obj_t *parent) {
    g_wifi_icon_cont = nullptr;
    memset(g_wifi_arcs, 0, sizeof(g_wifi_arcs));

    // 26×24 container; arc origin at (13,22). Arcs sweep 225°→315° so all pixels
    // land above y=22 — no overflow, no clipping flag needed.
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 26, 24);
    lv_obj_align(cont, LV_ALIGN_LEFT_MID, 55, -4);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_remove_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Origin dot
    lv_obj_t *dot = lv_obj_create(cont);
    lv_obj_set_size(dot, 4, 4);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, C(0xE6E8EB), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_pos(dot, 11, 18);   // center at (13,20)
    lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    static const int radii[3] = { 5, 9, 13 };
    for (int i = 0; i < 3; i++) {
        int r = radii[i];
        lv_obj_t *arc = lv_arc_create(cont);
        lv_obj_set_size(arc, r * 2, r * 2);
        lv_obj_set_pos(arc, 13 - r, 22 - r);   // center arc widget at (13,22)

        lv_arc_set_angles(arc, 225, 315);

        // Hide background ring and knob
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        // Indicator style
        lv_obj_set_style_arc_width(arc, 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, C(0xE6E8EB), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
        // Widget background transparent
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(arc, 0, 0);
        lv_obj_set_style_pad_all(arc, 0, 0);
        lv_obj_remove_flag(arc, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        g_wifi_arcs[i] = arc;
    }

    g_wifi_icon_cont = cont;
    update_wifi_icon();
}

static void style_screen(lv_obj_t *s) {
    lv_obj_set_size(s, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_style_bg_color(s, C(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_remove_flag(s, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                          LV_OBJ_FLAG_SCROLL_ELASTIC |
                                          LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                          LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                                          LV_OBJ_FLAG_SCROLL_CHAIN_VER));
}

static void no_decor(lv_obj_t *o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, (lv_obj_flag_t)((lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE)));
}

// ── Badge widget ─────────────────────────────────────────────────────────
static lv_obj_t *create_badge(lv_obj_t *parent, int idx, int size) {
    const LineInfo &L   = LINES[idx];
    lv_color_t clr      = C(L.color);
    lv_color_t text_clr = L.text_dark ? C(0x0E0F11) : C(0xFFFFFF);

    int stroke   = size * 17 / 100;
    int bar_h    = size * 30 / 100;
    int overhang = size * 11 / 100;
    int cont_w   = size + 2 * overhang;

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, cont_w, size);
    no_decor(cont);

    lv_obj_t *ring = lv_obj_create(cont);
    lv_obj_set_size(ring, size, size);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, clr, 0);
    lv_obj_set_style_border_width(ring, stroke, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_remove_flag(ring, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *bar = lv_obj_create(cont);
    lv_obj_set_size(bar, cont_w, bar_h);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(bar, clr, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, disp_names[idx]);
    lv_obj_set_style_text_color(lbl, text_clr, 0);
    lv_obj_set_style_text_font(lbl, badge_font((int)strlen(disp_names[idx]), size >= 100), 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    return cont;
}

// ── Shared header builder ─────────────────────────────────────────────────
static lv_obj_t *build_header(lv_obj_t *scr) {
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, TFT_HOR_RES, 56);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, C(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, C(0x1A1F26), 0);
    lv_obj_set_style_border_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_style_pad_ver(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    return hdr;
}

static lv_obj_t *build_back_btn(lv_obj_t *hdr) {
    lv_obj_t *btn = lv_button_create(hdr);
    lv_obj_set_size(btn, 32, 32);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn, C(0x1A1F26), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 24);
    lv_obj_t *ico = lv_label_create(btn);
    lv_label_set_text(ico, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(ico, C(0xE6E8EB), 0);
    lv_obj_align(ico, LV_ALIGN_CENTER, 0, 0);
    return btn;
}

// ── Home screen ────────────────────────────────────────────────────────────
static void tile_cb(lv_event_t *e) {
    if (g_was_swipe) return;
    navigate_to(SCREEN_DETAIL, (int)(uintptr_t)lv_event_get_user_data(e));
}

static void home_swipe_pressed_cb(lv_event_t *) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    g_swipe_start_x = p.x;
    g_was_swipe = false;
    if (g_tile_cont) {
        lv_anim_delete(g_tile_cont, nullptr);
        g_cont_start_x = (int16_t)lv_obj_get_x(g_tile_cont);
    }
}

static void home_swipe_pressing_cb(lv_event_t *) {
    if (!g_tile_cont) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int dx   = (int)p.x - g_swipe_start_x;
    int nx   = (int)g_cont_start_x + dx;
    int minx = -(g_home_page_count - 1) * TFT_HOR_RES;
    if (nx > 0)    nx = nx / 3;
    if (nx < minx) nx = minx + (nx - minx) / 3;
    lv_obj_set_x(g_tile_cont, nx);
    if (dx > 10 || dx < -10) g_was_swipe = true;
}

static void home_swipe_released_cb(lv_event_t *) {
    if (!g_tile_cont) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int dx = (int)p.x - g_swipe_start_x;
    if (dx < -40 && g_home_page < g_home_page_count - 1) {
        g_home_page++;
        g_was_swipe = true;
    } else if (dx > 40 && g_home_page > 0) {
        g_home_page--;
        g_was_swipe = true;
    }
    for (int i = 0; i < g_home_page_count && i < 8; i++) {
        if (g_page_dots[i])
            lv_obj_set_style_bg_color(g_page_dots[i],
                i == g_home_page ? C(0xE6E8EB) : C(0x3A4048), 0);
    }
    int target_x = -g_home_page * TFT_HOR_RES;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_tile_cont);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_x((lv_obj_t *)obj, v);
    });
    lv_anim_set_values(&a, lv_obj_get_x(g_tile_cont), target_x);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void build_home(lv_obj_t *scr) {
    g_time_lbl = nullptr;

    int all_sorted[NUM_LINES], total = 0;
    for (int pass = 2; pass >= 0; pass--) {
        for (int i = 0; i < NUM_LINES; i++) {
            if (line_enabled[i] && (int)line_statuses[i].level == pass)
                all_sorted[total++] = i;
        }
    }

    g_home_page_count = (total + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
    if (g_home_page_count < 1) g_home_page_count = 1;
    if (g_home_page >= g_home_page_count) g_home_page = g_home_page_count - 1;

    lv_obj_t *hdr = build_header(scr);

    g_time_lbl = lv_label_create(hdr);
    lv_label_set_text(g_time_lbl, clock_str().c_str());
    lv_obj_set_style_text_color(g_time_lbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(g_time_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(g_time_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    create_wifi_icon(hdr);

    char upd[28];
    updated_str(upd, sizeof(upd));
    lv_obj_t *upd_lbl = lv_label_create(hdr);
    lv_label_set_text(upd_lbl, upd);
    lv_obj_set_style_text_color(upd_lbl, C(0x6E7681), 0);
    lv_obj_set_style_text_font(upd_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(upd_lbl, LV_ALIGN_RIGHT_MID, -42, 0);

    lv_obj_t *sbtn = lv_button_create(hdr);
    lv_obj_set_size(sbtn, 32, 32);
    lv_obj_align(sbtn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sbtn, C(0x1A1F26), 0);
    lv_obj_set_style_bg_opa(sbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sbtn, 0, 0);
    lv_obj_set_style_radius(sbtn, 8, 0);
    lv_obj_set_style_pad_all(sbtn, 0, 0);
    lv_obj_add_event_cb(sbtn, [](lv_event_t *) { navigate_to(SCREEN_SETTINGS); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sico = lv_label_create(sbtn);
    lv_label_set_text(sico, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(sico, C(0x9AA3AD), 0);
    lv_obj_align(sico, LV_ALIGN_CENTER, 0, 0);

    if (total == 0) {
        lv_obj_t *empty = lv_label_create(scr);
        lv_label_set_text(empty, "No lines selected.\nTap " LV_SYMBOL_SETTINGS " to configure.");
        lv_obj_set_style_text_color(empty, C(0x6E7681), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    static const int COLS = 4, ROWS = 3;
    static const int BADGE_SIZE = 70;
    int pad    = 12, gap = 8;
    int grid_w = TFT_HOR_RES - 2 * pad;
    int grid_h = TFT_VER_RES - 56 - 2 * pad - 24;
    int tile_w = (grid_w - (COLS - 1) * gap) / COLS;
    int tile_h = (grid_h - (ROWS - 1) * gap) / ROWS;
    int badge_cont_w = BADGE_SIZE + 2 * (BADGE_SIZE * 11 / 100);

    g_tile_cont = lv_obj_create(scr);
    lv_obj_set_size(g_tile_cont, g_home_page_count * TFT_HOR_RES, TFT_VER_RES - 56);
    lv_obj_set_pos(g_tile_cont, -g_home_page * TFT_HOR_RES, 56);
    no_decor(g_tile_cont);

    for (int pg = 0; pg < g_home_page_count; pg++) {
        int ps = pg * LINES_PER_PAGE;
        int pe = ps + LINES_PER_PAGE;
        if (pe > total) pe = total;
        for (int t = 0; t < pe - ps; t++) {
            int idx                = all_sorted[ps + t];
            const StatusInfo &st   = line_statuses[idx];
            const StatusTone &tone = STATUS_TONES[(int)st.level];
            int col = t % COLS;
            int row = t / COLS;
            int tx  = pg * TFT_HOR_RES + pad + col * (tile_w + gap);
            int ty  = pad + row * (tile_h + gap);

            lv_obj_t *tile = lv_button_create(g_tile_cont);
            lv_obj_set_size(tile, tile_w, tile_h);
            lv_obj_set_pos(tile, tx, ty);
            lv_obj_set_style_bg_color(tile, C(tone.bg), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_radius(tile, 10, 0);
            lv_obj_set_style_pad_all(tile, 0, 0);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(tile, home_swipe_pressed_cb,  LV_EVENT_PRESSED,  nullptr);
            lv_obj_add_event_cb(tile, home_swipe_pressing_cb, LV_EVENT_PRESSING, nullptr);
            lv_obj_add_event_cb(tile, home_swipe_released_cb, LV_EVENT_RELEASED, nullptr);
            lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

            bool has_status_text = (st.level != STATUS_GOOD);
            int text_reserve = has_status_text ? 26 : 14;
            int badge_top    = (tile_h - text_reserve) / 2 - BADGE_SIZE / 2;
            if (badge_top < 6) badge_top = 6;

            lv_obj_t *badge = create_badge(tile, idx, BADGE_SIZE);
            lv_obj_set_pos(badge, (tile_w - badge_cont_w) / 2, badge_top);

            if (has_status_text) {
                lv_obj_t *stlbl = lv_label_create(tile);
                lv_label_set_text(stlbl, st.headline);
                lv_obj_set_style_text_color(stlbl, C(tone.fg), 0);
                lv_obj_set_style_text_font(stlbl, &lv_font_montserrat_10, 0);
                lv_obj_align(stlbl, LV_ALIGN_BOTTOM_MID, 0, -8);
            }
        }
    }

    lv_obj_add_event_cb(scr, home_swipe_pressed_cb,  LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(scr, home_swipe_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(scr, home_swipe_released_cb, LV_EVENT_RELEASED, nullptr);

    memset(g_page_dots, 0, sizeof(g_page_dots));
    if (g_home_page_count > 1) {
        static const int DOT_D = 7, DOT_GAP = 6;
        int dots_w = g_home_page_count * DOT_D + (g_home_page_count - 1) * DOT_GAP;
        int dot_x0 = (TFT_HOR_RES - dots_w) / 2;
        int dot_y  = TFT_VER_RES - 14;
        for (int p = 0; p < g_home_page_count && p < 8; p++) {
            lv_obj_t *dot = lv_obj_create(scr);
            lv_obj_set_size(dot, DOT_D, DOT_D);
            lv_obj_set_pos(dot, dot_x0 + p * (DOT_D + DOT_GAP), dot_y);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, p == g_home_page ? C(0xE6E8EB) : C(0x3A4048), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            g_page_dots[p] = dot;
        }
    }
}

// ── Detail screen ─────────────────────────────────────────────────────────
static void back_to_home(lv_event_t *)     { navigate_to(SCREEN_HOME); }
static void back_to_settings(lv_event_t *) { navigate_to(SCREEN_SETTINGS); }

static void build_detail(lv_obj_t *scr, int idx) {
    const LineInfo   &L    = LINES[idx];
    const StatusInfo &st   = line_statuses[idx];
    const StatusTone &tone = STATUS_TONES[(int)st.level];

    lv_obj_t *hdr  = build_header(scr);
    lv_obj_t *bbtn = build_back_btn(hdr);
    lv_obj_add_event_cb(bbtn, back_to_home, LV_EVENT_CLICKED, nullptr);

    char title[64];
    snprintf(title, sizeof(title), "%s line", L.name);
    lv_obj_t *tlbl = lv_label_create(hdr);
    lv_label_set_text(tlbl, title);
    lv_obj_set_style_text_color(tlbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_16, 0);
    lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 44, 0);

    static const int BADGE_SIZE = 140;
    int badge_cont_w = BADGE_SIZE + 2 * (BADGE_SIZE * 11 / 100);
    int content_y    = 76;

    lv_obj_t *badge = create_badge(scr, idx, BADGE_SIZE);
    lv_obj_set_pos(badge, (TFT_HOR_RES - badge_cont_w) / 2, content_y);
    content_y += BADGE_SIZE + 18;

    lv_obj_t *pill = lv_obj_create(scr);
    lv_obj_set_height(pill, 30);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, C(tone.bg), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_radius(pill, 15, 0);
    lv_obj_set_style_pad_hor(pill, 14, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_style_pad_column(pill, 8, 0);
    lv_obj_remove_flag(pill, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dot = lv_obj_create(pill);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, C(tone.dot), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *plbl = lv_label_create(pill);
    lv_label_set_text(plbl, st.headline);
    lv_obj_set_style_text_color(plbl, C(tone.fg), 0);
    lv_obj_set_style_text_font(plbl, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(plbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(pill);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, content_y);
    content_y += 42;

    lv_obj_t *detail = lv_label_create(scr);
    lv_label_set_text(detail, st.detail);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail, 440);
    lv_obj_set_style_text_color(detail, C(0xC6CBD2), 0);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, content_y);

    char upd[32];
    updated_str(upd, sizeof(upd));
    lv_obj_t *ulbl = lv_label_create(scr);
    lv_label_set_text(ulbl, upd);
    lv_obj_set_style_text_color(ulbl, C(0x6E7681), 0);
    lv_obj_set_style_text_font(ulbl, &lv_font_montserrat_10, 0);
    lv_obj_align(ulbl, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// ── Settings screen ───────────────────────────────────────────────────────
static void save_line_prefs();  // forward declaration

static void refresh_toggle(int i) {
    bool on = line_enabled[i];
    if (g_toggle_track[i])
        lv_obj_set_style_bg_color(g_toggle_track[i], on ? C(0x39B57C) : C(0x2A3038), 0);
    if (g_toggle_knob[i])
        lv_obj_align(g_toggle_knob[i], LV_ALIGN_LEFT_MID, on ? 16 : 2, 0);
}

static void settings_row_cb(lv_event_t *e) {
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    line_enabled[i] = !line_enabled[i];
    refresh_toggle(i);
    save_line_prefs();
}

static void settings_all_cb(lv_event_t *) {
    for (int i = 0; i < NUM_LINES; i++) { line_enabled[i] = true;  refresh_toggle(i); }
    save_line_prefs();
}

static void settings_none_cb(lv_event_t *) {
    for (int i = 0; i < NUM_LINES; i++) { line_enabled[i] = false; refresh_toggle(i); }
    save_line_prefs();
}

static void build_settings_lines(lv_obj_t *scr) {
    for (int i = 0; i < NUM_LINES; i++) g_toggle_track[i] = g_toggle_knob[i] = nullptr;

    lv_obj_t *hdr  = build_header(scr);
    lv_obj_t *bbtn = build_back_btn(hdr);
    lv_obj_add_event_cb(bbtn, back_to_settings, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *tlbl = lv_label_create(hdr);
    lv_label_set_text(tlbl, "Lines");
    lv_obj_set_style_text_color(tlbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_16, 0);
    lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 44, 0);

    lv_obj_t *sub = lv_obj_create(scr);
    lv_obj_set_size(sub, TFT_HOR_RES, 40);
    lv_obj_set_pos(sub, 0, 56);
    lv_obj_set_style_bg_color(sub, C(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(sub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sub, 0, 0);
    lv_obj_set_style_pad_hor(sub, 12, 0);
    lv_obj_set_style_pad_ver(sub, 0, 0);
    lv_obj_remove_flag(sub, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *show_lbl = lv_label_create(sub);
    lv_label_set_text(show_lbl, "SHOW ON DASHBOARD");
    lv_obj_set_style_text_color(show_lbl, C(0x6E7681), 0);
    lv_obj_set_style_text_font(show_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(show_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    struct { const char *txt; lv_event_cb_t cb; int w; } chips[] = {
        { "NONE", settings_none_cb, 50 },
        { "ALL",  settings_all_cb,  38 },
    };
    int chip_x = -0;
    for (int c = 0; c < 2; c++) {
        lv_obj_t *chip = lv_button_create(sub);
        lv_obj_set_size(chip, chips[c].w, 24);
        lv_obj_align(chip, LV_ALIGN_RIGHT_MID, chip_x, 0);
        chip_x -= chips[c].w + 6;
        lv_obj_set_style_bg_color(chip, C(0x1A1F26), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_radius(chip, 7, 0);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_add_event_cb(chip, chips[c].cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *clbl = lv_label_create(chip);
        lv_label_set_text(clbl, chips[c].txt);
        lv_obj_set_style_text_color(clbl, C(0x9AA3AD), 0);
        lv_obj_set_style_text_font(clbl, &lv_font_montserrat_10, 0);
        lv_obj_align(clbl, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_pos(list, 0, 96);
    lv_obj_set_size(list, TFT_HOR_RES, TFT_VER_RES - 96);
    lv_obj_set_style_bg_color(list, C(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    static const int ROW_H = 54, ROW_GAP = 4;

    for (int i = 0; i < NUM_LINES; i++) {
        const LineInfo   &L    = LINES[i];
        const StatusInfo &st   = line_statuses[i];
        const StatusTone &tone = STATUS_TONES[(int)st.level];
        bool on = line_enabled[i];

        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, TFT_HOR_RES - 24, ROW_H);
        lv_obj_set_pos(row, 12, i * (ROW_H + ROW_GAP) + 4);
        lv_obj_set_style_bg_color(row, C(0x14181E), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, settings_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 28, 28);
        lv_obj_set_pos(dot, 12, (ROW_H - 28) / 2);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, C(L.color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        lv_obj_t *nlbl = lv_label_create(row);
        lv_label_set_text(nlbl, L.name);
        lv_obj_set_style_text_color(nlbl, C(0xE6E8EB), 0);
        lv_obj_set_style_text_font(nlbl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(nlbl, 52, 10);
        lv_obj_remove_flag(nlbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *slbl = lv_label_create(row);
        lv_label_set_text(slbl, st.headline);
        lv_obj_set_style_text_color(slbl, C(tone.dot), 0);
        lv_obj_set_style_text_font(slbl, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(slbl, 52, 30);
        lv_obj_remove_flag(slbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *track = lv_obj_create(row);
        lv_obj_set_size(track, 36, 22);
        lv_obj_set_pos(track, (TFT_HOR_RES - 24) - 36 - 12, (ROW_H - 22) / 2);
        lv_obj_set_style_radius(track, 11, 0);
        lv_obj_set_style_bg_color(track, on ? C(0x39B57C) : C(0x2A3038), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(track, 0, 0);
        lv_obj_set_style_pad_all(track, 0, 0);
        lv_obj_remove_flag(track, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        g_toggle_track[i] = track;

        lv_obj_t *knob = lv_obj_create(track);
        lv_obj_set_size(knob, 18, 18);
        lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(knob, C(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(knob, 0, 0);
        lv_obj_set_style_pad_all(knob, 0, 0);
        lv_obj_remove_flag(knob, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_align(knob, LV_ALIGN_LEFT_MID, on ? 16 : 2, 0);
        g_toggle_knob[i] = knob;
    }
}

// ── WiFi credential helpers ───────────────────────────────────────────────
static void save_line_prefs() {
    Preferences prefs;
    prefs.begin("lines", false);
    uint32_t mask = 0;
    for (int i = 0; i < NUM_LINES; i++)
        if (line_enabled[i]) mask |= (1u << i);
    prefs.putUInt("mask", mask);
    prefs.end();
}

static void load_line_prefs() {
    Preferences prefs;
    prefs.begin("lines", true);
    if (!prefs.isKey("mask")) { prefs.end(); return; }
    uint32_t mask = prefs.getUInt("mask", 0xFFFFFFFF);
    prefs.end();
    for (int i = 0; i < NUM_LINES; i++)
        line_enabled[i] = (mask >> i) & 1;
}

static void load_wifi_credentials() {
    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("password", "");
    prefs.end();
    strncpy(g_wifi_ssid,     ssid.c_str(), 63);
    strncpy(g_wifi_password, pass.c_str(), 63);
    g_wifi_ssid[63] = g_wifi_password[63] = '\0';
    Serial.printf("[WiFi] Loaded from NVS: ssid='%s' pass_len=%d\n", g_wifi_ssid, (int)strlen(g_wifi_password));
}

static const char *wifi_status_str(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:      return "IDLE";
        case WL_NO_SSID_AVAIL:    return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:   return "SCAN_COMPLETED";
        case WL_CONNECTED:        return "CONNECTED";
        case WL_CONNECT_FAILED:   return "CONNECT_FAILED";
        case WL_CONNECTION_LOST:  return "CONNECTION_LOST";
        case WL_DISCONNECTED:     return "DISCONNECTED";
        default:                  return "UNKNOWN";
    }
}

static void wifi_connect_and_save() {
    if (g_ssid_ta) strncpy(g_wifi_ssid,     lv_textarea_get_text(g_ssid_ta), 63);
    if (g_pwd_ta)  strncpy(g_wifi_password,  lv_textarea_get_text(g_pwd_ta),  63);
    g_wifi_ssid[63] = g_wifi_password[63] = '\0';

    Serial.printf("[WiFi] SSID='%s' pass_len=%d\n", g_wifi_ssid, (int)strlen(g_wifi_password));

    if (g_wifi_keyboard) lv_obj_add_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);

    if (strlen(g_wifi_ssid) == 0) {
        if (g_wifi_status_lbl) lv_label_set_text(g_wifi_status_lbl, "Enter an SSID first");
        return;
    }

    if (g_wifi_status_lbl) lv_label_set_text(g_wifi_status_lbl, "Connecting...");
    lv_timer_handler();

    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(g_wifi_ssid, g_wifi_password);

    uint32_t start = millis();
    wl_status_t last_status = WL_IDLE_STATUS;
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        wl_status_t cur = WiFi.status();
        if (cur != last_status) {
            Serial.printf("[WiFi] status: %s (+%lums)\n", wifi_status_str(cur), millis() - start);
            last_status = cur;
        }
        lv_timer_handler();
        delay(50);
    }

    wl_status_t final_status = WiFi.status();
    Serial.printf("[WiFi] done after %lums, status=%s\n", millis() - start, wifi_status_str(final_status));

    if (final_status == WL_CONNECTED) {
        Serial.printf("[WiFi] IP=%s\n", WiFi.localIP().toString().c_str());
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.putString("ssid",     g_wifi_ssid);
        prefs.putString("password", g_wifi_password);
        prefs.end();
        configure_ntp();
        if (g_wifi_status_lbl) lv_label_set_text(g_wifi_status_lbl, "Connected & saved");
    } else {
        WiFi.disconnect(false);
        if (g_wifi_status_lbl) lv_label_set_text(g_wifi_status_lbl, "Connection failed - check credentials");
    }
}

// ── WiFi config screen ────────────────────────────────────────────────────
static void build_wifi_config(lv_obj_t *scr) {
    g_wifi_keyboard   = nullptr;
    g_ssid_ta         = nullptr;
    g_pwd_ta          = nullptr;
    g_wifi_status_lbl = nullptr;

    lv_obj_t *hdr  = build_header(scr);
    lv_obj_t *bbtn = build_back_btn(hdr);
    lv_obj_add_event_cb(bbtn, back_to_settings, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *tlbl = lv_label_create(hdr);
    lv_label_set_text(tlbl, "Wi-Fi");
    lv_obj_set_style_text_color(tlbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_16, 0);
    lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 44, 0);

    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, "Network (SSID)");
    lv_obj_set_style_text_color(ssid_lbl, C(0x6E7681), 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(ssid_lbl, 20, 68);

    lv_obj_t *ssid_ta = lv_textarea_create(scr);
    lv_obj_set_pos(ssid_ta, 20, 86);
    lv_textarea_set_one_line(ssid_ta, true);
    lv_obj_set_size(ssid_ta, TFT_HOR_RES - 40, 48);
    lv_textarea_set_placeholder_text(ssid_ta, "Enter network name");
    lv_textarea_set_text(ssid_ta, g_wifi_ssid);
    lv_obj_set_style_bg_color(ssid_ta, C(0x14181E), 0);
    lv_obj_set_style_bg_opa(ssid_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ssid_ta, C(0x2A3038), 0);
    lv_obj_set_style_border_width(ssid_ta, 1, 0);
    lv_obj_set_style_border_color(ssid_ta, C(0x39B57C), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ssid_ta, 8, 0);
    lv_obj_set_style_text_color(ssid_ta, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(ssid_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_hor(ssid_ta, 12, 0);
    lv_obj_set_style_pad_ver(ssid_ta, 15, 0);

    lv_obj_t *pwd_lbl = lv_label_create(scr);
    lv_label_set_text(pwd_lbl, "Password");
    lv_obj_set_style_text_color(pwd_lbl, C(0x6E7681), 0);
    lv_obj_set_style_text_font(pwd_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(pwd_lbl, 20, 148);

    lv_obj_t *pwd_ta = lv_textarea_create(scr);
    lv_obj_set_pos(pwd_ta, 20, 166);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_size(pwd_ta, TFT_HOR_RES - 40, 48);
    lv_textarea_set_password_mode(pwd_ta, true);
    lv_textarea_set_placeholder_text(pwd_ta, "Enter password");
    lv_textarea_set_text(pwd_ta, g_wifi_password);
    lv_obj_set_style_bg_color(pwd_ta, C(0x14181E), 0);
    lv_obj_set_style_bg_opa(pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pwd_ta, C(0x2A3038), 0);
    lv_obj_set_style_border_width(pwd_ta, 1, 0);
    lv_obj_set_style_border_color(pwd_ta, C(0x39B57C), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(pwd_ta, 8, 0);
    lv_obj_set_style_text_color(pwd_ta, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(pwd_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_hor(pwd_ta, 12, 0);
    lv_obj_set_style_pad_ver(pwd_ta, 15, 0);

    lv_obj_t *save_btn = lv_button_create(scr);
    lv_obj_set_size(save_btn, TFT_HOR_RES - 40, 48);
    lv_obj_set_pos(save_btn, 20, 230);
    lv_obj_set_style_bg_color(save_btn, C(0x39B57C), 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 10, 0);
    lv_obj_set_style_pad_all(save_btn, 0, 0);
    lv_obj_remove_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Connect & Save");
    lv_obj_set_style_text_color(save_lbl, C(0xFFFFFF), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(save_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(save_btn, [](lv_event_t *) {
        wifi_connect_and_save();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *status_lbl = lv_label_create(scr);
    char init_status[80] = "";
    if (WiFi.status() == WL_CONNECTED)
        snprintf(init_status, sizeof(init_status), "Connected to: %s", WiFi.SSID().c_str());
    else if (strlen(g_wifi_ssid) > 0)
        snprintf(init_status, sizeof(init_status), "Saved: %s", g_wifi_ssid);
    lv_label_set_text(status_lbl, init_status);
    lv_obj_set_style_text_color(status_lbl, C(0x9AA3AD), 0);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(status_lbl, TFT_HOR_RES - 40);
    lv_obj_set_pos(status_lbl, 20, 288);
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    g_wifi_status_lbl = status_lbl;

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, TFT_HOR_RES, 220);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    g_wifi_keyboard = kb;
    g_ssid_ta       = ssid_ta;
    g_pwd_ta        = pwd_ta;

    lv_obj_add_event_cb(ssid_ta, [](lv_event_t *) {
        if (g_wifi_keyboard) {
            lv_keyboard_set_textarea(g_wifi_keyboard, g_ssid_ta);
            lv_obj_remove_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_FOCUSED, nullptr);

    lv_obj_add_event_cb(pwd_ta, [](lv_event_t *) {
        if (g_wifi_keyboard) {
            lv_keyboard_set_textarea(g_wifi_keyboard, g_pwd_ta);
            lv_obj_remove_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_FOCUSED, nullptr);

    lv_obj_add_event_cb(kb, [](lv_event_t *e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
            lv_obj_add_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_ALL, nullptr);
}

// ── Settings menu screen ──────────────────────────────────────────────────
static void build_settings(lv_obj_t *scr) {
    lv_obj_t *hdr  = build_header(scr);
    lv_obj_t *bbtn = build_back_btn(hdr);
    lv_obj_add_event_cb(bbtn, back_to_home, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *tlbl = lv_label_create(hdr);
    lv_label_set_text(tlbl, "Settings");
    lv_obj_set_style_text_color(tlbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_16, 0);
    lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 44, 0);

    struct MenuItem { const char *icon; const char *label; ScreenType target; };
    static const MenuItem items[] = {
        { LV_SYMBOL_LIST, "Lines", SCREEN_SETTINGS_LINES },
        { LV_SYMBOL_WIFI, "Wi-Fi", SCREEN_WIFI_CONFIG    },
    };

    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_button_create(scr);
        lv_obj_set_size(row, TFT_HOR_RES - 24, 64);
        lv_obj_set_pos(row, 12, 68 + i * 72);
        lv_obj_set_style_bg_color(row, C(0x14181E), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, [](lv_event_t *e) {
            navigate_to((ScreenType)(uintptr_t)lv_event_get_user_data(e));
        }, LV_EVENT_CLICKED, (void *)(uintptr_t)items[i].target);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, items[i].icon);
        lv_obj_set_style_text_color(icon, C(0x9AA3AD), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, items[i].label);
        lv_obj_set_style_text_color(lbl, C(0xE6E8EB), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 46, 0);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *chev = lv_label_create(row);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chev, C(0x3A4048), 0);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_16, 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_remove_flag(chev, LV_OBJ_FLAG_CLICKABLE);
    }
}

// ── Navigation ───────────────────────────────────────────────────────────
static void delete_scr_async(void *param) {
    lv_obj_delete((lv_obj_t *)param);
}

static void navigate_to(ScreenType type, int line_idx) {
    g_current_screen  = type;
    g_time_lbl        = nullptr;
    g_wifi_icon_cont  = nullptr;
    memset(g_wifi_arcs, 0, sizeof(g_wifi_arcs));
    g_tile_cont       = nullptr;
    g_wifi_keyboard   = nullptr;
    g_ssid_ta         = nullptr;
    g_pwd_ta          = nullptr;
    g_wifi_status_lbl = nullptr;
    if (type != SCREEN_HOME) g_home_page = 0;

    lv_obj_t *old_scr = lv_screen_active();

    lv_obj_t *new_scr = lv_obj_create(nullptr);
    style_screen(new_scr);

    switch (type) {
        case SCREEN_HOME:           build_home(new_scr);             break;
        case SCREEN_DETAIL:         build_detail(new_scr, line_idx); break;
        case SCREEN_SETTINGS:       build_settings(new_scr);         break;
        case SCREEN_SETTINGS_LINES: build_settings_lines(new_scr);   break;
        case SCREEN_WIFI_CONFIG:    build_wifi_config(new_scr);      break;
    }

    lv_scr_load(new_scr);
    if (old_scr) lv_async_call(delete_scr_async, old_scr);
}

// ── TfL API ───────────────────────────────────────────────────────────────
// GET https://api.tfl.gov.uk/Line/Mode/tube,dlr,elizabeth-line,tram,overground/Status
// severity >= 10 → GOOD, 7–9 → MINOR, ≤ 6 → SEVERE
static void fetch_tfl_status() {
    // Reconnect if needed
    if (WiFi.status() != WL_CONNECTED) {
        if (strlen(g_wifi_ssid) == 0) return;
        Serial.println("[TfL] WiFi down, reconnecting...");
        WiFi.begin(g_wifi_ssid, g_wifi_password);
        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
            lv_timer_handler();
            delay(50);
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[TfL] Reconnect failed");
            return;
        }
        configure_ntp();
    }

    Serial.printf("[TfL] Fetching... free heap: %lu PSRAM: %lu\n",
        (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

    // Allocate response buffer from PSRAM (128 KB — TfL full-status response is ~50–100 KB)
    static const int BUF_SIZE = 131072;
    char *buf = (char *)ps_malloc(BUF_SIZE);
    if (!buf) {
        Serial.println("[TfL] PSRAM alloc failed for response buffer");
        return;
    }

    Serial.println("[TfL] Creating WiFiClientSecure...");
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);  // seconds

    Serial.println("[TfL] Starting HTTP GET...");
    HTTPClient http;
    http.useHTTP10(true);  // HTTP/1.0: no chunked encoding, server closes connection when done
    http.begin(client, "https://api.tfl.gov.uk/Line/Mode/tube,dlr,elizabeth-line,tram,overground/Status");
    http.setTimeout(15000);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    Serial.printf("[TfL] HTTP response code: %d\n", code);
    if (code != 200) {
        Serial.printf("[TfL] Error body: %s\n", http.getString().c_str());
        http.end();
        free(buf);
        return;
    }

    int content_len = http.getSize();
    Serial.printf("[TfL] Content-Length: %d  connected: %d\n", content_len, (int)http.connected());

    // getStreamPtr() gives raw stream; safe to use with HTTP/1.0 (no chunk headers).
    // Loop exits on TCP close (!connected) or 3 s idle after receiving data.
    WiFiClient *stream = http.getStreamPtr();
    int total = 0;
    uint32_t last_data_ms = millis();
    uint32_t deadline     = millis() + 30000;
    while (total < BUF_SIZE - 1 && millis() < deadline) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes((uint8_t *)(buf + total),
                                      min(avail, BUF_SIZE - 1 - total));
            total += n;
            last_data_ms = millis();
        } else if (!http.connected()) {
            Serial.printf("[TfL] Connection closed after %d bytes\n", total);
            break;
        } else if (total > 0 && millis() - last_data_ms > 3000) {
            Serial.printf("[TfL] Idle 3s after %d bytes — treating as end\n", total);
            break;
        } else {
            delay(5);
        }
    }
    bool timed_out = (millis() >= deadline);
    buf[total] = '\0';
    http.end();
    client.stop();

    Serial.printf("[TfL] Read complete: %d bytes, timed_out=%d, free_heap=%lu\n",
        total, (int)timed_out, (unsigned long)ESP.getFreeHeap());
    if (total == 0) { free(buf); return; }

    // Print first 120 chars so we can confirm it's valid JSON
    Serial.printf("[TfL] Body start: %.120s\n", buf);

    Serial.println("[TfL] Parsing JSON...");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, total);
    free(buf);

    Serial.printf("[TfL] Parse result: %s\n", err.c_str());
    if (err) return;

    int matched = 0;
    for (JsonObject line_obj : doc.as<JsonArray>()) {
        const char *id = line_obj["id"];
        if (!id) continue;
        for (int i = 0; i < NUM_LINES; i++) {
            if (strcmp(LINES[i].id, id) != 0) continue;
            JsonArray statuses = line_obj["lineStatuses"].as<JsonArray>();
            if (statuses.isNull() || statuses.size() == 0) break;
            JsonObject s0      = statuses[0];
            int         sev    = s0["statusSeverity"]            | 10;
            const char *desc   = s0["statusSeverityDescription"] | "Good service";
            const char *reason = s0["reason"]                    | "";

            line_statuses[i].level = (sev >= 10) ? STATUS_GOOD
                                   : (sev >= 7)  ? STATUS_MINOR
                                                 : STATUS_SEVERE;
            strncpy(line_statuses[i].headline, desc, 79);
            line_statuses[i].headline[79] = '\0';
            const char *det = (strlen(reason) > 0) ? reason : desc;
            strncpy(line_statuses[i].detail, det, 319);
            line_statuses[i].detail[319] = '\0';
            Serial.printf("[TfL]   %s → sev=%d %s\n", id, sev, desc);
            matched++;
            break;
        }
    }

    g_last_fetch_ms = millis();
    g_ever_fetched  = true;
    strncpy(g_last_fetch_time, clock_str().c_str(), 5);
    g_last_fetch_time[5] = '\0';
    Serial.printf("[TfL] Done: %d lines matched. Free heap: %lu\n",
        matched, (unsigned long)ESP.getFreeHeap());

    if (g_current_screen == SCREEN_HOME) navigate_to(SCREEN_HOME);
}

// ── Fetch timer ───────────────────────────────────────────────────────────
static bool g_fetch_requested = false;

static void fetch_timer_cb(lv_timer_t *) {
    g_fetch_requested = true;
}

// ── LVGL platform callbacks ───────────────────────────────────────────────
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_disp_flush_ready(disp);
}

static void my_touchpad_read(lv_indev_t *, lv_indev_data_t *data) {
    if (touch_has_signal()) {
        if (touch_touched()) {
            data->state   = LV_INDEV_STATE_PRESSED;
            data->point.x = touch_last_x;
            data->point.y = touch_last_y;
        } else if (touch_released()) {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t my_tick() { return millis(); }

// ── Clock timer ───────────────────────────────────────────────────────────
static void clock_cb(lv_timer_t *) {
    if (g_time_lbl) lv_label_set_text(g_time_lbl, clock_str().c_str());
    update_wifi_icon();
}

// ── setup / loop ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Allocate draw buffer from PSRAM before WiFi/LVGL claim internal heap
    draw_buf = (uint8_t *)ps_malloc(DRAW_BUF_SIZE);
    if (!draw_buf) {
        Serial.println("[WARN] PSRAM alloc failed, falling back to DRAM for draw_buf");
        draw_buf = (uint8_t *)malloc(DRAW_BUF_SIZE);
    }

    // Allocate status array from PSRAM (20 × ~400 bytes = ~8 KB)
    line_statuses = (StatusInfo *)ps_calloc(NUM_LINES, sizeof(StatusInfo));
    if (!line_statuses) {
        Serial.println("[WARN] PSRAM alloc failed for line_statuses, using DRAM");
        line_statuses = (StatusInfo *)calloc(NUM_LINES, sizeof(StatusInfo));
    }

    Serial.printf("[Mem] Free heap: %lu  Free PSRAM: %lu\n",
        (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

    // Init WiFi early so it can claim internal DRAM before LVGL buffers
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);

    touch_init();

    bus = new Arduino_ESP32SPI(
        GFX_NOT_DEFINED, 39, 48, 47, GFX_NOT_DEFINED);

    Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
        18, 17, 16, 21,
        11, 12, 13, 14, 0,
        8, 20, 3, 46, 9, 10,
        4, 5, 6, 7, 15,
        1, 10, 8, 50,
        1, 10, 8, 20,
        0, GFX_NOT_DEFINED, false, 0, 0, TFT_HOR_RES * 10);

    gfx = new Arduino_RGB_Display(
        480, 480, rgbpanel, 0, true,
        bus, GFX_NOT_DEFINED,
        st7701_4848s040_init_operations, sizeof(st7701_4848s040_init_operations));

    if (!gfx->begin()) Serial.println("gfx->begin() failed!");

    pinMode(GFX_BL, OUTPUT);
    analogWrite(GFX_BL, TFT_BRIGHTNESS);
    gfx->fillScreen(RGB565_BLACK);
    gfx->setRotation(3);

    lv_init();
    lv_tick_set_cb(my_tick);

    lv_display_t *disp = lv_display_create(TFT_HOR_RES, TFT_VER_RES);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, draw_buf, nullptr, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_rotation(disp, TFT_ROTATION);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // App init
    for (int i = 0; i < NUM_LINES; i++) {
        line_enabled[i] = true;  // default; overwritten by load_line_prefs() below
        // Default status until first fetch
        line_statuses[i].level = STATUS_GOOD;
        strncpy(line_statuses[i].headline, "Loading...", 79);
        strncpy(line_statuses[i].detail,   "Fetching status from TfL...", 319);
        // Build uppercase display name
        const char *src = LINES[i].short_name ? LINES[i].short_name : LINES[i].name;
        int j = 0;
        for (; src[j] && j < 19; j++)
            disp_names[i][j] = (src[j] >= 'a' && src[j] <= 'z') ? src[j] - 32 : src[j];
        disp_names[i][j] = '\0';
    }

    lv_timer_create(clock_cb, 10000, nullptr);

    load_line_prefs();

    // Load saved WiFi credentials and start background connect
    load_wifi_credentials();
    if (strlen(g_wifi_ssid) > 0) {
        WiFi.begin(g_wifi_ssid, g_wifi_password);
        Serial.printf("[WiFi] Background connect to '%s'\n", g_wifi_ssid);
    }

    navigate_to(SCREEN_HOME);

    // Recurring fetch every 60 s; first fetch is triggered in loop() on WiFi connect
    lv_timer_create(fetch_timer_cb, 60000, nullptr);
}

void loop() {
    if (!g_wifi_was_connected && WiFi.status() == WL_CONNECTED) {
        g_wifi_was_connected = true;
        configure_ntp();
        g_fetch_requested = true;
    }
    if (g_fetch_requested) {
        g_fetch_requested = false;
        fetch_tfl_status();
    }
    lv_timer_handler();
}
