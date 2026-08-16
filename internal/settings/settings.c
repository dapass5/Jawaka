#include "internal/settings/settings.h"

#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/launcher/system_names.h"
#include "internal/platform/device.h"
#include "internal/platform/leaf_version.h"
#include "internal/platform/platform_id.h"
#include "internal/scrape/scrape_catalog.h"
#include "internal/settings/appearance.h"
#include "internal/i18n/i18n.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ─── Data tables ──────────────────────────────────────────────────────── */

const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT] = {
    "Jawaka-Tabs",
    "Jawaka-Vertical",
    "Jawaka-Horizontal",
    "Jawaka-Coverflow",
};

/* Focus-on-Tabs: only Jawaka-Tabs is an actively-supported layout. The others
   stay in the build but cannot be switched to from the theme picker — the
   cycler skips them and jw__apply_theme refuses them. */
const bool kJawakaThemeEnabled[JW_SETTINGS_THEME_COUNT] = {
    true,   /* Jawaka-Tabs */
    false,  /* Jawaka-Vertical */
    false,  /* Jawaka-Horizontal */
    false,  /* Jawaka-Coverflow */
};

const char *const kPillShapeLabels[JW_SETTINGS_PILL_SHAPE_COUNT] = {
    JW_UI("Rounded"),
    JW_UI("Soft"),
    JW_UI("Square"),
    JW_UI("Leaf"),
};

const char *const kFontSizeLabels[JW_SETTINGS_FONT_SIZE_COUNT] = {
    JW_UI("Small"),
    JW_UI("Default"),
    JW_UI("Large"),
    JW_UI("Extra Large"),
};

/* The value tables backing these labels (pill radius, corner mask, font bump)
   are the canonical ones in appearance.c, shared with the daemon's env export.
   Keep the label row counts locked to them. */
_Static_assert(JW_SETTINGS_PILL_SHAPE_COUNT == JW_APPEARANCE_PILL_SHAPE_COUNT,
               "pill shape label/value table counts out of sync");
_Static_assert(JW_SETTINGS_FONT_SIZE_COUNT == JW_APPEARANCE_FONT_SIZE_COUNT,
               "font size label/value table counts out of sync");

const char *const kClockStyleLabels[JW_SETTINGS_CLOCK_STYLE_COUNT] = {
    JW_UI("Hidden"),
    JW_UI("24 Hour"),
    JW_UI("AM/PM"),
    JW_UI("12 Hour"),
};

/* Curated color schemes selectable from the Appearance > Color Scheme row. Each
   sets the seven color roles at once. Order: accent (chrome bars), background,
   text, hint, selection (selected-row pill), button label, button glyph bg.
   Selected-row text auto-contrasts against the selection pill at apply time. */
typedef struct {
    const char *name;
    const char *accent, *bg, *text, *hint, *selection, *btn_label, *btn_bg;
} jw__color_scheme;

/* Two ROYGBIV runs, Leaf leading: seven dark schemes (Leaf, then the spectrum in
   R-O-Y-B-I-V order), then the seven light schemes in the same order so each light
   sits a hue-twin of the dark above it. The selected-row text auto-contrast in
   cat_finalize_theme_colors picks the readable one of {text,bg}, so dark text lands
   on the bright selection pill on both dark and light pages. */
static const jw__color_scheme kColorSchemes[] = {
    /* name        accent     bg         text       hint       selection  btn_label  btn_bg     */
    /* Dark */
    { "Leaf",      "#1E331E", "#0F160E", "#E8F1E3", "#7E9579", "#7FB069", "#0F160E", "#7FB069" },
    { "Rose",      "#33222E", "#1C1620", "#F0E6EC", "#A88A98", "#EB6F92", "#1C1620", "#EB6F92" },
    { "Ember",     "#3A2A22", "#1A1413", "#F2EAE2", "#A38A7A", "#FF8A4C", "#1A1413", "#FF8A4C" },
    { "Goldenrod", "#332C16", "#15120A", "#F2EEDD", "#A89A6E", "#E8C24A", "#15120A", "#E8C24A" },
    { "Tide",      "#173042", "#0D1620", "#E2EEF4", "#6E8A9A", "#4FA3E0", "#0D1620", "#4FA3E0" },
    { "Indigo",    "#232544", "#131426", "#E6E7F4", "#7C7EA0", "#818CF5", "#131426", "#818CF5" },
    { "Orchid",    "#2E2240", "#181226", "#ECE4F2", "#8E7CB0", "#C792EA", "#181226", "#C792EA" },
    /* Light */
    { "Meadow",    "#9CCB85", "#D1D0A6", "#1B2E1B", "#5E7654", "#7FB069", "#1B2E1B", "#7FB069" },
    { "Petal",     "#F2BFCE", "#DEC9D0", "#3A2630", "#8A6E78", "#E886A4", "#3A2630", "#E886A4" },
    { "Apricot",   "#EAC99C", "#E6D2B0", "#332518", "#8A7050", "#EE9F54", "#332518", "#EE9F54" },
    { "Wheat",     "#E6D38A", "#ECE6C2", "#332C12", "#897F50", "#CDB23E", "#332C12", "#CDB23E" },
    { "Sky",       "#9CC3E0", "#BFCED7", "#1B2A33", "#5A7280", "#6FA8DC", "#1B2A33", "#6FA8DC" },
    { "Periwinkle","#BFC0E6", "#CFCFE2", "#22243A", "#6E708E", "#8088E6", "#22243A", "#8088E6" },
    { "Lavender",  "#D0BCE4", "#D2C9DC", "#2A2238", "#786E8A", "#BE8FE2", "#2A2238", "#BE8FE2" },
    /* Mono — a grayscale dark/light pair, deliberately OUTSIDE the two ROYGBIV
       runs (achromatic, so no hue slot / hue-twin). Pure neutral grays; the
       selection pill carries the contrast by VALUE since there's no hue to lean
       on (light pill on the dark page, dark pill on the light page). */
    { "Ebony",     "#262626", "#0E0E0E", "#ECECEC", "#8C8C8C", "#CFCFCF", "#0E0E0E", "#CFCFCF" },
    { "Birch",     "#D8D8D8", "#EDEDED", "#1A1A1A", "#767676", "#4A4A4A", "#EDEDED", "#4A4A4A" },
};
#define JW_COLOR_SCHEME_COUNT ((int)(sizeof(kColorSchemes) / sizeof(kColorSchemes[0])))
#define JW_COLOR_SCHEME_DEFAULT 0   /* Leaf — the Dweezil/Leaf identity theme */

static void jw__apply_color_scheme(jw_settings_ui *ui, int index, bool *theme_changed);

#define JW_SETTINGS_VALUE_MAX 64
typedef enum {
    JW_SETTING_PILL_SHAPE_INDEX = 0,
    JW_SETTING_FONT_FAMILY_INDEX,
    JW_SETTING_FONT_SIZE_INDEX,
    JW_SETTING_SHOW_HINTS,
    JW_SETTING_CLOCK_STYLE_INDEX,
    JW_SETTING_SHOW_BATTERY,
    JW_SETTING_SHOW_BATTERY_LEVEL,
    JW_SETTING_SHOW_WIFI,
    JW_SETTING_SHOW_BLUETOOTH,
    JW_SETTING_SHOW_VOLUME,
    JW_SETTING_COLOR_SCHEME_INDEX,
    JW_SETTING_STARTUP_TAB_INDEX,
    JW_SETTING_AUTO_SLEEP_SECONDS,
    JW_SETTING_BOOT_SPLASH_ENABLED,
    JW_SETTING_SCREENSHOTS_ENABLED,
    JW_SETTING_RECORDING_ENABLED,
    JW_SETTING_RECORDING_SPLIT,
    JW_SETTING_RECORDING_KEEP_SRC,
    JW_SETTING_GAME_PERFORMANCE_PROFILE,
    JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT,
    JW_SETTING_PLATFORM_VOLUME_PERCENT,
    JW_SETTING_ACCENT_COLOR,
    JW_SETTING_BG_COLOR,
    JW_SETTING_TEXT_COLOR,
    JW_SETTING_HINT_COLOR,
    JW_SETTING_HIGHLIGHT_COLOR,
    JW_SETTING_BUTTON_LABEL_COLOR,
    JW_SETTING_BUTTON_GLYPH_BG_COLOR,
    JW_SETTING_TIMEZONE,
    JW_SETTING_SS_USER,
    JW_SETTING_SS_PASS,
    JW_SETTING_SS_VERIFIED,
    JW_SETTING_SS_MAXTHREADS,
    JW_SETTING_SS_REQUESTS_TODAY,
    JW_SETTING_SS_MAX_REQUESTS,
    JW_SETTING_SCRAPE_ARTWORK_PRIO,
    JW_SETTING_SCRAPE_REGION_PRIO,
    JW_SETTING_RA_USER,
    JW_SETTING_RA_PASS,
    JW_SETTING_TAB_GLIDE,
    JW_SETTING_REFRESH_RATE_HZ,
    JW_SETTING_BFI_ENABLED,
    JW_SETTING_HDMI_OUTPUT_MODE,
    JW_SETTING_HOME_TAB_ORDER,
    JW_SETTING_COUNT,
} jw__setting_key;

static const char *const kSettingKeys[JW_SETTING_COUNT] = {
    [JW_SETTING_PILL_SHAPE_INDEX] = "pill_shape_index",
    [JW_SETTING_FONT_FAMILY_INDEX] = "font_family_index",
    [JW_SETTING_FONT_SIZE_INDEX] = "font_size_index",
    [JW_SETTING_SHOW_HINTS] = "show_hints",
    [JW_SETTING_CLOCK_STYLE_INDEX] = "clock_style_index",
    [JW_SETTING_SHOW_BATTERY] = "show_battery",
    [JW_SETTING_SHOW_BATTERY_LEVEL] = "show_battery_level",
    [JW_SETTING_SHOW_WIFI] = "show_wifi",
    [JW_SETTING_SHOW_BLUETOOTH] = "show_bluetooth",
    [JW_SETTING_SHOW_VOLUME] = "show_volume",
    [JW_SETTING_COLOR_SCHEME_INDEX] = "color_scheme_index",
    [JW_SETTING_STARTUP_TAB_INDEX] = "startup_tab_index",
    [JW_SETTING_AUTO_SLEEP_SECONDS] = "auto_sleep_seconds",
    [JW_SETTING_BOOT_SPLASH_ENABLED] = "boot_splash_enabled",
    [JW_SETTING_SCREENSHOTS_ENABLED] = "screenshots_enabled",
    [JW_SETTING_RECORDING_ENABLED]   = "recording_enabled",
    [JW_SETTING_RECORDING_SPLIT]     = "recording_split",
    [JW_SETTING_RECORDING_KEEP_SRC]  = "recording_keep_source",
    [JW_SETTING_GAME_PERFORMANCE_PROFILE] = "platform.performance.game_profile",
    [JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT] = "platform.brightness_percent",
    [JW_SETTING_PLATFORM_VOLUME_PERCENT] = "platform.volume_percent",
    [JW_SETTING_ACCENT_COLOR] = "accent_color",
    [JW_SETTING_BG_COLOR] = "bg_color",
    [JW_SETTING_TEXT_COLOR] = "text_color",
    [JW_SETTING_HINT_COLOR] = "hint_color",
    [JW_SETTING_HIGHLIGHT_COLOR] = "highlight_color",
    [JW_SETTING_BUTTON_LABEL_COLOR] = "button_label_color",
    [JW_SETTING_BUTTON_GLYPH_BG_COLOR] = "button_glyph_bg_color",
    [JW_SETTING_TIMEZONE] = "timezone",
    [JW_SETTING_SS_USER] = "screenscraper_user",
    [JW_SETTING_SS_PASS] = "screenscraper_pass",
    [JW_SETTING_SS_VERIFIED] = "screenscraper_verified",
    [JW_SETTING_SS_MAXTHREADS] = "screenscraper_maxthreads",
    [JW_SETTING_SS_REQUESTS_TODAY] = "screenscraper_requests_today",
    [JW_SETTING_SS_MAX_REQUESTS] = "screenscraper_max_requests",
    [JW_SETTING_SCRAPE_ARTWORK_PRIO] = "scrape.artwork_priority",
    [JW_SETTING_SCRAPE_REGION_PRIO] = "scrape.region_priority",
    [JW_SETTING_RA_USER] = "retroachievements_user",
    [JW_SETTING_RA_PASS] = "retroachievements_pass",
    [JW_SETTING_TAB_GLIDE] = "tab_glide",
    [JW_SETTING_REFRESH_RATE_HZ] = "refresh_rate_hz",
    [JW_SETTING_BFI_ENABLED] = "bfi_enabled",
    [JW_SETTING_HDMI_OUTPUT_MODE] = "hdmi_output_mode",
    [JW_SETTING_HOME_TAB_ORDER] = "home_tab_order",
};

static const char *const kTabSwitchLabels[] = { JW_UI("Snap"), JW_UI("Glide") };
#define JW_TAB_SWITCH_COUNT 2

/* Curated time-zone list for Settings > Behavior > Time Zone. Each entry maps a
   friendly label to an IANA zone id, exported as the TZ environment variable. The
   clock uses localtime(), which honors TZ, so picking a zone corrects the clock
   instantly and flows to launched apps. zoneinfo for every entry ships in the
   rootfs (/usr/share/zoneinfo), so no data needs bundling. ASCII labels only
   (the launcher font subset has no extended-Latin glyphs). */
/* Ordered by UTC (standard-time) offset, the convention OS time-zone pickers use.
   `off` is the displayed base offset (DST shifts it at runtime); ASCII only. */
typedef struct { const char *label; const char *tz; const char *off; } jw__timezone_entry;
static const jw__timezone_entry kTimeZones[] = {
    { "US Hawaii",          "Pacific/Honolulu",    "UTC-10"   },
    { "US Alaska",          "America/Anchorage",   "UTC-9"    },
    { "US Pacific",         "America/Los_Angeles", "UTC-8"    },
    { "US Mountain",        "America/Denver",      "UTC-7"    },
    { "US Arizona",         "America/Phoenix",     "UTC-7"    },
    { "US Central",         "America/Chicago",     "UTC-6"    },
    { "US Eastern",         "America/New_York",    "UTC-5"    },
    { "Brazil (East)",      "America/Sao_Paulo",   "UTC-3"    },
    { "UTC",                "UTC",                 "UTC"      },
    { "UK / Ireland",       "Europe/London",       "UTC+0"    },
    { "Central Europe",     "Europe/Paris",        "UTC+1"    },
    { "Eastern Europe",     "Europe/Athens",       "UTC+2"    },
    { "India",              "Asia/Kolkata",        "UTC+5:30" },
    { "China",              "Asia/Shanghai",       "UTC+8"    },
    { "Japan / Korea",      "Asia/Tokyo",          "UTC+9"    },
    { "Sydney",             "Australia/Sydney",    "UTC+10"   },
};
#define JW_TIMEZONE_COUNT ((int)(sizeof(kTimeZones) / sizeof(kTimeZones[0])))
/* Rows visible at once in the picker pane (the list scrolls past this). Matches
   the System Update picker, which uses the same two-line item height + pane. */
#define JW_TIMEZONE_VISIBLE_ROWS 7

static const char *jw__timezone_label(const char *tz) {
    if (!tz || !tz[0]) return "System default";
    for (int i = 0; i < JW_TIMEZONE_COUNT; ++i)
        if (strcmp(kTimeZones[i].tz, tz) == 0) return kTimeZones[i].label;
    return tz;   /* unknown id: show the raw zone */
}

static int jw__timezone_index_of(const char *tz) {
    if (tz && tz[0])
        for (int i = 0; i < JW_TIMEZONE_COUNT; ++i)
            if (strcmp(kTimeZones[i].tz, tz) == 0) return i;
    return 0;
}

/* Set TZ and refresh libc's timezone state so the very next localtime() (the
   status-bar clock) reflects the new zone without a restart. Empty tz leaves the
   system default in place. */
static void jw__apply_timezone(const char *tz) {
    if (tz && tz[0]) setenv("TZ", tz, 1);
    tzset();
}

static bool jw__setting_has(char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX],
                            const unsigned char found[JW_SETTING_COUNT],
                            jw__setting_key key) {
    return found[key] && values[key][0];
}

static int jw__load_setting_values(const char *db_path,
                                   char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX],
                                   unsigned char found[JW_SETTING_COUNT]) {
    memset(values, 0, sizeof(char) * JW_SETTING_COUNT * JW_SETTINGS_VALUE_MAX);
    memset(found, 0, sizeof(unsigned char) * JW_SETTING_COUNT);
    if (!db_path || !db_path[0]) {
        return -1;
    }

    jw_db_setting_query queries[JW_SETTING_COUNT];
    for (int i = 0; i < JW_SETTING_COUNT; i++) {
        queries[i].key = kSettingKeys[i];
        queries[i].out = values[i];
        queries[i].out_size = JW_SETTINGS_VALUE_MAX;
        queries[i].found = 0;
    }
    if (jw_db_get_settings(db_path, queries, JW_SETTING_COUNT) != 0) {
        return -1;
    }
    for (int i = 0; i < JW_SETTING_COUNT; i++) {
        found[i] = queries[i].found ? 1 : 0;
    }
    return 0;
}

/* Startup-tab options for Settings > Behavior. Order and index MIRROR the
   launcher's jw_tab enum so the persisted "startup_tab_index" maps 1:1 to the
   tab the launcher opens on boot. */
static const char *kStartupTabLabels[] = {
    "Recents", "Favorites", "Games", "Apps",
};
#define JW_STARTUP_TAB_COUNT ((int)(sizeof(kStartupTabLabels) / sizeof(kStartupTabLabels[0])))
#define JW_STARTUP_TAB_DEFAULT 2   /* Games */

/* Parse the "home_tab_order" CSV (visible jw_tab indices in display order) into
   order[JW_HOME_TABS_COUNT] + *visible. Defensive: dedupe, drop out-of-range /
   invalid tokens, then append any tabs not listed (hidden) after the visible
   ones. An empty/missing/all-invalid CSV falls back to all tabs visible in
   natural order. *visible is always >= 1. */
static void jw__parse_home_tab_order(const char *csv, int *order, int *visible) {
    bool used[JW_HOME_TABS_COUNT] = { false };
    int vis = 0;

    const char *p = (csv && csv[0]) ? csv : NULL;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) { p++; continue; }   /* not a number: skip a char */
        p = end;
        if (v >= 0 && v < JW_HOME_TABS_COUNT && !used[(int)v]) {
            order[vis++] = (int)v;
            used[(int)v] = true;
        }
    }

    /* Empty / all-invalid → default to every tab visible in natural order. */
    if (vis == 0) {
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) { order[i] = i; used[i] = true; }
        vis = JW_HOME_TABS_COUNT;
    } else {
        /* Append the hidden tabs after the visible ones so the editor can list
           and re-enable them. */
        int tail = vis;
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++)
            if (!used[i]) order[tail++] = i;
    }
    *visible = vis;
}

/* Serialize order[]/visible into a CSV of the visible jw_tab indices, e.g.
   "2,1,3,0" or "2,1,3". */
static void jw__home_tab_order_to_csv(const int *order, int visible,
                                      char *csv, size_t csv_size) {
    size_t len = 0;
    if (csv_size > 0) csv[0] = '\0';
    for (int i = 0; i < visible && i < JW_HOME_TABS_COUNT; i++) {
        int n = snprintf(csv + len, csv_size - len, "%s%d",
                         i > 0 ? "," : "", order[i]);
        if (n < 0 || (size_t)n >= csv_size - len) break;
        len += (size_t)n;
    }
}

/* Move order[from] to position to, shifting the entries between. */
static void jw__home_tab_order_move(int *order, int from, int to) {
    if (from == to) return;
    int v = order[from];
    if (from < to)
        memmove(order + from, order + from + 1, (size_t)(to - from) * sizeof(int));
    else
        memmove(order + to + 1, order + to, (size_t)(from - to) * sizeof(int));
    order[to] = v;
}

/* Auto-sleep options for Settings > Behavior. The label index is persisted as
   "auto_sleep_seconds" (the value, not the index) so the daemon reads seconds
   directly with no shared table. */
static const char *kAutoSleepLabels[]  = { "Off", "15 sec", "30 sec", "45 sec", "1 min", "2 min", "5 min", "10 min" };
static const int   kAutoSleepSeconds[] = {     0,       15,       30,       45,      60,     120,     300,      600 };
#define JW_AUTO_SLEEP_COUNT   ((int)(sizeof(kAutoSleepLabels) / sizeof(kAutoSleepLabels[0])))
#define JW_AUTO_SLEEP_DEFAULT 0   /* Off by default (index into the tables above).
                                     Deep-suspend wake is not yet reliable, so
                                     auto-sleep stays opt-in until that is solid. */

/* Panel refresh rates offered in Settings > Display & Sound, low to high. The
   modeline writer scales the dot clock to any rate (782842 * hz / 1e6), so this
   table is the only place the choice is fixed; the daemon clamps to 60..120.

   Each rate is the right answer for something, which is the whole selection
   rule - a rate earns a slot by dividing evenly into some content rate:
     60  - NTSC 60fps at a perfect 1:1, lowest power, the safe default
     100 - PAL 50fps at a perfect 2:2 (the only rate that divides into 50)
     120 - NTSC 60fps at 2:2 (so BFI has a black frame to insert), 24fps at 5:5
   90 was dropped: it is 1.5x of 60fps, so it forced an alternating 1-then-2
   refresh hold (3:2 pulldown) that juddered 60fps games WORSE than plain 60 Hz,
   and it divided evenly into nothing anyone plays. It shipped first only
   because it was the rate the feature was originally proven at. */
static const int kPanelRefreshHz[] = { 60, 100, 120 };
#define JW_PANEL_REFRESH_COUNT ((int)(sizeof(kPanelRefreshHz) / sizeof(kPanelRefreshHz[0])))

static bool jw__is_panel_refresh_hz(int hz) {
    for (int i = 0; i < JW_PANEL_REFRESH_COUNT; i++) {
        if (kPanelRefreshHz[i] == hz) {
            return true;
        }
    }
    return false;
}

/* Index of the offered rate closest to hz, so a rate that is no longer offered
   (a device carried over from when 90 Hz was on the menu) still cycles somewhere
   sensible: from 90, one step down lands on 60 and one step up on 100. */
static int jw__nearest_refresh_index(const int *rates, int n, int hz) {
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (abs(rates[i] - hz) < abs(rates[best] - hz)) {
            best = i;
        }
    }
    return best;
}

static const jw_platform_perf_profile kGamePerfProfiles[] = {
    JW_PLATFORM_PERF_PROFILE_AUTO,
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
};
#define JW_GAME_PERF_PROFILE_COUNT \
    ((int)(sizeof(kGamePerfProfiles) / sizeof(kGamePerfProfiles[0])))
#define JW_GAME_PERF_PROFILE_DEFAULT 0

static int jw__game_perf_index_for_profile(jw_platform_perf_profile profile) {
    for (int i = 0; i < JW_GAME_PERF_PROFILE_COUNT; i++) {
        if (kGamePerfProfiles[i] == profile) {
            return i;
        }
    }
    return JW_GAME_PERF_PROFILE_DEFAULT;
}

/* Top-level Settings categories, grouped by theme: look & feel, connectivity,
   games & content, system. The A handler maps each row to its screen by row
   index (a positional `idx == N` chain), so this label order and those checks
   must stay in lockstep — reordering here means renumbering there. */
static const char *kHomeCategoryLabels[] = {
    "Appearance",
    "Display & Sound",
    "Lighting",
    "Network",
    "Bluetooth",
    "Game Art",
    "Accounts",
    "General",
    "Controls & Feedback",
    "Services",
};
/* System Update and About are not listed here — they live in the System menu
   (the Menu-button popup), hosted there via jw_settings_ui_open(). */
#define JW_SETTINGS_CATEGORY_COUNT 10

/* Visible rows in the Network page's scanned-network list (scrolls beyond). */
#define JW_WIFI_LIST_ROWS 6
#define JW_NETWORK_ROW_WIFI 0
#define JW_NETWORK_ROW_ADB  1
#define JW_NETWORK_FIXED_ROWS 2
/* Re-trigger a background scan at most this often while the page is open. */
#define JW_WIFI_SCAN_INTERVAL_MS 6000
#define JW_BT_POLL_INTERVAL_MS 2000
#define JW_BT_SCAN_INTERVAL_MS 12000
#define JW_BT_ENTRY_DEFER_MS 250
#define JW_SERVICES_POLL_INTERVAL_MS 1000
#define JW_SETTINGS_DISPLAY_COUNT JW_DISPLAY_ROW_COUNT
#define JW_UPDATE_PICKER_VISIBLE_ROWS 7

static const char *kLedModeLabels[JW_LED_MODE_COUNT] = {
    JW_UI("Static"),
    JW_UI("Breath"),
    JW_UI("Rainbow"),
    JW_UI("Comet"),
    JW_UI("Sweep"),
    JW_UI("Fountain"),
    JW_UI("Hiccup"),
};


/* ─── Helpers ──────────────────────────────────────────────────────────── */

static int jw__scrape_csv_to_order(const char *csv,
                                   const jw_ss_option *catalog, int catalog_count,
                                   const char *const *fallback, int fallback_count,
                                   int *order);

static int jw__find_theme_index(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < JW_SETTINGS_THEME_COUNT; i++) {
        if (strcmp(kJawakaThemes[i], name) == 0) return i;
    }
    return 0;
}

static void jw__refresh_brightness(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    int percent = -1;
    if (jw_ipc_platform_brightness(ui->socket_path, &percent) == 0 && percent >= 0)
        ui->brightness_percent = jw_platform_clamp_brightness_percent(percent);
}

/* Pull the live volume from the platform (pactl-backed). Re-querying keeps the
   settings value tied to reality, so OSD/hardware-key changes made outside
   settings aren't stale here and a subsequent step adjusts the true value. */
static void jw__refresh_volume(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    int percent = -1;
    if (jw_ipc_platform_volume(ui->socket_path, &percent) == 0 && percent >= 0) {
        if (percent > 100) percent = 100;
        ui->volume_percent = percent;
    }
}

/* Fold an audio-status reply into the UI. Split from the fetch so the same
   handling serves both a direct query and a sample taken off the render thread
   by the launcher's background poller. */
static void jw__apply_audio_status(jw_settings_ui *ui,
                                   const jw_ipc_audio_status *status) {
    if (!ui || !status) return;
    ui->audio_output = status->output;
    ui->audio_available_outputs = status->available_outputs;
    ui->test_sound_playing = (status->test_playing != 0);
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        ui->audio_volumes[i] = status->volume_percent[i];
    }
    if (ui->audio_output >= 0 &&
        ui->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT &&
        ui->audio_volumes[ui->audio_output] >= 0) {
        ui->volume_percent = ui->audio_volumes[ui->audio_output];
    }
}

static void jw__refresh_audio_status(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    jw_ipc_audio_status status;
    if (jw_ipc_platform_audio_status(ui->socket_path, &status) != 0) {
        return;
    }
    jw__apply_audio_status(ui, &status);
}

/* Apply a snapshot the background poller took, so the Display & Sound page can
   follow the hardware brightness/volume keys without the render thread ever
   making a blocking IPC call. A round trip to jawakad costs ~110ms of latency
   (almost none of it work), and this page used to make four of them in a row
   every 300ms, which is what made its cursor movement drag. */
void jw_settings_ui_apply_av(jw_settings_ui *ui, int brightness_percent,
                             const jw_ipc_audio_status *audio) {
    if (!ui) return;
    if (brightness_percent >= 0) {
        ui->brightness_percent = jw_platform_clamp_brightness_percent(brightness_percent);
    }
    if (audio) {
        jw__apply_audio_status(ui, audio);
    }
}

/* Pull the current LED config from jawakad's cached state (set once the user has
   configured it; otherwise the defaults stand and stock rainbow keeps running). */
static void jw__refresh_led(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    int enabled = ui->led_enabled ? 1 : 0;
    int r = ui->led_color.r, g = ui->led_color.g, b = ui->led_color.b;
    int brightness = ui->led_brightness, speed = ui->led_speed;
    char mode[16] = "FOREVER";
    if (jw_ipc_get_led(ui->socket_path, &enabled, mode, sizeof(mode),
                       &r, &g, &b, &brightness, &speed) == 0) {
        ui->led_enabled = enabled != 0;
        jw_led_mode m = JW_LED_MODE_STATIC;
        jw_led_mode_parse(mode, &m);
        ui->led_mode = (int)m;
        ui->led_color.r = (unsigned char)r;
        ui->led_color.g = (unsigned char)g;
        ui->led_color.b = (unsigned char)b;
        ui->led_color.a = 255;
        ui->led_brightness = brightness;
        ui->led_speed = speed;
    }
}

static void jw__refresh_secondary_sd_status(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    snprintf(ui->secondary_sd_status, sizeof(ui->secondary_sd_status), "%s", "Unavailable");
    if (!ui->socket_path[0]) {
        return;
    }

    jw_ipc_storage_status_info storage;
    if (jw_ipc_get_storage_status(ui->socket_path, "secondary_sd",
                                  &storage, NULL, 0) != 0) {
        return;
    }
    snprintf(ui->secondary_sd_status, sizeof(ui->secondary_sd_status), "%s",
             storage.busy ? T("Busy") : (storage.mounted ? T("Mounted") : T("Not mounted")));
}

static void jw__refresh_adb(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->adb_enabled = -1;
    ui->adb_intent_enabled = -1;
    ui->adb_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    int enabled = -1;
    int intent = -1;
    bool supported = false;
    if (jw_ipc_get_adb(ui->socket_path, &enabled, &intent, &supported) == 0) {
        ui->adb_supported = supported;
        ui->adb_enabled = enabled;
        ui->adb_intent_enabled = intent;
    }
}

/* Load the rumble/haptics settings straight from the DB (the daemon reads the
   same keys; defaults match the init defaults). */
static void jw__refresh_rumble(jw_settings_ui *ui) {
    char v[16] = "";
    if (jw_db_get_setting(ui->db_path, "rumble_enabled", v, sizeof(v)) == 0 && v[0])
        ui->rumble_enabled = (strcmp(v, "0") != 0);
    v[0] = '\0';
    if (jw_db_get_setting(ui->db_path, "rumble_strength", v, sizeof(v)) == 0 && v[0]) {
        int s = atoi(v);
        ui->rumble_strength = s < 0 ? 0 : (s > 100 ? 100 : s);
    }
    v[0] = '\0';
    if (jw_db_get_setting(ui->db_path, "rumble_nav", v, sizeof(v)) == 0 && v[0])
        ui->rumble_nav = (strcmp(v, "1") == 0);
    v[0] = '\0';
    if (jw_db_get_setting(ui->db_path, "rumble_game", v, sizeof(v)) == 0 && v[0])
        ui->rumble_game = (strcmp(v, "0") != 0);
}

static void jw__refresh_boot_splash(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->boot_splash_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    int enabled = -1;
    bool supported = false;
    if (jw_ipc_get_boot_splash(ui->socket_path, &enabled, &supported) == 0) {
        ui->boot_splash_supported = supported;
        if (enabled >= 0) {
            ui->boot_splash_enabled = enabled != 0;
        }
    }
}

static void jw__refresh_refresh_rate(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->refresh_rate_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    int hz = -1;
    bool supported = false;
    if (jw_ipc_get_refresh_rate(ui->socket_path, &hz, &supported) == 0) {
        ui->refresh_rate_supported = supported;
        /* Reflect the panel's actual current rate when the daemon reports it
           (truth over the persisted mirror, e.g. after moving the card). Any
           positive rate is taken as-is rather than filtered against the offered
           list: the live mode IS the truth, so a panel left at a retired rate
           (90 Hz, from before it left the menu) must read 90 rather than have
           the row quietly claim 60. Cycling off it then retires it for good. */
        if (hz > 0) {
            ui->refresh_rate_hz = hz;
        }
    }
}

static void jw__refresh_hdmi(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->hdmi_supported = false;
    ui->hdmi_connected = -1;
    if (!ui->socket_path[0]) {
        return;
    }
    int connected = -1, mode = -1;
    bool supported = false;
    if (jw_ipc_get_hdmi_status(ui->socket_path, &connected, &mode, &supported) == 0) {
        ui->hdmi_supported = supported;
        ui->hdmi_connected = connected;
        /* The persisted setting is the source of truth for the chosen mode; only
           adopt the daemon's live mode when it actually has one applied. */
        if (mode >= 0 && mode <= 2) {
            ui->hdmi_output_mode = mode;
        }
    }
}

static void jw__refresh_performance(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->performance_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    jw_ipc_performance_status_info status;
    if (jw_ipc_get_performance_status(ui->socket_path, &status, NULL, 0) != 0) {
        return;
    }
    ui->performance_supported = status.supported;
    jw_platform_perf_profile profile;
    if (jw_platform_parse_perf_profile(status.global_profile, &profile)) {
        ui->game_perf_profile = jw__game_perf_index_for_profile(profile);
    }
}

static void jw__update_msg(jw_settings_ui *ui, const char *fmt, ...) {
    if (!ui) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->update_msg, sizeof(ui->update_msg), fmt ? fmt : "", ap);
    va_end(ap);
    ui->update_msg_ms = SDL_GetTicks();
    if (ui->update_msg_ms == 0) {
        ui->update_msg_ms = 1;
    }
}

static void jw__refresh_update_status(jw_settings_ui *ui, bool quiet) {
    if (!ui) {
        return;
    }
    if (!ui->socket_path[0]) {
        ui->update_have_status = false;
        if (!quiet) {
            jw__update_msg(ui, "Update service unavailable");
        }
        return;
    }

    char status[192] = { 0 };
    if (jw_ipc_update_status(ui->socket_path, &ui->update,
                             status, sizeof(status)) == 0) {
        ui->update_have_status = true;
        if (!quiet && status[0]) {
            jw__update_msg(ui, "%s", status);
        }
    } else {
        ui->update_have_status = false;
        if (!quiet) {
            jw__update_msg(ui, "%s", status[0] ? status : "Update status unavailable");
        }
    }
}

bool jw_settings_ui_wants_update_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_UPDATE &&
           (ui->update.download_active || ui->update.install_active);
}

void jw_settings_ui_refresh_update(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }

    if (ui->update_msg_ms && (int)(SDL_GetTicks() - ui->update_msg_ms) > 8000) {
        ui->update_msg[0] = '\0';
        ui->update_msg_ms = 0;
    }

    unsigned now = SDL_GetTicks();
    if (ui->update_next_poll_ms != 0 &&
        (int)(now - ui->update_next_poll_ms) < 0) {
        return;
    }

    jw__refresh_update_status(ui, true);
    ui->update_next_poll_ms =
        now + (jw_settings_ui_wants_update_poll(ui) ? 500u : 3000u);
    if (jw_settings_ui_wants_update_poll(ui)) {
        cat_request_frame_in(250);
    }
}

static int jw__bt_row_count(const jw_settings_ui *ui) {
    if (!ui || !ui->bt_radio_on) {
        return JW_BLUETOOTH_FIXED_ROWS;
    }
    /* Power + name + Paired header + paired devices + Nearby header + nearby devices. */
    return JW_BLUETOOTH_FIXED_ROWS + 2 + ui->bt_paired_count + ui->bt_nearby_count;
}

static void jw__bt_msg(jw_settings_ui *ui, const char *fmt, ...) {
    if (!ui) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->bt_msg, sizeof(ui->bt_msg), fmt, ap);
    va_end(ap);
    ui->bt_msg_ms = SDL_GetTicks();
    if (ui->bt_msg_ms == 0) {
        ui->bt_msg_ms = 1;
    }
}

static const jw_bt_device_t *jw__bt_cached_device_in_lists(
        const jw_bt_device_t *paired, int paired_count,
        const jw_bt_device_t *nearby, int nearby_count,
        const char *mac) {
    if (!mac || !jw_bt_mac_valid(mac)) {
        return NULL;
    }
    for (int i = 0; paired && i < paired_count; i++) {
        if (strcmp(paired[i].mac, mac) == 0) {
            return &paired[i];
        }
    }
    for (int i = 0; nearby && i < nearby_count; i++) {
        if (strcmp(nearby[i].mac, mac) == 0) {
            return &nearby[i];
        }
    }
    return NULL;
}

static bool jw__bt_name_is_mac(const char *name) {
    return name && jw_bt_mac_valid(name);
}

static void jw__bt_merge_summary_device(const jw_bt_device_t *old_paired,
                                        int old_paired_count,
                                        const jw_bt_device_t *old_nearby,
                                        int old_nearby_count,
                                        const jw_bt_device_t *summary,
                                        jw_bt_device_t *out) {
    if (!summary || !out) {
        return;
    }

    const jw_bt_device_t *cached = jw__bt_cached_device_in_lists(
        old_paired, old_paired_count, old_nearby, old_nearby_count, summary->mac);
    if (cached) {
        *out = *cached;
    } else {
        *out = *summary;
    }

    if (summary->name[0] &&
        (!out->name[0] || jw__bt_name_is_mac(out->name) ||
         !jw__bt_name_is_mac(summary->name))) {
        snprintf(out->name, sizeof(out->name), "%s", summary->name);
    }
    if (summary->alias[0] &&
        (!out->alias[0] || jw__bt_name_is_mac(out->alias) ||
         !jw__bt_name_is_mac(summary->alias))) {
        snprintf(out->alias, sizeof(out->alias), "%s", summary->alias);
    }
    if ((out->kind == JW_BT_DEVICE_UNKNOWN || out->kind == JW_BT_DEVICE_OTHER) &&
        summary->kind != JW_BT_DEVICE_UNKNOWN &&
        summary->kind != JW_BT_DEVICE_OTHER) {
        out->kind = summary->kind;
    }

    snprintf(out->mac, sizeof(out->mac), "%s", summary->mac);
    out->paired = summary->paired;
    out->connected = summary->connected;
}

