// TubeStatus — London Underground status board for ESP32-4848S040 (480×480 LVGL)
// Design source: claude.ai/design
// Network: TfL Unified API (stubbed — see fetch_tfl_status() below)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ESP32_4848S040.h>
#include <lvgl.h>
#include "touch.h"

// ── Hardware ────────────────────────────────────────────────────────────────
#define GFX_BL         38
#define TFT_HOR_RES    480
#define TFT_VER_RES    480
#define TFT_ROTATION   LV_DISPLAY_ROTATION_90
#define TFT_BRIGHTNESS 255
#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

static Arduino_ESP32SPI   *bus;
static Arduino_RGB_Display *gfx;
static uint32_t draw_buf[DRAW_BUF_SIZE / 4];

// ── Line data ────────────────────────────────────────────────────────────────
#define NUM_LINES      20
#define LINES_PER_PAGE 12  // 4 cols × 3 rows

typedef enum { STATUS_GOOD = 0, STATUS_MINOR, STATUS_SEVERE } StatusLevel;

struct LineInfo {
    const char *id;
    const char *name;
    const char *short_name;  // nullptr → use name
    uint32_t    color;       // 0xRRGGBB (TfL official)
    bool        text_dark;   // true → dark text on badge bar
};

struct StatusInfo {
    StatusLevel  level;
    const char  *headline;
    const char  *detail;
};

struct StatusTone {
    uint32_t bg, fg, dot;
};

static const LineInfo LINES[NUM_LINES] = {
    // ── Tube ────────────────────────────────────────────────────────────────
    // API ids confirmed from TfL Unified API
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
    // ── Other modes ─────────────────────────────────────────────────────────]
    { "dlr",             "DLR",                nullptr,   0x009BBB, false },
    // Overground (2024 rebrand — 6 named lines)
    { "lioness",         "Lioness",            nullptr,   0xF882B8, true  },
    { "mildmay",         "Mildmay",            nullptr,   0x0098A4, false },
    { "windrush",        "Windrush",           nullptr,   0xDB261B, false },
    { "weaver",          "Weaver",             nullptr,   0x92298D, false },
    { "suffragette",     "Suffragette",        "Suffrgt", 0x507326, false },
    { "liberty",         "Liberty",            nullptr,   0x751056, false },
    { "tram",            "Tram",               nullptr,   0x84B817, true  },
};

// Stub data matching realistic TfL API output.
// Wire fetch_tfl_status() below to populate these from:
//   GET https://api.tfl.gov.uk/Line/Mode/tube,dlr,elizabeth-line,tram,overground/Status
//   Response: [{id, lineStatuses:[{statusSeverity, statusSeverityDescription, reason}]}]
//   Map: severity 10 → GOOD, 7/9 → MINOR, ≤6 → SEVERE
static StatusInfo line_statuses[NUM_LINES] = {
    // Tube
    { STATUS_GOOD,   "Good service",   "Good service on the Bakerloo line." },
    { STATUS_SEVERE, "Severe delays",  "Severe delays between Marble Arch and Liverpool Street due to a signal failure at Bond Street. Tickets accepted on local buses." },
    { STATUS_GOOD,   "Good service",   "Good service on the Circle line." },
    { STATUS_MINOR,  "Minor delays",   "Minor delays westbound between Earl's Court and Wimbledon due to an earlier incident at Parsons Green. Service is recovering." },
    { STATUS_GOOD,   "Good service",   "Good service on the Elizabeth line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Hammersmith & City line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Jubilee line." },
    { STATUS_MINOR,  "Minor delays",   "Minor delays between Aldgate and Baker Street due to train cancellations." },
    { STATUS_GOOD,   "Good service",   "Good service on the Northern line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Piccadilly line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Victoria line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Waterloo & City line." },
    // Other modes
    { STATUS_GOOD,   "Good service",   "Good service on the DLR." },
    { STATUS_GOOD,   "Good service",   "Good service on the Lioness line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Mildmay line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Windrush line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Weaver line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Suffragette line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Liberty line." },
    { STATUS_GOOD,   "Good service",   "Good service on the Tram network." },
};

static const StatusTone STATUS_TONES[3] = {
    { 0x1A1F26, 0xE6E8EB, 0x39B57C }, // good  — dark bg, light text, green dot
    { 0x5A3D14, 0xFFE3B0, 0xF2A93B }, // minor — amber bg
    { 0x6B1721, 0xFFD9DD, 0xFF5168 }, // severe — red bg
};

// ── App state ─────────────────────────────────────────────────────────────
static bool line_enabled[NUM_LINES];
static int  last_updated_min = 2;              // stub: pretend fetched 2min ago
static char disp_names[NUM_LINES][20];         // uppercase badge labels