static void jw__bt_merge_summary_list(const jw_bt_device_t *old_paired,
                                      int old_paired_count,
                                      const jw_bt_device_t *old_nearby,
                                      int old_nearby_count,
                                      jw_bt_device_t *dst,
                                      int *dst_count,
                                      const jw_bt_device_t *summary,
                                      int summary_count) {
    if (!dst || !dst_count || !summary || summary_count < 0) {
        return;
    }
    int count = summary_count > JW_BT_MAX_DEVICES ? JW_BT_MAX_DEVICES : summary_count;
    jw_bt_device_t merged[JW_BT_MAX_DEVICES];
    for (int i = 0; i < count; i++) {
        jw__bt_merge_summary_device(old_paired, old_paired_count,
                                    old_nearby, old_nearby_count,
                                    &summary[i], &merged[i]);
    }
    memcpy(dst, merged, sizeof(jw_bt_device_t) * (size_t)count);
    *dst_count = count;
}

static void jw__refresh_bluetooth_lists(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }

    if (!jw_bt_available()) {
        memset(&ui->bt_status, 0, sizeof(ui->bt_status));
        ui->bt_radio_on = false;
        ui->bt_paired_count = 0;
        ui->bt_nearby_count = 0;
        ui->bluetooth_list.cursor = 0;
        ui->bluetooth_list.scroll_offset = 0;
        ui->bt_state_cached = 0;
        return;
    }

    int rows = 0;
    jw_bt_status_t status;
    if (jw_bt_status(&status) != 0) {
        if (!ui->bt_status.available &&
            ui->bt_paired_count == 0 &&
            ui->bt_nearby_count == 0) {
            ui->bt_radio_on = false;
        }
        goto clamp_cursor;
    }

    ui->bt_status = status;
    ui->bt_radio_on = ui->bt_status.powered || jw_bt_radio_is_on();
    if (!ui->bt_radio_on) {
        ui->bt_paired_count = 0;
        ui->bt_nearby_count = 0;
        goto clamp_cursor;
    }

    jw_bt_device_t paired[JW_BT_MAX_DEVICES];
    jw_bt_device_t nearby[JW_BT_MAX_DEVICES];
    int paired_count = 0;
    int nearby_count = 0;
    if (jw_bt_list_summaries(paired, JW_BT_MAX_DEVICES, &paired_count,
                             nearby, JW_BT_MAX_DEVICES, &nearby_count) == 0) {
        jw_bt_device_t old_paired[JW_BT_MAX_DEVICES];
        jw_bt_device_t old_nearby[JW_BT_MAX_DEVICES];
        int old_paired_count = ui->bt_paired_count;
        int old_nearby_count = ui->bt_nearby_count;
        memcpy(old_paired, ui->bt_paired, sizeof(old_paired));
        memcpy(old_nearby, ui->bt_nearby, sizeof(old_nearby));
        jw__bt_merge_summary_list(old_paired, old_paired_count,
                                  old_nearby, old_nearby_count,
                                  ui->bt_paired, &ui->bt_paired_count,
                                  paired, paired_count);
        jw__bt_merge_summary_list(old_paired, old_paired_count,
                                  old_nearby, old_nearby_count,
                                  ui->bt_nearby, &ui->bt_nearby_count,
                                  nearby, nearby_count);
    }

clamp_cursor:
    rows = jw__bt_row_count(ui);
    if (rows < 1) {
        rows = 1;
    }
    if (ui->bluetooth_list.cursor >= rows) {
        ui->bluetooth_list.cursor = rows - 1;
    }
    /* Keep the status-bar icon in sync with the radio. While the Bluetooth page
       is open the background status poller masks the BT sample (the page owns the
       live read), so the icon's cached state is only refreshed from here. */
    if (ui->show_bluetooth) {
        ui->bt_state_cached = ui->bt_radio_on
                                  ? (jw_bt_any_connected() == 1 ? 2 : 1)
                                  : 0;
    }
}

static bool jw__bt_scan_start(jw_settings_ui *ui, bool manual) {
    if (!ui || ui->bt_op != JW_BT_OP_NONE) {
        return false;
    }
    if (!jw_bt_available()) {
        if (manual) {
            jw__bt_msg(ui, "Bluetooth unavailable");
        }
        return false;
    }
    if (!ui->bt_radio_on) {
        if (manual) {
            jw__bt_msg(ui, "Bluetooth is off");
        }
        return false;
    }
    if (jw_bt_scan_start() != 0) {
        if (manual) {
            jw__bt_msg(ui, "Could not start Bluetooth scan");
        }
        return false;
    }
    ui->bt_op = JW_BT_OP_SCAN;
    ui->bt_op_manual = manual;
    ui->bt_next_scan_ms = SDL_GetTicks() + JW_BT_SCAN_INTERVAL_MS;
    if (manual) {
        jw__bt_msg(ui, "Scanning Bluetooth...");
    }
    cat_request_frame_in(250);
    return true;
}

bool jw_settings_ui_wants_bluetooth_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_BLUETOOTH;
}

static void jw__set_audio_output(jw_settings_ui *ui, jw_platform_audio_output output,
                                 char *status_buf, size_t status_size);

void jw_settings_ui_refresh_bluetooth(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }

    if (!jw_bt_available()) {
        ui->bt_op = JW_BT_OP_NONE;
        ui->bt_op_manual = false;
        jw__refresh_bluetooth_lists(ui);
        return;
    }

    if (ui->bt_msg_ms && (int)(SDL_GetTicks() - ui->bt_msg_ms) > 6000) {
        ui->bt_msg[0] = '\0';
        ui->bt_msg_ms = 0;
    }

    if (ui->bt_op != JW_BT_OP_NONE) {
        char message[128] = { 0 };
        jw_bt_operation_status st =
            (ui->bt_op == JW_BT_OP_SCAN)
            ? jw_bt_scan_poll(message, sizeof(message))
            : jw_bt_connect_poll(message, sizeof(message));
        if (st == JW_BT_OP_RUNNING) {
            cat_request_frame_in(250);
            return;
        }
        if (st == JW_BT_OP_OK || st == JW_BT_OP_FAILED || st == JW_BT_OP_TIMEOUT) {
            bool was_connect = (ui->bt_op != JW_BT_OP_SCAN);
            bool quiet_auto_scan = (ui->bt_op == JW_BT_OP_SCAN && !ui->bt_op_manual);
            if (!quiet_auto_scan) {
                jw__bt_msg(ui, "%s", message[0] ? message :
                           (st == JW_BT_OP_OK ? "Bluetooth done" : "Bluetooth failed"));
            }
            ui->bt_op = JW_BT_OP_NONE;
            ui->bt_op_manual = false;
            jw__refresh_bluetooth_lists(ui);
            ui->bt_next_poll_ms = SDL_GetTicks() + JW_BT_POLL_INTERVAL_MS;
            /* A successful connect should make the headset the audio output on its
               own — otherwise you connect headphones and still hear the speaker
               until you dig into Sound settings. The route is guarded daemon-side
               (BLUETOOTH output is only "available" when an audio sink is actually
               connected), so connecting a non-audio device here is a no-op. */
            if (was_connect && st == JW_BT_OP_OK) {
                char route_status[128];
                jw__set_audio_output(ui, JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH,
                                     route_status, sizeof(route_status));
            }
            cat_request_frame();
            return;
        }
    }

    unsigned now = SDL_GetTicks();
    if (ui->bt_next_poll_ms == 0 || (int)(now - ui->bt_next_poll_ms) >= 0) {
        jw__refresh_bluetooth_lists(ui);
        ui->bt_next_poll_ms = now + JW_BT_POLL_INTERVAL_MS;
    }

    /* No background auto-scan. It re-scanned every ~12s, and each scan pinned the
       page on "Bluetooth is busy" for ~8s and churned the BT radio (which starves
       WiFi on this combo chip). Discovery is manual only now: the Paired list is
       always shown, and the user presses Scan (X) when they want to find a new
       device to pair. */
}

/* Push the current LED config to jawakad (applies to hardware + persists). */
static void jw__apply_led(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    char status[64];
    int mode = ui->led_mode;
    if (mode < 0 || mode >= JW_LED_MODE_COUNT) mode = 0;
    jw_ipc_set_led(ui->socket_path, ui->led_enabled ? 1 : 0,
                   jw_led_mode_name((jw_led_mode)mode),
                   ui->led_color.r, ui->led_color.g, ui->led_color.b,
                   ui->led_brightness, ui->led_speed, status, sizeof(status));
}

static void jw__apply_persisted_overrides_from_values(
        char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX],
        const unsigned char found[JW_SETTING_COUNT]) {
    ap_theme *t = cat_get_theme();

    {
        int idx = jw__setting_has(values, found, JW_SETTING_PILL_SHAPE_INDEX)
                  ? atoi(values[JW_SETTING_PILL_SHAPE_INDEX])
                  : JW_SETTINGS_PILL_SHAPE_DEFAULT;   /* fresh install → Leaf */
        if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT) {
            t->pill_radius_ratio = kJawakaPillRadiusValues[idx];
            t->pill_corner_mask  = kJawakaPillCornerMasks[idx];
        }
    }

    /* Font size and family can reload the glyph atlas; keep the same behavior
       as jw_settings_apply_persisted_overrides(), but use the already-loaded
       setting values instead of re-opening SQLite for each key. */
    {
        int bump = cat_get_font_bump();
        if (jw__setting_has(values, found, JW_SETTING_FONT_SIZE_INDEX)) {
            int idx = atoi(values[JW_SETTING_FONT_SIZE_INDEX]);
            if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT)
                bump = kJawakaFontSizeValues[idx];
        }
        /* Always resolve and apply the selected family explicitly. We used to
           short-circuit when CAT_FONT_PATH was inherited from the daemon and only
           set the bump — but cat_set_font_bump reloads via theme.font_path, which
           can be empty here, so a font-size change fell back through the candidate
           list to res/font.ttf and clobbered the family to the rounded default.
           Setting the path first keeps both family and bump correct on every apply. */
        int fidx = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
        if (jw__setting_has(values, found, JW_SETTING_FONT_FAMILY_INDEX)) {
            int idx = atoi(values[JW_SETTING_FONT_FAMILY_INDEX]);
            if (idx >= 0 && idx < JW_APPEARANCE_FONT_FAMILY_COUNT)
                fidx = idx;
        }
        /* Language-aware: this path re-applies appearance from the DB and would
           otherwise clobber the CJK face the daemon resolved into our
           environment at spawn, putting the themed Latin family back. */
        snprintf(t->font_path, sizeof(t->font_path), "%s",
                 jw_appearance_font_path_for_language(fidx, jw_i18n_language()));
        if (bump != cat_get_font_bump())
            cat_set_font_bump(bump);
        else
            cat_reload_fonts(t->font_path);
    }

    if (jw__setting_has(values, found, JW_SETTING_ACCENT_COLOR))
        t->accent = cat_hex_to_color(values[JW_SETTING_ACCENT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_TEXT_COLOR))
        t->text = cat_hex_to_color(values[JW_SETTING_TEXT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_HINT_COLOR))
        t->hint = cat_hex_to_color(values[JW_SETTING_HINT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_HIGHLIGHT_COLOR))
        t->highlight = cat_hex_to_color(values[JW_SETTING_HIGHLIGHT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_BG_COLOR))
        t->background = cat_hex_to_color(values[JW_SETTING_BG_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_BUTTON_LABEL_COLOR))
        t->button_label = cat_hex_to_color(values[JW_SETTING_BUTTON_LABEL_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_BUTTON_GLYPH_BG_COLOR))
        t->button_glyph_bg = cat_hex_to_color(values[JW_SETTING_BUTTON_GLYPH_BG_COLOR]);

    cat_finalize_theme_colors(t);

    /* Apply the persisted time zone so the clock is correct from launch. */
    if (jw__setting_has(values, found, JW_SETTING_TIMEZONE) &&
        values[JW_SETTING_TIMEZONE][0])
        jw__apply_timezone(values[JW_SETTING_TIMEZONE]);
}

void jw_settings_toggle_led(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    /* Reflect the daemon's current state first so the toggle is correct even if
       it was last changed elsewhere, then flip and apply. */
    jw__refresh_led(ui);
    ui->led_enabled = !ui->led_enabled;
    jw__apply_led(ui);
}

static void jw__persist(const jw_settings_ui *ui, const char *key, const char *val) {
    if (ui->db_path[0])
        jw_db_set_setting(ui->db_path, key, val);
    /* Haptic: a setting changed. Skip keys with their own live feedback
       (strength = slider preview, enable = confirmation buzz). Gated in the
       daemon by rumble_enabled, so nothing buzzes when haptics are off. */
    if (ui->socket_path[0] &&
        strcmp(key, "rumble_strength") != 0 &&
        strcmp(key, "rumble_enabled") != 0)
        jw_ipc_rumble(ui->socket_path, "select");
}

static void jw__persist_int(const jw_settings_ui *ui, const char *key, int val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    jw__persist(ui, key, buf);
}

/* Persist the current Home Tabs arrangement to "home_tab_order" and, if the
   persisted Startup Tab is now hidden, snap it to the first visible tab and
   persist that too (so the launcher never boots onto a hidden tab). */
static void jw__home_tabs_persist(jw_settings_ui *ui) {
    char csv[JW_SETTINGS_VALUE_MAX] = "";
    jw__home_tab_order_to_csv(ui->home_tab_order, ui->home_tab_visible,
                              csv, sizeof(csv));
    jw__persist(ui, "home_tab_order", csv);

    bool startup_visible = false;
    for (int i = 0; i < ui->home_tab_visible; i++)
        if (ui->home_tab_order[i] == ui->startup_tab_index) { startup_visible = true; break; }
    if (!startup_visible && ui->home_tab_visible > 0) {
        ui->startup_tab_index = ui->home_tab_order[0];
        jw__persist_int(ui, "startup_tab_index", ui->startup_tab_index);
    }
}

static void jw__persist_bool(const jw_settings_ui *ui, const char *key, bool val) {
    jw__persist(ui, key, val ? "1" : "0");
}

static void jw__persist_color(const jw_settings_ui *ui, const char *key, ap_color c) {
    char hex[16];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", c.r, c.g, c.b);
    jw__persist(ui, key, hex);
}

/* ─── Services (app-services-v1) ───────────────────────────────────────── */

/* Refresh the CTL-1 service-list snapshot. Returns the number of services
 * known to the daemon (0 = none, -1 = the query failed / daemon has no
 * supervisor). The Services screen is offered only when this is > 0. */
static int jw__refresh_services(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) {
        return -1;
    }
    jw_ipc_service_info refreshed[JW_IPC_SVC_LIST_MAX];
    int count = 0;
    if (jw_ipc_service_list(ui->socket_path, refreshed,
                            JW_IPC_SVC_LIST_MAX, &count) != 0) {
        return -1;
    }
    int visible = count;
    if (visible > 0) {
        memcpy(ui->services, refreshed,
               (size_t)visible * sizeof(ui->services[0]));
    }
    ui->services_count = visible;
    if (visible <= 0) {
        ui->services_list.cursor = 0;
        ui->services_list.scroll_offset = 0;
        if (ui->screen == JW_SETTINGS_SERVICES) {
            ui->screen = JW_SETTINGS_HOME;
            /* The disappearing Services row was the final Home category.
             * Clamp both list coordinates before the next render/input pass
             * sees the now-shorter list. */
            int home_count = JW_SETTINGS_CATEGORY_COUNT - 1;
            if (ui->home_list.cursor >= home_count) {
                ui->home_list.cursor = home_count - 1;
            }
            if (ui->home_list.scroll_offset > ui->home_list.cursor) {
                ui->home_list.scroll_offset = ui->home_list.cursor;
            }
        }
    } else if (ui->services_list.cursor >= visible) {
        ui->services_list.cursor = visible - 1;
        if (ui->services_list.scroll_offset > ui->services_list.cursor) {
            ui->services_list.scroll_offset = ui->services_list.cursor;
        }
    }
    return visible;
}

/* SVC-1's hiding rule: CTL-1 omits invalid-discovery-only rows, but includes
 * valid services, retained desired state, and actionable stale generations.
 * A non-empty canonical list therefore means the screen is relevant. */
static bool jw__services_available(jw_settings_ui *ui) {
    if (ui) ui->services_count = 0;
    return jw__refresh_services(ui) > 0;
}

/* Defined with the General page renderer. The count is dynamic because the
   Language row exists only when a translation is installed. */
static int jw__behavior_rows(const jw_settings_ui *ui);

static int jw__home_category_count(const jw_settings_ui *ui) {
    /* Services is the final category and is genuinely absent on a clean
       system, per SVC-1. The snapshot is refreshed whenever Settings is
       entered and when leaving the Services screen. */
    return JW_SETTINGS_CATEGORY_COUNT -
           ((!ui || ui->services_count <= 0) ? 1 : 0);
}

bool jw_settings_ui_wants_services_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_SERVICES;
}

void jw_settings_ui_refresh_services(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    unsigned now = SDL_GetTicks();
    if (ui->services_next_poll_ms != 0 &&
        (int)(now - ui->services_next_poll_ms) < 0) {
        return; /* not due yet */
    }
    int refresh_result = jw__refresh_services(ui);
    if (refresh_result < 0 && ui->screen == JW_SETTINGS_SERVICES) {
        snprintf(ui->services_msg, sizeof(ui->services_msg), "%s",
                 "Service status unavailable");
    } else if (refresh_result >= 0 &&
               strcmp(ui->services_msg, "Service status unavailable") == 0) {
        ui->services_msg[0] = '\0';
    }
    ui->services_next_poll_ms = now + JW_SERVICES_POLL_INTERVAL_MS;
}

/* ─── Lifecycle ────────────────────────────────────────────────────────── */

void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                          const char *initial_theme_name,
                          const char *socket_path) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->open   = false;
    ui->screen = JW_SETTINGS_HOME;
    ui->wifi_monitor_fd = -1;
    ui->adb_enabled = -1;
    ui->adb_intent_enabled = -1;
    ui->update_have_status = false;
    ui->update_next_poll_ms = 0;
    ui->update_bar_pct = 0.0f;
    ui->bt_op = JW_BT_OP_NONE;
    ui->bt_op_manual = false;
    cat_list_state_init(&ui->home_list,       JW_SETTINGS_CATEGORY_COUNT);
    cat_list_state_init(&ui->appearance_list,  JW_APPEAR_ROW_COUNT);
    cat_list_state_init(&ui->colors_list,      JW_COLOR_ROW_COUNT);
    cat_list_state_init(&ui->layout_list,      JW_LAYOUT_ROW_COUNT);
    cat_list_state_init(&ui->statusbar_list,   JW_STATUSBAR_ROW_COUNT);
    cat_list_state_init(&ui->display_list,     JW_SETTINGS_DISPLAY_COUNT);
    cat_list_state_init(&ui->network_list,     JW_WIFI_LIST_ROWS);
    cat_list_state_init(&ui->bluetooth_list,   JW_BLUETOOTH_LIST_ROWS);
    cat_list_state_init(&ui->lighting_list,    JW_LIGHTING_ROW_COUNT);
    cat_list_state_init(&ui->accounts_list,    JW_ACCOUNTS_ROW_COUNT);
    cat_list_state_init(&ui->scraping_list,    JW_SCRAPING_ROW_COUNT);
    cat_list_state_init(&ui->scrape_edit_list, 8);
    cat_list_state_init(&ui->scrape_download_list, 8);
    /* en is always offered; the rest are whatever tables are installed. Resolved
       once at init rather than per frame -- this stats the filesystem. */
    snprintf(ui->languages[0], sizeof(ui->languages[0]), "%s", "en");
    ui->language_count = 1;
    {
        const char *found[8];
        size_t n = jw_i18n_available(found, 8);
        for (size_t i = 0; i < n && ui->language_count < 8; i++) {
            snprintf(ui->languages[ui->language_count],
                     sizeof(ui->languages[0]), "%s", found[i]);
            ui->language_count++;
        }
    }
    snprintf(ui->language, sizeof(ui->language), "%s", jw_i18n_language());

    cat_list_state_init(&ui->behavior_list,    jw__behavior_rows(ui));
    cat_list_state_init(&ui->controls_list,     JW_CONTROLS_ROW_COUNT);
    cat_list_state_init(&ui->home_tabs_list,   JW_HOME_TABS_COUNT);
    cat_list_state_init(&ui->update_list,      JW_UPDATE_ROW_COUNT);
    cat_list_state_init(&ui->update_picker_list, JW_UPDATE_PICKER_VISIBLE_ROWS);
    cat_list_state_init(&ui->timezone_picker_list, JW_TIMEZONE_VISIBLE_ROWS);
    cat_list_state_init(&ui->placeholder_list, 1);
    cat_list_state_init(&ui->services_list,    JW_IPC_SVC_LIST_MAX);
    cat_scroll_state_init(&ui->about_scroll);
    ui->theme_index       = jw__find_theme_index(initial_theme_name);
    ui->color_scheme_index = -1;   /* custom until a scheme is loaded below */
    ui->pill_shape_index  = JW_SETTINGS_PILL_SHAPE_DEFAULT;
    ui->font_family_index = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
    ui->timezone[0]       = '\0';  /* "" = follow system tz until the user picks */
    ui->font_size_index   = 1;
    ui->tab_glide         = 1;     /* Glide by default — matches Leaf's soft feel; Snap is opt-out */
    ui->update_channel_index = JW_UPDATE_CHANNEL_STABLE_IDX;  /* Stable unless loaded below */
    ui->show_hints        = true;
    ui->clock_style_index = 1;
    ui->show_battery      = true;
    ui->show_battery_level = false;
    ui->show_wifi         = true;
    ui->show_bluetooth    = true;
    ui->show_volume       = true;
    ui->startup_tab_index = JW_STARTUP_TAB_DEFAULT;
    jw__parse_home_tab_order(NULL, ui->home_tab_order, &ui->home_tab_visible);
    ui->home_tabs_grabbed = false;
    ui->auto_sleep_index  = JW_AUTO_SLEEP_DEFAULT;
    ui->boot_splash_enabled = true;
    ui->boot_splash_supported = false;
    ui->screenshots_enabled = false;   /* opt-in */
    ui->recording_enabled   = false;   /* opt-in, same as screenshots */
    /* On by default: it is inert for clips that already fit, and without it a
       long recording hands you a file too big to post with no way back except
       flipping this and re-converting. */
    ui->recording_split     = true;
    /* The .mkv holds lossless audio and is the only re-convertible source, so
       discarding it should be a deliberate choice. */
    ui->recording_keep_src  = true;
    ui->rumble_enabled = true;    /* haptics default on */
    ui->rumble_strength = 65;     /* ~Medium */
    ui->rumble_nav = false;       /* per-move tick opt-in */
    ui->rumble_game = true;       /* in-game rumble default on */
    ui->layout_mode = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW)
                          ? 1 : 0;
    ui->refresh_rate_hz   = 60;
    ui->refresh_rate_supported = false;
    ui->bfi_enabled       = false;
    ui->hdmi_output_mode  = 0;       /* off */
    ui->hdmi_connected    = -1;
    ui->hdmi_supported    = false;
    ui->game_perf_profile = JW_GAME_PERF_PROFILE_DEFAULT;
    ui->performance_supported = false;
    ui->brightness_percent = 50;
    ui->volume_percent     = 50;
    ui->audio_output       = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    ui->audio_available_outputs = JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_SPEAKER);
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        ui->audio_volumes[i] = -1;
    }
    ui->audio_volumes[JW_PLATFORM_AUDIO_OUTPUT_SPEAKER] = ui->volume_percent;
    ui->led_enabled    = false;
    ui->led_mode       = 0;   /* static */
    ui->led_color      = cat_hex_to_color("#FFFFFF");
    ui->led_brightness = 5;
    ui->led_speed      = 5;
    snprintf(ui->secondary_sd_status, sizeof(ui->secondary_sd_status), "%s",
             "Unavailable");

    if (db_path && db_path[0])
        snprintf(ui->db_path, sizeof(ui->db_path), "%s", db_path);
    if (socket_path && socket_path[0])
        snprintf(ui->socket_path, sizeof(ui->socket_path), "%s", socket_path);

    /* Restore persisted overrides. The index reads below keep the settings
       UI's own state in sync with the DB; the theme itself (all 7 colors,
       pill shape, font size) is applied by the shared override helper. */
    if (ui->db_path[0]) {
        char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX];
        unsigned char found[JW_SETTING_COUNT];
        if (jw__load_setting_values(db_path, values, found) == 0) {
            if (jw__setting_has(values, found, JW_SETTING_PILL_SHAPE_INDEX)) {
                int idx = atoi(values[JW_SETTING_PILL_SHAPE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT)
                    ui->pill_shape_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_FONT_FAMILY_INDEX)) {
                int idx = atoi(values[JW_SETTING_FONT_FAMILY_INDEX]);
                if (idx >= 0 && idx < JW_APPEARANCE_FONT_FAMILY_COUNT)
                    ui->font_family_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_FONT_SIZE_INDEX)) {
                int idx = atoi(values[JW_SETTING_FONT_SIZE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT)
                    ui->font_size_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_TAB_GLIDE))
                ui->tab_glide = (strcmp(values[JW_SETTING_TAB_GLIDE], "0") != 0) ? 1 : 0;
            {
                char chan[16] = "";
                if (jw_db_get_setting(db_path, "update_channel", chan, sizeof(chan)) == 0)
                    ui->update_channel_index = (strcmp(chan, "beta") == 0)
                        ? JW_UPDATE_CHANNEL_BETA_IDX : JW_UPDATE_CHANNEL_STABLE_IDX;
            }
            if (jw__setting_has(values, found, JW_SETTING_SHOW_HINTS))
                ui->show_hints = (strcmp(values[JW_SETTING_SHOW_HINTS], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_CLOCK_STYLE_INDEX)) {
                int idx = atoi(values[JW_SETTING_CLOCK_STYLE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                    ui->clock_style_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_TIMEZONE))
                snprintf(ui->timezone, sizeof(ui->timezone), "%s",
                         values[JW_SETTING_TIMEZONE]);
            if (jw__setting_has(values, found, JW_SETTING_SS_USER))
                snprintf(ui->ss_username, sizeof(ui->ss_username), "%s",
                         values[JW_SETTING_SS_USER]);
            if (jw__setting_has(values, found, JW_SETTING_SS_VERIFIED))
                ui->ss_verified = (strcmp(values[JW_SETTING_SS_VERIFIED], "1") == 0);
            if (jw__setting_has(values, found, JW_SETTING_SS_MAXTHREADS))
                ui->ss_max_threads = atoi(values[JW_SETTING_SS_MAXTHREADS]);
            if (jw__setting_has(values, found, JW_SETTING_SS_REQUESTS_TODAY))
                ui->ss_requests_today = atoi(values[JW_SETTING_SS_REQUESTS_TODAY]);
            if (jw__setting_has(values, found, JW_SETTING_SS_MAX_REQUESTS))
                ui->ss_max_requests = atoi(values[JW_SETTING_SS_MAX_REQUESTS]);
            ui->scrape_artwork_included = jw__scrape_csv_to_order(
                jw__setting_has(values, found, JW_SETTING_SCRAPE_ARTWORK_PRIO)
                    ? values[JW_SETTING_SCRAPE_ARTWORK_PRIO] : NULL,
                jw_ss_media_types, jw_ss_media_types_count,
                jw_ss_default_artwork_priority,
                jw_ss_default_artwork_priority_count,
                ui->scrape_artwork_order);
            ui->scrape_region_included = jw__scrape_csv_to_order(
                jw__setting_has(values, found, JW_SETTING_SCRAPE_REGION_PRIO)
                    ? values[JW_SETTING_SCRAPE_REGION_PRIO] : NULL,
                jw_ss_regions, jw_ss_regions_count,
                jw_ss_default_region_priority,
                jw_ss_default_region_priority_count,
                ui->scrape_region_order);
            if (jw__setting_has(values, found, JW_SETTING_RA_USER))
                snprintf(ui->ra_username, sizeof(ui->ra_username), "%s",
                         values[JW_SETTING_RA_USER]);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY))
                ui->show_battery = (strcmp(values[JW_SETTING_SHOW_BATTERY], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY_LEVEL))
                ui->show_battery_level = (strcmp(values[JW_SETTING_SHOW_BATTERY_LEVEL], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_WIFI))
                ui->show_wifi = (strcmp(values[JW_SETTING_SHOW_WIFI], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BLUETOOTH))
                ui->show_bluetooth = (strcmp(values[JW_SETTING_SHOW_BLUETOOTH], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_VOLUME))
                ui->show_volume = (strcmp(values[JW_SETTING_SHOW_VOLUME], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_COLOR_SCHEME_INDEX)) {
                int idx = atoi(values[JW_SETTING_COLOR_SCHEME_INDEX]);
                if (idx >= 0 && idx < JW_COLOR_SCHEME_COUNT)
                    ui->color_scheme_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_STARTUP_TAB_INDEX)) {
                int idx = atoi(values[JW_SETTING_STARTUP_TAB_INDEX]);
                if (idx >= 0 && idx < JW_STARTUP_TAB_COUNT)
                    ui->startup_tab_index = idx;
            }
            jw__parse_home_tab_order(
                jw__setting_has(values, found, JW_SETTING_HOME_TAB_ORDER)
                    ? values[JW_SETTING_HOME_TAB_ORDER] : NULL,
                ui->home_tab_order, &ui->home_tab_visible);
            /* Stored as seconds (what the daemon reads); map back to the label index. */
            if (jw__setting_has(values, found, JW_SETTING_AUTO_SLEEP_SECONDS)) {
                int seconds = atoi(values[JW_SETTING_AUTO_SLEEP_SECONDS]);
                for (int i = 0; i < JW_AUTO_SLEEP_COUNT; i++) {
                    if (kAutoSleepSeconds[i] == seconds) { ui->auto_sleep_index = i; break; }
                }
            }
            if (jw__setting_has(values, found, JW_SETTING_BOOT_SPLASH_ENABLED))
                ui->boot_splash_enabled = (strcmp(values[JW_SETTING_BOOT_SPLASH_ENABLED], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SCREENSHOTS_ENABLED))
                ui->screenshots_enabled = (strcmp(values[JW_SETTING_SCREENSHOTS_ENABLED], "1") == 0);
            if (jw__setting_has(values, found, JW_SETTING_RECORDING_ENABLED))
                ui->recording_enabled = (strcmp(values[JW_SETTING_RECORDING_ENABLED], "1") == 0);
            /* These two default ON, so an absent key must not read as off -- test
               against "0" rather than for "1" the way the opt-in flags do. */
            if (jw__setting_has(values, found, JW_SETTING_RECORDING_SPLIT))
                ui->recording_split = (strcmp(values[JW_SETTING_RECORDING_SPLIT], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_RECORDING_KEEP_SRC))
                ui->recording_keep_src = (strcmp(values[JW_SETTING_RECORDING_KEEP_SRC], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_REFRESH_RATE_HZ)) {
                int hz = atoi(values[JW_SETTING_REFRESH_RATE_HZ]);
                if (jw__is_panel_refresh_hz(hz)) ui->refresh_rate_hz = hz;
            }
            if (jw__setting_has(values, found, JW_SETTING_BFI_ENABLED))
                ui->bfi_enabled = (strcmp(values[JW_SETTING_BFI_ENABLED], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_HDMI_OUTPUT_MODE)) {
                int m = atoi(values[JW_SETTING_HDMI_OUTPUT_MODE]);
                if (m >= 0 && m <= 2) ui->hdmi_output_mode = m;
            }
            if (jw__setting_has(values, found, JW_SETTING_GAME_PERFORMANCE_PROFILE)) {
                jw_platform_perf_profile profile;
                if (jw_platform_parse_perf_profile(
                        values[JW_SETTING_GAME_PERFORMANCE_PROFILE], &profile)) {
                    ui->game_perf_profile = jw__game_perf_index_for_profile(profile);
                }
            }
            if (jw__setting_has(values, found, JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT))
                ui->brightness_percent = jw_platform_clamp_brightness_percent(
                    atoi(values[JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT]));
            if (jw__setting_has(values, found, JW_SETTING_PLATFORM_VOLUME_PERCENT)) {
                int volume = atoi(values[JW_SETTING_PLATFORM_VOLUME_PERCENT]);
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                ui->volume_percent = volume;
                ui->audio_volumes[JW_PLATFORM_AUDIO_OUTPUT_SPEAKER] = volume;
            }

            jw__apply_persisted_overrides_from_values(values, found);

            /* Fresh install (no color ever persisted) → default to the Leaf scheme,
               the project's identity theme. Returning users keep their colors. */
            if (!jw__setting_has(values, found, JW_SETTING_ACCENT_COLOR))
                jw__apply_color_scheme(ui, JW_COLOR_SCHEME_DEFAULT, NULL);
        }
    }
}

void jw_settings_ui_enter(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = true;
    ui->screen = JW_SETTINGS_HOME;
    ui->services_count = 0;
    (void)jw__refresh_services(ui);
    int home_count = jw__home_category_count(ui);
    if (ui->home_list.cursor >= home_count) {
        ui->home_list.cursor = home_count - 1;
    }
}

void jw_settings_ui_close(jw_settings_ui *ui) {
    if (!ui) return;
    if (ui->bt_op != JW_BT_OP_NONE) {
        jw_bt_cancel_operation();
        ui->bt_op = JW_BT_OP_NONE;
        ui->bt_op_manual = false;
    }
    if (ui->scrape_detail_art) {
        SDL_DestroyTexture(ui->scrape_detail_art);
        ui->scrape_detail_art = NULL;
    }
    ui->scrape_detail_art_w = ui->scrape_detail_art_h = 0;
    ui->open = false;
    ui->screen = JW_SETTINGS_HOME;
}

/* Open the UI directly on a specific screen and prime its state, so a host that
   doesn't go through the Settings home (e.g. jawaka-menu's System popup) lands
   the same as picking the category would. Currently used for About and System
   Update; mirrors the per-screen entry priming the home A-handler does. */
static void jw__update_check_releases(jw_settings_ui *ui, char *status_buf,
                                      size_t status_size);
static void jw__stats_snapshot_invalidate(void);
void jw_settings_ui_open(jw_settings_ui *ui, jw_settings_screen screen) {
    if (!ui) return;
    ui->open = true;
    ui->screen = screen;
    if (screen == JW_SETTINGS_UPDATE) {
        ui->update_list.cursor = 0;
        ui->update_list.scroll_offset = 0;
        ui->update_msg[0] = '\0';
        ui->update_msg_ms = 0;
        ui->update_checked_this_visit = false;  /* don't assert a cached result */
        jw__refresh_update_status(ui, false);
        /* Auto-start a release check on open so the user sees it searching (with a
           spinner) immediately, instead of a stale "Not checked". The daemon runs
           the fetch on a worker thread and returns at once, so this doesn't block. */
        {
            char check_status[192] = { 0 };
            jw__update_check_releases(ui, check_status, sizeof(check_status));
        }
        ui->update_next_poll_ms = SDL_GetTicks() + 500;
    } else if (screen == JW_SETTINGS_ABOUT) {
        cat_scroll_state_init(&ui->about_scroll);   /* start at top */
    } else if (screen == JW_SETTINGS_LIBRARY) {
        cat_scroll_state_init(&ui->library_scroll);
        jw__stats_snapshot_invalidate();   /* re-read fresh on first frame */
    } else if (screen == JW_SETTINGS_PLAYTIME) {
        cat_scroll_state_init(&ui->playtime_scroll);
        jw__stats_snapshot_invalidate();   /* re-read fresh on first frame */
    } else if (screen == JW_SETTINGS_SERVICES) {
        if (jw__refresh_services(ui) <= 0) {
            /* Preserve the same hidden-on-clean-system rule for hosts that
               open a settings page directly instead of using the home list. */
            ui->screen = JW_SETTINGS_HOME;
        } else {
            ui->services_list.cursor = 0;
            ui->services_list.scroll_offset = 0;
            ui->services_msg[0] = '\0';
            ui->services_next_poll_ms =
                SDL_GetTicks() + JW_SERVICES_POLL_INTERVAL_MS;
        }
    }
}

bool jw_settings_ui_is_open(const jw_settings_ui *ui) {
    return ui && ui->open;
}

jw_settings_screen jw_settings_ui_screen(const jw_settings_ui *ui) {
    return ui ? ui->screen : JW_SETTINGS_HOME;
}

bool jw_settings_ui_wants_av_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_DISPLAY;
}

void jw_settings_ui_refresh_av(jw_settings_ui *ui) {
    jw__refresh_brightness(ui);
    jw__refresh_volume(ui);
    jw__refresh_audio_status(ui);
    jw__refresh_led(ui);
}

bool jw_settings_show_hints(const jw_settings_ui *ui) {
    return !ui || ui->show_hints;
}

bool jw_settings_tab_glide(const jw_settings_ui *ui) {
    return ui && ui->tab_glide != 0;
}

void jw_settings_status_bar_opts(const jw_settings_ui *ui, cat_status_bar_opts *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ui) {
        out->show_clock = CAT_CLOCK_AUTO;
        out->show_battery = true;
        out->show_wifi = true;
        return;
    }
    if (ui->clock_style_index == 0) {
        out->show_clock = CAT_CLOCK_HIDE;
    } else {
        out->show_clock = CAT_CLOCK_SHOW;
        out->use_24h = (ui->clock_style_index == 1);
        out->no_ampm = (ui->clock_style_index == 3);
    }
    out->show_battery = ui->show_battery;
    out->show_battery_level = ui->show_battery_level;
    out->show_wifi = ui->show_wifi;
    /* We own the radio read (jw_wifi_*), so feed the icon our value rather than
       letting Catastrophe shell out itself — keeps the icon and Network page on
       one source. */
    out->wifi_supplied = true;
    out->wifi_strength = ui->show_wifi ? ui->wifi_strength_cached : 0;
    out->show_bluetooth = ui->show_bluetooth;
    out->bt_state = ui->show_bluetooth ? ui->bt_state_cached : 0;
    out->show_volume = ui->show_volume;
    out->volume_percent = ui->show_volume ? ui->volume_percent : -1;
}

bool jw_settings_show_volume(const jw_settings_ui *ui) {
    return ui && ui->show_volume;
}

void jw_settings_ui_refresh_volume(jw_settings_ui *ui) {
    jw__refresh_volume(ui);
}

bool jw_settings_show_wifi(const jw_settings_ui *ui) {
    return ui && ui->show_wifi;
}

void jw_settings_ui_refresh_wifi_strength(jw_settings_ui *ui) {
    if (ui) {
        ui->wifi_strength_cached = jw_wifi_strength_now();
    }
}

int jw_settings_bt_state_now(void) {
    if (!jw_bt_radio_is_on()) return 0;
    return (jw_bt_any_connected() == 1) ? 2 : 1;
}

void jw_settings_ui_refresh_bt_state(jw_settings_ui *ui) {
    if (ui) {
        ui->bt_state_cached = jw_settings_bt_state_now();
    }
}

void jw_settings_load_status_prefs(const char *db_path,
                                   cat_status_bar_opts *out_opts,
                                   bool *out_show_hints) {
    /* Defaults match jw_settings_ui_init: hints on, 24h clock, battery + wifi on. */
    int  clock_style_index = 1;
    bool show_battery      = true;
    bool show_battery_level = false;
    bool show_wifi         = true;
    bool show_bluetooth    = true;
    bool show_volume       = true;
    int  volume_percent    = -1;
    bool show_hints        = true;

    if (db_path && db_path[0]) {
        char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX];
        unsigned char found[JW_SETTING_COUNT];
        if (jw__load_setting_values(db_path, values, found) == 0) {
            if (jw__setting_has(values, found, JW_SETTING_CLOCK_STYLE_INDEX)) {
                int idx = atoi(values[JW_SETTING_CLOCK_STYLE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                    clock_style_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY))
                show_battery = (strcmp(values[JW_SETTING_SHOW_BATTERY], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY_LEVEL))
                show_battery_level = (strcmp(values[JW_SETTING_SHOW_BATTERY_LEVEL], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_WIFI))
                show_wifi = (strcmp(values[JW_SETTING_SHOW_WIFI], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BLUETOOTH))
                show_bluetooth = (strcmp(values[JW_SETTING_SHOW_BLUETOOTH], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_VOLUME))
                show_volume = (strcmp(values[JW_SETTING_SHOW_VOLUME], "0") != 0);
            /* The menu has no live volume poll; use the daemon's last persisted value. */
            if (jw__setting_has(values, found, JW_SETTING_PLATFORM_VOLUME_PERCENT))
                volume_percent = atoi(values[JW_SETTING_PLATFORM_VOLUME_PERCENT]);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_HINTS))
                show_hints = (strcmp(values[JW_SETTING_SHOW_HINTS], "0") != 0);
        }
    }

    if (out_opts) {
        memset(out_opts, 0, sizeof(*out_opts));
        if (clock_style_index == 0) {
            out_opts->show_clock = CAT_CLOCK_HIDE;
        } else {
            out_opts->show_clock = CAT_CLOCK_SHOW;
            out_opts->use_24h = (clock_style_index == 1);
            out_opts->no_ampm = (clock_style_index == 3);
        }
        out_opts->show_battery = show_battery;
        out_opts->show_battery_level = show_battery_level;
        out_opts->show_wifi = show_wifi;
        out_opts->show_bluetooth = show_bluetooth;
        out_opts->bt_state = show_bluetooth ? jw_settings_bt_state_now() : 0;
        out_opts->show_volume = show_volume;
        out_opts->volume_percent = show_volume ? volume_percent : -1;
    }
    if (out_show_hints)
        *out_show_hints = show_hints;
}

/* ─── Render helpers ───────────────────────────────────────────────────── */

static void jw__draw_header(const char *title, int x, int y, int w) {
    /* Translated here rather than at the 22 call sites. Safe for any string:
       T() returns its argument on a miss, so a dynamic title passes through. */
    title = T(title);
    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);
    int large_h = TTF_FontHeight(large);
    cat_draw_text_ellipsized(large, title, x + cat_scale(4), y, theme->text, w - cat_scale(8));
    cat_draw_rect(x, y + large_h + cat_scale(4), w, 1, cat_hex_to_color("#ffffff20"));
}

static int jw__header_h(void) {
    return TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + cat_scale(10);
}

/* Canonical vertical advance for one sub-header caption line — the single source
   of truth for sub-header line spacing across every settings page. */
static int jw__subheader_line_h(TTF_Font *f) {
    return TTF_FontHeight(f) + cat_scale(8);
}

/* One settings page = a single full-width column: the title header strip, an
   optional sub-header strip, then the content box (mirrors jw__browse_boxes with
   the cover column collapsed to zero). cat_box_carve_top with height 0 is a no-op,
   so a header-less / sub-header-less page costs nothing. Pure geometry, no drawing:
   the title still draws via jw__draw_header (its geometry equals the carved header
   strip); info lines draw into *sub; rows/list fill the returned content rect.
   *header and *sub are nullable. */
static SDL_Rect jw__settings_boxes(int x, int y, int w, int h,
                                   bool show_header, int sub_h,
                                   SDL_Rect *header, SDL_Rect *sub) {
    cat_box page = { x, y, w, h, 0, 0, 0, 0 };
    cat_box hdr = cat_box_carve_top(&page, show_header ? jw__header_h() : 0);
    cat_box sb  = cat_box_carve_top(&page, sub_h);
    if (header) *header = cat_box_content(&hdr);
    if (sub)    *sub    = cat_box_content(&sb);
    return cat_box_content(&page);
}

/* Fixed-row settings pages pre-render the same moving focus layer used by the
   scrollable lists, caching each row's coverage for continuous text colors. */
#define JW__SETTINGS_FOCUS_CACHE_MAX 64
static const cat_list_state *jw__settings_focus_list;
static float jw__settings_focus_cache[JW__SETTINGS_FOCUS_CACHE_MAX];
static int jw__settings_focus_count;

static float jw__settings_row_focus(const cat_list_state *list, int row) {
    if (list == jw__settings_focus_list && row >= 0 &&
        row < jw__settings_focus_count) {
        return jw__settings_focus_cache[row];
    }
    return list && list->cursor == row ? 1.0f : 0.0f;
}

static void jw__begin_settings_rows(const cat_list_state *list,
                                    int x, int y, int w,
                                    int item_count, int item_h);

static void jw__render_list_row_impl(const cat_list_state *list, int x, int y,
                                     int w, int row, const char *label,
                                     const char *value, bool cycler, int item_h,
                                     const ap_color *value_override) {
    /* Both strings are translated here, not at the 45 call sites. T() falls back
       to its argument, so values that are data rather than UI text -- a game
       count, a timezone, a Bluetooth device name -- miss the table and pass
       through unchanged. That is what makes wrapping the helper safe rather than
       having to classify every call site. */
    label = T(label);
    if (value) value = T(value);
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int iy = y + row * item_h;
    float focus = jw__settings_row_focus(list, row);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    /* Unselected rows may override the value color (e.g. a warning tint on the
       Beta update channel); the selected pill always uses highlighted_text. */
    ap_color value_c = cat_draw_color_lerp(
        value_override ? *value_override : theme->hint,
        theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    if (value) {
        int body_h = TTF_FontHeight(body);
        int vw = cat_measure_text(body, value);
        if (cycler) {
            /* Solid triangles flank the value, matching the tab-switcher
               affordance: ◀ value ▶. Triangles are sized to the text cap and
               vertically centered. */
            int tri_h = body_h / 2;
            int tri_w = tri_h * 3 / 4;
            int gap   = cat_scale(8);
            int total = tri_w + gap + vw + gap + tri_w;
            int rx    = x + w - total - cat_scale(16);
            if (rx < x + w / 2) rx = x + w / 2;
            int tri_y = ty + (body_h - tri_h) / 2;
            cat_draw_triangle(rx, tri_y, tri_w, tri_h, CAT_DIR_LEFT, value_c);
            cat_draw_text(body, value, rx + tri_w + gap, ty, value_c);
            cat_draw_triangle(rx + tri_w + gap + vw + gap, tri_y, tri_w, tri_h,
                              CAT_DIR_RIGHT, value_c);
        } else {
            int vx = x + w - vw - cat_scale(16);
            if (vx < x + w / 2) vx = x + w / 2;
            cat_draw_text(body, value, vx, ty, value_c);
        }
    }
}

/* Backward-compatible wrapper: no value-color override. */
static void jw__render_list_row_h(const cat_list_state *list, int x, int y,
                                  int w, int row, const char *label,
                                  const char *value, bool cycler, int item_h) {
    jw__render_list_row_impl(list, x, y, w, row, label, value, cycler, item_h, NULL);
}

/* The canonical settings list row: medium font + cat_scale(12) padding. */
static void jw__render_list_row(const cat_list_state *list, int x, int y,
                                int w, int row, const char *label,
                                const char *value, bool cycler) {
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__render_list_row_h(list, x, y, w, row, label, value, cycler, item_h);
}

/* Canonical row with a value-color override (unselected rows only). */
static void jw__render_list_row_vc(const cat_list_state *list, int x, int y,
                                   int w, int row, const char *label,
                                   const char *value, bool cycler, ap_color value_c) {
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__render_list_row_impl(list, x, y, w, row, label, value, cycler, item_h, &value_c);
}

/* Row pitch for the Display & Sound page. The Brightness/Volume sliders need the
   taller slot for their track, so the Audio Output row (a plain list row between
   them) must share this same pitch — otherwise the three rows, each positioned by
   row*own_height, overlap and gap.

   avail_h is the height of the content area the rows are drawn into. The natural
   pitch is used whenever the rows fit, which is every case at the default font
   size; past that the padding is spent down so the last row still lands inside
   the box. Without that, the page ran off the bottom at Extra Large and the
   button-hint bar covered the Test Sound row (label and value both clipped).
   The floor keeps a row taller than its own text, so compressing can never clip
   the glyphs — the rows are padding-heavy (28 units against the 12 a plain nav
   row uses), so there is a lot to give back before that floor is reached.
   Pass avail_h <= 0 to ask for the natural pitch without any fitting. */
static int jw__display_row_h_fit(int avail_h) {
    int natural = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(28);
    if (avail_h <= 0 || JW_DISPLAY_ROW_COUNT * natural <= avail_h) {
        return natural;
    }
    int fitted = avail_h / JW_DISPLAY_ROW_COUNT;
    int floor_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(10);
    return fitted < floor_h ? floor_h : fitted;
}

static void jw__render_nav_row(const cat_list_state *list, int x, int y,
                               int w, int row, const char *label) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int iy = y + row * item_h;
    float focus = jw__settings_row_focus(list, row);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    ap_color tc = cat_draw_color_lerp(theme->text,
                                       theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    /* Translated here for the same reason jw__render_list_row_impl does it: the
       label belongs to the renderer, not to its call sites. Missing that made
       every nav row in Settings render English while its translation sat in the
       table unused -- extraction had always found these strings, so the .po was
       right and only the lookup was absent. */
    cat_draw_text_ellipsized(body, T(label), x + cat_scale(12), ty, tc, w - cat_scale(24));
}

static void jw__render_color_swatch(int x, int list_y, int w, int row, ap_color c) {
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int swatch_w = cat_scale(48);
    int swatch_h = cat_scale(14);
    int sx = x + w - cat_scale(20) - swatch_w;
    int sy = list_y + row * item_h + (item_h - swatch_h) / 2;
    cat_draw_pill(sx, sy, swatch_w, swatch_h, c);
}

/* ─── Page renderers ───────────────────────────────────────────────────── */

/* Settings home uses the same split focus/content rendering as launcher browse
   lists: the pill moves independently while category labels stay fixed. */
static void jw__draw_settings_focus(int x, int y, int w, int h, void *user) {
    (void)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = y + (h - pill_h) / 2;
    cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);
}

static void jw__draw_settings_list(int x, int y, int w, int h, int item_count,
                                   const cat_list_state *state, int item_height,
                                   cat_list_layered_item_draw_fn draw_item,
                                   void *user) {
    cat_draw_list_pane_layered(x, y, w, h, item_count, state, item_height,
                               jw__draw_settings_focus, draw_item, user);
}

static void jw__cache_settings_focus(int idx, int x, int y, int w, int h,
                                     float focus, void *user) {
    (void)x; (void)y; (void)w; (void)h; (void)user;
    if (idx >= 0 && idx < jw__settings_focus_count)
        jw__settings_focus_cache[idx] = focus;
}

static void jw__begin_settings_rows_with_focus(
                                    const cat_list_state *list,
                                    int x, int y, int w,
                                    int item_count, int item_h,
                                    cat_list_focus_draw_fn draw_focus) {
    if (!list || item_count <= 0 || item_h <= 0) return;
    if (item_count > JW__SETTINGS_FOCUS_CACHE_MAX)
        item_count = JW__SETTINGS_FOCUS_CACHE_MAX;
    memset(jw__settings_focus_cache, 0, sizeof(jw__settings_focus_cache));
    jw__settings_focus_list = list;
    jw__settings_focus_count = item_count;
    cat_draw_list_pane_layered(x, y, w, item_count * item_h,
                               item_count, list, item_h,
                               draw_focus,
                               jw__cache_settings_focus, NULL);
}

static void jw__begin_settings_rows(const cat_list_state *list,
                                    int x, int y, int w,
                                    int item_count, int item_h) {
    jw__begin_settings_rows_with_focus(list, x, y, w, item_count, item_h,
                                       jw__draw_settings_focus);
}

/* One Services row: the service id plus a compact "state · Start with Leaf"
 * summary. The list is a live CTL-1 snapshot refreshed on entry and after
 * every action. */
static void jw__draw_service_item(int idx, int ix, int iy, int iw, int ih,
                                  float focus, void *user) {
    const jw_settings_ui *ui = (const jw_settings_ui *)user;
    if (!ui || idx < 0 || idx >= ui->services_count) return;
    const jw_ipc_service_info *svc = &ui->services[idx];
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    ap_color tc = cat_draw_color_lerp(theme->text,
                                       theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    /* Shorten the reverse-DNS id to its last component for the list. */
    const char *short_id = strrchr(svc->id, '.');
    short_id = short_id ? short_id + 1 : svc->id;
    cat_draw_text_ellipsized(body, short_id, ix + cat_scale(12), ty,
                             tc, iw / 2 - cat_scale(20));

    char value[96];
    snprintf(value, sizeof(value), "%s%s", svc->state,
             svc->desired_enabled ? " · auto" : "");
    int vw = cat_measure_text(body, value);
    int vx = ix + iw - vw - cat_scale(16);
    if (vx < ix + iw / 2) vx = ix + iw / 2;
    ap_color vc = cat_draw_color_lerp(theme->hint,
                                       theme->highlighted_text, focus);
    cat_draw_text(body, value, vx, ty, vc);
}

static void jw__render_services(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Services", x, y, w);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);
    int dy = content.y;

    if (ui->services_msg[0]) {
        cat_draw_text_ellipsized(small, ui->services_msg, x + cat_scale(12), dy,
                                 theme->emphasis, w - cat_scale(24));
        dy += line_h + cat_scale(6);
    } else {
        dy += cat_scale(6);
    }

    if (ui->services_count <= 0) {
        cat_draw_text(small, T("No services installed"), x + cat_scale(12), dy,
                      theme->hint);
        return;
    }

    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { content.x, dy, content.w, (content.y + content.h) - dy,
                   0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, ui->services_count, &vis,
                                   &item_h);
    ((cat_list_state *)&ui->services_list)->visible_rows = vis;
    jw__draw_settings_list(lr.x, lr.y, lr.w, lr.h, ui->services_count,
                           &ui->services_list, item_h, jw__draw_service_item,
                           (void *)ui);
}

/* List-pane row for the Settings home categories — mirrors jw__render_nav_row but
   positioned by the scrolling list pane, so the list fills the page and scrolls
   instead of overflowing under the footer. */
static void jw__draw_home_item(int idx, int ix, int iy, int iw, int ih,
                               float focus, void *user) {
    (void)user;
    if (idx < 0 || idx >= JW_SETTINGS_CATEGORY_COUNT) return;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    ap_color tc = cat_draw_color_lerp(theme->text,
                                       theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, T(kHomeCategoryLabels[idx]), ix + cat_scale(12), ty,
                             tc, iw - cat_scale(24));
}

static void jw__render_home(const jw_settings_ui *ui, int x, int y, int w, int h) {
    /* No "Settings" header — the tab bar above already names this screen, so the
       category list fills the full height and scrolls rather than overflowing. */
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, false, 0, NULL, NULL);
    cat_box lb = { content.x, content.y, content.w, content.h, 0, 0, 0, 0 };
    int vis = 0;
    int category_count = jw__home_category_count(ui);
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, category_count, &vis, &item_h);
    ((cat_list_state *)&ui->home_list)->visible_rows = vis;
    jw__draw_settings_list(lr.x, lr.y, lr.w, lr.h, category_count,
                           &ui->home_list, item_h, jw__draw_home_item, NULL);
}

static void jw__render_appearance(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Appearance", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->appearance_list, x, ly, w,
                            JW_APPEAR_ROW_COUNT, item_h);
    /* Layout switching is gated to Tabs, so this row instead cycles the curated
       color schemes (Aurora/Ember/…); "Custom" once colors are hand-edited. */
    const char *scheme_name =
        (ui->color_scheme_index >= 0 && ui->color_scheme_index < JW_COLOR_SCHEME_COUNT)
        ? kColorSchemes[ui->color_scheme_index].name : "Custom";
    jw__render_list_row(&ui->appearance_list, x, ly, w, JW_APPEAR_THEME,
                        "Color Scheme", scheme_name, true);
    jw__render_nav_row(&ui->appearance_list, x, ly, w, JW_APPEAR_COLORS, "Colors");
    jw__render_nav_row(&ui->appearance_list, x, ly, w, JW_APPEAR_LAYOUT, "Layout");
    jw__render_nav_row(&ui->appearance_list, x, ly, w, JW_APPEAR_STATUSBAR, "Status Bar");
}

static void jw__render_colors(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *t = cat_get_theme();
    jw__draw_header("Colors", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->colors_list, x, ly, w,
                            JW_COLOR_ROW_COUNT, item_h);

    /* A function-local table, so T() is legal here and no JW_UI marker is
       needed -- the strings reach the extractor through the T funnel like any
       ordinary call site. The row renderer translates too, which is harmless:
       T() of an already-translated string simply misses and returns it. */
    struct { const char *label; ap_color color; } rows[] = {
        { T("Accent"),            t->accent },
        { T("Background"),        t->background },
        { T("Text"),              t->text },
        { T("Selection"),         t->highlight },
        { T("Secondary Text"),    t->hint },
        { T("Button Text"),       t->button_label },
        { T("Button Background"), t->button_glyph_bg },
    };

    for (int i = 0; i < JW_COLOR_ROW_COUNT; i++) {
        jw__render_list_row(&ui->colors_list, x, ly, w, i, rows[i].label, NULL, false);
        jw__render_color_swatch(x, ly, w, i, rows[i].color);
    }
}

static void jw__render_layout(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Layout", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->layout_list, x, ly, w,
                            JW_LAYOUT_ROW_COUNT, item_h);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_HOME_STYLE,
                        "Home Layout", ui->layout_mode == 1 ? "Coverflow" : "Tabs", true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_PILL_SHAPE,
                        "List Style", kPillShapeLabels[ui->pill_shape_index], true);
    /* The themed families have no CJK glyphs, so a CJK language pins the face.
       Showing that in the row beats an option that looks available and is not. */
    bool font_locked = jw_i18n_language_is_cjk(jw_i18n_language());
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_FAMILY,
                        "Font",
                        font_locked ? "Source Han Sans"
                                    : kJawakaFontFamilyLabels[ui->font_family_index],
                        !font_locked);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_SIZE,
                        "Font Size", kFontSizeLabels[ui->font_size_index], true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_TAB_SWITCH,
                        "Tab Switching", kTabSwitchLabels[ui->tab_glide ? 1 : 0], true);

    int tab = (ui->startup_tab_index >= 0 && ui->startup_tab_index < JW_STARTUP_TAB_COUNT)
              ? ui->startup_tab_index : JW_STARTUP_TAB_DEFAULT;
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_STARTUP_TAB,
                        "Startup Tab", kStartupTabLabels[tab], true);
    jw__render_nav_row(&ui->layout_list, x, ly, w, JW_LAYOUT_HOME_TABS,
                       "Home Tabs");
}

/* The Status Bar "Battery" row cycles four display modes. They're stored as the
   two persisted bools show_battery (icon) + show_battery_level (number), so the
   mode is just a UI view over that pair. */
enum {
    JW_BATTERY_OFF = 0,   /* neither icon nor number */
    JW_BATTERY_ICON,      /* icon only */
    JW_BATTERY_PERCENT,   /* number only */
    JW_BATTERY_BOTH,      /* icon + number */
    JW_BATTERY_MODE_COUNT
};
static const char *const kBatteryModeLabels[JW_BATTERY_MODE_COUNT] = {
    JW_UI("Hidden"), JW_UI("Icon"), JW_UI("Percent"), JW_UI("Both")
};
static int jw__battery_mode(bool icon, bool number) {
    if (icon && number) return JW_BATTERY_BOTH;
    if (number)         return JW_BATTERY_PERCENT;
    if (icon)           return JW_BATTERY_ICON;
    return JW_BATTERY_OFF;
}
static void jw__battery_mode_to_flags(int mode, bool *icon, bool *number) {
    *icon   = (mode == JW_BATTERY_ICON || mode == JW_BATTERY_BOTH);
    *number = (mode == JW_BATTERY_PERCENT || mode == JW_BATTERY_BOTH);
}

/* Status-bar visibility toggles read as Visible/Hidden rather than On/Off.
   Translated here rather than at the call sites: a helper is a function, so it
   can call T() directly, and T is the first funnel the extractor looks for -- so
   the strings reach the .po with no registry entry to keep in step. Returning
   bare literals is why four Status Bar rows rendered English. */
static inline const char *jw__vis_label(bool visible) {
    return visible ? T("Visible") : T("Hidden");
}

static void jw__render_statusbar(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Status Bar", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->statusbar_list, x, ly, w,
                            JW_STATUSBAR_ROW_COUNT, item_h);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_HINTS,
                        "Button Hints", jw__vis_label(ui->show_hints), true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_CLOCK,
                        "Clock", kClockStyleLabels[ui->clock_style_index], true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_BATTERY,
                        "Battery",
                        kBatteryModeLabels[jw__battery_mode(ui->show_battery, ui->show_battery_level)],
                        true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_WIFI,
                        "Wi-Fi", jw__vis_label(ui->show_wifi), true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_BLUETOOTH,
                        "Bluetooth", jw__vis_label(ui->show_bluetooth), true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_VOLUME,
                        "Volume", jw__vis_label(ui->show_volume), true);
}

/* One labelled slider row (Brightness / Volume) on the Display & Sound page.
   item_h is the page's fitted row pitch; every row on the page must be passed the
   same value or they overlap and gap, since each positions itself as row*item_h. */
static void jw__draw_slider_row(const jw_settings_ui *ui, int x, int y_base, int w,
                                int row, const char *label, int percent, int item_h) {
    label = T(label);   /* value_str is "%d%%" -- a number needs no lookup */
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int iy = y_base + row * item_h;
    float focus = jw__settings_row_focus(&ui->display_list, row);
    int pill_h = item_h - cat_scale(6);
    int pill_y = iy + cat_scale(3);
    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + cat_scale(8);

    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    char value_str[32];
    snprintf(value_str, sizeof(value_str), "%d%%", percent);
    int vw = cat_measure_text(body, value_str);
    cat_draw_text(body, value_str, x + w - vw - cat_scale(16), ty, value_c);

    int track_x = x + cat_scale(12);
    int track_y = pill_y + pill_h - cat_scale(16);
    int track_w = w - cat_scale(32);
    int fill_w = (track_w * percent) / 100;
    cat_draw_rect(track_x, track_y, track_w, cat_scale(4), cat_hex_to_color("#ffffff33"));
    cat_draw_rect(track_x, track_y, fill_w, cat_scale(4), value_c);
}

static void jw__draw_audio_output_row(const jw_settings_ui *ui, int x, int y_base, int w,
                                      int item_h) {
    jw_platform_audio_output output = ui->audio_output;
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        output = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    }
    /* Only show the cycle affordance when there is more than one output to pick
       (e.g. headphones plugged in), so a lone "Speaker" doesn't look switchable.
       Shares the slider pitch so all three rows line up. */
    int navail = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        if (ui->audio_available_outputs & JW_PLATFORM_AUDIO_OUTPUT_BIT(i)) {
            navail++;
        }
    }
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_OUTPUT,
                          "Audio Output", jw_platform_audio_output_label(output),
                          navail > 1, item_h);
}

static void jw__draw_display_focus(int x, int y, int w, int h, void *user) {
    (void)user;
    ap_theme *theme = cat_get_theme();
    cat_draw_pill(x, y + cat_scale(3), w - cat_scale(4),
                  h - cat_scale(6), theme->highlight);
}

static void jw__render_display(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Display & Sound", x, y, w);
    /* Fit the rows to the box rather than assuming the natural pitch clears it.
       The box already excludes the button-hint bar (the launcher subtracts the
       footer height before handing this height over), so rows that exceed it are
       drawn straight past their own bottom edge and end up underneath the hints. */
    SDL_Rect content = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);
    int y_base = content.y;
    int item_h = jw__display_row_h_fit(content.h);
    jw__begin_settings_rows_with_focus(&ui->display_list, x, y_base, w,
                                       JW_DISPLAY_ROW_COUNT, item_h,
                                       jw__draw_display_focus);
    jw__draw_slider_row(ui, x, y_base, w, JW_DISPLAY_BRIGHTNESS, "Brightness",
                        ui->brightness_percent, item_h);
    /* Display refresh rate (kPanelRefreshHz). Cycler when the platform supports it. */
    char refresh_val[16];
    snprintf(refresh_val, sizeof(refresh_val), T("%d Hz"), ui->refresh_rate_hz);
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_REFRESH_RATE,
                          "Refresh Rate", refresh_val,
                          ui->refresh_rate_supported, item_h);
    /* Black Frame Insertion (RetroArch strobing) — cuts motion blur by blanking
       one refresh per emulated frame, so it needs a rate that is twice a content
       rate. Greyed with a "100/120 Hz only" hint elsewhere.
       When it IS on, the value names the content rate it is currently set up for
       (50 fps at 100Hz, 60 fps at 120Hz). That matters because BFI is a GLOBAL
       setting with no per-game override: left on at 100Hz, a 60fps game is paced
       to 50 and runs at 83% speed. Naming the rate makes the row say what it is
       for instead of a bare "On" that hides the pairing. */
    int bfi_fps = jw_bfi_content_fps(ui->refresh_rate_hz);
    bool bfi_avail = (bfi_fps > 0);
    char bfi_val[24];
    if (!bfi_avail) {
        snprintf(bfi_val, sizeof(bfi_val), "%s", T("100/120 Hz only"));
    } else if (ui->bfi_enabled) {
        snprintf(bfi_val, sizeof(bfi_val), T("On (%d fps)"), bfi_fps);
    } else {
        snprintf(bfi_val, sizeof(bfi_val), "%s", T("Off"));
    }
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_BFI,
                          "Black Frame Insertion", bfi_val,
                          bfi_avail, item_h);
    /* HDMI external output (4:3 pillarbox / stretch). Cycler when a TV is
       plugged in; greyed "Not connected" otherwise. */
    static const char *const kHdmiVals[] = { JW_UI("Off"), JW_UI("4:3"), JW_UI("Stretch") };
    bool hdmi_avail = ui->hdmi_supported && ui->hdmi_connected == 1;
    int hdmi_idx = (ui->hdmi_output_mode >= 0 && ui->hdmi_output_mode <= 2)
                       ? ui->hdmi_output_mode : 0;
    /* When sending to the TV, append the live negotiated mode so a 120Hz fallback
       to 720p (a 60Hz-only TV, or a cable that can't carry 1080p120) is visible at
       a glance. Resolution follows the live refresh: 120 -> 1080p120, else 720p60. */
    char hdmi_val[40];
    const char *hdmi_text;
    if (hdmi_avail && hdmi_idx != 0) {
        snprintf(hdmi_val, sizeof(hdmi_val), "%s \xc2\xb7 %s", kHdmiVals[hdmi_idx],
                 ui->refresh_rate_hz >= 120 ? "1080p120" : "720p60");
        hdmi_text = hdmi_val;
    } else if (hdmi_avail) {
        hdmi_text = kHdmiVals[hdmi_idx];
    } else {
        hdmi_text = ui->hdmi_supported ? T("Not connected") : T("Unavailable");
    }
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_HDMI,
                          "HDMI Output", hdmi_text, hdmi_avail, item_h);
    jw__draw_audio_output_row(ui, x, y_base, w, item_h);
    jw__draw_slider_row(ui, x, y_base, w, JW_DISPLAY_VOLUME, "Volume",
                        ui->volume_percent, item_h);
    /* Action row: toggles a short clip on the current output so the user can
       verify sound (and which device it lands on). Shows Stop while playing. */
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_TEST_SOUND,
                          "Test Sound", ui->test_sound_playing ? "Stop" : "Play",
                          false, item_h);
}

static void jw__render_lighting(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Lighting", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->lighting_list, x, ly, w,
                            JW_LIGHTING_ROW_COUNT, item_h);
    int mode = (ui->led_mode >= 0 && ui->led_mode < JW_LED_MODE_COUNT) ? ui->led_mode : 0;
    char bright[8], speed[8];
    snprintf(bright, sizeof(bright), "%d", ui->led_brightness);
    snprintf(speed,  sizeof(speed),  "%d", ui->led_speed);

    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_ENABLE,
                        "Enable", ui->led_enabled ? "On" : "Off", true);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_MODE,
                        "Mode", kLedModeLabels[mode], true);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_COLOR,
                        "Color", NULL, false);
    jw__render_color_swatch(x, ly, w, JW_LIGHTING_COLOR, ui->led_color);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_BRIGHTNESS,
                        "Brightness", bright, true);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_SPEED,
                        "Speed", speed, true);
}

static void jw__refresh_wifi(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    if (!jw_wifi_available()) {
        memset(&ui->wifi, 0, sizeof(ui->wifi));
        ui->wifi_strength_cached = 0;
        return;
    }
    if (jw_wifi_status(&ui->wifi) != 0) {
        /* jw_wifi_status already zeroed the struct (valid = false). */
    }
    /* Keep the status-bar icon in sync with the page's own reading. */
    ui->wifi_strength_cached = ui->wifi.connected ? ui->wifi.strength : 0;
}

bool jw_settings_ui_wants_wifi_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_NETWORK;
}

/* Re-read the cached scan results into the ui (deduped/sorted by the module). */
static void jw__refresh_wifi_scan(jw_settings_ui *ui) {
    if (!jw_wifi_available()) {
        ui->wifi_network_count = 0;
        if (ui->network_list.cursor >= JW_NETWORK_FIXED_ROWS) {
            ui->network_list.cursor = JW_NETWORK_ROW_WIFI;
        }
        return;
    }
    int n = jw_wifi_scan_results(ui->wifi.ssid, ui->wifi_networks,
                                 JW_WIFI_MAX_NETWORKS);
    ui->wifi_network_count = (n > 0) ? n : 0;
    int row_count = JW_NETWORK_FIXED_ROWS +
                    (ui->wifi_radio_on ? ui->wifi_network_count : 0);
    if (row_count < JW_NETWORK_FIXED_ROWS) {
        row_count = JW_NETWORK_FIXED_ROWS;
    }
    if (ui->network_list.cursor >= row_count) {
        ui->network_list.cursor = row_count - 1;
    }
}

/* Set the Network-page feedback toast (timestamped so it auto-expires). */
static void jw__wifi_msg(jw_settings_ui *ui, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->wifi_msg, sizeof(ui->wifi_msg), fmt, ap);
    va_end(ap);
    ui->wifi_msg_ms = SDL_GetTicks();
    if (ui->wifi_msg_ms == 0) ui->wifi_msg_ms = 1;   /* 0 means "none" */
}

/* Begin a join attempt: record it and attach to the wpa event socket so we can
   catch a WRONG_KEY auth failure (the only reliable wrong-password signal). */
static void jw__wifi_attempt_begin(jw_settings_ui *ui, const char *ssid) {
    snprintf(ui->wifi_attempt_ssid, sizeof(ui->wifi_attempt_ssid), "%s", ssid);
    ui->wifi_attempt_ms = SDL_GetTicks();
    if (ui->wifi_monitor_fd >= 0) {
        jw_wifi_monitor_close(ui->wifi_monitor_fd);
    }
    ui->wifi_monitor_fd = jw_wifi_monitor_open();
}

static void jw__wifi_attempt_clear(jw_settings_ui *ui) {
    ui->wifi_attempt_ssid[0] = '\0';
    if (ui->wifi_monitor_fd >= 0) {
        jw_wifi_monitor_close(ui->wifi_monitor_fd);
        ui->wifi_monitor_fd = -1;
    }
}

void jw_settings_ui_refresh_wifi(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    /* Expire the feedback toast so it doesn't linger after the action is over. */
    if (ui->wifi_msg_ms && (int)(SDL_GetTicks() - ui->wifi_msg_ms) > 6000) {
        ui->wifi_msg[0] = '\0';
        ui->wifi_msg_ms = 0;
    }

    if (!jw_wifi_available()) {
        jw__refresh_adb(ui);
        memset(&ui->wifi, 0, sizeof(ui->wifi));
        ui->wifi_radio_on = false;
        ui->wifi_strength_cached = 0;
        ui->wifi_network_count = 0;
        ui->wifi_next_poll_ms = SDL_GetTicks() + 2000;
        return;
    }

    /* Drain the platform event source every frame (cheap, non-blocking) so a
       fast auth failure isn't missed between the throttled platform polls. */
    jw_wifi_evt evt = ui->wifi_attempt_ssid[0] ? jw_wifi_monitor_poll(ui->wifi_monitor_fd)
                                               : JW_WIFI_EVT_NONE;

    /* Self-throttle the platform Wi-Fi polls to ~2s. */
    unsigned now = SDL_GetTicks();
    if (evt == JW_WIFI_EVT_NONE && ui->wifi_next_poll_ms != 0 &&
        (int)(now - ui->wifi_next_poll_ms) < 0) {
        return;
    }
    jw__refresh_adb(ui);
    ui->wifi_radio_on = jw_wifi_radio_is_on();
    if (!ui->wifi_radio_on) {
        /* Radio off — nothing to poll or scan; clear stale state. */
        memset(&ui->wifi, 0, sizeof(ui->wifi));
        ui->wifi_network_count = 0;
        if (ui->network_list.cursor >= JW_NETWORK_FIXED_ROWS) {
            ui->network_list.cursor = JW_NETWORK_ROW_WIFI;
        }
        ui->wifi_next_poll_ms = now + 2000;
        return;
    }
    jw__refresh_wifi(ui);
    jw__refresh_wifi_scan(ui);

    /* Resolve a pending connect attempt:
       - success: associated (COMPLETED) on the target SSID;
       - WRONG_KEY event: definitive wrong password;
       - auth-fail event (SAE/WPA3 bad key, assoc reject): likely wrong password;
       - timeout (12s): neither.
       On any failure, forget the bad profile and recover the prior network — a
       connect uses select_network, which disabled it. The monitor is closed on
       resolve, so recovery's own churn events are never misread. */
    if (ui->wifi_attempt_ssid[0]) {
        bool connected = ui->wifi.connected &&
                         strcmp(ui->wifi.ssid, ui->wifi_attempt_ssid) == 0;
        bool failed = (evt != JW_WIFI_EVT_NONE) ||
                      (int)(now - ui->wifi_attempt_ms) > 12000;
        if (connected) {
            jw__wifi_msg(ui, "Connected to %s",
                     ui->wifi_attempt_ssid);
            jw__wifi_attempt_clear(ui);
        } else if (failed) {
            if (evt == JW_WIFI_EVT_WRONG_KEY) {
                /* Only a DEFINITIVE wrong key forgets the profile — never a
                   generic/timeout failure, which on this flaky radio can hit a
                   perfectly-good saved network and would otherwise destroy a
                   correct saved password. */
                char bad[64];
                snprintf(bad, sizeof(bad), "%s", ui->wifi_attempt_ssid);
                jw_wifi_forget(bad);
                jw__wifi_msg(ui, "Wrong password");
                jw__refresh_wifi_scan(ui);
            } else {
                jw__wifi_msg(ui, "Couldn't connect — check password");
            }
            jw_wifi_recover();     /* restore the network we were kicked off */
            jw__wifi_attempt_clear(ui);
        }
    }

    /* Trigger a fresh scan on a slower cadence than the result re-read. */
    if (ui->wifi_next_scan_ms == 0 || (int)(now - ui->wifi_next_scan_ms) >= 0) {
        jw_wifi_scan_start();
        ui->wifi_next_scan_ms = now + JW_WIFI_SCAN_INTERVAL_MS;
    }
    ui->wifi_next_poll_ms = now + 2000;
}