// ── Pagination state ──────────────────────────────────────────────────────
static int        g_home_page       = 0;
static int        g_home_page_count = 1;
static int16_t    g_swipe_start_x   = 0;
static bool       g_was_swipe       = false;
static lv_obj_t  *g_tile_cont       = nullptr;
static int16_t    g_cont_start_x    = 0;
static lv_obj_t  *g_page_dots[8]    = {};

// ── UI refs ────────────────────────────────────────────────────────────────
typedef enum { SCREEN_HOME, SCREEN_DETAIL, SCREEN_SETTINGS } ScreenType;

static lv_obj_t *g_time_lbl          = nullptr;  // updated by clock timer
static lv_obj_t *g_toggle_track[NUM_LINES] = {};
static lv_obj_t *g_toggle_knob[NUM_LINES]  = {};

// ── Forward declarations ────────────────────────────────────────────────
static void navigate_to(ScreenType type, int line_idx = 0);

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

static String clock_str() {
    // No RTC/NTP in stub — show time progressing from 12:00 since boot.
    int total_m = 720 + (int)(millis() / 60000);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", (total_m / 60) % 24, total_m % 60);
    return String(buf);
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
// Ring style: hollow colored circle with a colored horizontal bar (name band).
// The bar extends 11% beyond the circle diameter on each side.
// Container is intentionally wider than `size` to hold the bar overhang.
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

    // Hollow ring
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

    // Name band — same color as ring, spans full container width
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

    // Build full sorted list: severe → minor → good
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

    // Header
    lv_obj_t *hdr = build_header(scr);

    g_time_lbl = lv_label_create(hdr);
    lv_label_set_text(g_time_lbl, clock_str().c_str());
    lv_obj_set_style_text_color(g_time_lbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(g_time_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(g_time_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    char upd[24];
    snprintf(upd, sizeof(upd), "Updated %dm ago", last_updated_min);
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

    // Grid layout — always 4 cols × 3 rows, reserve 24px at bottom for dots
    static const int COLS = 4, ROWS = 3;
    static const int BADGE_SIZE = 70;
    int pad    = 12, gap = 8;
    int grid_w = TFT_HOR_RES - 2 * pad;                             // 456
    int grid_h = TFT_VER_RES - 56 - 2 * pad - 24;                  // 376
    int tile_w = (grid_w - (COLS - 1) * gap) / COLS;                // 108
    int tile_h = (grid_h - (ROWS - 1) * gap) / ROWS;                // 120
    int badge_cont_w = BADGE_SIZE + 2 * (BADGE_SIZE * 11 / 100);    //  86

    // Wide container spanning all pages side-by-side; slides on swipe
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

    // Swipe on screen background (gaps between tiles)
    lv_obj_add_event_cb(scr, home_swipe_pressed_cb,  LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(scr, home_swipe_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(scr, home_swipe_released_cb, LV_EVENT_RELEASED, nullptr);

    // Page indicator dots (stored for live colour updates on swipe)
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
static void back_to_home(lv_event_t *) { navigate_to(SCREEN_HOME); }

static void build_detail(lv_obj_t *scr, int idx) {
    const LineInfo   &L    = LINES[idx];
    const StatusInfo &st   = line_statuses[idx];
    const StatusTone &tone = STATUS_TONES[(int)st.level];

    // Header
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

    // Large badge
    static const int BADGE_SIZE = 140;
    int badge_cont_w = BADGE_SIZE + 2 * (BADGE_SIZE * 11 / 100);
    int content_y    = 76;

    lv_obj_t *badge = create_badge(scr, idx, BADGE_SIZE);
    lv_obj_set_pos(badge, (TFT_HOR_RES - badge_cont_w) / 2, content_y);
    content_y += BADGE_SIZE + 18;

    // Status pill (flex row: dot + label)
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

    // Center pill after content is added
    lv_obj_update_layout(pill);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, content_y);
    content_y += 42;

    // Detail text
    lv_obj_t *detail = lv_label_create(scr);
    lv_label_set_text(detail, st.detail);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail, 440);
    lv_obj_set_style_text_color(detail, C(0xC6CBD2), 0);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, content_y);

    // Last updated (pinned to bottom)
    char upd[32];
    snprintf(upd, sizeof(upd), "Last updated %dm ago", last_updated_min);
    lv_obj_t *ulbl = lv_label_create(scr);
    lv_label_set_text(ulbl, upd);
    lv_obj_set_style_text_color(ulbl, C(0x6E7681), 0);
    lv_obj_set_style_text_font(ulbl, &lv_font_montserrat_10, 0);
    lv_obj_align(ulbl, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// ── Settings screen ───────────────────────────────────────────────────────
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
}

static void settings_all_cb(lv_event_t *) {
    for (int i = 0; i < NUM_LINES; i++) { line_enabled[i] = true;  refresh_toggle(i); }
}

static void settings_none_cb(lv_event_t *) {
    for (int i = 0; i < NUM_LINES; i++) { line_enabled[i] = false; refresh_toggle(i); }
}

static void build_settings(lv_obj_t *scr) {
    for (int i = 0; i < NUM_LINES; i++) g_toggle_track[i] = g_toggle_knob[i] = nullptr;

    // Header
    lv_obj_t *hdr  = build_header(scr);
    lv_obj_t *bbtn = build_back_btn(hdr);
    lv_obj_add_event_cb(bbtn, back_to_home, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *tlbl = lv_label_create(hdr);
    lv_label_set_text(tlbl, "Lines");
    lv_obj_set_style_text_color(tlbl, C(0xE6E8EB), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_16, 0);
    lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 44, 0);

    // Sub-header row
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

    // All / None chips — positioned from the right
    struct { const char *txt; lv_event_cb_t cb; int w; } chips[] = {
        { "NONE", settings_none_cb, 50 },
        { "ALL",  settings_all_cb,  38 },
    };
    int chip_x = -0;  // offset from right
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

    // Scrollable line list
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

        // Color dot
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 28, 28);
        lv_obj_set_pos(dot, 12, (ROW_H - 28) / 2);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, C(L.color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        // Line name
        lv_obj_t *nlbl = lv_label_create(row);
        lv_label_set_text(nlbl, L.name);
        lv_obj_set_style_text_color(nlbl, C(0xE6E8EB), 0);
        lv_obj_set_style_text_font(nlbl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(nlbl, 52, 10);
        lv_obj_remove_flag(nlbl, LV_OBJ_FLAG_CLICKABLE);

        // Status text
        lv_obj_t *slbl = lv_label_create(row);
        lv_label_set_text(slbl, st.headline);
        lv_obj_set_style_text_color(slbl, C(tone.dot), 0);
        lv_obj_set_style_text_font(slbl, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(slbl, 52, 30);
        lv_obj_remove_flag(slbl, LV_OBJ_FLAG_CLICKABLE);

        // Toggle track
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

        // Toggle knob
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

// ── Navigation ───────────────────────────────────────────────────────────
// Reset page when navigating to a non-home screen so returning home lands on p0
static void delete_scr_async(void *param) {
    lv_obj_delete((lv_obj_t *)param);
}

static void navigate_to(ScreenType type, int line_idx) {
    g_time_lbl  = nullptr;
    g_tile_cont = nullptr;
    if (type != SCREEN_HOME) g_home_page = 0;

    lv_obj_t *old_scr = lv_screen_active();

    lv_obj_t *new_scr = lv_obj_create(nullptr);
    style_screen(new_scr);

    switch (type) {
        case SCREEN_HOME:     build_home(new_scr);            break;
        case SCREEN_DETAIL:   build_detail(new_scr, line_idx); break;
        case SCREEN_SETTINGS: build_settings(new_scr);        break;
    }

    lv_scr_load(new_scr);

    // Defer deletion so we're safely off the old screen's call stack
    if (old_scr) lv_async_call(delete_scr_async, old_scr);
}

// ── TfL API stub ─────────────────────────────────────────────────────────
// Replace with WiFiClient + HTTPClient + ArduinoJson to populate line_statuses[].
// Expected JSON shape per element:
//   {"id":"central","lineStatuses":[{"statusSeverity":10,"reason":""}]}
static void fetch_tfl_status() {
    // TODO: WiFi connect, GET the endpoint, parse JSON, populate line_statuses[]
    // and call navigate_to(SCREEN_HOME) to refresh.
    last_updated_min = 0;
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
}

// ── setup / loop ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
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
        0, GFX_NOT_DEFINED, false, 0, 0, TFT_HOR_RES * 20);

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
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_rotation(disp, TFT_ROTATION);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // App init
    for (int i = 0; i < NUM_LINES; i++) {
        line_enabled[i] = true;
        const char *src = LINES[i].short_name ? LINES[i].short_name : LINES[i].name;
        int j = 0;
        for (; src[j] && j < 19; j++)
            disp_names[i][j] = (src[j] >= 'a' && src[j] <= 'z') ? src[j] - 32 : src[j];
        disp_names[i][j] = '\0';
    }

    lv_timer_create(clock_cb, 10000, nullptr);

    navigate_to(SCREEN_HOME);
}

void loop() {
    lv_timer_handler();
    // delay(2);
}