/* Fixed rows: Wi-Fi toggle, ADB action; scanned networks follow. */
typedef struct {
    const jw_wifi_network_t *nets;
    bool wifi_available;
    bool radio_on;
    int adb_enabled;
    int adb_intent_enabled;
    bool adb_supported;
} jw__wifi_list_ctx;

static void jw__draw_wifi_item(int idx, int ix, int iy, int iw, int ih,
                               float focus, void *user) {
    const jw__wifi_list_ctx *ctx = (const jw__wifi_list_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    if (idx == JW_NETWORK_ROW_WIFI) {
        /* The on/off toggle row. */
        cat_draw_text(body, T("Wi-Fi"), ix + cat_scale(12), ty, label_c);
        /* Action verb (what A will do), not a bare state word — "Turn On" when
           the radio is off, "Turn Off" when it's on, so the button is unambiguous. */
        const char *action = ctx->wifi_available
            ? (ctx->radio_on ? T("Turn Off") : T("Turn On"))
            : T("Unavailable");
        int vw = cat_measure_text(body, action);
        cat_draw_text(body, action, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    if (idx == JW_NETWORK_ROW_ADB) {
        const char *value = ctx->adb_supported ? T("Unavailable") : T("Unsupported");
        if (ctx->adb_supported && ctx->adb_enabled == 1) {
            value = T("Enabled");
        } else if (ctx->adb_supported && ctx->adb_intent_enabled == 1) {
            value = T("Repair");
        } else if (ctx->adb_supported && ctx->adb_enabled == 0) {
            value = T("Enable");
        }

        cat_draw_text(body, T("ADB"), ix + cat_scale(12), ty, label_c);
        int vw = cat_measure_text(body, value);
        cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    const jw_wifi_network_t *net = &ctx->nets[idx - JW_NETWORK_FIXED_ROWS];

    /* Left: SSID, with a leading "* " on the connected network. */
    char label[80];
    snprintf(label, sizeof(label), "%s%s", net->current ? "* " : "", net->ssid);
    cat_draw_text_ellipsized(body, label, ix + cat_scale(12), ty, label_c,
                             iw / 2);

    /* Right: signal word, "Open" prefix for unsecured nets, "saved" suffix for
       networks with a stored profile. */
    const char *word = (net->strength >= 3) ? "Strong" :
                       (net->strength == 2) ? "Good"   : "Weak";
    const char *open_prefix = net->secured ? "" : "Open  ";
    const char *saved_suffix = net->saved ? "  saved" : "";
    char value[56];
    snprintf(value, sizeof(value), "%s%s%s", open_prefix, word, saved_suffix);
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_network(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Network", x, y, w);
    int item_h = TTF_FontHeight(body) + cat_scale(12);

    const jw_wifi_status_t *wifi = &ui->wifi;

    char status_val[48];
    char signal_val[32];
    bool wifi_available = jw_wifi_available();
    if (!wifi_available) {
        snprintf(status_val, sizeof(status_val), "%s", T("Unavailable"));
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    } else if (!ui->wifi_radio_on) {
        snprintf(status_val, sizeof(status_val), "%s", T("Off"));
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    } else if (!wifi->valid) {
        snprintf(status_val, sizeof(status_val), "%s", T("Unavailable"));
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    } else if (wifi->connected) {
        snprintf(status_val, sizeof(status_val), "%s", T("Connected"));
        /* Word + dBm, derived from RSSI with the same thresholds as the status
           icon, so the two always agree. */
        const char *level = (wifi->strength >= 3) ? T("Strong") :
                            (wifi->strength == 2) ? T("Good")   :
                            (wifi->strength == 1) ? T("Weak")   : "—";
        if (wifi->rssi != 0)
            snprintf(signal_val, sizeof(signal_val), "%s (%d dBm)", level, wifi->rssi);
        else
            snprintf(signal_val, sizeof(signal_val), "%s", level);
    } else {
        snprintf(status_val, sizeof(status_val), "%s",
                 wifi->state[0] ? wifi->state : T("Disconnected"));
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    }

    /* ── Current-connection summary (small caption lines) — drawn from the box
       model's content origin; the list fills the remainder below. ── */
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);
    int dy = content.y;
    char line[160];

    snprintf(line, sizeof(line), T("Status: %s"), status_val);
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->text);
    dy += line_h;

    snprintf(line, sizeof(line), T("Network: %s"),
             (wifi->valid && wifi->ssid[0]) ? wifi->ssid : "—");
    cat_draw_text_ellipsized(small, line, x + cat_scale(12), dy, theme->text, w - cat_scale(24));
    dy += line_h;

    snprintf(line, sizeof(line), T("Signal: %s"), signal_val);
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h;

    snprintf(line, sizeof(line), T("IP: %s"),
             (wifi->valid && wifi->ip[0]) ? wifi->ip : "—");
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h + cat_scale(6);

    /* Action feedback (always visible, regardless of the hint setting). */
    if (ui->wifi_msg[0]) {
        cat_draw_text_ellipsized(small, ui->wifi_msg, x + cat_scale(12), dy,
                                 theme->emphasis, w - cat_scale(24));
        dy += line_h + cat_scale(6);
    }

    /* ── List: fixed controls first, then scanned networks ── */
    int count = JW_NETWORK_FIXED_ROWS +
                ((wifi_available && ui->wifi_radio_on) ? ui->wifi_network_count : 0);
    jw__wifi_list_ctx ctx = {
        ui->wifi_networks,
        wifi_available,
        ui->wifi_radio_on,
        ui->adb_enabled,
        ui->adb_intent_enabled,
        ui->adb_supported,
    };
    cat_box lb = { content.x, dy, content.w, (content.y + content.h) - dy, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
    ((cat_list_state *)&ui->network_list)->visible_rows = vis;
    jw__draw_settings_list(lr.x, lr.y, lr.w, lr.h, count,
                           &ui->network_list, item_h,
                           jw__draw_wifi_item, &ctx);
    if (wifi_available && ui->wifi_radio_on && ui->wifi_network_count == 0) {
        cat_draw_text(small, T("Scanning…"), x + cat_scale(12),
                      dy + item_h * JW_NETWORK_FIXED_ROWS, theme->hint);
    }
}

typedef struct {
    const jw_settings_ui *ui;
} jw__bt_list_ctx;

/* The "Paired"/"Nearby" rows are section labels, not selectable items — the
   cursor must skip over them during navigation. */
static bool jw__bt_row_is_header(const jw_settings_ui *ui, int row) {
    if (!ui || !ui->bt_radio_on) return false;
    return row == JW_BLUETOOTH_FIXED_ROWS ||
           row == JW_BLUETOOTH_FIXED_ROWS + 1 + ui->bt_paired_count;
}

static const jw_bt_device_t *jw__bt_row_device(const jw_settings_ui *ui, int row,
                                               bool *out_paired) {
    if (out_paired) {
        *out_paired = false;
    }
    if (!ui || !ui->bt_radio_on || row < JW_BLUETOOTH_FIXED_ROWS + 1) {
        return NULL;
    }

    int paired_start = JW_BLUETOOTH_FIXED_ROWS + 1;
    int paired_end = paired_start + ui->bt_paired_count;
    if (row >= paired_start && row < paired_end) {
        if (out_paired) {
            *out_paired = true;
        }
        return &ui->bt_paired[row - paired_start];
    }

    int nearby_header = paired_end;
    int nearby_start = nearby_header + 1;
    int nearby_end = nearby_start + ui->bt_nearby_count;
    if (row >= nearby_start && row < nearby_end) {
        return &ui->bt_nearby[row - nearby_start];
    }
    return NULL;
}

static void jw__draw_bt_item(int idx, int ix, int iy, int iw, int ih,
                             float focus, void *user) {
    const jw__bt_list_ctx *ctx = (const jw__bt_list_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    bool paired_device = false;
    const jw_bt_device_t *dev = jw__bt_row_device(ui, idx, &paired_device);

    if (ui && ui->bt_radio_on &&
        (idx == JW_BLUETOOTH_FIXED_ROWS ||
         idx == JW_BLUETOOTH_FIXED_ROWS + 1 + ui->bt_paired_count)) {
        const char *label = (idx == JW_BLUETOOTH_FIXED_ROWS) ? T("Paired") : T("Nearby");
        const char *value = NULL;
        if (idx == JW_BLUETOOTH_FIXED_ROWS && ui->bt_paired_count == 0) {
            value = T("None");
        } else if (idx != JW_BLUETOOTH_FIXED_ROWS && ui->bt_nearby_count == 0) {
            value = (ui->bt_op == JW_BT_OP_SCAN) ? T("Scanning") : T("None");
        }
        ap_color header_c = cat_draw_color_lerp(theme->emphasis,
                                                 theme->highlighted_text, focus);
        cat_draw_text(body, label, ix + cat_scale(12), ty, header_c);
        if (value) {
            int vw = cat_measure_text(body, value);
            ap_color value_c = cat_draw_color_lerp(theme->hint,
                                                    theme->highlighted_text, focus);
            cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
        }
        return;
    }

    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);

    if (idx == JW_BLUETOOTH_ROW_POWER) {
        const char *value = T("Unavailable");
        if (ui && ui->bt_status.available) {
            value = ui->bt_radio_on ? T("Turn Off") : T("Turn On");
        }
        cat_draw_text(body, T("Bluetooth"), ix + cat_scale(12), ty, label_c);
        int vw = cat_measure_text(body, value);
        cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    if (idx == JW_BLUETOOTH_ROW_NAME) {
        const char *name = (ui && ui->bt_status.local_name[0])
                         ? ui->bt_status.local_name : "-";
        cat_draw_text(body, T("Bluetooth Name"), ix + cat_scale(12), ty, label_c);
        int vw = cat_measure_text(body, name);
        int vx = ix + iw - vw - cat_scale(16);
        if (vx < ix + iw / 2) {
            cat_draw_text_ellipsized(body, name, ix + iw / 2, ty, value_c,
                                     iw / 2 - cat_scale(16));
        } else {
            cat_draw_text(body, name, vx, ty, value_c);
        }
        return;
    }

    if (!dev) {
        return;
    }

    /* Leading device-type icon (headset / controller / generic), theme-tinted, so
       the row sorts by kind at a glance before the name is read. The icon carries
       the type, so Nearby rows no longer need kind text on the right; Paired rows
       keep their Connected/Paired state there. */
    int name_x = ix + cat_scale(12);
    int icon_px = cat_device_icon_px();
    if (icon_px > 0) {
        cat_device_icon dicon =
            (dev->kind == JW_BT_DEVICE_HEADSET) ? CAT_DEVICE_ICON_HEADSET :
            (dev->kind == JW_BT_DEVICE_JOYPAD)  ? CAT_DEVICE_ICON_CONTROLLER :
                                                  CAT_DEVICE_ICON_BLUETOOTH;
        cat_draw_device_icon(dicon, name_x, pill_y + (pill_h - icon_px) / 2, label_c);
        name_x += icon_px + cat_scale(8);
    }

    int right = ix + iw - cat_scale(16);
    if (paired_device) {
        const char *value = dev->connected ? "Connected" : "Paired";
        int vw = cat_measure_text(body, value);
        cat_draw_text(body, value, right - vw, ty, value_c);
        right -= vw + cat_scale(8);
    }

    const char *label = dev->name[0] ? dev->name : dev->mac;
    int name_max = right - name_x;
    if (name_max < cat_scale(24)) {
        name_max = cat_scale(24);
    }
    cat_draw_text_ellipsized(body, label, name_x, ty, label_c, name_max);
}

static void jw__render_bluetooth(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Bluetooth", x, y, w);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);
    int dy = content.y;

    const char *status = "Unavailable";
    if (ui->bt_status.available) {
        status = ui->bt_radio_on ? "On" : "Off";
    }
    char line[160];
    snprintf(line, sizeof(line), "Status: %s", status);
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->text);
    dy += line_h;

    snprintf(line, sizeof(line), "Connected: %s",
             (ui->bt_status.available && ui->bt_status.any_connected) ? "Yes" : "No");
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h;

    if (ui->bt_msg[0]) {
        cat_draw_text_ellipsized(small, ui->bt_msg, x + cat_scale(12), dy,
                                 theme->emphasis, w - cat_scale(24));
        dy += line_h + cat_scale(6);
    } else {
        dy += cat_scale(6);
    }

    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int rows = jw__bt_row_count(ui);
    jw__bt_list_ctx ctx = { ui };
    cat_box lb = { content.x, dy, content.w, (content.y + content.h) - dy, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, rows, &vis, &item_h);
    ((cat_list_state *)&ui->bluetooth_list)->visible_rows = vis;
    jw__draw_settings_list(lr.x, lr.y, lr.w, lr.h,
                           rows, &ui->bluetooth_list, item_h,
                           jw__draw_bt_item, &ctx);
}

/* ─── Scraping priorities ──────────────────────────────────────────────── */

/* Parse a CSV of catalog values into a full permutation of catalog indices:
   matched values first (in CSV order, the included zone), unmatched catalog
   entries appended in catalog order. Returns the included count. An empty
   CSV applies the fallback value list instead. */
static int jw__scrape_csv_to_order(const char *csv,
                                   const jw_ss_option *catalog, int catalog_count,
                                   const char *const *fallback, int fallback_count,
                                   int *order) {
    bool used[JW_SCRAPE_PRIO_SLOTS] = { false };
    int included = 0;

    if (catalog_count > JW_SCRAPE_PRIO_SLOTS) catalog_count = JW_SCRAPE_PRIO_SLOTS;

    const char *p = (csv && csv[0]) ? csv : NULL;
    while (p && *p && included < catalog_count) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        for (int i = 0; i < catalog_count && len > 0; i++) {
            if (!used[i] && strlen(catalog[i].value) == len &&
                strncmp(catalog[i].value, p, len) == 0) {
                order[included++] = i;
                used[i] = true;
                break;
            }
        }
        p = comma ? comma + 1 : NULL;
    }

    if (included == 0 && fallback) {
        for (int f = 0; f < fallback_count && included < catalog_count; f++) {
            for (int i = 0; i < catalog_count; i++) {
                if (!used[i] && strcmp(catalog[i].value, fallback[f]) == 0) {
                    order[included++] = i;
                    used[i] = true;
                    break;
                }
            }
        }
    }

    int tail = included;
    for (int i = 0; i < catalog_count; i++) {
        if (!used[i]) order[tail++] = i;
    }
    return included;
}

static void jw__scrape_persist_order(const jw_settings_ui *ui, bool region) {
    const jw_ss_option *catalog = region ? jw_ss_regions : jw_ss_media_types;
    const int *order = region ? ui->scrape_region_order : ui->scrape_artwork_order;
    int included = region ? ui->scrape_region_included : ui->scrape_artwork_included;

    char csv[JW_SETTINGS_VALUE_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < included; i++) {
        int n = snprintf(csv + len, sizeof(csv) - len, "%s%s",
                         i > 0 ? "," : "", catalog[order[i]].value);
        if (n < 0 || (size_t)n >= sizeof(csv) - len) break;
        len += (size_t)n;
    }
    jw__persist(ui, region ? "scrape.region_priority" : "scrape.artwork_priority",
                csv);
}

/* Move order[from] to position to, shifting the entries between. */
static void jw__scrape_order_move(int *order, int from, int to) {
    if (from == to) return;
    int v = order[from];
    if (from < to) {
        memmove(order + from, order + from + 1, (size_t)(to - from) * sizeof(int));
    } else {
        memmove(order + to + 1, order + to, (size_t)(from - to) * sizeof(int));
    }
    order[to] = v;
}

typedef struct {
    const jw_settings_ui *ui;
} jw__scrape_edit_ctx;

static void jw__draw_scrape_edit_focus(int x, int y, int w, int h, void *user) {
    jw__scrape_edit_ctx *ctx = (jw__scrape_edit_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = y + (h - pill_h) / 2;
    ap_color color = (ctx && ctx->ui && ctx->ui->scrape_edit_grabbed)
                   ? theme->accent : theme->highlight;
    cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, color);
}

static void jw__draw_scrape_edit_item(int idx, int ix, int iy, int iw, int ih,
                                      float focus, void *user) {
    jw__scrape_edit_ctx *ctx = (jw__scrape_edit_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (!ui) return;

    bool region = ui->scrape_edit_is_region;
    const jw_ss_option *catalog = region ? jw_ss_regions : jw_ss_media_types;
    int catalog_count = region ? jw_ss_regions_count : jw_ss_media_types_count;
    const int *order = region ? ui->scrape_region_order : ui->scrape_artwork_order;
    int included = region ? ui->scrape_region_included : ui->scrape_artwork_included;
    if (idx < 0 || idx >= catalog_count) return;

    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    bool grabbed_row = idx == ui->scrape_edit_list.cursor &&
                       ui->scrape_edit_grabbed;

    bool is_included = idx < included;
    ap_color label_c = cat_draw_color_lerp(
        is_included ? theme->text : theme->hint,
        theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    char label[64];
    if (is_included) {
        snprintf(label, sizeof(label), "%d. %s", idx + 1,
                 catalog[order[idx]].display);
    } else {
        snprintf(label, sizeof(label), "%s", catalog[order[idx]].display);
    }
    cat_draw_text_ellipsized(body, label, ix + cat_scale(12), ty, label_c,
                             iw * 2 / 3);

    const char *value = grabbed_row ? T("Moving") : (is_included ? T("On") : T("Off"));
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_scrape_priority(const jw_settings_ui *ui,
                                       int x, int y, int w, int h) {
    bool region = ui->scrape_edit_is_region;
    jw__draw_header(region ? "Region Priority" : "Artwork Priority", x, y, w);

    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int sub_h = jw__subheader_line_h(small) + cat_scale(6);
    SDL_Rect sub;
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, sub_h, NULL, &sub);
    cat_draw_text_ellipsized(small, T("A: On/Off   X: Grab to reorder"),
                             sub.x + cat_scale(12), sub.y, theme->hint,
                             sub.w - cat_scale(24));

    int count = region ? jw_ss_regions_count : jw_ss_media_types_count;
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
    ((cat_list_state *)&ui->scrape_edit_list)->visible_rows = vis;
    jw__scrape_edit_ctx ctx = { ui };
    cat_draw_list_pane_layered(lr.x, lr.y, lr.w, lr.h, count,
                               &ui->scrape_edit_list, item_h,
                               jw__draw_scrape_edit_focus,
                               jw__draw_scrape_edit_item, &ctx);
}

static void jw__scrape_download_clear_rows(jw_settings_ui *ui) {
    if (!ui) return;
    memset(ui->scrape_download_rows, 0, sizeof(ui->scrape_download_rows));
    ui->scrape_download_row_count = 0;
    ui->scrape_download_total_missing = 0;
    ui->scrape_download_total_games = 0;
}

static const char *jw__trim_start(const char *s) {
    if (!s) return "";
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static size_t jw__trim_len(const char *s) {
    const char *start = jw__trim_start(s);
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
    return len;
}

static void jw__copy_trimmed_or_fallback(char *out, size_t out_size,
                                         const char *value,
                                         const char *fallback) {
    if (!out || out_size == 0) return;
    const char *start = jw__trim_start(value);
    size_t len = jw__trim_len(value);
    if (len == 0) {
        snprintf(out, out_size, "%s", fallback ? fallback : "");
        return;
    }
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static int jw__scrape_label_equal(const char *a, const char *b) {
    const char *as = jw__trim_start(a);
    const char *bs = jw__trim_start(b);
    size_t al = jw__trim_len(a);
    size_t bl = jw__trim_len(b);
    if (al != bl) return 0;
    for (size_t i = 0; i < al; i++) {
        unsigned char ac = (unsigned char)as[i];
        unsigned char bc = (unsigned char)bs[i];
        if (tolower(ac) != tolower(bc)) return 0;
    }
    return 1;
}

static int jw__scrape_download_row_cmp(const void *a, const void *b) {
    const jw_scrape_download_row *ra = (const jw_scrape_download_row *)a;
    const jw_scrape_download_row *rb = (const jw_scrape_download_row *)b;
    int rc = strcasecmp(ra->label, rb->label);
    if (rc != 0) return rc;
    return strcasecmp(ra->system, rb->system);
}

static void jw__append_truncated(char *out, size_t out_size,
                                 size_t *pos, const char *s) {
    if (!out || out_size == 0 || !pos || !s) return;
    while (*s && *pos + 1 < out_size) {
        out[*pos] = *s;
        *pos += 1;
        s += 1;
    }
    out[*pos] = '\0';
}

static void jw__scrape_download_suffix_label(jw_scrape_download_row *row) {
    if (!row) return;
    char base[JW_SCRAPE_DOWNLOAD_LABEL_MAX];
    snprintf(base, sizeof(base), "%s", row->label);

    size_t pos = 0;
    row->label[0] = '\0';
    jw__append_truncated(row->label, sizeof(row->label), &pos, base);
    jw__append_truncated(row->label, sizeof(row->label), &pos, " (");
    jw__append_truncated(row->label, sizeof(row->label), &pos, row->system);
    jw__append_truncated(row->label, sizeof(row->label), &pos, ")");
}

/* scrape_missing_cache is the IPC source of truth; rebuild these derived
   display rows whenever that cache is repopulated. */
static void jw__scrape_download_rebuild_rows(jw_settings_ui *ui) {
    if (!ui) return;
    jw__scrape_download_clear_rows(ui);

    const jw_ipc_scrape_missing_info *m = &ui->scrape_missing_cache;
    int count = m->system_count;
    if (count > JW_IPC_MISSING_MAX_SYSTEMS) count = JW_IPC_MISSING_MAX_SYSTEMS;

    for (int i = 0; i < count; i++) {
        const jw_ipc_scrape_missing_row *src = &m->systems[i];
        jw_scrape_download_row *dst = &ui->scrape_download_rows[ui->scrape_download_row_count];

        snprintf(dst->system, sizeof(dst->system), "%s", src->system);
        dst->missing = src->missing;
        dst->total = src->total;
        ui->scrape_download_total_missing += src->missing;
        ui->scrape_download_total_games += src->total;

        char display[JW_SCRAPE_DOWNLOAD_LABEL_MAX];
        jw_system_display_name(ui->db_path, src->system, display, sizeof(display));
        jw__copy_trimmed_or_fallback(dst->label, sizeof(dst->label),
                                     display, src->system);
        ui->scrape_download_row_count++;
    }

    for (int i = 0; i < ui->scrape_download_row_count; i++) {
        int duplicate = 0;
        for (int j = 0; j < ui->scrape_download_row_count; j++) {
            if (i != j &&
                jw__scrape_label_equal(ui->scrape_download_rows[i].label,
                                       ui->scrape_download_rows[j].label)) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            jw__scrape_download_suffix_label(&ui->scrape_download_rows[i]);
        }
    }

    if (ui->scrape_download_row_count > 1) {
        qsort(ui->scrape_download_rows,
              (size_t)ui->scrape_download_row_count,
              sizeof(ui->scrape_download_rows[0]),
              jw__scrape_download_row_cmp);
    }
}

/* "Scrape Missing Artwork" picker: row 0 = All Systems, rows 1.. = one system
   each. Y toggles missing-only vs replace-all; in replace mode the right-hand
   count is the system's total games rather than its missing count. */
typedef struct {
    const jw_settings_ui *ui;
    bool replace;        /* replace-all mode */
    int  all_count;      /* count for the "All Systems" row (mode-dependent) */
} jw__scrape_download_ctx;

static void jw__draw_scrape_download_item(int idx, int ix, int iy, int iw, int ih,
                                          float focus, void *user) {
    jw__scrape_download_ctx *c = (jw__scrape_download_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    const char *label = T("All Systems");
    char value[32];
    int n;
    if (idx == 0) {
        n = c->all_count;
    } else if (idx - 1 < c->ui->scrape_download_row_count) {
        const jw_scrape_download_row *r = &c->ui->scrape_download_rows[idx - 1];
        label = r->label;
        n = c->replace ? r->total : r->missing;
    } else {
        n = 0;
    }
    snprintf(value, sizeof(value), c->replace ? "%d total" : "%d missing", n);
    cat_draw_text_ellipsized(body, label, ix + cat_scale(18), ty, label_c,
                             iw / 2);
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_scrape_download(const jw_settings_ui *ui,
                                       int x, int y, int w, int h) {
    bool replace = ui->scrape_download_replace;
    char header[48];
    snprintf(header, sizeof(header), "Scrape Artwork - %s",
             replace ? T("Replace All") : T("Missing"));
    jw__draw_header(header, x, y, w);

    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    int sub_h = jw__subheader_line_h(small) + cat_scale(6);
    SDL_Rect sub;
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, sub_h, NULL, &sub);

    if (!ui->scrape_missing_have_cache) {
        cat_draw_text_ellipsized(small, T("Scanning library..."),
                                 sub.x + cat_scale(12), sub.y, theme->hint,
                                 sub.w - cat_scale(24));
        cat_draw_text(body, T("Scanning library..."),
                      c.x + cat_scale(4), c.y + cat_scale(4), theme->hint);
        return;
    }

    /* Grand total for the "All Systems" row + subline (replace = every game). */
    int all_count = replace
        ? ui->scrape_download_total_games
        : ui->scrape_download_total_missing;

    char subline[96];
    snprintf(subline, sizeof(subline),
             replace ? "%d games across %d systems"
                     : "%d missing across %d systems",
             all_count, ui->scrape_download_row_count);
    cat_draw_text_ellipsized(small, subline, sub.x + cat_scale(12), sub.y,
                             theme->hint, sub.w - cat_scale(24));

    int count = ui->scrape_download_row_count + 1;   /* +1 for "All Systems" */
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
    ((cat_list_state *)&ui->scrape_download_list)->visible_rows = vis;
    jw__scrape_download_ctx ctx = { ui, replace, all_count };
    jw__draw_settings_list(lr.x, lr.y, lr.w, lr.h, count,
                           (cat_list_state *)&ui->scrape_download_list, item_h,
                           jw__draw_scrape_download_item, &ctx);
}

static const char *jw__scrape_queue_state_label(jw_ipc_scrape_row_state state) {
    switch (state) {
        case JW_IPC_SCRAPE_ROW_QUEUED:    return JW_UI("Queued");
        case JW_IPC_SCRAPE_ROW_HASH:      return JW_UI("Hashing");
        case JW_IPC_SCRAPE_ROW_SEARCH:    return JW_UI("Searching");
        case JW_IPC_SCRAPE_ROW_DOWNLOAD:  return JW_UI("Downloading");
        case JW_IPC_SCRAPE_ROW_SAVE:      return JW_UI("Saving");
        case JW_IPC_SCRAPE_ROW_DONE:      return JW_UI("Done");
        case JW_IPC_SCRAPE_ROW_NOT_FOUND: return JW_UI("Not Found");
        case JW_IPC_SCRAPE_ROW_ERROR:     return JW_UI("Error");
        case JW_IPC_SCRAPE_ROW_CANCELLED: return JW_UI("Cancelled");
        default:                          return JW_UI("Queued");
    }
}

static cat_queue_status jw__scrape_queue_cat_status(jw_ipc_scrape_row_state state) {
    switch (state) {
        case JW_IPC_SCRAPE_ROW_DONE:      return CAT_QUEUE_DONE;
        case JW_IPC_SCRAPE_ROW_NOT_FOUND:
        case JW_IPC_SCRAPE_ROW_ERROR:     return CAT_QUEUE_FAILED;
        case JW_IPC_SCRAPE_ROW_CANCELLED: return CAT_QUEUE_SKIPPED;
        case JW_IPC_SCRAPE_ROW_QUEUED:    return CAT_QUEUE_PENDING;
        default:                          return CAT_QUEUE_RUNNING;
    }
}

static void jw__scrape_queue_format_eta(int seconds, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (seconds < 0) {
        buf[0] = '\0';
    } else if (seconds < 60) {
        snprintf(buf, buf_size, "%ds", seconds);
    } else {
        int minutes = (seconds + 59) / 60;
        if (minutes < 60)
            snprintf(buf, buf_size, "%dm", minutes);
        else
            snprintf(buf, buf_size, "%dh %dm", minutes / 60, minutes % 60);
    }
}

static jw_ipc_scrape_queue_info *jw__scrape_queue_cache(jw_settings_ui *ui) {
    if (!ui) return NULL;
    if (!ui->scrape_queue_cache) {
        ui->scrape_queue_cache =
            (jw_ipc_scrape_queue_info *)calloc(1, sizeof(*ui->scrape_queue_cache));
    }
    return ui->scrape_queue_cache;
}

static void jw__scrape_queue_refresh_settings_cache(jw_settings_ui *ui,
                                                    bool force) {
    if (!ui || !ui->socket_path[0]) return;
    unsigned now = SDL_GetTicks();
    if (!force && ui->scrape_queue_have_cache &&
        now < ui->scrape_queue_next_poll_ms) {
        return;
    }
    jw_ipc_scrape_queue_info *info = jw__scrape_queue_cache(ui);
    if (info && jw_ipc_scrape_queue(ui->socket_path, 0, 1, info) == 0) {
        ui->scrape_queue_have_cache = true;
        ui->scrape_queue_next_poll_ms = now + 1000u;
    }
}

static void jw__scrape_queue_settings_value(jw_settings_ui *ui,
                                            char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    snprintf(buf, buf_size, "%s", "Open");
    jw__scrape_queue_refresh_settings_cache(ui, false);
    if (!ui || !ui->scrape_queue_have_cache) {
        return;
    }
    const jw_ipc_scrape_queue_info *q = ui->scrape_queue_cache;
    if (!q) return;
    int failed = q->failed + q->not_found;
    if (strcmp(q->state, "paused-quota") == 0) {
        snprintf(buf, buf_size, "%s", "Quota paused");
    } else if (q->active > 0 || q->queued > 0) {
        snprintf(buf, buf_size, T("%d/%d done"), q->done, q->total);
    } else if (q->total <= 0) {
        snprintf(buf, buf_size, "%s", "Empty");
    } else if (failed > 0) {
        snprintf(buf, buf_size, T("%d done, %d failed"), q->done, failed);
    } else {
        snprintf(buf, buf_size, T("%d done"), q->done);
    }
}

/* ── Scrape queue: native settings page (replaces the cat_queue_viewer modal) ──
 * Reuses the data layer above (cache, state labels, settings-value); renders
 * ui->scrape_queue_cache directly through the box model. Poll is driven from the
 * render fn (throttled), so no host-loop wiring is needed. */

/* Full-row poll into the shared cache. The Game Art row's
   jw__scrape_queue_refresh_settings_cache fetches a single row for its summary;
   the page needs every row. Throttled — safe to call every frame. */
static void jw__scrape_queue_poll(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    unsigned now = SDL_GetTicks();
    if (ui->scrape_queue_have_cache && now < ui->scrape_queue_next_poll_ms) return;
    jw_ipc_scrape_queue_info *info = jw__scrape_queue_cache(ui);
    if (info && jw_ipc_scrape_queue(ui->socket_path, 0,
                                    JW_IPC_SCRAPE_QUEUE_MAX_ROWS, info) == 0) {
        ui->scrape_queue_have_cache = true;
        ui->scrape_queue_next_poll_ms = now + 500u;
    }
}

/* True while any job is pending/running — drives the live redraw and selects
   X = Stop All vs Clear Done. */
static bool jw__scrape_queue_active(const jw_ipc_scrape_queue_info *q) {
    return q && (q->active > 0 || q->queued > 0);
}

static bool jw__scrape_queue_row_passes(jw_ipc_scrape_row_state st, int filter) {
    cat_queue_status s = jw__scrape_queue_cat_status(st);
    switch (filter) {
        case 1:  return s == CAT_QUEUE_PENDING || s == CAT_QUEUE_RUNNING; /* Busy   */
        case 2:  return s == CAT_QUEUE_DONE || s == CAT_QUEUE_SKIPPED;    /* Done   */
        case 3:  return s == CAT_QUEUE_FAILED;                            /* Failed */
        default: return true;                                            /* All    */
    }
}

/* Build the visible (filtered) row-index list into idx[]; returns the count. */
static int jw__scrape_queue_filter_rows(const jw_ipc_scrape_queue_info *q,
                                        int filter, int *idx, int max) {
    if (!q || !idx) return 0;
    int n = 0;
    for (int i = 0; i < q->row_count && n < max; i++) {
        if (jw__scrape_queue_row_passes(q->rows[i].state, filter)) idx[n++] = i;
    }
    return n;
}

static const char *jw__scrape_queue_filter_label(int filter) {
    switch (filter) {
        case 1:  return JW_UI("Busy");
        case 2:  return JW_UI("Done");
        case 3:  return JW_UI("Failed");
        default: return JW_UI("All");
    }
}

/* Sub-header summary line (counts + quota/threads/ETA), from the old modal
   summary but driven by the cache instead of a callback context. */
static void jw__scrape_queue_summary(const jw_ipc_scrape_queue_info *q,
                                     char *buf, size_t buf_size) {
    if (!buf || buf_size == 0 || !q) return;
    int failed = q->failed + q->not_found;
    char eta[32] = "";
    jw__scrape_queue_format_eta(q->eta_seconds, eta, sizeof(eta));
    char api[64] = "";
    if (q->max_requests > 0) {
        snprintf(api, sizeof(api), " | API %d/%d", q->requests_today, q->max_requests);
    }
    char threads[40] = "";
    if (q->max_threads > 0) {
        snprintf(threads, sizeof(threads), " | %d/%d threads", q->permits, q->max_threads);
    } else if (q->permits > 0) {
        snprintf(threads, sizeof(threads), " | %d thread", q->permits);
    }
    char eta_part[64] = "";
    if (eta[0]) snprintf(eta_part, sizeof(eta_part), " | ETA %s", eta);
    char shown[48] = "";
    if (q->row_count > 0 && q->row_count < q->total) {
        snprintf(shown, sizeof(shown), " | showing first %d", q->row_count);
    }
    const char *prefix = strcmp(q->state, "paused-quota") == 0 ? "Quota paused, " : "";
    snprintf(buf, buf_size, "%s%d/%d done, %d failed, %d busy%s%s%s%s",
             prefix, q->done, q->total, failed, q->active + q->queued,
             api, threads, eta_part, shown);
}

static void jw__scrape_start_status(const jw_ipc_scrape_start_info *info,
                                    bool missing_only,
                                    char *buf, size_t buf_size) {
    if (!info || !buf || buf_size == 0) return;
    if (info->queue_full) {
        if (info->enqueued > 0) {
            snprintf(buf, buf_size, "Queue full: queued %d of %d",
                     info->enqueued, info->requested);
        } else {
            snprintf(buf, buf_size, "%s", "Scrape queue is full");
        }
    } else if (info->enqueued > 0) {
        if (info->already_queued > 0) {
            snprintf(buf, buf_size, "Queued %d, %d already queued",
                     info->enqueued, info->already_queued);
        } else {
            snprintf(buf, buf_size, "Queued %d artwork job%s",
                     info->enqueued, info->enqueued == 1 ? "" : "s");
        }
    } else if (info->already_queued > 0) {
        snprintf(buf, buf_size, "%d artwork job%s already queued",
                 info->already_queued,
                 info->already_queued == 1 ? "" : "s");
    } else if (missing_only && info->skipped_existing > 0) {
        snprintf(buf, buf_size, "%s", "No missing artwork");
    } else if (info->requested == 0) {
        snprintf(buf, buf_size, "%s",
                 missing_only ? "No missing artwork" : "No games to scrape");
    } else {
        snprintf(buf, buf_size, "%s", "No new scrape jobs");
    }
}

typedef struct {
    const jw_settings_ui *ui;
    const int *idx;
    int count;
} jw__scrape_queue_draw_ctx;

static void jw__draw_scrape_queue_focus(int x, int y, int w, int h, void *user) {
    (void)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int block_h = TTF_FontHeight(body) + cat_scale(2) + TTF_FontHeight(small);
    int pill_h = block_h + cat_scale(10);
    int pill_y = y + (h - pill_h) / 2;
    cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);
}

/* One queue row: title + system subtitle + a status-colored state badge. */
static void jw__draw_scrape_queue_item(int i, int ix, int iy, int iw, int ih,
                                       float focus, void *user) {
    jw__scrape_queue_draw_ctx *c = (jw__scrape_queue_draw_ctx *)user;
    if (!c || !c->ui || !c->ui->scrape_queue_cache || i < 0 || i >= c->count) return;
    const jw_ipc_scrape_queue_row *row = &c->ui->scrape_queue_cache->rows[c->idx[i]];

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    /* fit_rows stretches rows to fill the box, so center the two-line text block
       (and the pill that wraps it) vertically rather than top-aligning, and use
       the standard cat_scale(12) left inset. */
    int line_gap = cat_scale(2);
    int block_h  = TTF_FontHeight(body) + line_gap + TTF_FontHeight(small);
    int block_y  = iy + (ih - block_h) / 2;
    int tx       = ix + cat_scale(24);
    int text_w   = iw - cat_scale(24) - cat_scale(12);
    /* Title in normal text; the "system - state" subtitle carries the status
       color (green done / red failed / accent running / hint queued) so a row's
       state reads at a glance without a cramped right-hand badge. */
    ap_color main_c = cat_draw_color_lerp(theme->text,
                                           theme->highlighted_text, focus);
    ap_color sub_base;
    switch (jw__scrape_queue_cat_status(row->state)) {
        case CAT_QUEUE_DONE:    sub_base = cat_hex_to_color("#64c864"); break;
        case CAT_QUEUE_FAILED:  sub_base = cat_hex_to_color("#ff6464"); break;
        case CAT_QUEUE_RUNNING: sub_base = theme->accent;               break;
        default:                sub_base = theme->hint;                 break;
    }
    ap_color sub_c = cat_draw_color_lerp(sub_base,
                                          theme->highlighted_text, focus);

    const char *label = row->display_name[0] ? row->display_name : row->rom_path;
    char subtitle[160];
    snprintf(subtitle, sizeof(subtitle), "%s  -  %s", row->system,
             jw__scrape_queue_state_label(row->state));
    cat_draw_text_ellipsized(body, label, tx, block_y, main_c, text_w);
    cat_draw_text_ellipsized(small, subtitle, tx,
                             block_y + TTF_FontHeight(body) + line_gap,
                             sub_c, text_w);
}

/* Result-detail drill for a finished row (A button). Transient cat_detail_screen,
   like Leaf's other detail views; the main queue page stays on the box model. */
/* One scrape job's result — a native settings sub-screen (was a cat_detail_screen
   modal). Status/System/ROM/Output stack full-width up top so long paths get the
   most room; the scraped art sits centered below, aspect-fit (the old modal forced
   it into a fixed 220x160 box, squishing it) and rounded to match the games tab. */
static void jw__render_scrape_queue_detail(const jw_settings_ui *ui,
                                           int x, int y, int w, int h) {
    const jw_ipc_scrape_queue_row *row = &ui->scrape_queue_detail_row;
    jw__draw_header(row->display_name[0] ? row->display_name : "Scrape Result", x, y, w);

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);

    /* Full-width key/value rows: keys are drawn whole (never ellipsized), values
       ellipsize to the rest of the width. */
    const char *info[][2] = {
        { "Status", jw__scrape_queue_state_label(row->state) },
        { "System", row->system },
        { "ROM",    row->rom_path },
        { JW_UI("Output"), row->output_path[0] ? row->output_path : "Not written" },
    };
    int key_w = 0;
    for (int i = 0; i < 4; i++) {
        int kw = cat_measure_text(body, info[i][0]);
        if (kw > key_w) key_w = kw;
    }
    key_w += cat_scale(18);

    /* Overflowing values scroll through (loop) instead of truncating — same
       calm continuous marquee as the About/Accounts rows. */
    static uint32_t last_ms = 0;
    uint32_t now = SDL_GetTicks();
    uint32_t dt  = last_ms ? now - last_ms : 0u;
    last_ms = now;
    cat_marquee *mq = ((jw_settings_ui *)ui)->scrape_detail_marquee;
    bool animating = false;

    int line_h = TTF_FontHeight(body) + cat_scale(12);
    int ly = c.y;
    for (int i = 0; i < 4; i++) {
        cat_draw_text(body, info[i][0], c.x, ly, theme->hint);
        mq[i].mode = CAT_MARQUEE_LOOP;
        if (cat_draw_text_marquee(body, info[i][1], c.x + key_w, ly, theme->text,
                                  c.w - key_w, &mq[i], dt))
            animating = true;
        ly += line_h;
    }
    if (animating) cat_request_frame();

    /* Error/cancel note below the info. */
    const char *desc = NULL;
    if (row->state == JW_IPC_SCRAPE_ROW_NOT_FOUND)
        desc = T("Not found in the ScreenScraper.fr database.");
    else if (row->state == JW_IPC_SCRAPE_ROW_ERROR)
        desc = row->message[0] ? row->message : "Artwork scrape failed.";
    else if (row->state == JW_IPC_SCRAPE_ROW_CANCELLED)
        desc = row->message[0] ? row->message : "Scrape was cancelled before completion.";
    if (desc) {
        ly += cat_scale(4);
        cat_draw_text_ellipsized(small, desc, c.x, ly, theme->hint, c.w);
        ly += TTF_FontHeight(small);
    }

    /* The scraped art (decoded once on open), centered in the space below the
       text, aspect-fit and rounded to the current List Style (same recipe as
       jw__draw_cover_fit). */
    SDL_Texture *art = ui->scrape_detail_art;
    int tw = ui->scrape_detail_art_w, th = ui->scrape_detail_art_h;
    if (art && tw > 0 && th > 0) {
        int top = ly + cat_scale(16);
        int avail_h = (c.y + c.h) - top;
        if (avail_h > cat_scale(40)) {
            int dw = c.w;
            int dh = th * dw / tw;
            if (dh > avail_h) { dh = avail_h; dw = tw * dh / th; }
            int dx = c.x + (c.w - dw) / 2;
            int dy = top + (avail_h - dh) / 2;
            int smaller = dw < dh ? dw : dh;
            int radius  = (int)(theme->pill_radius_ratio * smaller * 0.26f + 0.5f);
            unsigned corners = (unsigned)theme->pill_corner_mask;
            if (corners == 0) corners = CAT_CORNER_ALL;
            cat_draw_image_rounded_ex(art, dx, dy, dw, dh, radius, corners);
        }
    }
}

static void jw__render_scrape_queue(const jw_settings_ui *ui,
                                    int x, int y, int w, int h) {
    char header[64];
    snprintf(header, sizeof(header), T("Scrape Queue - %s"),
             T(jw__scrape_queue_filter_label(ui->scrape_queue_filter)));
    jw__draw_header(header, x, y, w);
    jw__scrape_queue_poll((jw_settings_ui *)ui);   /* throttled live poll */

    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    int sub_h = jw__subheader_line_h(small) + cat_scale(6);
    SDL_Rect sub;
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, sub_h, NULL, &sub);

    const jw_ipc_scrape_queue_info *q =
        ui->scrape_queue_have_cache ? ui->scrape_queue_cache : NULL;

    char summary[160] = "";
    if (q) jw__scrape_queue_summary(q, summary, sizeof(summary));
    /* The counts/API/threads/ETA line runs long — scroll it through rather than
       truncating (it keeps scrolling as the live counts update). */
    static cat_marquee summ_mq;
    static uint32_t summ_last_ms = 0;
    uint32_t summ_now = SDL_GetTicks();
    uint32_t summ_dt  = summ_last_ms ? summ_now - summ_last_ms : 0u;
    summ_last_ms = summ_now;
    summ_mq.mode = CAT_MARQUEE_LOOP;
    if (cat_draw_text_marquee(small, summary[0] ? summary : T("Empty"),
                              sub.x + cat_scale(12), sub.y, theme->hint,
                              sub.w - cat_scale(24), &summ_mq, summ_dt))
        cat_request_frame();

    int idx[JW_IPC_SCRAPE_QUEUE_MAX_ROWS];
    int count = q ? jw__scrape_queue_filter_rows(q, ui->scrape_queue_filter,
                                                 idx, JW_IPC_SCRAPE_QUEUE_MAX_ROWS)
                  : 0;

    cat_list_state *list = (cat_list_state *)&ui->scrape_queue_list;
    if (list->cursor >= count) list->cursor = count > 0 ? count - 1 : 0;

    if (count == 0) {
        const char *msg = (q && q->total > 0) ? "No jobs match this filter."
                                              : "No scrape jobs.";
        int tw = cat_measure_text(body, msg);
        cat_draw_text(body, msg, c.x + (c.w - tw) / 2,
                      c.y + (c.h - TTF_FontHeight(body)) / 2, theme->hint);
    } else {
        int item_h = TTF_FontHeight(body) + TTF_FontHeight(small) + cat_scale(12);
        cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
        int vis = 0;
        SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
        list->visible_rows = vis;
        jw__scrape_queue_draw_ctx dctx = { ui, idx, count };
        cat_draw_list_pane_layered(lr.x, lr.y, lr.w, lr.h, count, list, item_h,
                                   jw__draw_scrape_queue_focus,
                                   jw__draw_scrape_queue_item, &dctx);
    }

    if (jw__scrape_queue_active(q)) cat_request_frame();
}

static void jw__render_scraping(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Game Art", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->scraping_list, x, ly, w,
                            JW_SCRAPING_ROW_COUNT, item_h);

    char download_value[64];
    if (ui->scrape_missing_have_cache) {
        int n = ui->scrape_missing_cache.total_missing;
        snprintf(download_value, sizeof(download_value), "%d missing", n);
    } else {
        download_value[0] = '\0';
    }
    jw__render_list_row(&ui->scraping_list, x, ly, w, JW_SCRAPING_DOWNLOAD,
                        "Scrape Artwork", download_value, false);

    char queue_value[64];
    jw__scrape_queue_settings_value((jw_settings_ui *)ui, queue_value,
                                    sizeof(queue_value));
    jw__render_list_row(&ui->scraping_list, x, ly, w, JW_SCRAPING_QUEUE,
                        "Scrape Queue", queue_value, false);

    char artwork_value[64];
    if (ui->scrape_artwork_included > 0) {
        snprintf(artwork_value, sizeof(artwork_value), T("%s first"),
                 jw_ss_media_types[ui->scrape_artwork_order[0]].display);
    } else {
        snprintf(artwork_value, sizeof(artwork_value), "None selected");
    }
    jw__render_list_row(&ui->scraping_list, x, ly, w, JW_SCRAPING_ARTWORK,
                        "Artwork Priority", artwork_value, false);

    char region_value[64];
    if (ui->scrape_region_included > 0) {
        snprintf(region_value, sizeof(region_value), T("%s first"),
                 jw_ss_regions[ui->scrape_region_order[0]].display);
    } else {
        snprintf(region_value, sizeof(region_value), "None selected");
    }
    jw__render_list_row(&ui->scraping_list, x, ly, w, JW_SCRAPING_REGION,
                        "Region Priority", region_value, false);
}

/* Account row: like jw__render_list_row, but the status value marquees while the
   row is selected so a long "Signed in as … threads … quota …" line scrolls into
   view instead of running off the right edge. Unselected rows ellipsize it. The
   value stays right-aligned when it already fits. Returns true while animating. */
/* label only: `value` here can be an account name the user typed, and a name
   that happened to match a UI key would otherwise be "translated". */
static bool jw__render_account_row(const cat_list_state *list, int x, int y,
                                   int w, int row, const char *label,
                                   const char *value, cat_marquee *mq,
                                   uint32_t dt) {
    label = T(label);   /* value stays raw: it can be a user-supplied account name */
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int iy = y + row * item_h;
    float focus = jw__settings_row_focus(list, row);
    bool settled = focus >= 0.999f;
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, label_c,
                             w / 2 - cat_scale(20));

    bool anim = false;
    if (value && value[0]) {
        int value_x = x + w / 2;
        int value_w = (x + w - cat_scale(16)) - value_x;
        int vw = cat_measure_text(body, value);
        if (value_w <= 0) {
            /* no room */
        } else if (vw <= value_w) {
            cat_draw_text(body, value, x + w - vw - cat_scale(16), ty, value_c);
        } else if (settled) {
            if (mq) mq->mode = CAT_MARQUEE_LOOP;
            anim = cat_draw_text_marquee(body, value, value_x, ty, value_c,
                                         value_w, mq, dt);
        } else {
            if (mq) mq->elapsed_ms = 0;   /* restart the scroll next time it's selected */
            cat_draw_text_ellipsized(body, value, value_x, ty, value_c, value_w);
        }
    }
    return anim;
}

static void jw__render_accounts(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Accounts", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->accounts_list, x, ly, w,
                            JW_ACCOUNTS_ROW_COUNT, item_h);

    /* Per-row marquee state (one Accounts screen is live at a time). */
    static cat_marquee mq[JW_ACCOUNTS_ROW_COUNT];
    static uint32_t    last_ms = 0;
    uint32_t now = SDL_GetTicks();
    uint32_t dt  = (last_ms == 0) ? 0u : (now - last_ms);
    last_ms = now;
    bool anim = false;

    /* "Signed in" only after the daemon has validated the credentials against
       the API (scrape-validate IPC); "Saved" means stored but unverified
       (daemon or network unavailable at sign-in). */
    char ss_value[160];
    if (ui->ss_username[0] && ui->ss_verified) {
        char quota[32] = "";
        if (ui->ss_max_requests > 0) {
            snprintf(quota, sizeof(quota), ", quota %d/%d",
                     ui->ss_requests_today, ui->ss_max_requests);
        }
        if (ui->ss_max_threads > 0) {
            snprintf(ss_value, sizeof(ss_value), "Signed in as %s - %d thread%s%s",
                     ui->ss_username, ui->ss_max_threads,
                     ui->ss_max_threads == 1 ? "" : "s", quota);
        } else {
            snprintf(ss_value, sizeof(ss_value), "Signed in as %s%s",
                     ui->ss_username, quota);
        }
    } else if (ui->ss_username[0]) {
        snprintf(ss_value, sizeof(ss_value), "Saved: %s (unverified)", ui->ss_username);
    } else if (ui->ss_rejected) {
        snprintf(ss_value, sizeof(ss_value), "Rejected - wrong username or password");
    } else {
        snprintf(ss_value, sizeof(ss_value), "Not signed in");
    }
    anim |= jw__render_account_row(&ui->accounts_list, x, ly, w,
                                   JW_ACCOUNTS_SCREENSCRAPER, "ScreenScraper.fr",
                                   ss_value, &mq[JW_ACCOUNTS_SCREENSCRAPER], dt);
    char ra_value[96];
    if (ui->ra_username[0]) {
        snprintf(ra_value, sizeof(ra_value), "Saved: %s", ui->ra_username);
    } else {
        snprintf(ra_value, sizeof(ra_value), "Not signed in");
    }
    anim |= jw__render_account_row(&ui->accounts_list, x, ly, w,
                                   JW_ACCOUNTS_RETROACHIEVEMENTS, "RetroAchievements",
                                   ra_value, &mq[JW_ACCOUNTS_RETROACHIEVEMENTS], dt);
    if (anim) cat_request_frame();
}

/* ─── About ────────────────────────────────────────────────────────────── */

typedef struct { const char *name; const char *license; } jw__about_credit;

/* Third-party components shipped with / used by the CFW and their licenses.
   Short names only — the full license texts ship with each component and live
   in the respective repos. */
static const jw__about_credit kAboutCredits[] = {
    { "Jawaka + Catastrophe",                   "MIT" },
    { "RetroArch",                              "GPLv3" },
    { "Libretro cores",                         "GPL / per-core" },
    { "SDL2 / SDL2_image / SDL2_ttf",           "Zlib" },
    { "FreeType",                               "FreeType License" },
    { "HarfBuzz",                               "MIT" },
    { "libpng",                                 "libpng License" },
    { "zlib",                                   "Zlib License" },
    { "SQLite",                                 "Public Domain" },
    { "cJSON",                                  "MIT" },
    { "System icons (libretro Systematic)",     "CC BY-SA 4.0" },
    { "Coverflow console art (Evan Amos)",       "Public Domain" },
    { "Fonts: Space Grotesk, Inter, Rounded M+, Nunito, Baloo 2, Fredoka, "
      "Lexend, IBM Plex Sans, Noto Sans, Source Han Sans", "SIL OFL 1.1" },
    { "Keyboard icons (Nerd Fonts)",            "MIT" },
    { "Dropbear SSH",                           "MIT-style" },
};
#define JW_ABOUT_CREDIT_COUNT ((int)(sizeof(kAboutCredits) / sizeof(kAboutCredits[0])))

/* One row of the About content. The whole thing — identity, a System block, a
   Library block, and the open-source components — is a flat list of these,
   rendered inside one scroll view. */
typedef enum {
    JW_ABOUT_PLAIN,     /* single left label (identity)            */
    JW_ABOUT_HEADING,   /* section heading (accent)                */
    JW_ABOUT_FIELD,     /* field name (left, dim) : value (right)  */
    JW_ABOUT_CREDIT     /* component (left) : license (right, dim) */
} jw__about_kind;

typedef struct {
    jw__about_kind kind;
    char label[128];   /* roomy enough for the full bundled-font credit line */
    char value[72];
} jw__about_row;

#define JW_ABOUT_MAX_ROWS 64

typedef struct {
    const jw__about_row *rows;
    int                  count;
    TTF_Font            *font;
    int                  row_h;
} jw__about_ctx;

/* cat_scroll_view content callback: lay out every row at its natural position.
   Long labels/values marquee instead of truncating; the scroll view applies the
   offset and clips. */
/* Record how far this scroll view can actually travel, so the input path can
   clamp before the haptic wrapper reads the position. cat_draw_scroll_view
   derives the same number, but only at render time -- and by then the wrapper
   has already decided a press that will be clamped away counted as movement. */
static void jw__scroll_publish_max(int *out_max, int content_h, int view_h) {
    int max_offset = content_h - view_h;
    *out_max = max_offset > 0 ? max_offset : 0;
}

/* Move a scroll view, clamping BOTH ends. The clamp used to live here because
   cat_scroll_state_move only handled the zero end, which made Down at the bottom
   report movement that the next render quietly undid. It lives in the widget
   now, which is also what lets it tell the bottom of the page from a real
   scroll and report the boundary itself. */
static void jw__scroll_move(cat_scroll_state *s, int delta_px, int max_offset) {
    cat_scroll_state_move_clamped(s, delta_px, max_offset);
}

static void jw__draw_about_rows(int x, int y, int w, void *user) {
    const jw__about_ctx *ctx = (const jw__about_ctx *)user;
    ap_theme *theme = cat_get_theme();
    /* Per-row marquee state (only one About screen is live at a time). */
    static cat_marquee label_mq[JW_ABOUT_MAX_ROWS];
    static cat_marquee value_mq[JW_ABOUT_MAX_ROWS];
    static uint32_t last_ms = 0;
    uint32_t now = SDL_GetTicks();
    uint32_t dt  = (last_ms == 0) ? 0u : (now - last_ms);
    last_ms = now;

    bool animating = false;
    for (int i = 0; i < ctx->count && i < JW_ABOUT_MAX_ROWS; i++) {
        const jw__about_row *r = &ctx->rows[i];
        int row_y = y + i * ctx->row_h;
        if (r->kind == JW_ABOUT_HEADING) {
            if (cat_draw_text_marquee(ctx->font, r->label, x, row_y, theme->emphasis, w, &label_mq[i], dt))
                animating = true;
            continue;
        }
        if (r->kind == JW_ABOUT_PLAIN) {
            if (cat_draw_text_marquee(ctx->font, r->label, x, row_y, theme->text, w, &label_mq[i], dt))
                animating = true;
            continue;
        }
        /* FIELD: dim label + bright value. CREDIT: bright name + dim license. */
        int label_w, value_x;
        ap_color label_col, value_col;
        if (r->kind == JW_ABOUT_FIELD) {
            label_w   = w * 40 / 100;
            value_x   = x + w * 42 / 100;
            label_col = theme->hint;
            value_col = theme->text;
        } else {
            label_w   = w * 56 / 100;
            value_x   = x + w * 60 / 100;
            label_col = theme->text;
            value_col = theme->hint;
        }
        int value_w = (x + w) - value_x;
        /* Every overflowing line scrolls through continuously and wraps (no
           ping-pong bounce) for a calmer, uniform read. */
        label_mq[i].mode = CAT_MARQUEE_LOOP;
        value_mq[i].mode = CAT_MARQUEE_LOOP;
        if (cat_draw_text_marquee(ctx->font, r->label, x, row_y, label_col, label_w, &label_mq[i], dt))
            animating = true;
        if (cat_draw_text_marquee(ctx->font, r->value, value_x, row_y, value_col, value_w, &value_mq[i], dt))
            animating = true;
    }
    if (animating) cat_request_frame();
}

/* Append a row (no-op once full). */
static void jw__about_push(jw__about_row *rows, int *n, jw__about_kind kind,
                           const char *label, const char *value) {
    if (*n >= JW_ABOUT_MAX_ROWS) return;
    rows[*n].kind = kind;
    snprintf(rows[*n].label, sizeof(rows[*n].label), "%s", label ? T(label) : "");
    snprintf(rows[*n].value, sizeof(rows[*n].value), "%s", value ? value : "");
    (*n)++;
}

/* Page title for the Info pages (Device / Library / Playtime), matching the game
   browser's drilled-in title: CAT_FONT_EXTRA_LARGE, theme text, no underline — so
   the system side reads the same as the content side. Returns the content top y
   and the remaining height below the title. */
static void jw__draw_info_title(const char *title, int x, int y, int w, int h,
                                int *out_top, int *out_h) {
    title = T(title);
    TTF_Font *tf = cat_get_font(CAT_FONT_EXTRA_LARGE);
    cat_draw_text_ellipsized(tf, title, x + cat_scale(4), y,
                             cat_get_theme()->text, w - cat_scale(8));
    int top = y + TTF_FontHeight(tf) + cat_scale(4);
    if (out_top) *out_top = top;
    if (out_h)   *out_h   = (y + h) - top;
}

static void jw__render_about(const jw_settings_ui *ui, int x, int y, int w, int h) {
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(6);
    int top, view_h;
    jw__draw_info_title("Device", x, y, w, h, &top, &view_h);

    /* Live system facts, refreshed about once a second while the page is open
       (cheap read; only one Device screen is live at a time). Library counts now
       live on their own Info > Library page, so they aren't duplicated here. */
    static jw_system_info    info;
    static uint32_t          last_refresh = 0;
    static bool              have = false;
    uint32_t now = SDL_GetTicks();
    if (!have || now - last_refresh > 1000) {
        jw_platform_system_info(ui->db_path, &info);
        last_refresh = now;
        have = true;
    }

    jw__about_row rows[JW_ABOUT_MAX_ROWS];
    int n = 0;
    char buf[72];

    jw_installed_release release;
    memset(&release, 0, sizeof(release));
    const char *internal_data = getenv("UMRK_INTERNAL_DATA_PATH");
    bool have_release =
        internal_data && internal_data[0] &&
        jw_installed_release_read(internal_data, &release) == 0 &&
        (release.version[0] || release.release_id[0]);
    char identity[96];
    snprintf(identity, sizeof(identity), "Leaf  %s",
             (have_release && release.version[0]) ? release.version : "Unknown");
    jw__about_push(rows, &n, JW_ABOUT_PLAIN, identity, "");

    jw__about_push(rows, &n, JW_ABOUT_HEADING, "System", "");
    /* Release id pins the exact installed artifact; show it when it adds info
       beyond the version string (per the OTA installed-version contract). */
    if (have_release && release.release_id[0] &&
        strcmp(release.release_id, release.version) != 0) {
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Release",
                       release.release_id);
    }
    snprintf(buf, sizeof(buf), "%s", info.os_version[0] ? info.os_version : "?");
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "OS", buf);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Kernel", info.kernel[0] ? info.kernel : "—");
    /* Hardware: prefer the parsed labeled lines (SoC / Power / Board / RAM);
       fall back to the raw device-tree model when nothing parsed. */
    if (info.soc[0] || info.pmic[0] || info.board[0]) {
        if (info.soc[0])
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "SoC", info.soc);
        if (info.pmic[0]) {
            snprintf(buf, sizeof(buf), "%s PMIC", info.pmic);
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "Power", buf);
        }
        if (info.board[0])
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "Board", info.board);
        if (info.ram_type[0] && info.mem_total_kb > 0) {
            long ram_gb = (info.mem_total_kb + 524288) / 1048576;   /* round kB → GB */
            if (ram_gb < 1) ram_gb = 1;
            snprintf(buf, sizeof(buf), "%ld GB %s", ram_gb, info.ram_type);
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "RAM", buf);
        } else if (info.ram_type[0]) {
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "RAM", info.ram_type);
        }
    } else {
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Device", info.device[0] ? info.device : "—");
    }
    if (info.mem_total_kb > 0) {
        snprintf(buf, sizeof(buf), "%ld / %ld MB", info.mem_avail_kb / 1024, info.mem_total_kb / 1024);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Memory free", buf);
    }
    if (info.sd_total_mb > 0) {
        snprintf(buf, sizeof(buf), "%ld / %ld GB", info.sd_free_mb / 1024, info.sd_total_mb / 1024);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Storage free", buf);
    }
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "IPv4",
                   info.ipv4[0] ? info.ipv4 : (info.ip[0] ? info.ip : "—"));
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "IPv6", info.ipv6[0] ? info.ipv6 : "—");
    if (info.battery_percent >= 0) {
        snprintf(buf, sizeof(buf), "%d%%%s", info.battery_percent, info.charging ? " (charging)" : "");
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Battery", buf);
    }
    if (info.cpu_temp_c > 0) {
        snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", info.cpu_temp_c);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "CPU temp", buf);
    }
    if (info.uptime_s > 0) {
        snprintf(buf, sizeof(buf), "%ldh %ldm", info.uptime_s / 3600, (info.uptime_s % 3600) / 60);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Uptime", buf);
    }

    jw__about_push(rows, &n, JW_ABOUT_HEADING, "Open-source components", "");
    for (int i = 0; i < JW_ABOUT_CREDIT_COUNT; i++)
        jw__about_push(rows, &n, JW_ABOUT_CREDIT, kAboutCredits[i].name, kAboutCredits[i].license);

    /* Everything scrolls together in one (non-selectable) scroll view. Up/down
       scroll; a scrollbar appears when the content outgrows the pane. The scroll
       offset lives in ui->about_scroll — the settings render path is const by
       convention, but the scroll view must persist its clamped offset across
       frames, so this one field is the deliberate exception. */
    int row_h     = TTF_FontHeight(small) + cat_scale(8);
    int content_h = n * row_h;
    jw__scroll_publish_max(&((jw_settings_ui *)ui)->about_scroll_max, content_h, view_h);
    jw__about_ctx ctx = { rows, n, small, row_h };
    cat_draw_scroll_view(x + pad, top, w - pad * 2, view_h, content_h,
                         (cat_scroll_state *)&ui->about_scroll,
                         jw__draw_about_rows, &ctx);
}

/* "12h 30m" / "45m" / "<1m" / "—" for a playtime in seconds. */
static void jw__fmt_playtime(long long s, char *buf, size_t n) {
    if (s <= 0)     { snprintf(buf, n, "%s", "\xe2\x80\x94"); return; }  /* em dash */
    long long h = s / 3600, m = (s % 3600) / 60;
    if (h > 0)      snprintf(buf, n, "%lldh %lldm", h, m);
    else if (m > 0) snprintf(buf, n, "%lldm", m);
    else            snprintf(buf, n, "<1m");
}

/* "today" / "yesterday" / "3 days ago" / "2 weeks ago" / "—" for a unix time. */
static void jw__fmt_ago(long long when, char *buf, size_t n) {
    if (when <= 0) { snprintf(buf, n, "%s", "\xe2\x80\x94"); return; }
    long long d = ((long long)time(NULL) - when) / 86400;
    if (d <= 0)       snprintf(buf, n, "today");
    else if (d == 1)  snprintf(buf, n, "yesterday");
    else if (d < 7)   snprintf(buf, n, "%lld days ago", d);
    else if (d < 30)  snprintf(buf, n, "%lld week%s ago",  d / 7,  d / 7  == 1 ? "" : "s");
    else if (d < 365) snprintf(buf, n, "%lld month%s ago", d / 30, d / 30 == 1 ? "" : "s");
    else              snprintf(buf, n, "%lld year%s ago",  d / 365, d / 365 == 1 ? "" : "s");
}

/* Set by jw_settings_ui_open for the Info pages so the first frame after a
   (re)open re-reads fresh, instead of serving a snapshot up to 1s stale (e.g.
   right after a rescan). The 1s TTL still applies to subsequent frames. */
static bool jw__stats_force_reread = false;
static void jw__stats_snapshot_invalidate(void) { jw__stats_force_reread = true; }

/* Refresh the shared stats snapshot about once a second (cheap reads; only one
   Info page is live at a time, so a single static cache is fine). */
static const jw_library_stats *jw__stats_snapshot(const jw_settings_ui *ui) {
    static jw_library_stats st;
    static uint32_t last = 0;
    static bool     have = false;
    uint32_t now = SDL_GetTicks();
    if (!have || jw__stats_force_reread || now - last > 1000) {
        if (jw_db_read_stats(ui->db_path, &st) != 0) memset(&st, 0, sizeof(st));
        last = now;
        have = true;
        jw__stats_force_reread = false;
    }
    return &st;
}

/* Info > Library: counts, box-art coverage, and a per-system breakdown. Same
   box model + scroll-view + row machinery as About. */
static void jw__render_library(const jw_settings_ui *ui, int x, int y, int w, int h) {
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(6);
    int top, view_h;
    jw__draw_info_title("Library", x, y, w, h, &top, &view_h);

    const jw_library_stats *st = jw__stats_snapshot(ui);

    jw__about_row rows[JW_ABOUT_MAX_ROWS];
    int n = 0;
    char buf[72];

    snprintf(buf, sizeof(buf), "%d", st->game_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Games", buf);
    snprintf(buf, sizeof(buf), "%d", st->system_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Systems", buf);
    snprintf(buf, sizeof(buf), "%d", st->app_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Apps", buf);
    snprintf(buf, sizeof(buf), "%d", st->favorite_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Favorites", buf);
    if (st->game_count > 0) {
        int pct = (st->art_covered * 100 + st->game_count / 2) / st->game_count;
        snprintf(buf, sizeof(buf), "%d%% (%d/%d)", pct, st->art_covered, st->game_count);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Box art", buf);
    }

    if (st->system_count > 0) {
        jw__about_push(rows, &n, JW_ABOUT_HEADING, "By system", "");
        for (int i = 0; i < st->system_count && n < JW_ABOUT_MAX_ROWS; i++) {
            snprintf(buf, sizeof(buf), "%d", st->systems[i].game_count);
            jw__about_push(rows, &n, JW_ABOUT_FIELD, st->systems[i].system, buf);
        }
    }

    int row_h = TTF_FontHeight(small) + cat_scale(8);
    jw__about_ctx ctx = { rows, n, small, row_h };
    jw__scroll_publish_max(&((jw_settings_ui *)ui)->library_scroll_max, n * row_h, view_h);
    cat_draw_scroll_view(x + pad, top, w - pad * 2, view_h, n * row_h,
                         (cat_scroll_state *)&ui->library_scroll,
                         jw__draw_about_rows, &ctx);
}

/* Info > Playtime: totals, most-played games, and per-system hours. */
static void jw__render_playtime(const jw_settings_ui *ui, int x, int y, int w, int h) {
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(6);
    int top, view_h;
    jw__draw_info_title("Playtime", x, y, w, h, &top, &view_h);

    const jw_library_stats *st = jw__stats_snapshot(ui);

    jw__about_row rows[JW_ABOUT_MAX_ROWS];
    int n = 0;
    char buf[72];

    jw__fmt_playtime(st->total_playtime_s, buf, sizeof(buf));
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Total", buf);
    snprintf(buf, sizeof(buf), "%d", st->games_played);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Games played", buf);
    jw__fmt_ago(st->last_played, buf, sizeof(buf));
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Last played", buf);

    if (st->top_count > 0) {
        jw__about_push(rows, &n, JW_ABOUT_HEADING, "Most played", "");
        for (int i = 0; i < st->top_count && n < JW_ABOUT_MAX_ROWS; i++) {
            jw__fmt_playtime(st->top[i].playtime_s, buf, sizeof(buf));
            jw__about_push(rows, &n, JW_ABOUT_FIELD, st->top[i].name, buf);
        }
    }

    /* Per-system hours, only for systems that have actually been played. */
    bool any = false;
    for (int i = 0; i < st->system_count; i++)
        if (st->systems[i].playtime_s > 0) { any = true; break; }
    if (any) {
        jw__about_push(rows, &n, JW_ABOUT_HEADING, "By system", "");
        for (int i = 0; i < st->system_count && n < JW_ABOUT_MAX_ROWS; i++) {
            if (st->systems[i].playtime_s <= 0) continue;
            jw__fmt_playtime(st->systems[i].playtime_s, buf, sizeof(buf));
            jw__about_push(rows, &n, JW_ABOUT_FIELD, st->systems[i].system, buf);
        }
    }

    if (st->total_playtime_s <= 0)
        jw__about_push(rows, &n, JW_ABOUT_PLAIN, "No playtime recorded yet.", "");

    int row_h = TTF_FontHeight(small) + cat_scale(8);
    jw__about_ctx ctx = { rows, n, small, row_h };
    jw__scroll_publish_max(&((jw_settings_ui *)ui)->playtime_scroll_max, n * row_h, view_h);
    cat_draw_scroll_view(x + pad, top, w - pad * 2, view_h, n * row_h,
                         (cat_scroll_state *)&ui->playtime_scroll,
                         jw__draw_about_rows, &ctx);
}

static void jw__render_controls(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Controls & Feedback", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->controls_list, x, ly, w,
                            JW_CONTROLS_ROW_COUNT, item_h);

    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_RUMBLE,
                        "Rumble", ui->rumble_enabled ? "On" : "Off", true);

    char strength[16];
    snprintf(strength, sizeof(strength), "%d%%", ui->rumble_strength);
    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_STRENGTH,
                        "Strength", (ui->rumble_enabled || ui->rumble_game) ? strength : "-", true);

    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_NAV,
                        "Cursor Movement",
                        ui->rumble_enabled ? (ui->rumble_nav ? "On" : "Off") : "-", true);

    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_GAME,
                        "Game Rumble",
                        ui->rumble_game ? "On" : "Off", true);

    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_SCREENSHOTS,
                        "Screenshots", ui->screenshots_enabled ? "On" : "Off", true);

    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_RECORDING,
                        "Recording", ui->recording_enabled ? "On" : "Off", true);

    /* Both dependants read "-" while recording is off, matching how the rumble
       rows above dim when the master is off. */
    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_REC_SPLIT,
                        "Split Over 10MB",
                        ui->recording_enabled ? (ui->recording_split ? "On" : "Off") : "-", true);

    jw__render_list_row(&ui->controls_list, x, ly, w, JW_CONTROLS_REC_KEEP,
                        "Keep Original",
                        ui->recording_enabled ? (ui->recording_keep_src ? "On" : "Off") : "-", true);
}

/* Endonyms, so a speaker can find their own language without reading English.
   These render correctly even while the UI is still English: Catastrophe
   substitutes any string carrying CJK codepoints onto the CJK face, so 中文 is
   legible in a Latin-themed UI. Unknown codes fall back to the raw code, which
   is ugly but always true. */
static const char *jw__language_label(const char *code) {
    if (!code || !code[0] || strcmp(code, "en") == 0) return "English";
    if (strcmp(code, "zh_CN") == 0) return "中文";
    if (strcmp(code, "zh_TW") == 0) return "繁體中文";
    if (strcmp(code, "ja") == 0)    return "日本語";
    if (strcmp(code, "ko") == 0)    return "한국어";
    return code;
}

/* The Language row exists only when there is something to switch to. A picker
   offering one option reads as broken, and hiding it means a translator makes
   the row appear simply by dropping a .tsv on the card. */
static int jw__behavior_rows(const jw_settings_ui *ui) {
    return ui->language_count > 1 ? JW_BEHAVIOR_ROW_COUNT
                                  : JW_BEHAVIOR_ROW_COUNT - 1;
}

static void jw__render_behavior(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("General", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->behavior_list, x, ly, w,
                            jw__behavior_rows(ui), item_h);

    int sleep_idx = (ui->auto_sleep_index >= 0 && ui->auto_sleep_index < JW_AUTO_SLEEP_COUNT)
                    ? ui->auto_sleep_index : JW_AUTO_SLEEP_DEFAULT;
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_AUTO_SLEEP,
                        "Auto Sleep", kAutoSleepLabels[sleep_idx], true);

    int perf_idx = (ui->game_perf_profile >= 0 &&
                    ui->game_perf_profile < JW_GAME_PERF_PROFILE_COUNT)
                 ? ui->game_perf_profile
                 : JW_GAME_PERF_PROFILE_DEFAULT;
    const char *perf = ui->performance_supported
                     ? jw_platform_perf_profile_label(kGamePerfProfiles[perf_idx])
                     : "Unavailable";
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_PERFORMANCE,
                        "Game Performance", perf, ui->performance_supported);

    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_TIMEZONE,
                        "Time Zone", jw__timezone_label(ui->timezone), true);

    const char *splash = ui->boot_splash_supported
                         ? (ui->boot_splash_enabled ? "On" : "Off")
                         : "Unavailable";
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_BOOT_SPLASH,
                        "Boot Splash", splash, ui->boot_splash_supported);

    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_RESET_RETROARCH,
                        "Reset RetroArch Config", "Defaults", true);
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_UNMOUNT_SECONDARY,
                        "Unmount Secondary SD",
                        ui->secondary_sd_status[0] ? ui->secondary_sd_status : "Unavailable",
                        true);

    if (ui->language_count > 1) {
        jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_LANGUAGE,
                            "Language", jw__language_label(ui->language), true);
    }
}

/* Home Tabs editor — hide/reorder the launcher's home tabs. Modeled on the
   scrape-priority editor: rows are the tabs in display order, the first
   home_tab_visible are shown ("On"), the rest hidden ("Off"). A toggles
   visibility (guarded to keep at least one visible), X grabs the row to reorder
   with Up/Down. */
typedef struct { const jw_settings_ui *ui; } jw__home_tabs_ctx;

static void jw__draw_home_tab_focus(int x, int y, int w, int h, void *user) {
    jw__home_tabs_ctx *ctx = (jw__home_tabs_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = y + (h - pill_h) / 2;
    ap_color color = (ctx && ctx->ui && ctx->ui->home_tabs_grabbed)
                   ? theme->accent : theme->highlight;
    cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, color);
}

static void jw__draw_home_tab_item(int idx, int ix, int iy, int iw, int ih,
                                   float focus, void *user) {
    jw__home_tabs_ctx *ctx = (jw__home_tabs_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (!ui || idx < 0 || idx >= JW_HOME_TABS_COUNT) return;

    int tab = ui->home_tab_order[idx];
    if (tab < 0 || tab >= JW_STARTUP_TAB_COUNT) return;

    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    bool grabbed_row = idx == ui->home_tabs_list.cursor && ui->home_tabs_grabbed;

    bool is_visible = idx < ui->home_tab_visible;
    ap_color label_c = cat_draw_color_lerp(
        is_visible ? theme->text : theme->hint,
        theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    cat_draw_text_ellipsized(body, T(kStartupTabLabels[tab]), ix + cat_scale(12), ty,
                             label_c, iw * 2 / 3);

    const char *value = grabbed_row ? T("Moving") : (is_visible ? T("On") : T("Off"));
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_home_tabs(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Home Tabs", x, y, w);

    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    /* Button hints live in the footer bar (jw__draw_settings_footer), not on the
       page. */
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);

    int count = JW_HOME_TABS_COUNT;
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
    ((cat_list_state *)&ui->home_tabs_list)->visible_rows = vis;
    jw__home_tabs_ctx ctx = { ui };
    cat_draw_list_pane_layered(lr.x, lr.y, lr.w, lr.h, count,
                               &ui->home_tabs_list, item_h,
                               jw__draw_home_tab_focus,
                               jw__draw_home_tab_item, &ctx);
}

static void jw__format_update_size(long long bytes, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (bytes <= 0) {
        snprintf(out, out_size, "%s", "-");
        return;
    }
    if (bytes >= 1024LL * 1024LL * 1024LL) {
        snprintf(out, out_size, "%.1f GB",
                 (double)bytes / (double)(1024LL * 1024LL * 1024LL));
    } else if (bytes >= 1024LL * 1024LL) {
        snprintf(out, out_size, "%.1f MB",
                 (double)bytes / (double)(1024LL * 1024LL));
    } else {
        snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
    }
}

static const char *jw__update_state_label(const jw_ipc_update_status_info *u) {
    if (!u || !u->state[0]) {
        return "Idle";
    }
    if (strcmp(u->state, "checking") == 0) return "Checking...";
    if (strcmp(u->state, "up-to-date") == 0) return "Up to date";
    if (strcmp(u->state, "available") == 0) return "Available";
    if (strcmp(u->state, "downloading") == 0) return "Downloading";
    if (strcmp(u->state, "downloaded") == 0) return "Downloaded";
    if (strcmp(u->state, "installing") == 0) return "Installing";
    if (strcmp(u->state, "armed") == 0) return "Restart needed";
    if (strcmp(u->state, "cancelled") == 0) return "Cancelled";
    if (strcmp(u->state, "incompatible") == 0) return "Incompatible";
    if (strcmp(u->state, "error") == 0) return "Error";
    return "Idle";
}

/* True when the offered release is the one already installed — so we don't tempt
   the user into re-downloading/re-installing the version they're on. */
static bool jw__update_is_current(const jw_ipc_update_status_info *u) {
    return u && u->release_id[0] && u->current_release_id[0] &&
           strcmp(u->release_id, u->current_release_id) == 0;
}

static void jw__update_download_label(const jw_settings_ui *ui,
                                      char *out,
                                      size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const jw_ipc_update_status_info *u = ui ? &ui->update : NULL;
    if (!ui || !ui->update_have_status) {
        snprintf(out, out_size, "%s", "Unavailable");
    } else if (u->download_active) {
        if (u->download_percent >= 0) {
            snprintf(out, out_size, "Cancel %d%%", u->download_percent);
        } else {
            snprintf(out, out_size, "%s", "Cancel");
        }
    } else if (u->downloaded) {
        snprintf(out, out_size, "%s", "Verified");
    } else if (jw__update_is_current(u)) {
        snprintf(out, out_size, "%s", "Up to date");
    } else if (u->compatible && u->artifact_name[0]) {
        snprintf(out, out_size, "%s", "Download");
    } else {
        snprintf(out, out_size, "%s", "Unavailable");
    }
}

static const char *jw__update_blocked_label(const char *reason) {
    if (!reason || !reason[0]) return "Blocked";
    if (strcmp(reason, "install_again") == 0) return "Install Again";
    if (strcmp(reason, "primary_slot_needed") == 0) return "Primary Slot";
    if (strcmp(reason, "not_idle") == 0) return "Close Apps";
    if (strcmp(reason, "space_low") == 0) return "No Space";
    if (strcmp(reason, "battery_low") == 0) return "Battery Low";
    if (strcmp(reason, "download_active") == 0) return "Downloading";
    if (strcmp(reason, "not_downloaded") == 0) return "Download First";
    if (strcmp(reason, "download_missing") == 0) return "Missing";
    if (strcmp(reason, "unsupported_handoff") == 0) return "Unsupported";
    if (strcmp(reason, "unsupported_artifact") == 0) return "Unsupported";
    return "Blocked";
}

static void jw__update_install_label(const jw_settings_ui *ui,
                                     char *out,
                                     size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const jw_ipc_update_status_info *u = ui ? &ui->update : NULL;
    if (!ui || !ui->update_have_status) {
        snprintf(out, out_size, "%s", "Unavailable");
    } else if (u->install_active) {
        snprintf(out, out_size, "%s", "Installing");
    } else if (u->install_armed) {
        snprintf(out, out_size, "%s", "Restart");
    } else if (u->install_ready) {
        snprintf(out, out_size, "%s", "Install");
    } else if (u->install_needs_confirmation) {
        snprintf(out, out_size, "%s", "Confirm");
    } else if (u->install_blocked) {
        snprintf(out, out_size, "%s",
                 jw__update_blocked_label(u->install_reason));
    } else if (u->downloaded) {
        snprintf(out, out_size, "%s", "Ready Check");
    } else if (u->install_result_state[0]) {
        if (strcmp(u->install_result_state, "installed") == 0) {
            snprintf(out, out_size, "%s", "Installed");
        } else if (strcmp(u->install_result_state, "armed") == 0) {
            snprintf(out, out_size, "%s", "Restart Needed");
        } else if (strcmp(u->install_result_state, "error") == 0) {
            snprintf(out, out_size, "%s", "Failed");
        } else {
            snprintf(out, out_size, "%s", u->install_result_state);
        }
    } else {
        snprintf(out, out_size, "%s", "Unavailable");
    }
}

static void jw__draw_update_progress(const jw_settings_ui *ui,
                                     int x, int y, int w) {
    if (!ui || !ui->update.download_active || ui->update.download_percent < 0) {
        return;
    }
    /* Ease the displayed fill toward the polled percent so the bar advances
       smoothly between the 500ms progress updates instead of jumping. The render
       path is const; the eased value is render-only animation state (mutated via
       the same const-cast idiom used elsewhere in this file). Paired with the
       per-frame redraw request in jw__render_update so it actually animates. */
    jw_settings_ui *m = (jw_settings_ui *)ui;
    float target = (float)ui->update.download_percent;
    if (target < m->update_bar_pct) {
        m->update_bar_pct = target;          /* snap back on a new/reset download */
    } else {
        m->update_bar_pct += (target - m->update_bar_pct) * 0.18f;
        if (target - m->update_bar_pct < 0.5f) {
            m->update_bar_pct = target;
        }
    }
    int pct = (int)(m->update_bar_pct + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    ap_theme *theme = cat_get_theme();
    int track_h = cat_scale(5);
    int fill_w = (w * pct) / 100;
    cat_draw_rect(x, y, w, track_h, cat_hex_to_color("#ffffff33"));
    cat_draw_rect(x, y, fill_w, track_h, theme->accent);
}

static void jw__draw_update_activity(const jw_settings_ui *ui,
                                     int x, int y, int w) {
    /* Indeterminate bar for any in-progress activity (install or release check).
       The caller decides when to show it; don't re-gate on install_active here
       or it never draws while checking. */
    if (!ui || w <= 0) {
        return;
    }
    ap_theme *theme = cat_get_theme();
    int track_h = cat_scale(5);
    int segment_w = w / 3;
    int min_segment = cat_scale(28);
    if (segment_w < min_segment) {
        segment_w = min_segment;
    }
    if (segment_w > w) {
        segment_w = w;
    }

    int travel = w + segment_w;
    int pos = (int)((SDL_GetTicks() / 8u) % (unsigned)travel) - segment_w;
    int fill_x = x + pos;
    int fill_w = segment_w;
    if (fill_x < x) {
        fill_w -= x - fill_x;
        fill_x = x;
    }
    if (fill_x + fill_w > x + w) {
        fill_w = x + w - fill_x;
    }

    cat_draw_rect(x, y, w, track_h, cat_hex_to_color("#ffffff33"));
    if (fill_w > 0) {
        cat_draw_rect(fill_x, y, fill_w, track_h, theme->accent);
    }
}

static void jw__render_update(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("System Update", x, y, w);
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    /* Hybrid page: a runtime-variable status block (message + optional progress
       bar) then fixed rows, so it keeps its own dy flow below the content origin
       rather than carving a fixed sub-header strip. */
    int dy = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;

    const jw_ipc_update_status_info *u = &ui->update;
    bool checking = ui->update_have_status && strcmp(u->state, "checking") == 0;
    char message[320];
    if (ui->update_have_status && u->install_active && u->install_message[0]) {
        snprintf(message, sizeof(message), "%s", T(u->install_message));
    } else if (ui->update_msg[0]) {
        snprintf(message, sizeof(message), "%s", ui->update_msg);
    } else if (ui->update_have_status) {
        if (u->install_message[0] &&
            (u->install_active || u->install_armed ||
             u->install_blocked || u->install_needs_confirmation ||
             u->install_ready)) {
            snprintf(message, sizeof(message), "%s", T(u->install_message));
        } else if (u->message[0]) {
            snprintf(message, sizeof(message), "%s", u->message);
        } else if (u->install_result_message[0]) {
            snprintf(message, sizeof(message), T("Last update: %s"),
                     T(u->install_result_message));
        } else {
            snprintf(message, sizeof(message), "%s", jw__update_state_label(u));
        }
    } else {
        snprintf(message, sizeof(message), "%s", T("Update status unavailable"));
    }

    cat_draw_text_ellipsized(small, message, x + cat_scale(12), dy,
                             theme->hint, w - cat_scale(24));
    dy += line_h;

    if (ui->update.download_active) {
        jw__draw_update_progress(ui, x + cat_scale(12), dy + cat_scale(2),
                                 w - cat_scale(24));
        dy += cat_scale(12);
    } else if (ui->update.install_active || checking) {
        jw__draw_update_activity(ui, x + cat_scale(12), dy + cat_scale(2),
                                 w - cat_scale(24));
        dy += cat_scale(12);
    }

    int ly = dy + cat_scale(2);
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__begin_settings_rows(&ui->update_list, x, ly, w,
                            JW_UPDATE_ROW_COUNT, item_h);
    char download_value[48];
    char install_value[64];
    char current_value[128];
    char candidate_value[224];
    char check_value[64];
    char size_value[64];
    jw__update_download_label(ui, download_value, sizeof(download_value));
    jw__update_install_label(ui, install_value, sizeof(install_value));

    snprintf(check_value, sizeof(check_value), "%s", jw__update_state_label(u));
    snprintf(current_value, sizeof(current_value), "%s",
             (ui->update_have_status && u->current_release_id[0])
             ? u->current_release_id
             : (ui->update_have_status && u->current_unknown ? "Unknown" : "-"));
    if (ui->update_have_status && u->release_id[0]) {
        if (jw__update_is_current(u)) {
            snprintf(candidate_value, sizeof(candidate_value), "%s (current)",
                     u->release_id);
        } else {
            jw__format_update_size(u->artifact_size, size_value, sizeof(size_value));
            snprintf(candidate_value, sizeof(candidate_value), "%s (%s)",
                     u->release_id, size_value);
        }
    } else if (ui->update_have_status && u->install_result_release_id[0]) {
        snprintf(candidate_value, sizeof(candidate_value), "Last: %s",
                 u->install_result_release_id);
    } else if (ui->update_have_status && strcmp(u->state, "up-to-date") == 0) {
        snprintf(candidate_value, sizeof(candidate_value), "%s", "None");
    } else {
        snprintf(candidate_value, sizeof(candidate_value), "%s", "-");
    }

    /* Until a real release check runs this visit, don't present the daemon's
       cached result as fresh truth — show "Not checked" and dash the result
       rows. Skip the suppression when there's in-flight work to show (a download
       or install in progress / a verified artifact / a pending restart). */
    if (!ui->update_checked_this_visit && ui->update_have_status && !checking &&
        !u->download_active && !u->install_active && !u->install_armed &&
        !u->downloaded) {
        snprintf(check_value, sizeof(check_value), "%s", "Not checked");
        snprintf(download_value, sizeof(download_value), "%s", "-");
        snprintf(install_value, sizeof(install_value), "%s", "-");
        snprintf(candidate_value, sizeof(candidate_value), "%s", "-");
    }

    bool on_beta = (ui->update_channel_index == JW_UPDATE_CHANNEL_BETA_IDX);
    if (on_beta) {
        /* Amber-tint the value so "you're on the tester channel" reads at a glance. */
        ap_color warn = cat_hex_to_color("#E8A44C");
        jw__render_list_row_vc(&ui->update_list, x, ly, w, JW_UPDATE_ROW_CHANNEL,
                               "Update Channel", "Beta", true, warn);
    } else {
        jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_CHANNEL,
                            "Update Channel", "Stable", true);
    }

    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_CHECK,
                        "Check Releases", check_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_DOWNLOAD,
                        "Download", download_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_INSTALL,
                        "Install", install_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_CURRENT,
                        "Current", current_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_AVAILABLE,
                        "Available", candidate_value, false);

    /* Keep redrawing every frame while a transfer is active so the install
       spinner and download bar animate at the render loop's frame rate. Without
       this the screen only repaints on the 250ms poll-redraw, so the time-based
       spinner (SDL_GetTicks) and the progress fill visibly stutter at ~4 fps. */
    if (ui->update.download_active || ui->update.install_active || checking) {
        cat_request_frame();
    }
}

typedef struct {
    const jw_settings_ui *ui;
} jw__update_picker_ctx;

static void jw__draw_update_option_focus(int x, int y, int w, int h, void *user) {
    (void)user;
    ap_theme *theme = cat_get_theme();
    int pill_h = h - cat_scale(4);
    cat_draw_pill(x, y + cat_scale(2), w - cat_scale(4), pill_h,
                  theme->highlight);
}

static const char *jw__update_option_badge(const jw_ipc_update_option_info *option,
                                           int idx) {
    if (!option) {
        return "";
    }
    if (option->selected) {
        return "Selected";
    }
    if (option->installed) {
        return "Installed";
    }
    return idx == 0 ? "Latest" : "Older";
}

static void jw__draw_update_option_item(int idx, int ix, int iy, int iw, int ih,
                                        float focus, void *user) {
    jw__update_picker_ctx *ctx = (jw__update_picker_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (!ui || idx < 0 || idx >= ui->update.option_count ||
        idx >= JW_IPC_UPDATE_MAX_OPTIONS) {
        return;
    }

    const jw_ipc_update_option_info *option = &ui->update.options[idx];
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(10);
    ap_color main_color = cat_draw_color_lerp(theme->text,
                                               theme->highlighted_text, focus);
    ap_color hint_color = cat_draw_color_lerp(theme->hint,
                                               theme->highlighted_text, focus);
    const char *label = option->release_id[0] ? option->release_id : "Leaf update";
    char detail[320];
    char size[64];
    jw__format_update_size(option->artifact_size, size, sizeof(size));
    snprintf(detail, sizeof(detail), "%s%s%s",
             option->artifact_name[0] ? option->artifact_name : option->artifact_kind,
             size[0] && strcmp(size, "-") != 0 ? "  " : "",
             size[0] && strcmp(size, "-") != 0 ? size : "");

    int badge_w = cat_scale(92);
    cat_draw_text_ellipsized(body, label, ix + pad, iy + cat_scale(5),
                             main_color, iw - pad * 2 - badge_w);
    cat_draw_text_ellipsized(small, detail, ix + pad,
                             iy + cat_scale(5) + TTF_FontHeight(body),
                             hint_color, iw - pad * 2 - badge_w);
    cat_draw_text_ellipsized(small, jw__update_option_badge(option, idx),
                             ix + iw - badge_w - pad, iy + cat_scale(8),
                             hint_color, badge_w);
}

static void jw__render_update_picker(const jw_settings_ui *ui,
                                      int x, int y, int w, int h) {
    jw__draw_header("Pick Update", x, y, w);
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int dy = y + jw__header_h() + cat_scale(6);
    int count = ui ? ui->update.option_count : 0;
    if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
        count = JW_IPC_UPDATE_MAX_OPTIONS;
    }

    const char *message = count > 0
        ? "Compatible releases"
        : "Check releases first";
    cat_draw_text_ellipsized(small, message, x + cat_scale(12), dy,
                             theme->hint, w - cat_scale(24));
    dy += TTF_FontHeight(small) + cat_scale(8);

    if (count > 0) {
        int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) +
                     TTF_FontHeight(small) + cat_scale(16);
        cat_box lb = { x, dy, w, h - (dy - y), 0, 0, 0, 0 };
        int vis = 0;
        SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
        ((cat_list_state *)&ui->update_picker_list)->visible_rows = vis;
        jw__update_picker_ctx ctx = { ui };
        cat_draw_list_pane_layered(lr.x, lr.y, lr.w, lr.h, count,
                                   &ui->update_picker_list, item_h,
                                   jw__draw_update_option_focus,
                                   jw__draw_update_option_item, &ctx);
    }
}

typedef struct {
    const jw_settings_ui *ui;
} jw__timezone_picker_ctx;

/* Single-line row: friendly label on the left ("* " marks the current zone),
   IANA id on the right in hint color — mirrors the Network/Bluetooth list rows. */
static void jw__draw_timezone_item(int idx, int ix, int iy, int iw, int ih,
                                   float focus, void *user) {
    jw__timezone_picker_ctx *ctx = (jw__timezone_picker_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (idx < 0 || idx >= JW_TIMEZONE_COUNT) {
        return;
    }
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    ap_color label_c = cat_draw_color_lerp(theme->text,
                                            theme->highlighted_text, focus);
    ap_color value_c = cat_draw_color_lerp(theme->hint,
                                            theme->highlighted_text, focus);
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    const char *cur = ui ? ui->timezone : "";
    bool is_current = (cur[0] && strcmp(cur, kTimeZones[idx].tz) == 0);
    char label[48];
    snprintf(label, sizeof(label), "%s%s", is_current ? "* " : "",
             T(kTimeZones[idx].label));
    cat_draw_text_ellipsized(body, label, ix + cat_scale(12), ty, label_c, iw / 2);

    int vw = cat_measure_text(body, kTimeZones[idx].off);
    cat_draw_text(body, kTimeZones[idx].off, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_timezone_picker(const jw_settings_ui *ui,
                                       int x, int y, int w, int h) {
    jw__draw_header("Time Zone", x, y, w);
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int sub_h = jw__subheader_line_h(small) + cat_scale(6);
    SDL_Rect sub;
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, sub_h, NULL, &sub);
    cat_draw_text_ellipsized(small, T("Set your local time zone"), sub.x + cat_scale(12),
                             sub.y, theme->hint, sub.w - cat_scale(24));

    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, JW_TIMEZONE_COUNT, &vis, &item_h);
    ((cat_list_state *)&ui->timezone_picker_list)->visible_rows = vis;
    jw__timezone_picker_ctx ctx = { ui };
    jw__draw_settings_list(lr.x, lr.y, lr.w, lr.h, JW_TIMEZONE_COUNT,
                           &ui->timezone_picker_list, item_h,
                           jw__draw_timezone_item, &ctx);
}

/* ─── Main render dispatch ─────────────────────────────────────────────── */

void jw_settings_ui_render(const jw_settings_ui *ui,
                            int x, int y, int w, int h) {
    if (!ui || !ui->open) return;
    switch (ui->screen) {
        case JW_SETTINGS_HOME:       jw__render_home(ui, x, y, w, h);       break;
        case JW_SETTINGS_APPEARANCE: jw__render_appearance(ui, x, y, w, h); break;
        case JW_SETTINGS_COLORS:     jw__render_colors(ui, x, y, w, h);     break;
        case JW_SETTINGS_LAYOUT:     jw__render_layout(ui, x, y, w, h);     break;
        case JW_SETTINGS_STATUS_BAR: jw__render_statusbar(ui, x, y, w, h);  break;
        case JW_SETTINGS_DISPLAY:    jw__render_display(ui, x, y, w, h);    break;
        case JW_SETTINGS_NETWORK:    jw__render_network(ui, x, y, w, h);    break;
        case JW_SETTINGS_BLUETOOTH:  jw__render_bluetooth(ui, x, y, w, h);  break;
        case JW_SETTINGS_LIGHTING:   jw__render_lighting(ui, x, y, w, h);   break;
        case JW_SETTINGS_ACCOUNTS:   jw__render_accounts(ui, x, y, w, h);                break;
        case JW_SETTINGS_SCRAPING:   jw__render_scraping(ui, x, y, w, h);                break;
        case JW_SETTINGS_SCRAPE_PRIORITY: jw__render_scrape_priority(ui, x, y, w, h);    break;
        case JW_SETTINGS_SCRAPE_QUEUE:    jw__render_scrape_queue(ui, x, y, w, h);       break;
        case JW_SETTINGS_SCRAPE_QUEUE_DETAIL: jw__render_scrape_queue_detail(ui, x, y, w, h); break;
        case JW_SETTINGS_SCRAPE_DOWNLOAD: jw__render_scrape_download(ui, x, y, w, h);     break;
        case JW_SETTINGS_BEHAVIOR:   jw__render_behavior(ui, x, y, w, h);                 break;
        case JW_SETTINGS_CONTROLS:   jw__render_controls(ui, x, y, w, h);                 break;
        case JW_SETTINGS_HOME_TABS:  jw__render_home_tabs(ui, x, y, w, h);               break;
        case JW_SETTINGS_UPDATE:     jw__render_update(ui, x, y, w, h);                  break;
        case JW_SETTINGS_UPDATE_PICKER: jw__render_update_picker(ui, x, y, w, h);        break;
        case JW_SETTINGS_TIMEZONE_PICKER: jw__render_timezone_picker(ui, x, y, w, h);   break;
        case JW_SETTINGS_ABOUT:      jw__render_about(ui, x, y, w, h);                   break;
        case JW_SETTINGS_LIBRARY:    jw__render_library(ui, x, y, w, h);                 break;
        case JW_SETTINGS_PLAYTIME:   jw__render_playtime(ui, x, y, w, h);                break;
        case JW_SETTINGS_SERVICES:   jw__render_services(ui, x, y, w, h);                break;
    }
}

void jw_settings_apply_persisted_overrides(const char *db_path) {
    if (!db_path || !db_path[0]) return;

    char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX];
    unsigned char found[JW_SETTING_COUNT];
    if (jw__load_setting_values(db_path, values, found) == 0)
        jw__apply_persisted_overrides_from_values(values, found);
}

/* Apply a curated color scheme. The seven color roles are written straight into
   the live theme in memory (instant — no per-key DB read-back), then all eight
   keys persist in a single transaction. Cycling schemes used to cost ~18 DB
   re-opens per press (8 writes + 10 read-backs); this is one open. */
static void jw__apply_color_scheme(jw_settings_ui *ui, int index, bool *theme_changed) {
    if (index < 0 || index >= JW_COLOR_SCHEME_COUNT) return;
    const jw__color_scheme *s = &kColorSchemes[index];

    ap_theme *t = cat_get_theme();
    t->accent          = cat_hex_to_color(s->accent);
    t->background      = cat_hex_to_color(s->bg);
    t->text            = cat_hex_to_color(s->text);
    t->hint            = cat_hex_to_color(s->hint);
    t->highlight       = cat_hex_to_color(s->selection);
    t->button_label    = cat_hex_to_color(s->btn_label);
    t->button_glyph_bg = cat_hex_to_color(s->btn_bg);
    cat_finalize_theme_colors(t);

    ui->color_scheme_index = index;
    if (theme_changed) *theme_changed = true;

    char idx_buf[16];
    snprintf(idx_buf, sizeof(idx_buf), "%d", index);
    const char *keys[] = {
        "accent_color", "bg_color", "text_color", "hint_color",
        "highlight_color", "button_label_color", "button_glyph_bg_color",
        "color_scheme_index",
    };
    const char *vals[] = {
        s->accent, s->bg, s->text, s->hint, s->selection,
        s->btn_label, s->btn_bg, idx_buf,
    };
    if (ui->db_path[0])
        jw_db_set_settings(ui->db_path, keys, vals,
                           (int)(sizeof(keys) / sizeof(keys[0])));
}

static void jw__cycle_color_scheme(jw_settings_ui *ui, int direction, bool *theme_changed) {
    int n = JW_COLOR_SCHEME_COUNT;
    int cur = ui->color_scheme_index;
    int next = (cur < 0) ? (direction > 0 ? 0 : n - 1)
                         : ((cur + direction + n) % n);
    jw__apply_color_scheme(ui, next, theme_changed);
    /* The applier stays silent -- jw__apply_layout calls it too, and that would
       double up. Cycling the scheme is the user action, so it ticks here. */
    if (ui->socket_path[0])
        jw_ipc_rumble(ui->socket_path, "select");
}

/* Switch the home layout (Tabs <-> Coverflow) live. Loading the matching theme
   makes its bundled assets resolve (e.g. the Coverflow console icons) and sets the
   layout; cat_stylesheet_apply overwrites the theme colours, so re-apply the user's
   colour scheme afterwards. Persist the choice as the theme name so a cold boot
   restores it, and signal a rebuild so the home list is rebuilt for the layout. */
static void jw__apply_layout(jw_settings_ui *ui, int mode, bool *theme_changed) {
    const char *tn = (mode == 1) ? "Jawaka-Coverflow" : "Jawaka-Tabs";
    cat_stylesheet ss;
    if (cat_stylesheet_load_theme(&ss, tn) != CAT_OK)
        return;
    cat_stylesheet_apply(&ss);
    if (ui->color_scheme_index >= 0) {
        bool ignored = false;
        jw__apply_color_scheme(ui, ui->color_scheme_index, &ignored);
    }
    /* cat_stylesheet_apply reset the theme to the new stylesheet's defaults,
       clobbering the user's persisted pill shape / font family / font size (the
       color scheme above is re-asserted for the same reason). Re-apply them so a
       live layout switch keeps appearance overrides instead of dropping them
       until another setting change or relaunch. */
    if (ui->db_path[0])
        jw_settings_apply_persisted_overrides(ui->db_path);
    ui->layout_mode = mode;
    /* Through jw__persist, not jw_db_set_setting: the write and the "a setting
       changed" tick are the same event, and splitting them is why this row was
       the one silent row on the Layout page. */
    jw__persist(ui, "theme_name", tn);
    if (theme_changed) *theme_changed = true;
}

static void jw__change_brightness(jw_settings_ui *ui, int delta,
                                  char *status_buf, size_t status_size) {
    int next = jw_platform_clamp_brightness_percent(ui->brightness_percent + delta);
    int resolved = next;
    if (ui->socket_path[0] &&
        jw_ipc_set_brightness(ui->socket_path, next, &resolved, status_buf,
                              (int)status_size) == 0) {
        ui->brightness_percent = jw_platform_clamp_brightness_percent(resolved);
        return;
    }
    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "%s", T("brightness failed"));
}

static void jw__change_volume(jw_settings_ui *ui, int delta,
                              char *status_buf, size_t status_size) {
    int next = ui->volume_percent + delta;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    int resolved = next;
    if (ui->socket_path[0] &&
        jw_ipc_set_volume(ui->socket_path, next, &resolved, status_buf,
                          (int)status_size) == 0) {
        if (resolved < 0) resolved = 0;
        if (resolved > 100) resolved = 100;
        ui->volume_percent = resolved;
        if (ui->audio_output >= 0 &&
            ui->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
            ui->audio_volumes[ui->audio_output] = resolved;
        }
        return;
    }
    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "%s", T("volume failed"));
}

static bool jw__audio_output_available(const jw_settings_ui *ui,
                                       jw_platform_audio_output output) {
    return ui && output >= 0 && output < JW_PLATFORM_AUDIO_OUTPUT_COUNT &&
           (ui->audio_available_outputs & JW_PLATFORM_AUDIO_OUTPUT_BIT(output));
}

static jw_platform_audio_output jw__next_audio_output(const jw_settings_ui *ui,
                                                      int direction) {
    jw_platform_audio_output choices[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    int count = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        jw_platform_audio_output output = (jw_platform_audio_output)i;
        if (jw__audio_output_available(ui, output)) {
            choices[count++] = output;
        }
    }
    if (count == 0) {
        return JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    }

    int current = 0;
    for (int i = 0; i < count; i++) {
        if (choices[i] == ui->audio_output) {
            current = i;
            break;
        }
    }

    int next = direction < 0
        ? (current + count - 1) % count
        : (current + 1) % count;
    return choices[next];
}

static void jw__set_audio_output(jw_settings_ui *ui, jw_platform_audio_output output,
                                 char *status_buf, size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        if (status_buf && status_size > 0)
            snprintf(status_buf, status_size, "%s", T("audio output failed"));
        return;
    }
    if (jw_ipc_set_audio_output(ui->socket_path, output, status_buf,
                                (int)status_size) == 0) {
        ui->audio_output = output;
        jw__refresh_audio_status(ui);
        return;
    }
    jw__refresh_audio_status(ui);
}

/* After a Bluetooth headset is disconnected or unpaired the codec is still
   pointed at the (now gone) Bluetooth output, so audio would play to nothing.
   When that is the case, fall back to the wired jack if it is plugged in, then
   USB-C, else the speaker — so sound returns on its own instead of staying
   silent, and lands on headphones rather than blaring out of the speaker if any
   are still connected. */
static void jw__bt_route_back_if_orphaned(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    jw__refresh_audio_status(ui);
    if (ui->audio_output != JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH ||
        jw__audio_output_available(ui, JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH)) {
        return;
    }
    jw_platform_audio_output back = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    if (jw__audio_output_available(ui, JW_PLATFORM_AUDIO_OUTPUT_HEADSET)) {
        back = JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    } else if (jw__audio_output_available(ui, JW_PLATFORM_AUDIO_OUTPUT_USB)) {
        back = JW_PLATFORM_AUDIO_OUTPUT_USB;
    }
    char status[128];
    jw__set_audio_output(ui, back, status, sizeof(status));
}

/* ─── Color picker helper ──────────────────────────────────────────────── */

static bool jw__pick_color(jw_settings_ui *ui, ap_color *target,
                           const char *db_key, int active_role) {
    ap_theme *theme = cat_get_theme();
    cat_color_picker_context context = {
        .roles = {
            { "Accent",     theme->accent },
            { "Background", theme->background },
            { "Text",       theme->text },
            { "Selection",  theme->highlight },
            { "Secondary",  theme->hint },
            { "Btn Text",   theme->button_label },
            { "Btn Bg",     theme->button_glyph_bg },
        },
        .role_count = JW_COLOR_ROW_COUNT,
        .active_role = active_role,
    };

    ap_color picked;
    if (cat_color_picker_ctx(*target, &picked, &context) == CAT_OK) {
        *target = picked;
        jw__persist_color(ui, db_key, picked);
        return true;
    }
    return false;
}

/* Jawaka-side shim over cat_confirmation: Catastrophe cannot know about the
   string table, so the message and footer labels are translated here. Every
   opts struct below is a stack local, so rewriting the pointers is safe. */
static int jw__confirmation(cat_message_opts *opts, cat_confirm_result *result) {
    if (opts->message) opts->message = T(opts->message);
    for (int i = 0; i < opts->footer_count; i++) {
        if (opts->footer[i].label)
            opts->footer[i].label = T(opts->footer[i].label);
    }
    return cat_confirmation(opts, result);
}

static bool jw__confirm_retroarch_reset(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Reset", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Reset shared RetroArch config to packaged defaults?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__reset_retroarch_config(jw_settings_ui *ui,
                                       char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!jw__confirm_retroarch_reset()) {
        snprintf(status_buf, status_size, "%s", T("RetroArch reset canceled"));
        return;
    }

    jw_ipc_reset_retroarch_config(ui->socket_path, status_buf, (int)status_size);
}

static bool jw__confirm_secondary_unmount(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Unmount", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Safely unmount the secondary SD card?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_adb_enable(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Enable", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Enable USB ADB and restore it at boot?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_adb_disable(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Disable", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Disable USB ADB and remove Leaf's boot restore marker?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_update_install(const char *release_id) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Install", .is_confirm = true },
    };
    char message[192];
    snprintf(message, sizeof(message), "Install Leaf %s?",
             release_id && release_id[0] ? release_id : "update");
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_update_unknown_preflight(const char *message) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Continue", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message && message[0]
                   ? message
                   : "Install update even though checks are incomplete?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_update_reboot(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Later", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Restart", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Restart now to finish installing?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_bt_unpair(const char *name) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Unpair", .is_confirm = true },
    };
    char message[160];
    snprintf(message, sizeof(message), "Unpair %s?", name && name[0] ? name : "device");
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__copy_status(char *status_buf, size_t status_size,
                            const char *value) {
    if (status_buf && status_size > 0) {
        snprintf(status_buf, status_size, "%s", value ? value : "");
    }
}

static void jw__settings_update_from_ipc(jw_settings_ui *ui,
                                         const jw_ipc_update_status_info *info,
                                         const char *message) {
    if (!ui) {
        return;
    }
    if (info) {
        ui->update = *info;
        ui->update_have_status = true;
        int count = ui->update.option_count;
        if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
            count = JW_IPC_UPDATE_MAX_OPTIONS;
        }
        if (count > 0) {
            int cursor = ui->update.selected_option >= 0
                ? ui->update.selected_option
                : ui->update_picker_list.cursor;
            if (cursor < 0) cursor = 0;
            if (cursor >= count) cursor = count - 1;
            ui->update_picker_list.cursor = cursor;
            if (ui->update_picker_list.scroll_offset > cursor) {
                ui->update_picker_list.scroll_offset = cursor;
            }
        } else {
            ui->update_picker_list.cursor = 0;
            ui->update_picker_list.scroll_offset = 0;
        }
    }
    if (message && message[0]) {
        jw__update_msg(ui, "%s", message);
    }
    ui->update_next_poll_ms = SDL_GetTicks() +
        (jw_settings_ui_wants_update_poll(ui) ? 500u : 3000u);
}

static int jw__update_option_count(const jw_settings_ui *ui) {
    if (!ui) {
        return 0;
    }
    int count = ui->update.option_count;
    if (count < 0) {
        count = 0;
    }
    if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
        count = JW_IPC_UPDATE_MAX_OPTIONS;
    }
    return count;
}

static void jw__update_check_releases(jw_settings_ui *ui,
                                      char *status_buf,
                                      size_t status_size);

static bool jw__confirm_update_picker_choice(const jw_settings_ui *ui,
                                             const jw_ipc_update_option_info *option,
                                             int idx) {
    if (!option) {
        return false;
    }
    if (!option->installed && idx == 0) {
        return true;
    }

    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Pick", .is_confirm = true },
    };
    char message[192];
    if (option->installed) {
        snprintf(message, sizeof(message), "Pick installed Leaf %s again?",
                 option->release_id[0] ? option->release_id : "release");
    } else {
        snprintf(message, sizeof(message), "Pick older Leaf %s?",
                 option->release_id[0] ? option->release_id : "release");
    }
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    (void)ui;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__open_update_picker(jw_settings_ui *ui,
                                   char *status_buf,
                                   size_t status_size) {
    if (!ui) {
        return;
    }
    if (jw__update_option_count(ui) <= 0) {
        jw__update_check_releases(ui, status_buf, status_size);
    }
    int count = jw__update_option_count(ui);
    if (count <= 0) {
        jw__copy_status(status_buf, status_size,
                        ui->update_msg[0] ? ui->update_msg : "No releases available");
        return;
    }
    if (ui->update_picker_list.cursor < 0 ||
        ui->update_picker_list.cursor >= count) {
        ui->update_picker_list.cursor =
            ui->update.selected_option >= 0 && ui->update.selected_option < count
            ? ui->update.selected_option
            : 0;
    }
    ui->screen = JW_SETTINGS_UPDATE_PICKER;
}

static void jw__select_update_picker_choice(jw_settings_ui *ui,
                                            char *status_buf,
                                            size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    int count = jw__update_option_count(ui);
    int idx = ui->update_picker_list.cursor;
    if (idx < 0 || idx >= count) {
        jw__copy_status(status_buf, status_size, "No update release selected");
        jw__update_msg(ui, "No update release selected");
        return;
    }

    const jw_ipc_update_option_info *option = &ui->update.options[idx];
    if (!jw__confirm_update_picker_choice(ui, option, idx)) {
        jw__copy_status(status_buf, status_size, "Update selection canceled");
        jw__update_msg(ui, "Update selection canceled");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    if (jw_ipc_update_select(ui->socket_path, option->index,
                             &info, status, sizeof(status)) == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
        ui->screen = JW_SETTINGS_UPDATE;
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update selection failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update selection failed");
    }
}

static void jw__update_check_releases(jw_settings_ui *ui,
                                      char *status_buf,
                                      size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    ui->update_checked_this_visit = true;   /* a real check ran -> results may show */
    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    if (jw_ipc_update_check(ui->socket_path, NULL, &info,
                            status, sizeof(status)) == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update check failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update check failed");
    }
}

static void jw__update_download_or_cancel(jw_settings_ui *ui,
                                          char *status_buf,
                                          size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    int rc = 0;
    if (!ui->update_checked_this_visit && !ui->update.download_active &&
        !ui->update.downloaded) {
        jw__copy_status(status_buf, status_size, "Check for updates first");
        jw__update_msg(ui, "Check for updates first");
        return;
    }

    if (ui->update.download_active) {
        rc = jw_ipc_update_cancel(ui->socket_path, &info, status, sizeof(status));
    } else if (ui->update.downloaded) {
        jw__copy_status(status_buf, status_size, "Update already downloaded");
        jw__update_msg(ui, "Update already downloaded");
        return;
    } else if (jw__update_is_current(&ui->update)) {
        jw__copy_status(status_buf, status_size, "Already on the latest version");
        jw__update_msg(ui, "Already on the latest version");
        return;
    } else if (ui->update.compatible && ui->update.artifact_name[0]) {
        rc = jw_ipc_update_download(ui->socket_path, &info, status, sizeof(status));
    } else {
        jw__copy_status(status_buf, status_size, "Check for updates first");
        jw__update_msg(ui, "Check for updates first");
        return;
    }

    if (rc == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update download failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update download failed");
    }
}

static void jw__update_install_or_reboot(jw_settings_ui *ui,
                                         char *status_buf,
                                         size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    if (!ui->update_checked_this_visit && !ui->update.install_active &&
        !ui->update.install_armed && !ui->update.downloaded) {
        jw__copy_status(status_buf, status_size, "Check for updates first");
        jw__update_msg(ui, "Check for updates first");
        return;
    }

    if (ui->update.install_armed) {
        if (!jw__confirm_update_reboot()) {
            jw__copy_status(status_buf, status_size, "Restart canceled");
            jw__update_msg(ui, "Restart canceled");
            return;
        }
        if (jw_ipc_platform_action(ui->socket_path, "reboot", 0) == 0) {
            jw__copy_status(status_buf, status_size, "Restarting");
            jw__update_msg(ui, "Restarting");
        } else {
            jw__copy_status(status_buf, status_size, "Restart failed");
            jw__update_msg(ui, "Restart failed");
        }
        return;
    }

    if (ui->update.install_active) {
        jw__refresh_update_status(ui, false);
        jw__copy_status(status_buf, status_size,
                        ui->update_msg[0] ? ui->update_msg : "Install in progress");
        return;
    }

    if (ui->update.install_blocked) {
        const char *msg = ui->update.install_message[0]
            ? ui->update.install_message
            : "Update install blocked";
        jw__copy_status(status_buf, status_size, msg);
        jw__update_msg(ui, "%s", msg);
        return;
    }

    if (!ui->update.downloaded) {
        jw__copy_status(status_buf, status_size, "Download update first");
        jw__update_msg(ui, "Download update first");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    if (jw_ipc_update_install_preflight(ui->socket_path, false, &info,
                                        status, sizeof(status)) != 0) {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update ready check failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update ready check failed");
        return;
    }
    jw__settings_update_from_ipc(ui, &info, status);

    if (ui->update.install_blocked) {
        const char *msg = ui->update.install_message[0]
            ? ui->update.install_message
            : "Update install blocked";
        jw__copy_status(status_buf, status_size, msg);
        jw__update_msg(ui, "%s", msg);
        return;
    }

    bool confirm_unknown_battery = false;
    if (ui->update.install_needs_confirmation) {
        const char *msg = ui->update.install_message[0]
            ? ui->update.install_message
            : "Install update even though checks are incomplete?";
        if (!jw__confirm_update_unknown_preflight(msg)) {
            jw__copy_status(status_buf, status_size, "Update install canceled");
            jw__update_msg(ui, "Update install canceled");
            return;
        }
        confirm_unknown_battery = true;
    } else if (!ui->update.install_ready) {
        jw__copy_status(status_buf, status_size, "Update is not ready to install");
        jw__update_msg(ui, "Update is not ready to install");
        return;
    }

    if (!jw__confirm_update_install(ui->update.release_id)) {
        jw__copy_status(status_buf, status_size, "Update install canceled");
        jw__update_msg(ui, "Update install canceled");
        return;
    }

    memset(&info, 0, sizeof(info));
    status[0] = '\0';
    if (jw_ipc_update_install(ui->socket_path, confirm_unknown_battery,
                              &info, status, sizeof(status)) == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
        cat_request_frame_in(500);
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update install failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update install failed");
    }
}

static void jw__set_adb(jw_settings_ui *ui, bool enabled,
                        char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!ui->adb_supported || ui->adb_enabled < 0) {
        snprintf(status_buf, status_size, "%s", T("ADB unavailable on this platform"));
        return;
    }

    if (enabled) {
        if (!jw__confirm_adb_enable()) {
            snprintf(status_buf, status_size, "%s", T("ADB enable canceled"));
            return;
        }
    } else if (!jw__confirm_adb_disable()) {
        snprintf(status_buf, status_size, "%s", T("ADB disable canceled"));
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_set_adb(ui->socket_path, enabled ? 1 : 0,
                       status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s",
                 enabled ? "ADB enable failed" : "ADB disable failed");
    }
    jw__refresh_adb(ui);
}

static void jw__set_boot_splash(jw_settings_ui *ui, bool enabled,
                                char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!ui->boot_splash_supported) {
        snprintf(status_buf, status_size, "%s", T("boot splash unavailable on this platform"));
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_set_boot_splash(ui->socket_path, enabled ? 1 : 0,
                               status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s",
                 enabled ? "boot splash enable failed" : "boot splash disable failed");
    }
    jw__refresh_boot_splash(ui);
    jw__persist_bool(ui, "boot_splash_enabled", ui->boot_splash_enabled);
}

static void jw__set_refresh_rate(jw_settings_ui *ui, int hz,
                                 char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!ui->refresh_rate_supported) {
        snprintf(status_buf, status_size, "%s", T("refresh rate unavailable on this platform"));
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_set_refresh_rate(ui->socket_path, hz, status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s", T("refresh rate change failed"));
        return;
    }
    /* The daemon restarts Weston and respawns this launcher, so re-querying now
       would read the pre-restart rate; persist the choice optimistically and let
       the fresh launcher pick up the live rate on its next status poll. */
    ui->refresh_rate_hz = hz;
    jw__persist_int(ui, "refresh_rate_hz", hz);
}

static void jw__set_hdmi_output(jw_settings_ui *ui, int mode,
                                char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }
    if (!ui->hdmi_supported) {
        snprintf(status_buf, status_size, "%s", T("HDMI output unavailable on this platform"));
        return;
    }
    if (mode < 0 || mode > 2) {
        mode = 0;
    }
    status_buf[0] = '\0';
    if (jw_ipc_set_hdmi_output(ui->socket_path, mode, status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s", T("HDMI output change failed"));
        return;
    }
    /* Switching to a TV output restarts Weston + respawns this launcher, so
       persist optimistically (same as the refresh-rate path). */
    ui->hdmi_output_mode = mode;
    jw__persist_int(ui, "hdmi_output_mode", mode);
}

static void jw__safe_unmount_secondary_sd(jw_settings_ui *ui,
                                          char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!jw__confirm_secondary_unmount()) {
        snprintf(status_buf, status_size, "%s", T("Unmount canceled"));
        jw__refresh_secondary_sd_status(ui);
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_safe_unmount_storage(ui->socket_path, "secondary_sd",
                                    status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s", T("Unmount failed"));
    }
    jw__refresh_secondary_sd_status(ui);
}

static bool jw__confirm_beta_channel(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Switch", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Beta builds are tester previews and may be unstable or lose "
                   "data. Switch to the Beta update channel?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return jw__confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

/* Toggle the update channel (Stable <-> Beta) from the System Update page.
   Switching TO Beta requires a confirm; switching back to Stable is silent. An
   active download/install/armed-reboot locks the toggle. On a real change we
   persist the key and re-run the check against the new channel. */
static void jw__cycle_update_channel(jw_settings_ui *ui, char *status_buf,
                                     size_t status_size) {
    /* Lock the toggle while any check or transfer is in flight. A running check
       won't restart for the new channel (the daemon no-ops a second check), so
       the old channel's results would populate the UI; and a completed-but-
       uninstalled download would be discarded by the re-check the switch kicks
       off (jw__clear_candidate clears downloaded/download_path). */
    bool checking = ui->update_have_status &&
                    strcmp(ui->update.state, "checking") == 0;
    if (checking || ui->update.download_active || ui->update.install_active ||
        ui->update.install_armed || ui->update.downloaded) {
        jw__copy_status(status_buf, status_size,
                        checking
                        ? "Wait for the release check to finish before switching channels"
                        : "Finish the current update before switching channels");
        return;
    }
    bool to_beta = (ui->update_channel_index != JW_UPDATE_CHANNEL_BETA_IDX);
    if (to_beta && !jw__confirm_beta_channel())
        return;   /* cancelled — stay on Stable */

    ui->update_channel_index = to_beta ? JW_UPDATE_CHANNEL_BETA_IDX
                                       : JW_UPDATE_CHANNEL_STABLE_IDX;
    jw__persist(ui, "update_channel", to_beta ? "beta" : "stable");
    /* Re-check against the new channel so the release rows repopulate. */
    jw__update_check_releases(ui, status_buf, status_size);
}

/* ─── Input dispatch ───────────────────────────────────────────────────── */

static bool jw__settings_handle_button_inner(jw_settings_ui *ui, cat_button button,
                                    char *status_buf, size_t status_size,
                                    bool *theme_changed) {
    if (!ui || !ui->open) return false;
    if (theme_changed) *theme_changed = false;

    switch (ui->screen) {

    /* ── Home ────────────────────────────────────────────────────────── */
    case JW_SETTINGS_HOME:
        {
        int category_count = jw__home_category_count(ui);
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->home_list, -1, category_count); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->home_list, +1, category_count); break;
            /* Left/Right jump to the top/bottom of the category list. Only safe on
               this pure-navigation list: the sub-pages use Left/Right to change
               values, so a jump there would be an easy mis-press. */
            case CAT_BTN_LEFT:  cat_list_state_jump(&ui->home_list, 0, category_count); break;
            case CAT_BTN_RIGHT: cat_list_state_jump(&ui->home_list, category_count - 1, category_count); break;
            case CAT_BTN_A: {
                int idx = ui->home_list.cursor;
                if (idx == 0) ui->screen = JW_SETTINGS_APPEARANCE;
                else if (idx == 1) {
                    ui->screen = JW_SETTINGS_DISPLAY;
                    /* Re-sync to live values so an OSD/hardware change made
                       outside settings isn't stale before we adjust. */
                    jw__refresh_brightness(ui);
                    jw__refresh_volume(ui);
                    jw__refresh_audio_status(ui);
                    jw__refresh_refresh_rate(ui);
                    jw__refresh_hdmi(ui);
                }
                else if (idx == 3) {
                    ui->screen = JW_SETTINGS_NETWORK;
                    ui->network_list.cursor = 0;
                    ui->network_list.scroll_offset = 0;
                    ui->wifi_msg[0] = '\0';
                    jw__wifi_attempt_clear(ui);
                    jw__refresh_adb(ui);
                    ui->wifi_radio_on = jw_wifi_available() && jw_wifi_radio_is_on();
                    jw__refresh_wifi(ui);          /* show status immediately */
                    if (ui->wifi_radio_on) {
                        jw_wifi_scan_start();      /* kick a scan */
                        jw__refresh_wifi_scan(ui); /* show any cached results now */
                    } else {
                        ui->wifi_network_count = 0;
                    }
                    unsigned now = SDL_GetTicks();
                    ui->wifi_next_poll_ms = now + 2000;            /* then live every ~2s */
                    ui->wifi_next_scan_ms = now + JW_WIFI_SCAN_INTERVAL_MS;
                }
                else if (idx == 4) {
                    ui->screen = JW_SETTINGS_BLUETOOTH;
                    ui->bluetooth_list.cursor = 0;
                    ui->bluetooth_list.scroll_offset = 0;
                    ui->bt_msg[0] = '\0';
                    ui->bt_op = JW_BT_OP_NONE;
                    ui->bt_op_manual = false;
                    unsigned now = SDL_GetTicks();
                    ui->bt_next_poll_ms = now + JW_BT_ENTRY_DEFER_MS;
                    ui->bt_next_scan_ms = now + JW_BT_ENTRY_DEFER_MS;
                    cat_request_frame_in(JW_BT_ENTRY_DEFER_MS);
                }
                else if (idx == 2) {
                    ui->screen = JW_SETTINGS_LIGHTING;
                    jw__refresh_led(ui);
                }
                else if (idx == 5) {
                    ui->screen = JW_SETTINGS_SCRAPING;
                    ui->scraping_list.cursor = 0;
                    ui->scraping_list.scroll_offset = 0;
                }
                else if (idx == 6) ui->screen = JW_SETTINGS_ACCOUNTS;
                else if (idx == 7) {
                    ui->screen = JW_SETTINGS_BEHAVIOR;
                    jw__refresh_boot_splash(ui);
                    jw__refresh_performance(ui);
                    jw__refresh_secondary_sd_status(ui);   /* Unmount SD row lives here now */
                }
                else if (idx == 8) {
                    ui->screen = JW_SETTINGS_CONTROLS;
                    jw__refresh_rumble(ui);
                }
                else if (idx == 9) {
                    /* CTL-1 omits invalid-only discoveries, so a reported row
                       is a valid service, retained history, or an actionable
                       stale generation. A clean system has no row here. */
                    if (jw__services_available(ui)) {
                        ui->screen = JW_SETTINGS_SERVICES;
                        ui->services_list.cursor = 0;
                        ui->services_list.scroll_offset = 0;
                        ui->services_msg[0] = '\0';
                        ui->services_next_poll_ms =
                            SDL_GetTicks() + JW_SERVICES_POLL_INTERVAL_MS;
                    } else {
                        if (status_buf && status_size > 0)
                            snprintf(status_buf, status_size, "%s",
                                     T("No services installed"));
                    }
                }
                break;
            }
            case CAT_BTN_B:
                jw_settings_ui_close(ui);
                return false;
            default: break;
        }
        break;
        }

    /* ── Appearance sub-menu ─────────────────────────────────────────── */
    case JW_SETTINGS_APPEARANCE:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->appearance_list, -1, JW_APPEAR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->appearance_list, +1, JW_APPEAR_ROW_COUNT); break;
            case CAT_BTN_LEFT:
                if (ui->appearance_list.cursor == JW_APPEAR_THEME)
                    jw__cycle_color_scheme(ui, -1, theme_changed);
                break;
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int row = ui->appearance_list.cursor;
                if (row == JW_APPEAR_THEME)
                    jw__cycle_color_scheme(ui, +1, theme_changed);
                else if (row == JW_APPEAR_COLORS)   ui->screen = JW_SETTINGS_COLORS;
                else if (row == JW_APPEAR_LAYOUT)   ui->screen = JW_SETTINGS_LAYOUT;
                else if (row == JW_APPEAR_STATUSBAR) ui->screen = JW_SETTINGS_STATUS_BAR;
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Colors ──────────────────────────────────────────────────────── */
    case JW_SETTINGS_COLORS:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->colors_list, -1, JW_COLOR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->colors_list, +1, JW_COLOR_ROW_COUNT); break;
            case CAT_BTN_A: {
                ap_theme *t = cat_get_theme();
                int row = ui->colors_list.cursor;
                bool changed = false;
                if (row == JW_COLOR_ACCENT)
                    changed = jw__pick_color(ui, &t->accent, "accent_color", row);
                else if (row == JW_COLOR_TEXT)
                    changed = jw__pick_color(ui, &t->text, "text_color", row);
                else if (row == JW_COLOR_HINT)
                    changed = jw__pick_color(ui, &t->hint, "hint_color", row);
                else if (row == JW_COLOR_HIGHLIGHT)
                    changed = jw__pick_color(ui, &t->highlight, "highlight_color", row);
                else if (row == JW_COLOR_BACKGROUND)
                    changed = jw__pick_color(ui, &t->background, "bg_color", row);
                else if (row == JW_COLOR_BTN_TEXT)
                    changed = jw__pick_color(ui, &t->button_label, "button_label_color", row);
                else if (row == JW_COLOR_BTN_BG)
                    changed = jw__pick_color(ui, &t->button_glyph_bg, "button_glyph_bg_color", row);
                if (changed) {
                    if (row == JW_COLOR_ACCENT)
                        cat_set_theme_color(NULL);
                    cat_finalize_theme_colors(t);
                    /* Hand-editing a color diverges from any preset → "Custom". */
                    if (ui->color_scheme_index != -1) {
                        ui->color_scheme_index = -1;
                        jw__persist_int(ui, "color_scheme_index", -1);
                    }
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Layout ──────────────────────────────────────────────────────── */
    case JW_SETTINGS_LAYOUT:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->layout_list, -1, JW_LAYOUT_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->layout_list, +1, JW_LAYOUT_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                int row = ui->layout_list.cursor;
                if (row == JW_LAYOUT_HOME_STYLE) {
                    int next = (ui->layout_mode + dir + 2) % 2;
                    if (next != ui->layout_mode)
                        jw__apply_layout(ui, next, theme_changed);
                } else if (row == JW_LAYOUT_PILL_SHAPE) {
                    int next = (ui->pill_shape_index + dir + JW_SETTINGS_PILL_SHAPE_COUNT) % JW_SETTINGS_PILL_SHAPE_COUNT;
                    ui->pill_shape_index = next;
                    cat_get_theme()->pill_radius_ratio = kJawakaPillRadiusValues[next];
                    cat_get_theme()->pill_corner_mask  = kJawakaPillCornerMasks[next];
                    jw__persist_int(ui, "pill_shape_index", next);
                } else if (row == JW_LAYOUT_FONT_FAMILY) {
                    /* None of the themed families carry CJK, so applying one here
                       would turn the whole UI into tofu. Say so rather than
                       silently doing nothing -- an unresponsive row reads as a
                       bug, and the user cannot see why it is inert. */
                    if (jw_i18n_language_is_cjk(jw_i18n_language())) {
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, status_size, "%s",
                                     T("font is fixed while a CJK language is selected"));
                        }
                        break;
                    }
                    int next = (ui->font_family_index + dir + JW_APPEARANCE_FONT_FAMILY_COUNT) %
                               JW_APPEARANCE_FONT_FAMILY_COUNT;
                    const char *path = jw_appearance_font_path_for_index(next);
                    if (cat_reload_fonts(path) == CAT_OK) {
                        ui->font_family_index = next;
                        jw__persist_int(ui, "font_family_index", next);
                        /* Font metrics changed -> the launcher must recompute its
                           cached list row height (cat_box_fit_rows only ever clamps
                           the row count DOWN, so a smaller font won't re-grow it
                           without a rebuild). Reuse the theme-changed signal that
                           drives jw__rebuild_for_layout. NOTE: this also resets the
                           home cursor to row 0 (same as a color-scheme change);
                           revisit with a position-preserving refit if breadcrumbs
                           get more serious. */
                        if (theme_changed) *theme_changed = true;
                    } else if (status_buf && status_size > 0) {
                        snprintf(status_buf, status_size, "%s", T("font load failed"));
                    }
                } else if (row == JW_LAYOUT_FONT_SIZE) {
                    int next = (ui->font_size_index + dir + JW_SETTINGS_FONT_SIZE_COUNT) % JW_SETTINGS_FONT_SIZE_COUNT;
                    ui->font_size_index = next;
                    /* Re-assert the selected family before the bump reload — otherwise
                       cat_set_font_bump reloads via theme.font_path, which can be empty,
                       and falls back to res/font.ttf (clobbering the chosen font). */
                    ap_theme *ft = cat_get_theme();
                    snprintf(ft->font_path, sizeof(ft->font_path), "%s",
                             jw_appearance_font_path_for_language(ui->font_family_index,
                                                                 jw_i18n_language()));
                    cat_set_font_bump(kJawakaFontSizeValues[next]);
                    jw__persist_int(ui, "font_size_index", next);
                    /* Recompute cached list row height (see the font-family note
                       above) — without this, shrinking the font leaves the list
                       rows stretched tall until a relaunch. */
                    if (theme_changed) *theme_changed = true;
                } else if (row == JW_LAYOUT_TAB_SWITCH) {
                    int next = (ui->tab_glide + dir + JW_TAB_SWITCH_COUNT) % JW_TAB_SWITCH_COUNT;
                    ui->tab_glide = next;
                    jw__persist_int(ui, "tab_glide", next);
                } else if (row == JW_LAYOUT_STARTUP_TAB) {
                    /* Cycle only over the currently-visible tabs (Home Tabs can
                       hide some), so Startup Tab can never point at a hidden tab.
                       Find where the current startup tab sits in the visible set,
                       step by dir within it, and adopt that tab. */
                    int vis = ui->home_tab_visible > 0 ? ui->home_tab_visible : 1;
                    int pos = 0;
                    for (int i = 0; i < vis; i++)
                        if (ui->home_tab_order[i] == ui->startup_tab_index) { pos = i; break; }
                    int next_pos = (pos + dir + vis) % vis;
                    ui->startup_tab_index = ui->home_tab_order[next_pos];
                    jw__persist_int(ui, "startup_tab_index", ui->startup_tab_index);
                } else if (row == JW_LAYOUT_HOME_TABS) {
                    if (button == CAT_BTN_A || button == CAT_BTN_RIGHT) {
                        ui->home_tabs_grabbed = false;
                        ui->home_tabs_list.cursor = 0;
                        ui->home_tabs_list.scroll_offset = 0;
                        ui->screen = JW_SETTINGS_HOME_TABS;
                    }
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Status Bar ──────────────────────────────────────────────────── */
    case JW_SETTINGS_STATUS_BAR:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->statusbar_list, -1, JW_STATUSBAR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->statusbar_list, +1, JW_STATUSBAR_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                int row = ui->statusbar_list.cursor;
                if (row == JW_STATUSBAR_HINTS) {
                    ui->show_hints = !ui->show_hints;
                    jw__persist_bool(ui, "show_hints", ui->show_hints);
                } else if (row == JW_STATUSBAR_CLOCK) {
                    int next = (ui->clock_style_index + dir + JW_SETTINGS_CLOCK_STYLE_COUNT) % JW_SETTINGS_CLOCK_STYLE_COUNT;
                    ui->clock_style_index = next;
                    jw__persist_int(ui, "clock_style_index", next);
                } else if (row == JW_STATUSBAR_BATTERY) {
                    int mode = jw__battery_mode(ui->show_battery, ui->show_battery_level);
                    mode = (mode + dir + JW_BATTERY_MODE_COUNT) % JW_BATTERY_MODE_COUNT;
                    jw__battery_mode_to_flags(mode, &ui->show_battery, &ui->show_battery_level);
                    jw__persist_bool(ui, "show_battery", ui->show_battery);
                    jw__persist_bool(ui, "show_battery_level", ui->show_battery_level);
                } else if (row == JW_STATUSBAR_WIFI) {
                    ui->show_wifi = !ui->show_wifi;
                    jw__persist_bool(ui, "show_wifi", ui->show_wifi);
                } else if (row == JW_STATUSBAR_BLUETOOTH) {
                    ui->show_bluetooth = !ui->show_bluetooth;
                    jw__persist_bool(ui, "show_bluetooth", ui->show_bluetooth);
                    if (ui->show_bluetooth) jw_settings_ui_refresh_bt_state(ui);
                } else if (row == JW_STATUSBAR_VOLUME) {
                    ui->show_volume = !ui->show_volume;
                    jw__persist_bool(ui, "show_volume", ui->show_volume);
                    if (ui->show_volume) jw__refresh_volume(ui);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Display & Sound (brightness + volume) ───────────────────────── */
    case JW_SETTINGS_DISPLAY:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->display_list, -1, JW_DISPLAY_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->display_list, +1, JW_DISPLAY_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                if (ui->display_list.cursor == JW_DISPLAY_BRIGHTNESS)
                    jw__change_brightness(ui, dir * JW_PLATFORM_BRIGHTNESS_STEP_PERCENT,
                                          status_buf, status_size);
                else if (ui->display_list.cursor == JW_DISPLAY_REFRESH_RATE) {
                    /* Cycle the refresh rate (left/right step, A advances). On a TV,
                       HDMI has no 100Hz mode and both it and 60 render as 720p60, so
                       the live-rate cycler would stick at 60 - offer just {60,120}
                       there (the two distinct HDMI modes). The internal panel does
                       every rate in kPanelRefreshHz. */
                    static const int tv_rates[] = {60, 120};
                    bool on_tv = ui->hdmi_connected == 1 && ui->hdmi_output_mode != 0;
                    const int *rates = on_tv ? tv_rates : kPanelRefreshHz;
                    int n = on_tv ? 2 : JW_PANEL_REFRESH_COUNT;
                    int cur = jw__nearest_refresh_index(rates, n, ui->refresh_rate_hz);
                    int next = (cur + dir + n) % n;
                    jw__set_refresh_rate(ui, rates[next], status_buf, status_size);
                }
                else if (ui->display_list.cursor == JW_DISPLAY_BFI) {
                    /* Black Frame Insertion: actionable only where a refresh is
                       spare to blank. Left/Right and A all just toggle on/off; the
                       daemon writes video_black_frame_insertion into the per-launch
                       RA config. The confirmation names the content rate, so
                       turning it on at 100Hz reads as "for 50 fps" rather than an
                       unqualified "on" the user has to pair up themselves. */
                    int fps = jw_bfi_content_fps(ui->refresh_rate_hz);
                    if (fps <= 0) {
                        snprintf(status_buf, status_size, "%s",
                                 T("Black Frame Insertion needs 100 or 120 Hz"));
                    } else {
                        ui->bfi_enabled = !ui->bfi_enabled;
                        jw__persist_int(ui, "bfi_enabled", ui->bfi_enabled ? 1 : 0);
                        if (ui->bfi_enabled) {
                            snprintf(status_buf, status_size,
                                     "Black Frame Insertion on for %d fps", fps);
                        } else {
                            snprintf(status_buf, status_size, "%s",
                                     T("Black Frame Insertion off"));
                        }
                    }
                }
                else if (ui->display_list.cursor == JW_DISPLAY_HDMI) {
                    /* Cycle Off -> 4:3 -> Stretch (left/right step, A advances).
                       Actionable only with a TV plugged in. */
                    if (!(ui->hdmi_supported && ui->hdmi_connected == 1)) {
                        snprintf(status_buf, status_size, "%s",
                                 ui->hdmi_supported ? "No HDMI cable connected"
                                                    : "HDMI output unavailable");
                    } else {
                        int next = (ui->hdmi_output_mode + dir + 3) % 3;
                        jw__set_hdmi_output(ui, next, status_buf, status_size);
                    }
                }
                else if (ui->display_list.cursor == JW_DISPLAY_OUTPUT)
                    jw__set_audio_output(ui, jw__next_audio_output(ui, dir),
                                         status_buf, status_size);
                else if (ui->display_list.cursor == JW_DISPLAY_VOLUME)
                    jw__change_volume(ui, dir * JW_PLATFORM_VOLUME_STEP_PERCENT,
                                      status_buf, status_size);
                else if (ui->display_list.cursor == JW_DISPLAY_TEST_SOUND &&
                         button == CAT_BTN_A) {
                    /* Toggle: the daemon plays the clip on the current output, or
                       stops it if already playing (left/right do nothing here).
                       Reflect the action from the last-known state; the 300ms
                       av-poll then syncs the Play/Stop label. */
                    bool was_playing = ui->test_sound_playing;
                    jw_ipc_platform_action(ui->socket_path, "play-test-sound", 0);
                    ui->test_sound_playing = !was_playing;
                    snprintf(status_buf, status_size, "%s",
                             was_playing ? T("Stopped test sound") : T("Playing test sound…"));
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Network (Wi-Fi scan/connect + ADB access) ───────────────────── */
    case JW_SETTINGS_NETWORK: {
        bool wifi_available = jw_wifi_available();
        int row_count = JW_NETWORK_FIXED_ROWS +
                        ((wifi_available && ui->wifi_radio_on) ? ui->wifi_network_count : 0);
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->network_list, -1, row_count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->network_list, +1, row_count);
                break;
            case CAT_BTN_A: {
                /* Row 0: toggle the radio. (Blocks briefly; turning OFF will drop
                   an ADB-over-Wi-Fi session — expected.) */
                if (ui->network_list.cursor == JW_NETWORK_ROW_WIFI) {
                    if (!wifi_available) {
                        jw__wifi_msg(ui, "Wi-Fi unavailable on this platform");
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                        }
                        break;
                    }
                    bool turning_on = !ui->wifi_radio_on;
                    jw__wifi_msg(ui, turning_on ? "Turning Wi-Fi on…"
                                                : "Turning Wi-Fi off…");
                    snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                    jw__wifi_attempt_clear(ui);
                    jw_wifi_set_radio(turning_on);
                    ui->wifi_radio_on = jw_wifi_radio_is_on();
                    ui->network_list.cursor = 0;
                    ui->wifi_next_poll_ms = SDL_GetTicks();
                    break;
                }

                if (ui->network_list.cursor == JW_NETWORK_ROW_ADB) {
                    if (!ui->adb_supported || ui->adb_enabled < 0) {
                        jw__wifi_msg(ui, "ADB unavailable on this platform");
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                        }
                        break;
                    }
                    bool enable = !(ui->adb_enabled == 1);
                    jw__set_adb(ui, enable, status_buf, status_size);
                    jw__wifi_msg(ui, "%s", status_buf && status_buf[0]
                                           ? status_buf : (enable ? "ADB enabled" : "ADB disabled"));
                    break;
                }

                /* Later rows: connect the selected network (open/saved direct; a
                   secured network with no saved profile prompts for the key). */
                int ni = ui->network_list.cursor - JW_NETWORK_FIXED_ROWS;
                if (!ui->wifi_radio_on || ni < 0 || ni >= ui->wifi_network_count) {
                    break;
                }
                const jw_wifi_network_t *net = &ui->wifi_networks[ni];

                if (net->current && ui->wifi.connected) {
                    /* A on the connected network disconnects it. */
                    if (jw_wifi_disconnect() == 0)
                        jw__wifi_msg(ui,
                                 "Disconnected from %s", net->ssid);
                    else
                        jw__wifi_msg(ui,
                                 "Could not disconnect");
                    jw__wifi_attempt_clear(ui);
                    snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                    ui->wifi_next_poll_ms = SDL_GetTicks();
                    break;
                }

                jw_wifi_connect_result r = jw_wifi_connect(net->ssid, net->secured);
                if (r == JW_WIFI_CONNECT_NEED_PASSWORD) {
                    cat_keyboard_result kb;
                    char prompt[160];
                    snprintf(prompt, sizeof(prompt),
                             "Password for %s\nStart: Confirm\nY: Cancel",
                             net->ssid);
                    if (cat_keyboard("", prompt, CAT_KB_GENERAL, &kb) == CAT_OK &&
                        kb.text[0]) {
                        r = jw_wifi_connect_psk(net->ssid, kb.text);
                    } else {
                        jw__wifi_msg(ui, "Cancelled");
                        snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                        break;
                    }
                }

                if (r == JW_WIFI_CONNECT_OK) {
                    jw__wifi_msg(ui,
                             "Connecting to %s…", net->ssid);
                    /* Track the attempt + attach the event monitor so the poll can
                       confirm success or catch a WRONG_KEY auth failure. */
                    jw__wifi_attempt_begin(ui, net->ssid);
                } else {
                    jw__wifi_msg(ui,
                             "Could not connect to %s", net->ssid);
                }
                snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                ui->wifi_next_poll_ms = SDL_GetTicks();   /* poll right away */
                break;
            }
            case CAT_BTN_Y: {
                /* Forget the selected network's saved profile (scan rows only). */
                int ni = ui->network_list.cursor - JW_NETWORK_FIXED_ROWS;
                if (ui->wifi_radio_on && ni >= 0 && ni < ui->wifi_network_count) {
                    const jw_wifi_network_t *net = &ui->wifi_networks[ni];
                    if (net->saved) {
                        if (jw_wifi_forget(net->ssid) == 0)
                            jw__wifi_msg(ui,
                                     "Forgot %s", net->ssid);
                        else
                            jw__wifi_msg(ui,
                                     "Could not forget %s", net->ssid);
                        jw_wifi_scan_start();
                        jw__refresh_wifi(ui);
                        jw__refresh_wifi_scan(ui);
                    } else {
                        jw__wifi_msg(ui,
                                 "%s isn't saved", net->ssid);
                    }
                    snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                }
                break;
            }
            case CAT_BTN_X:
                if (!ui->wifi_radio_on) {
                    break;   /* nothing to scan with the radio off */
                }
                jw_wifi_scan_start();
                jw__refresh_wifi(ui);
                jw__refresh_wifi_scan(ui);
                ui->wifi_next_scan_ms = SDL_GetTicks() + JW_WIFI_SCAN_INTERVAL_MS;
                jw__wifi_msg(ui, "Scanning Wi-Fi…");
                snprintf(status_buf, status_size, "Scanning Wi-Fi…");
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;
    }

    /* -- Bluetooth (scan/pair/connect/manage) ------------------------- */
    case JW_SETTINGS_BLUETOOTH: {
        int row_count = jw__bt_row_count(ui);
        if (row_count < 1) {
            row_count = 1;
        }
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->bluetooth_list, -1, row_count);
                for (int g = 0; g < row_count &&
                     jw__bt_row_is_header(ui, ui->bluetooth_list.cursor); g++)
                    cat_list_state_move(&ui->bluetooth_list, -1, row_count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->bluetooth_list, +1, row_count);
                for (int g = 0; g < row_count &&
                     jw__bt_row_is_header(ui, ui->bluetooth_list.cursor); g++)
                    cat_list_state_move(&ui->bluetooth_list, +1, row_count);
                break;
            case CAT_BTN_A: {
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw__bt_msg(ui, "Bluetooth is busy");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }

                int row = ui->bluetooth_list.cursor;
                if (row == JW_BLUETOOTH_ROW_POWER) {
                    bool turning_on = !ui->bt_radio_on;
                    jw__bt_msg(ui, turning_on ? "Turning Bluetooth on..."
                                               : "Turning Bluetooth off...");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    if (jw_bt_set_radio(turning_on) != 0) {
                        jw__bt_msg(ui, turning_on ? "Bluetooth on failed"
                                                  : "Bluetooth off failed");
                    } else {
                        jw__bt_msg(ui, turning_on ? "Bluetooth on"
                                                  : "Bluetooth off");
                        /* Persist the choice so jawakad restores it at boot: the
                           stock BLUETOOTH_PARAM flag gets clobbered back to on at
                           boot, so it can't hold the user's "keep it off". */
                        jw__persist(ui, "platform.bluetooth_enabled",
                                    turning_on ? "1" : "0");
                    }
                    jw__refresh_bluetooth_lists(ui);
                    ui->bt_next_poll_ms = SDL_GetTicks() + JW_BT_POLL_INTERVAL_MS;
                    break;
                }

                if (row == JW_BLUETOOTH_ROW_NAME) {
                    jw__bt_msg(ui, "Bluetooth name is read-only");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }

                bool paired_device = false;
                const jw_bt_device_t *dev = jw__bt_row_device(ui, row, &paired_device);
                if (!dev) {
                    break;
                }

                if (paired_device && dev->connected) {
                    bool disconnected = (jw_bt_disconnect(dev->mac) == 0);
                    if (disconnected)
                        jw__bt_msg(ui, "Disconnected %s", dev->name);
                    else
                        jw__bt_msg(ui, "Disconnect failed");
                    jw__refresh_bluetooth_lists(ui);
                    if (disconnected)
                        jw__bt_route_back_if_orphaned(ui);
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }

                bool pair_if_needed = !paired_device;
                if (jw_bt_connect_start(dev->mac, pair_if_needed) != 0) {
                    jw__bt_msg(ui, "Could not start Bluetooth connect");
                } else {
                    ui->bt_op = pair_if_needed ? JW_BT_OP_PAIR_CONNECT : JW_BT_OP_CONNECT;
                    ui->bt_op_manual = true;
                    jw__bt_msg(ui, pair_if_needed ? "Pairing %s..."
                                                  : "Connecting %s...",
                               dev->name[0] ? dev->name : dev->mac);
                    cat_request_frame_in(250);
                }
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->bt_msg);
                break;
            }
            case CAT_BTN_Y: {
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw__bt_msg(ui, "Bluetooth is busy");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }
                bool paired_device = false;
                const jw_bt_device_t *dev =
                    jw__bt_row_device(ui, ui->bluetooth_list.cursor, &paired_device);
                if (!dev || !paired_device) {
                    break;
                }
                const char *name = dev->name[0] ? dev->name : dev->mac;
                if (!jw__confirm_bt_unpair(name)) {
                    jw__bt_msg(ui, "Unpair canceled");
                    break;
                }
                bool forgot = (jw_bt_forget(dev->mac) == 0);
                if (forgot)
                    jw__bt_msg(ui, "Unpaired %s", name);
                else
                    jw__bt_msg(ui, "Unpair failed");
                jw__refresh_bluetooth_lists(ui);
                if (forgot)
                    jw__bt_route_back_if_orphaned(ui);
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->bt_msg);
                break;
            }
            case CAT_BTN_X:
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw__bt_msg(ui, "Bluetooth is busy");
                    break;
                }
                (void)jw__bt_scan_start(ui, true);
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->bt_msg);
                break;
            case CAT_BTN_B:
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw_bt_cancel_operation();
                    ui->bt_op = JW_BT_OP_NONE;
                    ui->bt_op_manual = false;
                }
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;
    }

    /* ── Lighting (LED ring) ─────────────────────────────────────────── */
    case JW_SETTINGS_LIGHTING:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->lighting_list, -1, JW_LIGHTING_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->lighting_list, +1, JW_LIGHTING_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                int row = ui->lighting_list.cursor;
                bool changed = true;
                if (row == JW_LIGHTING_ENABLE) {
                    ui->led_enabled = !ui->led_enabled;
                } else if (row == JW_LIGHTING_MODE) {
                    ui->led_mode = (ui->led_mode + dir + JW_LED_MODE_COUNT) % JW_LED_MODE_COUNT;
                } else if (row == JW_LIGHTING_COLOR) {
                    if (button == CAT_BTN_A)
                        changed = jw__pick_color(ui, &ui->led_color, "led_color", -1);
                    else
                        changed = false;
                } else if (row == JW_LIGHTING_BRIGHTNESS) {
                    int next = ui->led_brightness + dir;
                    if (next < 0) next = 0;
                    if (next > JW_LED_BRIGHTNESS_MAX) next = JW_LED_BRIGHTNESS_MAX;
                    changed = (next != ui->led_brightness);
                    ui->led_brightness = next;
                } else if (row == JW_LIGHTING_SPEED) {
                    int next = ui->led_speed + dir;
                    if (next < 0) next = 0;
                    if (next > JW_LED_SPEED_MAX) next = JW_LED_SPEED_MAX;
                    changed = (next != ui->led_speed);
                    ui->led_speed = next;
                }
                if (changed) jw__apply_led(ui);
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Accounts (placeholder) ──────────────────────────────────────── */
    case JW_SETTINGS_ACCOUNTS:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->accounts_list, -1, JW_ACCOUNTS_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->accounts_list, +1, JW_ACCOUNTS_ROW_COUNT);
                break;
            case CAT_BTN_A: {
                bool is_ss = ui->accounts_list.cursor == JW_ACCOUNTS_SCREENSCRAPER;
                if (is_ss) {
                    /* ScreenScraper: validate against the API through jawakad
                       (the daemon owns the dev-credential half of API auth).
                       A rejected login is surfaced in the Accounts row itself
                       (ss_rejected) because the hint-line status is hidden when
                       hints are off; an unreachable daemon/network saves the
                       credentials unverified for the first scrape to confirm. */
                    char entered[64];
                    snprintf(entered, sizeof(entered), "%s", ui->ss_username);
                    ui->ss_rejected = false;   /* fresh attempt */
                    char prompt[160];
                    cat_keyboard_result kb;
                    snprintf(prompt, sizeof(prompt),
                             "ScreenScraper username\nStart: Confirm\nY: Cancel");
                    if (cat_keyboard(entered, prompt, CAT_KB_GENERAL, &kb) != CAT_OK ||
                        !kb.text[0]) {
                        snprintf(status_buf, status_size, "Cancelled");
                        break;
                    }
                    snprintf(entered, sizeof(entered), "%.*s",
                             (int)sizeof(entered) - 1, kb.text);
                    cat_keyboard_result pw;
                    snprintf(prompt, sizeof(prompt),
                             "ScreenScraper password\nStart: Confirm\nY: Cancel");
                    if (cat_keyboard("", prompt, CAT_KB_GENERAL, &pw) != CAT_OK ||
                        !pw.text[0]) {
                        snprintf(status_buf, status_size, "Cancelled");
                        break;
                    }

                    jw_ipc_scrape_validate_info info;
                    int rc = ui->socket_path[0]
                        ? jw_ipc_scrape_validate(ui->socket_path, entered,
                                                 pw.text, &info)
                        : -1;
                    if (rc == 0 && info.valid) {
                        snprintf(ui->ss_username, sizeof(ui->ss_username),
                                 "%s", entered);
                        ui->ss_verified = true;
                        ui->ss_rejected = false;
                        ui->ss_max_threads = info.max_threads;
                        ui->ss_requests_today = info.requests_today;
                        ui->ss_max_requests = info.max_requests;
                        jw__persist(ui, "screenscraper_user", ui->ss_username);
                        jw__persist(ui, "screenscraper_pass", pw.text);
                        jw__persist(ui, "screenscraper_verified", "1");
                        jw__persist_int(ui, "screenscraper_maxthreads",
                                        info.max_threads);
                        jw__persist_int(ui, "screenscraper_requests_today",
                                        info.requests_today);
                        jw__persist_int(ui, "screenscraper_max_requests",
                                        info.max_requests);
                        if (info.max_requests > 0) {
                            snprintf(status_buf, status_size,
                                     "Signed in - %d thread%s, quota %d/%d today",
                                     info.max_threads,
                                     info.max_threads == 1 ? "" : "s",
                                     info.requests_today, info.max_requests);
                        } else {
                            snprintf(status_buf, status_size, "Signed in as %s",
                                     ui->ss_username);
                        }
                    } else if (rc == 0 && info.rejected) {
                        /* Wrong username/password — shown on the row (see above). */
                        ui->ss_rejected = true;
                        snprintf(status_buf, status_size,
                                 "Rejected - wrong username or password");
                    } else {
                        /* Daemon or network unavailable: keep them, unverified. */
                        snprintf(ui->ss_username, sizeof(ui->ss_username), "%s",
                                 entered);
                        ui->ss_verified = false;
                        ui->ss_rejected = false;
                        ui->ss_max_threads = 0;
                        ui->ss_requests_today = 0;
                        ui->ss_max_requests = 0;
                        jw__persist(ui, "screenscraper_user", ui->ss_username);
                        jw__persist(ui, "screenscraper_pass", pw.text);
                        jw__persist(ui, "screenscraper_verified", "0");
                        jw__persist(ui, "screenscraper_maxthreads", "");
                        jw__persist(ui, "screenscraper_requests_today", "");
                        jw__persist(ui, "screenscraper_max_requests", "");
                        snprintf(status_buf, status_size,
                                 "Saved - could not verify: %s",
                                 (rc == 0 && info.message[0]) ? info.message
                                                              : "daemon unavailable");
                    }
                    break;
                }

                /* RetroAchievements: stored for RetroArch, which validates at
                   game launch. */
                char prompt[160];
                cat_keyboard_result kb;
                snprintf(prompt, sizeof(prompt),
                         "RetroAchievements username\nStart: Confirm\nY: Cancel");
                if (cat_keyboard(ui->ra_username, prompt, CAT_KB_GENERAL, &kb) != CAT_OK ||
                    !kb.text[0]) {
                    snprintf(status_buf, status_size, "Cancelled");
                    break;
                }
                cat_keyboard_result pw;
                snprintf(prompt, sizeof(prompt),
                         "RetroAchievements password\nStart: Confirm\nY: Cancel");
                if (cat_keyboard("", prompt, CAT_KB_GENERAL, &pw) != CAT_OK ||
                    !pw.text[0]) {
                    snprintf(status_buf, status_size, "Cancelled");
                    break;
                }
                snprintf(ui->ra_username, sizeof(ui->ra_username), "%.*s",
                         (int)sizeof(ui->ra_username) - 1, kb.text);
                jw__persist(ui, "retroachievements_user", ui->ra_username);
                jw__persist(ui, "retroachievements_pass", pw.text);
                snprintf(status_buf, status_size,
                         "Saved - RetroArch signs in at game launch");
                break;
            }
            case CAT_BTN_Y:
                if (ui->accounts_list.cursor == JW_ACCOUNTS_SCREENSCRAPER &&
                    (ui->ss_username[0] || ui->ss_rejected)) {
                    ui->ss_username[0] = '\0';
                    ui->ss_verified = false;
                    ui->ss_rejected = false;
                    ui->ss_max_threads = 0;
                    ui->ss_requests_today = 0;
                    ui->ss_max_requests = 0;
                    jw__persist(ui, "screenscraper_user", "");
                    jw__persist(ui, "screenscraper_pass", "");
                    jw__persist(ui, "screenscraper_verified", "");
                    jw__persist(ui, "screenscraper_maxthreads", "");
                    jw__persist(ui, "screenscraper_requests_today", "");
                    jw__persist(ui, "screenscraper_max_requests", "");
                    snprintf(status_buf, status_size, "Signed out of ScreenScraper");
                } else if (ui->accounts_list.cursor == JW_ACCOUNTS_RETROACHIEVEMENTS &&
                           ui->ra_username[0]) {
                    ui->ra_username[0] = '\0';
                    jw__persist(ui, "retroachievements_user", "");
                    jw__persist(ui, "retroachievements_pass", "");
                    snprintf(status_buf, status_size, "Signed out of RetroAchievements");
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── Scraping ────────────────────────────────────────────────────── */
    case JW_SETTINGS_SCRAPING:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->scraping_list, -1, JW_SCRAPING_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->scraping_list, +1, JW_SCRAPING_ROW_COUNT);
                break;
            case CAT_BTN_A:
                if (ui->scraping_list.cursor == JW_SCRAPING_QUEUE) {
                    ui->screen = JW_SETTINGS_SCRAPE_QUEUE;
                    ui->scrape_queue_list.cursor = 0;
                    ui->scrape_queue_list.scroll_offset = 0;
                    ui->scrape_queue_filter = 0;
                    ui->scrape_queue_next_poll_ms = 0;   /* force an immediate poll */
                    break;
                }
                if (ui->scraping_list.cursor == JW_SCRAPING_DOWNLOAD) {
                    ui->scrape_download_list.cursor = 0;
                    ui->scrape_download_list.scroll_offset = 0;
                    ui->scrape_download_replace = false;   /* default: missing-only */
                    ui->scrape_missing_have_cache = false;
                    jw__scrape_download_clear_rows(ui);
                    if (ui->socket_path[0] &&
                        jw_ipc_scrape_missing_counts(
                            ui->socket_path, &ui->scrape_missing_cache) == 0) {
                        ui->scrape_missing_have_cache = true;
                        /* scrape_missing_cache was repopulated; refresh derived labels. */
                        jw__scrape_download_rebuild_rows(ui);
                    }
                    ui->screen = JW_SETTINGS_SCRAPE_DOWNLOAD;
                    break;
                }
                ui->scrape_edit_is_region =
                    ui->scraping_list.cursor == JW_SCRAPING_REGION;
                ui->scrape_edit_grabbed = false;
                ui->scrape_edit_list.cursor = 0;
                ui->scrape_edit_list.scroll_offset = 0;
                ui->screen = JW_SETTINGS_SCRAPE_PRIORITY;
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── Scrape queue (live job list) ────────────────────────────────── */
    case JW_SETTINGS_SCRAPE_QUEUE: {
        const jw_ipc_scrape_queue_info *q =
            ui->scrape_queue_have_cache ? ui->scrape_queue_cache : NULL;
        int idx[JW_IPC_SCRAPE_QUEUE_MAX_ROWS];
        int count = q ? jw__scrape_queue_filter_rows(q, ui->scrape_queue_filter,
                                                     idx, JW_IPC_SCRAPE_QUEUE_MAX_ROWS)
                      : 0;
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->scrape_queue_list, -1, count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->scrape_queue_list, +1, count);
                break;
            case CAT_BTN_Y:   /* cycle filter: All -> Busy -> Done -> Failed */
                ui->scrape_queue_filter = (ui->scrape_queue_filter + 1) % 4;
                ui->scrape_queue_list.cursor = 0;
                ui->scrape_queue_list.scroll_offset = 0;
                break;
            case CAT_BTN_A:   /* detail for a finished row */
                if (count > 0 && ui->scrape_queue_list.cursor < count) {
                    const jw_ipc_scrape_queue_row *row =
                        &q->rows[idx[ui->scrape_queue_list.cursor]];
                    cat_queue_status s = jw__scrape_queue_cat_status(row->state);
                    if (s == CAT_QUEUE_DONE || s == CAT_QUEUE_FAILED ||
                        s == CAT_QUEUE_SKIPPED) {
                        ui->scrape_queue_detail_row = *row;   /* snapshot for the page */
                        ui->screen = JW_SETTINGS_SCRAPE_QUEUE_DETAIL;
                        for (int k = 0; k < 4; k++)
                            ui->scrape_detail_marquee[k].elapsed_ms = 0;
                        /* Decode the art once here, not every frame (a per-frame
                           IMG_LoadTexture stutters the page). */
                        if (ui->scrape_detail_art) {
                            SDL_DestroyTexture(ui->scrape_detail_art);
                            ui->scrape_detail_art = NULL;
                        }
                        ui->scrape_detail_art_w = ui->scrape_detail_art_h = 0;
                        if (row->state == JW_IPC_SCRAPE_ROW_DONE &&
                            row->output_path[0] &&
                            access(row->output_path, R_OK) == 0) {
                            SDL_Texture *t = cat_load_image(row->output_path);
                            int tw = 0, th = 0;
                            if (t) SDL_QueryTexture(t, NULL, NULL, &tw, &th);
                            if (t && tw > 0 && th > 0) {
                                ui->scrape_detail_art = t;
                                ui->scrape_detail_art_w = tw;
                                ui->scrape_detail_art_h = th;
                            } else if (t) {
                                SDL_DestroyTexture(t);
                            }
                        }
                    }
                }
                break;
            case CAT_BTN_X:   /* Stop All while busy, else Clear Done */
                if (ui->socket_path[0]) {
                    if (jw__scrape_queue_active(q)) {
                        int stopped = 0;
                        (void)jw_ipc_scrape_stop_all(ui->socket_path, &stopped);
                        snprintf(status_buf, status_size, "Stopped %d job%s",
                                 stopped, stopped == 1 ? "" : "s");
                    } else if (q && q->done > 0) {
                        int cleared = 0;
                        (void)jw_ipc_scrape_clear_done(ui->socket_path, &cleared);
                        snprintf(status_buf, status_size, "Cleared %d", cleared);
                        ui->scrape_queue_list.cursor = 0;
                        ui->scrape_queue_list.scroll_offset = 0;
                    }
                    ui->scrape_queue_next_poll_ms = 0;   /* force a refresh */
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_SCRAPING;
                break;
            default:
                break;
        }
        break;
    }

    /* ── Scrape Missing Artwork picker (All Systems / per system) ─────── */
    case JW_SETTINGS_SCRAPE_DOWNLOAD: {
        int count = ui->scrape_missing_have_cache
            ? ui->scrape_download_row_count + 1
            : 0;
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->scrape_download_list, -1, count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->scrape_download_list, +1, count);
                break;
            case CAT_BTN_Y:   /* toggle missing-only vs replace-all */
                ui->scrape_download_replace = !ui->scrape_download_replace;
                break;
            case CAT_BTN_A:
                if (count > 0 && ui->socket_path[0] &&
                    ui->scrape_download_list.cursor < count) {
                    int cur = ui->scrape_download_list.cursor;
                    bool missing_only = !ui->scrape_download_replace;
                    jw_ipc_scrape_start_info info;
                    char st[96] = "";
                    int rc = (cur == 0)
                        ? jw_ipc_scrape_start_full(ui->socket_path, "all", "",
                                                   NULL, missing_only, &info,
                                                   st, sizeof(st))
                        : jw_ipc_scrape_start_full(
                              ui->socket_path, "system",
                              ui->scrape_download_rows[cur - 1].system, NULL,
                              missing_only, &info, st, sizeof(st));
                    if (rc == 0) {
                        jw__scrape_start_status(&info, missing_only,
                                                status_buf, status_size);
                        if (jw_ipc_scrape_missing_counts(
                                ui->socket_path, &ui->scrape_missing_cache) == 0) {
                            ui->scrape_missing_have_cache = true;
                            /* scrape_missing_cache was repopulated; refresh derived labels. */
                            jw__scrape_download_rebuild_rows(ui);
                        } else {
                            ui->scrape_missing_have_cache = false;
                            jw__scrape_download_clear_rows(ui);
                        }
                        ui->scrape_queue_next_poll_ms = 0;
                        if (info.enqueued > 0) {
                            ui->screen = JW_SETTINGS_SCRAPE_QUEUE;
                            ui->scrape_queue_list.cursor = 0;
                            ui->scrape_queue_list.scroll_offset = 0;
                            ui->scrape_queue_filter = 0;
                        }
                    } else {
                        snprintf(status_buf, status_size, "%s",
                                 st[0] ? st : "Scrape failed");
                    }
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_SCRAPING;
                break;
            default:
                break;
        }
        break;
    }

    /* ── Scrape job result (native detail page) ──────────────────────── */
    case JW_SETTINGS_SCRAPE_QUEUE_DETAIL:
        if (button == CAT_BTN_B) {
            if (ui->scrape_detail_art) {
                SDL_DestroyTexture(ui->scrape_detail_art);
                ui->scrape_detail_art = NULL;
            }
            ui->scrape_detail_art_w = ui->scrape_detail_art_h = 0;
            ui->screen = JW_SETTINGS_SCRAPE_QUEUE;
        }
        break;

    /* ── Scrape priority editor (artwork or region) ──────────────────── */
    case JW_SETTINGS_SCRAPE_PRIORITY: {
        bool region = ui->scrape_edit_is_region;
        int *order = region ? ui->scrape_region_order : ui->scrape_artwork_order;
        int *included = region ? &ui->scrape_region_included
                               : &ui->scrape_artwork_included;
        int count = region ? jw_ss_regions_count : jw_ss_media_types_count;
        int cursor = ui->scrape_edit_list.cursor;

        switch (button) {
            case CAT_BTN_UP:
            case CAT_BTN_DOWN: {
                int dir = button == CAT_BTN_UP ? -1 : +1;
                if (ui->scrape_edit_grabbed) {
                    /* Reorder within the included zone. */
                    int target = cursor + dir;
                    if (target >= 0 && target < *included) {
                        jw__scrape_order_move(order, cursor, target);
                        cat_list_state_move(&ui->scrape_edit_list, dir, count);
                        jw__scrape_persist_order(ui, region);
                    }
                } else {
                    cat_list_state_move(&ui->scrape_edit_list, dir, count);
                }
                break;
            }
            case CAT_BTN_A: {
                if (ui->scrape_edit_grabbed) {
                    ui->scrape_edit_grabbed = false;
                    break;
                }
                if (cursor < *included) {
                    /* Exclude: sink to the top of the excluded zone. */
                    jw__scrape_order_move(order, cursor, *included - 1);
                    *included -= 1;
                    cat_list_state_jump(&ui->scrape_edit_list, *included, count);
                } else {
                    /* Include: append to the included zone. */
                    jw__scrape_order_move(order, cursor, *included);
                    *included += 1;
                    cat_list_state_jump(&ui->scrape_edit_list, *included - 1, count);
                }
                jw__scrape_persist_order(ui, region);
                break;
            }
            case CAT_BTN_X:
                if (ui->scrape_edit_grabbed) {
                    ui->scrape_edit_grabbed = false;
                } else if (cursor < *included) {
                    ui->scrape_edit_grabbed = true;
                } else {
                    snprintf(status_buf, status_size,
                             "Excluded entries cannot be reordered");
                }
                break;
            case CAT_BTN_B:
                if (ui->scrape_edit_grabbed) {
                    ui->scrape_edit_grabbed = false;
                } else {
                    ui->screen = JW_SETTINGS_SCRAPING;
                }
                break;
            default:
                break;
        }
        break;
    }

    /* ── Home Tabs editor ─────────────────────────────────────────────── */
    case JW_SETTINGS_HOME_TABS: {
        int cursor = ui->home_tabs_list.cursor;
        int count = JW_HOME_TABS_COUNT;
        switch (button) {
            case CAT_BTN_UP:
            case CAT_BTN_DOWN: {
                int dir = button == CAT_BTN_UP ? -1 : +1;
                if (ui->home_tabs_grabbed) {
                    /* Reorder within the visible zone only. */
                    int target = cursor + dir;
                    if (target >= 0 && target < ui->home_tab_visible) {
                        jw__home_tab_order_move(ui->home_tab_order, cursor, target);
                        cat_list_state_move(&ui->home_tabs_list, dir, count);
                        jw__home_tabs_persist(ui);
                    }
                } else {
                    cat_list_state_move(&ui->home_tabs_list, dir, count);
                }
                break;
            }
            case CAT_BTN_A: {
                if (ui->home_tabs_grabbed) {
                    ui->home_tabs_grabbed = false;
                    break;
                }
                if (cursor < ui->home_tab_visible) {
                    /* Hiding: never drop the last visible tab (min-one guard). */
                    if (ui->home_tab_visible <= 1) {
                        snprintf(status_buf, status_size,
                                 "At least one tab must stay visible");
                        break;
                    }
                    /* Sink to the top of the hidden zone. */
                    jw__home_tab_order_move(ui->home_tab_order, cursor,
                                            ui->home_tab_visible - 1);
                    ui->home_tab_visible -= 1;
                    cat_list_state_jump(&ui->home_tabs_list, ui->home_tab_visible, count);
                } else {
                    /* Showing: append to the visible zone. */
                    jw__home_tab_order_move(ui->home_tab_order, cursor,
                                            ui->home_tab_visible);
                    ui->home_tab_visible += 1;
                    cat_list_state_jump(&ui->home_tabs_list,
                                        ui->home_tab_visible - 1, count);
                }
                jw__home_tabs_persist(ui);
                break;
            }
            case CAT_BTN_X:
                if (ui->home_tabs_grabbed) {
                    ui->home_tabs_grabbed = false;
                } else if (cursor < ui->home_tab_visible) {
                    ui->home_tabs_grabbed = true;
                } else {
                    snprintf(status_buf, status_size,
                             "Hidden tabs cannot be reordered");
                }
                break;
            case CAT_BTN_B:
                if (ui->home_tabs_grabbed)
                    ui->home_tabs_grabbed = false;
                else
                    ui->screen = JW_SETTINGS_LAYOUT;
                break;
            default:
                break;
        }
        break;
    }

    /* ── System Update ───────────────────────────────────────────────── */
    case JW_SETTINGS_UPDATE:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->update_list, -1, JW_UPDATE_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->update_list, +1, JW_UPDATE_ROW_COUNT);
                break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
                if (ui->update_list.cursor == JW_UPDATE_ROW_CHANNEL)
                    jw__cycle_update_channel(ui, status_buf, status_size);
                break;
            case CAT_BTN_X:
                if (ui->update.download_active) {
                    jw__update_download_or_cancel(ui, status_buf, status_size);
                } else {
                    jw__open_update_picker(ui, status_buf, status_size);
                }
                break;
            case CAT_BTN_A:
                if (ui->update_list.cursor == JW_UPDATE_ROW_CHANNEL) {
                    jw__cycle_update_channel(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_CHECK) {
                    jw__update_check_releases(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_DOWNLOAD) {
                    jw__update_download_or_cancel(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_INSTALL) {
                    jw__update_install_or_reboot(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_AVAILABLE) {
                    jw__open_update_picker(ui, status_buf, status_size);
                } else {
                    jw__refresh_update_status(ui, false);
                    jw__copy_status(status_buf, status_size,
                                    ui->update_msg[0] ? ui->update_msg : "Update status refreshed");
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── System Update picker ─────────────────────────────────────────── */
    case JW_SETTINGS_UPDATE_PICKER: {
        int count = jw__update_option_count(ui);
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->update_picker_list, -1, count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->update_picker_list, +1, count);
                break;
            case CAT_BTN_A:
                jw__select_update_picker_choice(ui, status_buf, status_size);
                break;
            case CAT_BTN_X:
                jw__update_check_releases(ui, status_buf, status_size);
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_UPDATE;
                break;
            default:
                break;
        }
        break;
    }

    /* ── About ───────────────────────────────────────────────────────── */
    case JW_SETTINGS_ABOUT: {
        int line_h = TTF_FontHeight(cat_get_font(CAT_FONT_SMALL)) + cat_scale(8);
        switch (button) {
            case CAT_BTN_UP:   jw__scroll_move(&ui->about_scroll, -line_h,
                                               ui->about_scroll_max); break;
            case CAT_BTN_DOWN: jw__scroll_move(&ui->about_scroll, +line_h,
                                               ui->about_scroll_max); break;
            case CAT_BTN_B:    ui->screen = JW_SETTINGS_HOME; break;
            default: break;
        }
        break;
    }

    case JW_SETTINGS_LIBRARY: {
        int line_h = TTF_FontHeight(cat_get_font(CAT_FONT_SMALL)) + cat_scale(8);
        switch (button) {
            case CAT_BTN_UP:   jw__scroll_move(&ui->library_scroll, -line_h,
                                               ui->library_scroll_max); break;
            case CAT_BTN_DOWN: jw__scroll_move(&ui->library_scroll, +line_h,
                                               ui->library_scroll_max); break;
            case CAT_BTN_B:    ui->screen = JW_SETTINGS_HOME; break;
            default: break;
        }
        break;
    }

    case JW_SETTINGS_PLAYTIME: {
        int line_h = TTF_FontHeight(cat_get_font(CAT_FONT_SMALL)) + cat_scale(8);
        switch (button) {
            case CAT_BTN_UP:   jw__scroll_move(&ui->playtime_scroll, -line_h,
                                               ui->playtime_scroll_max); break;
            case CAT_BTN_DOWN: jw__scroll_move(&ui->playtime_scroll, +line_h,
                                               ui->playtime_scroll_max); break;
            case CAT_BTN_B:    ui->screen = JW_SETTINGS_HOME; break;
            default: break;
        }
        break;
    }

    /* ── Services (app-services-v1) ────────────────────────────────────── */
    case JW_SETTINGS_SERVICES: {
        int rows = ui->services_count > 0 ? ui->services_count : 1;
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->services_list, -1, rows);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->services_list, +1, rows);
                break;
            case CAT_BTN_A: {
                int row = ui->services_list.cursor;
                if (row < 0 || row >= ui->services_count) {
                    break;
                }
                const jw_ipc_service_info *svc = &ui->services[row];
                if (strcmp(svc->state, "unavailable") == 0) {
                    snprintf(ui->services_msg, sizeof(ui->services_msg), "%s",
                             "Service unavailable");
                    break;
                }
                if (strcmp(svc->state, "stopping") == 0) {
                    snprintf(ui->services_msg, sizeof(ui->services_msg), "%s",
                             "Service is still stopping");
                    break;
                }
                /* A toggles the session run state: run a stopped service,
                   stop a running one. */
                bool running = strcmp(svc->state, "running") == 0 ||
                               strcmp(svc->state, "starting") == 0;
                const char *op = running ? "stop" : "run";
                char status[128] = { 0 };
                if (jw_ipc_service_ctl(ui->socket_path, op, svc->id,
                                       status, sizeof(status)) == 0) {
                    /* The daemon acknowledges the request, it does not wait
                       for the group to be gone -- the stop sequence runs on
                       its tick. Report the transition, not a completion the
                       poll below may well contradict a moment later. */
                    snprintf(ui->services_msg, sizeof(ui->services_msg),
                             "%s %s", running ? "Stopping" : "Starting",
                             svc->id);
                } else {
                    snprintf(ui->services_msg, sizeof(ui->services_msg),
                             "%s", status[0] ? status : "Request failed");
                }
                jw__refresh_services(ui);
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->services_msg);
                break;
            }
            case CAT_BTN_X: {
                int row = ui->services_list.cursor;
                if (row < 0 || row >= ui->services_count) {
                    break;
                }
                const jw_ipc_service_info *svc = &ui->services[row];
                if (strcmp(svc->state, "unavailable") == 0 &&
                    !svc->desired_enabled) {
                    snprintf(ui->services_msg, sizeof(ui->services_msg), "%s",
                             "Unavailable services cannot be enabled");
                    break;
                }
                /* X toggles persistent "Start with Leaf". */
                const char *op = svc->desired_enabled ? "disable" : "enable";
                char status[128] = { 0 };
                if (jw_ipc_service_ctl(ui->socket_path, op, svc->id,
                                       status, sizeof(status)) == 0) {
                    snprintf(ui->services_msg, sizeof(ui->services_msg),
                             "%s %s",
                             svc->desired_enabled ? "Disabled" : "Enabled",
                             svc->id);
                } else {
                    snprintf(ui->services_msg, sizeof(ui->services_msg),
                             "%s", status[0] ? status : "Request failed");
                }
                jw__refresh_services(ui);
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->services_msg);
                break;
            }
            case CAT_BTN_B:
                (void)jw__refresh_services(ui);
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;
    }

    /* ── Controls & Feedback ─────────────────────────────────────────── */
    case JW_SETTINGS_CONTROLS:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->controls_list, -1, JW_CONTROLS_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->controls_list, +1, JW_CONTROLS_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                if (ui->controls_list.cursor == JW_CONTROLS_RUMBLE) {
                    (void)dir;
                    ui->rumble_enabled = !ui->rumble_enabled;
                    jw__persist_bool(ui, "rumble_enabled", ui->rumble_enabled);
                    /* Confirmation buzz when switching haptics on. */
                    if (ui->rumble_enabled)
                        jw_ipc_rumble_preview(ui->socket_path, ui->rumble_strength);
                } else if (ui->controls_list.cursor == JW_CONTROLS_STRENGTH) {
                    if (!ui->rumble_enabled && !ui->rumble_game) break;
                    int s = ui->rumble_strength + dir * 5;
                    if (s < 0) s = 0;
                    if (s > 100) s = 100;
                    if (s != ui->rumble_strength) {
                        ui->rumble_strength = s;
                        jw__persist_int(ui, "rumble_strength", s);
                        /* Preview is menu feedback, so respect its independent switch. */
                        if (ui->rumble_enabled)
                            jw_ipc_rumble_preview(ui->socket_path, s);
                    }
                } else if (ui->controls_list.cursor == JW_CONTROLS_NAV) {
                    if (!ui->rumble_enabled) break;
                    (void)dir;
                    ui->rumble_nav = !ui->rumble_nav;
                    jw__persist_bool(ui, "rumble_nav", ui->rumble_nav);
                } else if (ui->controls_list.cursor == JW_CONTROLS_GAME) {
                    (void)dir;
                    ui->rumble_game = !ui->rumble_game;
                    /* Read by the daemon at game launch, so it takes effect on
                       the next launch -- no need to touch a running game. */
                    jw__persist_bool(ui, "rumble_game", ui->rumble_game);
                } else if (ui->controls_list.cursor == JW_CONTROLS_SCREENSHOTS) {
                    (void)dir;
                    ui->screenshots_enabled = !ui->screenshots_enabled;
                    /* Persist only; the daemon reads the DB key on each hotkey. */
                    jw__persist_bool(ui, "screenshots_enabled", ui->screenshots_enabled);
                } else if (ui->controls_list.cursor == JW_CONTROLS_RECORDING) {
                    (void)dir;
                    ui->recording_enabled = !ui->recording_enabled;
                    /* Persist only; the daemon reads the DB key on each hotkey. */
                    jw__persist_bool(ui, "recording_enabled", ui->recording_enabled);
                } else if (ui->controls_list.cursor == JW_CONTROLS_REC_SPLIT) {
                    if (!ui->recording_enabled) break;
                    (void)dir;
                    ui->recording_split = !ui->recording_split;
                    /* Read by the daemon when it dispatches the convert pass, so
                       it applies to the next game you exit -- nothing to change
                       about a recording already on the card. */
                    jw__persist_bool(ui, "recording_split", ui->recording_split);
                } else if (ui->controls_list.cursor == JW_CONTROLS_REC_KEEP) {
                    if (!ui->recording_enabled) break;
                    (void)dir;
                    ui->recording_keep_src = !ui->recording_keep_src;
                    jw__persist_bool(ui, "recording_keep_source", ui->recording_keep_src);
                }
                break;
            }
            case CAT_BTN_B:    ui->screen = JW_SETTINGS_HOME; break;
            default: break;
        }
        break;

    /* ── Behavior ────────────────────────────────────────────────────── */
    case JW_SETTINGS_BEHAVIOR:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->behavior_list, -1, jw__behavior_rows(ui)); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->behavior_list, +1, jw__behavior_rows(ui)); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                if (ui->behavior_list.cursor == JW_BEHAVIOR_LANGUAGE) {
                    if (ui->language_count <= 1) break;
                    int cur = 0;
                    for (int i = 0; i < ui->language_count; i++) {
                        if (strcmp(ui->languages[i], ui->language) == 0) { cur = i; break; }
                    }
                    int next = (cur + dir + ui->language_count) % ui->language_count;
                    if (next == cur) break;

                    /* The daemon persists this and restarts us; it does not come
                       back here. Do not persist locally as well -- a second write
                       would race the respawn, and jw__persist is the only writer
                       by contract anyway. */
                    char status[128] = "";
                    if (jw_ipc_set_language(ui->socket_path, ui->languages[next],
                                            status, sizeof(status)) == 0) {
                        snprintf(ui->language, sizeof(ui->language), "%s",
                                 ui->languages[next]);
                    }
                    if (status_buf && status_size > 0) {
                        snprintf(status_buf, (size_t)status_size, "%s",
                                 status[0] ? status : "language change failed");
                    }
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_AUTO_SLEEP) {
                    int next = (ui->auto_sleep_index + dir + JW_AUTO_SLEEP_COUNT)
                               % JW_AUTO_SLEEP_COUNT;
                    ui->auto_sleep_index = next;
                    /* Persist the seconds value (the daemon reads it directly). */
                    jw__persist_int(ui, "auto_sleep_seconds", kAutoSleepSeconds[next]);
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_BOOT_SPLASH) {
                    (void)dir;
                    jw__set_boot_splash(ui, !ui->boot_splash_enabled,
                                        status_buf, status_size);
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_PERFORMANCE) {
                    if (!ui->performance_supported) {
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, (size_t)status_size, "%s",
                                     T("performance unavailable"));
                        }
                        break;
                    }
                    int next = (ui->game_perf_profile + dir + JW_GAME_PERF_PROFILE_COUNT)
                               % JW_GAME_PERF_PROFILE_COUNT;
                    jw_platform_perf_profile profile = kGamePerfProfiles[next];
                    char status[128] = "";
                    if (jw_ipc_set_performance_profile(
                            ui->socket_path, "global",
                            jw_platform_perf_profile_name(profile),
                            status, sizeof(status)) == 0) {
                        ui->game_perf_profile = next;
                        jw__persist(ui, "platform.performance.game_profile",
                                    jw_platform_perf_profile_name(profile));
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, (size_t)status_size, "%s", status);
                        }
                    } else if (status_buf && status_size > 0) {
                        snprintf(status_buf, (size_t)status_size, "%s",
                                 status[0] ? status : "performance failed");
                    }
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_TIMEZONE) {
                    /* Open the picker (A / Right); a long list isn't a cycler. */
                    if (button == CAT_BTN_A || button == CAT_BTN_RIGHT) {
                        int cur = jw__timezone_index_of(ui->timezone);
                        ui->timezone_picker_list.cursor = cur;
                        /* Scroll so the current zone is on screen when it opens. */
                        int off = cur - (JW_TIMEZONE_VISIBLE_ROWS - 1);
                        ui->timezone_picker_list.scroll_offset = off > 0 ? off : 0;
                        ui->screen = JW_SETTINGS_TIMEZONE_PICKER;
                    }
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_RESET_RETROARCH) {
                    if (button == CAT_BTN_A)
                        jw__reset_retroarch_config(ui, status_buf, status_size);
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_UNMOUNT_SECONDARY) {
                    if (button == CAT_BTN_A)
                        jw__safe_unmount_secondary_sd(ui, status_buf, status_size);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Time Zone picker ─────────────────────────────────────────────── */
    case JW_SETTINGS_TIMEZONE_PICKER:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->timezone_picker_list, -1, JW_TIMEZONE_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->timezone_picker_list, +1, JW_TIMEZONE_COUNT);
                break;
            case CAT_BTN_A: {
                int idx = ui->timezone_picker_list.cursor;
                if (idx >= 0 && idx < JW_TIMEZONE_COUNT) {
                    snprintf(ui->timezone, sizeof(ui->timezone), "%s",
                             kTimeZones[idx].tz);
                    jw__persist(ui, "timezone", ui->timezone);
                    jw__apply_timezone(ui->timezone);   /* clock updates immediately */
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, (size_t)status_size, "Time zone: %s",
                                 kTimeZones[idx].label);
                }
                ui->screen = JW_SETTINGS_BEHAVIOR;
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_BEHAVIOR;
                break;
            default: break;
        }
        break;
    }

    return ui->open;
}

/* Sum of every settings list's cursor. Only the active screen's list moves on a
   given press, so a change in the sum means the cursor moved — lets us detect a
   real move (nav) vs a boundary (blocked) without a per-screen accessor. */
/* Public entry: run the real handler, then tick if the page changed.
 *
 * Cursor movement is no longer detected here. The lists and scroll views report
 * their own (see cat_ui_feedback_set), which is why the 24-case switch that used
 * to pair every screen with its control is gone -- pairing them by hand meant a
 * new screen was silent until someone remembered to add a case, and the
 * scroll-only pages were silent for exactly that reason.
 *
 * A page change is not movement, so it stays here: entering a sub-page or
 * backing out taps once. Value changes (Left/Right/A on a row) buzz via
 * jw__persist. */
bool jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                  char *status_buf, size_t status_size,
                                  bool *theme_changed) {
    if (!ui || !ui->open)
        return jw__settings_handle_button_inner(ui, button, status_buf,
                                                status_size, theme_changed);

    int scr0 = (int)ui->screen;

    bool still_open = jw__settings_handle_button_inner(ui, button, status_buf,
                                                       status_size, theme_changed);

    if ((int)ui->screen != scr0) {
        if (button == CAT_BTN_B && status_buf && status_size > 0)
            status_buf[0] = '\0';
        if (ui->socket_path[0])
            jw_ipc_rumble(ui->socket_path, "select");          /* page enter / back */
    }
    return still_open;
}
