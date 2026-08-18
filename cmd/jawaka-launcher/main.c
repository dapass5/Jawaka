#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "cJSON.h"
#include "internal/core/autodemo.h"
#include "internal/core/env.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/i18n/i18n.h"
#include "internal/focus/focus.h"
#include "internal/ipc/ipc_client.h"
#include "internal/launcher/console_colors.h"
#include "internal/launcher/coverflow.h"
#include "internal/launcher/focus_screen.h"
#include "internal/launcher/game_switcher.h"
#include "internal/launcher/system_names.h"
#include "internal/platform/bluetooth.h"
#include "internal/platform/cat_services.h"
#include "internal/platform/device.h"
#include "internal/platform/paths.h"
#include "internal/platform/platform_id.h"
#include "internal/platform/wifi.h"
#include "internal/retroarch/catalog.h"
#include "internal/settings/settings.h"
#include "internal/settings/theme_resolve.h"
#include "internal/store/pakrat_state.h"
#include "internal/store/pakrat_txn.h"

#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#define JW_MAX_SYSTEMS 64
#define JW_MAX_APPS    64
#define JW_MAX_PAKRAT_APPS 128
#define JW_OPENED_GAME_BROWSER_LIMIT 512
#define JW_GAME_LIST_RACE_SLACK 16
#define JW_MAX_FAVORITES 256   /* newest-first; a heavier list is truncated */
#define JW_MAX_RECENTS 64      /* most-recently-played first */
#define JW_MAX_SEARCH_RESULTS 128
#define JW_MAX_ACTION_ROWS 10
#define JW_MAX_CORE_CHOICES 24

#define JW_CONTENT_SETTING_CORE_ID "core_id"
#define JW_CONTENT_SETTING_PERFORMANCE_PROFILE "performance_profile"

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/* Button hint text: on device, return NULL so Catastrophe uses the canonical
 * button name (e.g. "L2", "MENU"). On desktop, show the keyboard shortcut.
 * Runtime check via cat_is_device() — no compile-flag branching. */
static inline const char *jw_hint(const char *desktop_key) {
    return CAT_PLATFORM_IS_DEVICE ? NULL : desktop_key;
}
static inline const char *jw_hint_device(const char *desktop_key, const char *device_key) {
    return CAT_PLATFORM_IS_DEVICE ? device_key : desktop_key;
}
#define JW_HINT(dk)            jw_hint(dk)
#define JW_HINT_DEVICE(dk, vk) jw_hint_device(dk, vk)

/* ─── Status bar — see jw__draw_status_bar after jw_launcher_state ──────── */

/* ─── Tabbed mode ─────────────────────────────────────────────────────────── */

typedef enum {
    JW_TAB_RECENTS = 0,
    JW_TAB_FAVORITES,
    JW_TAB_GAMES,
    JW_TAB_APPS,
    JW_TAB_COUNT
} jw_tab;

/* The home strip is games-only now; Settings lives in the MENU page's Settings
   tab (see jw__render_menu / jw__handle_menu_input). */
/* English keys; translated at each use via T(). */
static const char *kTabs[JW_TAB_COUNT] = { "Recents", "Favorites", "Games", "Apps" };

/* Forward declarations: shared preview helper used by Tabs games-tab and the
 * Vertical preview pane. Defined alongside jw__load_system_icon below. */
static void jw__draw_system_preview(int px, int py, int pw, int ph,
                                     const char *system_code, int game_count);

/* ─── Flat nav list (vertical + horizontal modes) ─────────────────────────── */

typedef enum {
    JW_FLAT_RECENTLY_PLAYED,
    JW_FLAT_FAVORITES,
    JW_FLAT_SYSTEM,
    JW_FLAT_APPS,
    JW_FLAT_SETTINGS,
    JW_FLAT_TOOLS,   /* horizontal: catch-all tile */
} jw_flat_kind;

typedef struct {
    jw_flat_kind kind;
    int          system_idx;
} jw_flat_item;

typedef enum {
    JW_ACTION_NONE = 0,
    JW_ACTION_GAME,
    JW_ACTION_SYSTEM
} jw_action_scope;

/* Action rows offer only what the underlying views can't already do directly:
   launching, favoriting, and reaching system defaults all have first-class
   buttons elsewhere, so they don't reappear here. */
typedef enum {
    JW_ACTION_ROW_SEARCH = 0,
    JW_ACTION_ROW_DISPLAY_NAME,
    JW_ACTION_ROW_CORE,
    JW_ACTION_ROW_PERFORMANCE,
    JW_ACTION_ROW_SCRAPE,        /* game: replace art */
    JW_ACTION_ROW_SCRAPE_CANCEL, /* swap-in while the target is queued */
    JW_ACTION_ROW_RESET
} jw_action_row_kind;

/* Coverflow runtime state (jw_games_cf, jw_cf_cube, and the jw_coverflow bundle)
   lives in internal/launcher/coverflow.h — the CF module owns those types. */

typedef struct {
    bool done;
    char code[64];
    char path[PATH_MAX];     /* empty when every candidate is known missing */
} jw_system_icon_memo;

/* ─── Launcher state ──────────────────────────────────────────────────────── */

/* Failsafe unlock chord: all five buttons pressed within the window, then held
   for the hold time, exits focus mode regardless of lock. */
#define JW_FOCUS_CHORD_WINDOW_MS 1000u
#define JW_FOCUS_CHORD_HOLD_MS   2000u

/* 5-Game Mode setup wizard steps (Actions tab -> Start 5-Game Mode). */
typedef enum {
    JW_FSETUP_PICK = 0,   /* multi-select up to 5 games from the browser */
    JW_FSETUP_ARRANGE,    /* reorder the picked tiles (X-grab) */
    JW_FSETUP_LOCK,       /* None / PIN */
    JW_FSETUP_PIN,        /* set + confirm the PIN (when lock == pin) */
    JW_FSETUP_STYLE,      /* Theme / Black & white */
    JW_FSETUP_CONFIRM,    /* preview + Start */
} jw_focus_setup_step;

typedef struct {
    /* tabbed mode */
    jw_tab             current_tab;
    /* Visible home tabs in display order (built from the "home_tab_order"
       setting): the launcher only ever shows/cycles these. */
    jw_tab             visible_tabs[JW_TAB_COUNT];
    int                visible_tab_count;
    /* Per-channel carousel cursor, so switching channels (Cover Flow Up/Down)
       returns to where you were in each rather than snapping back to the start. */
    int                tab_cursor[JW_TAB_COUNT];
    /* all modes */
    cat_list_state     list;
    /* data */
    jw_library_summary summary;
    jw_system_entry    systems[JW_MAX_SYSTEMS];
    jw_system_icon_memo system_icon_memos[JW_MAX_SYSTEMS];
    int                system_count;
    jw_app_entry       apps[JW_MAX_APPS];
    int                app_count;
    jw_pakrat_app_state pakrat_apps[JW_MAX_PAKRAT_APPS];
    int                pakrat_app_count;
    int                pakrat_load_rc;
    char               pakrat_message[160];
    bool               pakrat_open;
    bool               pakrat_detail_open;     /* drilled into a pak's full detail page */
    cat_scroll_state   pakrat_detail_scroll;
    cat_list_state     pakrat_list;
    jw_game_entry     *games;
    int                game_count;
    int                game_capacity;
    char               game_system[64];          /* system id (for queries) */
    char               game_system_display[64];  /* full name for the browser title */
    bool               games_open;
    bool               games_are_favorites;  /* browser is showing the Favorites list */
    cat_list_state     game_list;
    /* Favorites tab (tabbed layout): favorited games, reloaded on tab entry */
    jw_game_entry      favorites[JW_MAX_FAVORITES];
    int                favorites_count;
    /* Recents tab: most-recently-played games, reloaded on load + tab entry */
    jw_game_entry      recents[JW_MAX_RECENTS];
    int                recents_count;
    bool               apps_open;
    cat_list_state     app_list;
    jw_search_result   search_results[JW_MAX_SEARCH_RESULTS];
    int                search_count;
    char               search_query[256];
    bool               search_open;
    cat_list_state     search_list;
    /* flat nav (vertical / horizontal) */
    jw_flat_item       flat_items[JW_MAX_SYSTEMS + 6];
    int                flat_count;
    /* horizontal: tools sub-menu */
    bool               tools_open;
    cat_list_state     tools_list;
    /* Cover Flow runtime state (slide tween + both carousels + channel cube),
       bundled in internal/launcher/coverflow.{c,h}. */
    jw_coverflow       cf;
    /* Tab-switch slide (Glide setting): two content snapshots cross-slide. */
    bool               tab_anim_active;
    int                tab_anim_dir;        /* +1 = next (from right), -1 = prev */
    uint32_t           tab_anim_start_ms;
    uint32_t           flash_until_ms;   /* screenshot flash: white until this tick (0 = off) */
    SDL_Texture       *tab_anim_from;
    SDL_Texture       *tab_anim_to;
    int                tab_anim_y, tab_anim_h;
    /* curated per-console colors (Horizontal carousel; loaded from active theme) */
    jw_console_color_table console_colors;
    /* Game switcher: a dedicated recents/resume carousel opened with Select.
       Overlays the current layout; closing restores the prior view as-is. */
    bool               switcher_open;
    jw_game_switcher   switcher;
    /* Contextual system/game actions opened with X. */
    bool               actions_open;
    jw_action_scope    action_scope;
    cat_list_state     action_list;
    jw_action_row_kind action_rows[JW_MAX_ACTION_ROWS];
    int                action_row_count;
    jw_game_entry      action_game;
    char               action_system[64];
    char               action_system_display[64];
    jw_ra_core_choice  action_core_choices[JW_MAX_CORE_CHOICES];
    size_t             action_core_count;
    char               action_core_effective[64];
    char               action_core_game_override[64];
    char               action_core_system_override[64];
    char               action_perf_game_override[64];
    char               action_perf_system_override[64];
    char               action_system_display_override[64];
    bool               action_scrape_pending;   /* target has queued scrape work */
    /* System menu (MENU button): an in-launcher overlay, not a separate process,
       so open/close is instant (no respawn, no SDL/Wayland/DB re-init). The
       in-game menu is still its own process — a different path. */
    bool               menu_open;
    bool               menu_scanning;   /* Rescan in progress: pane shows "Scanning…" */
    int                menu_tab;        /* System menu tab: 0=Settings, 1=Actions, 2=Info */
    cat_list_state     menu_list;
    /* settings (Appearance/Library/Behavior/About) */
    jw_settings_ui     settings;
    /* status line */
    char               sdcard_root[PATH_MAX];
    char               state_dir[PATH_MAX];
    char               db_path[PATH_MAX];
    char               platform_root[PATH_MAX];
    char               socket_path[PATH_MAX];
    char               status[256];
    bool               scan_ready;
    bool               scan_running;
    bool               library_populated;
    int                library_generation;
    /* 5-Game Mode (focus mode): when active, the launcher renders only the
       curated focus screen and swallows normal navigation. Populated at startup
       from the JAWAKA_FOCUS_* env the daemon sets on each launcher spawn (so it
       survives return-from-game). See plans/five-game-mode.md. */
    bool               focus_active;
    bool               focus_bw;         /* style: Black & white vs Theme */
    jw_focus_lock      focus_lock;       /* none/pin */
    int                focus_cursor;     /* selected tile 0..focus_count-1 */
    int                focus_count;      /* resolved games (danglers dropped) */
    jw_game_entry      focus_games[JW_FOCUS_SCREEN_MAX_TILES];
    /* Focus-mode battery: the shared status poller publishes battery as a
       consume-once mailbox that only the normal render path drains, so focus
       mode reads power directly via IPC on its own ~5s cadence. */
    int                focus_batt_pct;   /* -1 unknown */
    int                focus_batt_chg;   /* -1 unknown, 0/1 */
    uint32_t           focus_batt_next_ms;
    /* Focus-mode Bluetooth: a paired-but-disconnected headset shows a corner pip
       and a Reconnect action in the unlock overlay. Polled on a throttle (the
       paired-list read shells out, so keep it off every frame). */
    bool               focus_bt_pip;
    char               focus_bt_headset_mac[24];
    uint32_t           focus_bt_next_ms;
    /* Failsafe unlock chord (always active in focus mode, any lock type): hold
       Menu+Select+Start+L2+R2 for ~2s to exit. A 5-button hold no kid hits by
       accident; the on-device "forgot the PIN" escape hatch. */
    bool               focus_chord_held;
    uint32_t           focus_chord_start_ms;
    /* MENU unlock overlay (the only way out of focus mode). */
    bool               focus_unlock_open;
    int                focus_pin[JW_FOCUS_PIN_LEN];
    int                focus_pin_slot;
    bool               focus_pin_error;
    char               focus_pin_hash[65]; /* from the DB, verified on submit */
    int                focus_unlock_confirm; /* 0 none, 1 reboot, 2 power off */
    /* Setup wizard (Actions tab -> Start 5-Game Mode). Reuses the games browser
       for the Pick step, marking into focus_setup_ids. */
    bool                focus_setup_open;
    jw_focus_setup_step focus_setup_step;
    int                 focus_setup_ids[JW_FOCUS_SCREEN_MAX_TILES]; /* picked, ordered */
    int                 focus_setup_count;
    jw_focus_lock       focus_setup_lock;
    jw_focus_style      focus_setup_style;
    int                 focus_setup_choice;   /* cursor within Lock/Style menus */
    /* Arrange step: X-grab reorder of the picked tiles. */
    int                 focus_setup_arrange_cursor;
    bool                focus_setup_grabbed;
    /* PIN set step: enter then confirm. */
    int                 focus_setup_pin[JW_FOCUS_PIN_LEN];
    int                 focus_setup_pin2[JW_FOCUS_PIN_LEN];
    int                 focus_setup_pin_slot;
    bool                focus_setup_pin_confirming; /* false=enter, true=re-enter */
    bool                focus_setup_pin_mismatch;
    /* Pick: a mode overlaid on the real Games-tab browser (systems -> games with
       the box-art pane). A toggles a game into focus_setup_ids; not a wizard step. */
    bool                focus_pick_active;
    char                focus_setup_note[96];   /* transient nudge, e.g. "5 max" */
    /* Suppresses haptics while the UI moves itself rather than the user moving
       it -- restoring a breadcrumb on startup, or reloading a list in place. */
    bool                haptics_muted;
} jw_launcher_state;

/* Fire a semantic UI haptic (see internal/ipc jw_ipc_rumble). Fire-and-forget:
   the daemon owns the vocabulary (single/double/triple tick) and the
   enable/nav gating, so call sites only name the event. */
static inline void jw__haptic(const jw_launcher_state *state, const char *event) {
    if (state && state->socket_path[0] && !state->haptics_muted)
        jw_ipc_rumble(state->socket_path, event);
}

/* Catastrophe's widgets report what happened to them; this turns that into what
   it feels like. It replaces the three hand-written "did the cursor move"
   checks this file and settings.c used to carry, each of which had to be
   remembered on every new screen and only ever covered the screens someone
   thought of.
 *
 * MOVED and ENTERED both ride the opt-in navigation tick. Typing is the
 * highest-frequency movement on the device, so anyone who turned per-move
 * feedback off wants the keyboard quiet too. */
static void jw__ui_feedback(cat_ui_feedback what, void *user) {
    const jw_launcher_state *state = (const jw_launcher_state *)user;
    switch (what) {
        case CAT_UI_MOVED:
        case CAT_UI_ENTERED:
            jw__haptic(state, "nav");
            break;
        case CAT_UI_SURFACE:
            jw__haptic(state, "select");
            break;
        case CAT_UI_EDGE:
            jw__haptic(state, "blocked");
            break;
    }
}

static void jw__system_icon_memo_clear(jw_launcher_state *state) {
    if (!state) return;
    memset(state->system_icon_memos, 0, sizeof(state->system_icon_memos));
}

/* ─── Home-tab visibility (hide + reorder) ────────────────────────────────── */

/* Parse the "home_tab_order" setting (CSV of visible jw_tab indices in display
   order) into state->visible_tabs[]/visible_tab_count. Defensive: dedupe, drop
   out-of-range/invalid tokens; an empty/missing/all-invalid value falls back to
   all tabs visible in natural order. Always yields at least one visible tab. */
static void jw__load_visible_tabs(jw_launcher_state *state, const char *db_path) {
    char csv[64] = { 0 };
    if (db_path && db_path[0])
        (void)jw_db_get_setting(db_path, "home_tab_order", csv, sizeof(csv));

    bool used[JW_TAB_COUNT] = { false };
    int n = 0;
    const char *p = csv[0] ? csv : NULL;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        p = end;
        if (v >= 0 && v < JW_TAB_COUNT && !used[(int)v]) {
            state->visible_tabs[n++] = (jw_tab)v;
            used[(int)v] = true;
        }
    }
    if (n == 0) {
        for (int i = 0; i < JW_TAB_COUNT; i++) state->visible_tabs[i] = (jw_tab)i;
        n = JW_TAB_COUNT;
    }
    state->visible_tab_count = n;
}

/* Present wrapper used everywhere instead of cat_present(). Normally a straight
   passthrough; while the screenshot flash is compositing, g_defer_present lets a
   render pass draw without presenting so the flash can lay a translucent white
   overlay over the finished scene and present once itself. */
static bool g_defer_present = false;
static void jw__present(void) {
    if (!g_defer_present) {
        cat_present();
    }
}

static bool jw__tab_is_visible(const jw_launcher_state *state, jw_tab tab) {
    for (int i = 0; i < state->visible_tab_count; i++)
        if (state->visible_tabs[i] == tab) return true;
    return false;
}

/* Position of tab within the visible set, or 0 if it isn't in it. */
static int jw__visible_tab_pos(const jw_launcher_state *state, jw_tab tab) {
    for (int i = 0; i < state->visible_tab_count; i++)
        if (state->visible_tabs[i] == tab) return i;
    return 0;
}

/* Snap current_tab onto the visible set if it points at a hidden tab. */
static void jw__reconcile_current_tab(jw_launcher_state *state) {
    if (state->visible_tab_count > 0 && !jw__tab_is_visible(state, state->current_tab))
        state->current_tab = state->visible_tabs[0];
}

/* Cover Flow reuses the tabbed data model (current_tab + per-tab lists +
   state->list.cursor), so any channel-aware logic — the Y favorite/remove
   handler, the hidden-tab cursor reconciliation — must treat both layouts the
   same. Widen a bare `layout == CAT_LAUNCHER_TABBED` check to this. */
static bool jw__layout_uses_channels(int layout) {
    return layout == CAT_LAUNCHER_TABBED || layout == CAT_LAUNCHER_COVERFLOW;
}

static void jw__draw_app_detail(const jw_launcher_state *state,
                                const jw_app_entry *app,
                                int detail_x, int detail_y,
                                int detail_w, int detail_h);
static void jw__load_pakrat_store(jw_launcher_state *state);
static int jw__draw_menu_tab_bar(const jw_launcher_state *state);
static int jw__pakrat_visible_rows(const jw_launcher_state *state);

/* Defined after the image helpers; used by the tabbed renderer above them. */
static void jw__render_favorites(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin);
/* Shared list+art renderer for the Favorites and Recents tabs (uses the image
   helpers, so it's defined late and forward-declared here). */
static void jw__render_game_list_pane(const jw_launcher_state *state,
                                      const jw_game_entry *entries, int count,
                                      int content_y, int content_h, int margin,
                                      const char *empty_msg);

static void jw__reset_game_data(jw_launcher_state *state) {
    if (!state) {
        return;
    }
    free(state->games);
    state->games = NULL;
    state->game_count = 0;
    state->game_capacity = 0;
}

static void jw__close_game_browser(jw_launcher_state *state) {
    if (!state) {
        return;
    }
    jw__reset_game_data(state);
    state->games_open = false;
    state->games_are_favorites = false;
    state->game_system[0] = '\0';
    state->game_system_display[0] = '\0';
    cat_list_state_init(&state->game_list, 1);
}

static void jw__replace_game_data(jw_launcher_state *state, jw_game_entry *games,
                                  int capacity, int count) {
    jw__reset_game_data(state);
    state->games = games;
    state->game_capacity = capacity;
    state->game_count = count;
}

typedef int (*jw__game_list_loader)(const char *db_path, jw_game_entry *out,
                                    int max_count, int *out_count);

static int jw__load_bounded_game_browser(const char *db_path,
                                         jw_launcher_state *state,
                                         jw__game_list_loader loader,
                                         int capacity) {
    if (!db_path || !state || !loader || capacity <= 0) {
        return -1;
    }

    jw_game_entry *games = calloc((size_t)capacity, sizeof(*games));
    if (!games) {
        return -1;
    }

    int count = 0;
    if (loader(db_path, games, capacity, &count) != 0) {
        free(games);
        return -1;
    }

    jw__replace_game_data(state, games, capacity, count);
    return 0;
}

/* Returns 0 on success, 1 when the system has no games, and -1 on load errors.
   Existing browser data is replaced only after the new full list has loaded. */
static int jw__load_system_games_full(const char *db_path, const char *system,
                                      jw_launcher_state *state, int retry_depth) {
    if (!db_path || !system || !system[0] || !state) {
        return -1;
    }

    int expected = 0;
    if (jw_db_count_games_for_system(db_path, system, &expected) != 0) {
        return -1;
    }
    if (expected <= 0) {
        return 1;
    }

    int capacity = expected;
    if (capacity <= INT_MAX - JW_GAME_LIST_RACE_SLACK) {
        capacity += JW_GAME_LIST_RACE_SLACK;
    }

    jw_game_entry *games = calloc((size_t)capacity, sizeof(*games));
    if (!games) {
        return -1;
    }

    int loaded = 0;
    if (jw_db_list_games_for_system(db_path, system, games, capacity, &loaded) != 0) {
        free(games);
        return -1;
    }

    if (loaded >= capacity) {
        int latest = 0;
        if (jw_db_count_games_for_system(db_path, system, &latest) == 0 &&
            latest > loaded) {
            if (retry_depth < 1) {
                free(games);
                return jw__load_system_games_full(db_path, system, state,
                                                  retry_depth + 1);
            }
            jw_log_warn("system game list for %s may be truncated (%d loaded, %d counted)",
                        system, loaded, latest);
        }
    }

    if (loaded <= 0) {
        free(games);
        return 1;
    }

    jw__replace_game_data(state, games, capacity, loaded);
    return 0;
}

/* Bounded string copy into a fixed field. Clipping here is intentional (the
   destinations are display/id fields sized for expected values), so this is an
   explicit truncating copy rather than a snprintf the optimizer flags. */
static void jw__str_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Strips trailing region/dump tags — " (USA)", " (E)", " [!]", etc. — from a
   ROM name for display only. The stored name (derived from the filename) is
   left intact so box-art matching and search keep working on the full name. */
static void jw__clean_rom_name(const char *raw, char *out, size_t out_size) {
    if (out_size == 0) return;
    snprintf(out, out_size, "%s", raw ? raw : "");
    int len = (int)strlen(out);
    for (;;) {
        while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) len--;
        if (len == 0) break;
        char close = out[len - 1];
        char open = (close == ')') ? '(' : (close == ']') ? '[' : '\0';
        if (open == '\0') break;              /* no trailing tag group */
        int i = len - 2;
        while (i >= 0 && out[i] != open) i--;
        if (i < 0) break;                     /* unbalanced — leave as-is */
        len = i;                              /* cut at the opening bracket */
    }
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) len--;
    if (len <= 0) { snprintf(out, out_size, "%s", raw ? raw : ""); return; }
    out[len] = '\0';
}

/* Draws a list-row name: the highlighted row scrolls its full name (looping
   marquee) while every other row ellipsizes. Only one row is highlighted at a
   time, so a single shared marquee state suffices; it resets whenever the
   highlighted text changes (cursor move or switching lists). */
static cat_marquee jw__row_marquee;
static char        jw__row_marquee_text[256];
static uint32_t    jw__row_marquee_ms;

static void jw__draw_row_name(TTF_Font *font, const char *text, int x, int y,
                              ap_color color, int max_w, bool selected) {
    if (!selected) {
        cat_draw_text_ellipsized(font, text, x, y, color, max_w);
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (strcmp(text, jw__row_marquee_text) != 0) {
        jw__row_marquee.elapsed_ms = 0;
        snprintf(jw__row_marquee_text, sizeof(jw__row_marquee_text), "%s", text);
        jw__row_marquee_ms = now;
    }
    uint32_t dt = (jw__row_marquee_ms == 0) ? 0u : (now - jw__row_marquee_ms);
    jw__row_marquee_ms = now;
    if (cat_draw_text_marquee(font, text, x, y, color, max_w, &jw__row_marquee, dt))
        cat_request_frame();
}

/* Sort the systems list alphabetically by display name (the DB returns them
   ordered by folder-code id, e.g. FC/MD/SFC, which isn't the user-facing order). */
static int jw__system_cmp_display(const void *a, const void *b) {
    const jw_system_entry *sa = (const jw_system_entry *)a;
    const jw_system_entry *sb = (const jw_system_entry *)b;
    return strcasecmp(sa->display_name, sb->display_name);
}

/* Fills each listed system's display_name from the catalog after a scan/load,
   then sorts the list alphabetically by that display name. */
static void jw__resolve_system_names(const char *db_path, jw_launcher_state *state) {
    for (int i = 0; i < state->system_count; i++) {
        jw_system_display_name(db_path, state->systems[i].name,
                               state->systems[i].display_name,
                               sizeof(state->systems[i].display_name));
    }
    if (state->system_count > 1) {
        qsort(state->systems, (size_t)state->system_count,
              sizeof(state->systems[0]), jw__system_cmp_display);
    }
}

static int jw__system_index_by_id(const jw_launcher_state *state,
                                  const char *system) {
    if (!state || !system || !system[0]) {
        return -1;
    }
    for (int i = 0; i < state->system_count; i++) {
        if (strcmp(state->systems[i].name, system) == 0) {
            return i;
        }
    }
    return -1;
}

static int jw__flat_cursor_for_system(const jw_launcher_state *state,
                                      const char *system) {
    if (!state || !system || !system[0]) {
        return -1;
    }
    for (int i = 0; i < state->flat_count; i++) {
        const jw_flat_item *it = &state->flat_items[i];
        if (it->kind == JW_FLAT_SYSTEM &&
            it->system_idx >= 0 && it->system_idx < state->system_count &&
            strcmp(state->systems[it->system_idx].name, system) == 0) {
            return i;
        }
    }
    return -1;
}

static void jw__draw_status_bar(const jw_launcher_state *state) {
    /* Coverflow hides the status bar entirely for now — chrome-light stage. */
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW)
        return;
    cat_status_bar_opts opts = {0};
    jw_settings_status_bar_opts(&state->settings, &opts);
    cat_draw_status_bar(&opts);
}

/* Draws the shared header used by the home tabs, the game browser, and search:
   the section tab bar (current section highlighted) with the status icons inline
   on the right. Returns the tab bar height so callers can place a sub-header /
   content beneath it. */
static int jw__draw_tab_header(const jw_launcher_state *state) {
    int bar_h  = cat_get_tab_bar_height();
    int pill_h = CAT_DS(CAT__PILL_SIZE);
    cat_status_bar_opts sb = {0};
    jw_settings_status_bar_opts(&state->settings, &sb);
    sb.no_pill    = true;
    sb.use_y      = true;
    sb.y_position = (bar_h - pill_h) / 2;
    cat_set_tab_bar_reserved_right(cat_get_status_bar_width(&sb) + CAT_S(12));
    /* Pick mode: the other tabs are non-navigable here, so collapse the bar to a
       single "Pick 5 Games" title (same chrome, so the header height + status bar
       stay put) instead of the confusing full four-tab bar. */
    if (state->focus_pick_active) {
        const char *pick_label = "Pick 5 Games";
        cat_draw_tab_bar(&pick_label, 1, 0);
        cat_draw_status_bar(&sb);
        return bar_h;
    }
    /* Draw only the visible tabs, in the user's display order, with the active
       highlight tracking current_tab's position within that set. */
    const char *labels[JW_TAB_COUNT];
    for (int i = 0; i < state->visible_tab_count; i++)
        labels[i] = T(kTabs[state->visible_tabs[i]]);
    cat_draw_tab_bar(labels, state->visible_tab_count,
                     jw__visible_tab_pos(state, state->current_tab));
    cat_draw_status_bar(&sb);
    return bar_h;
}

static int jw__footer_height(const jw_launcher_state *state) {
    return jw_settings_show_hints(&state->settings) ? cat_get_footer_height() : 0;
}

/* ─── Browse-page box model ──────────────────────────────────────────────────
 * The single layout used by every list-left / image-right page (Recents,
 * Favorites, Games, the in-system game browser, search). Built on Catastrophe's
 * cat_box (see Catastrophe plans/BOX_MODEL.md): carve the header off the top and
 * the hint bar off the bottom, inset the remaining content box by one base pad
 * on every side, and split it into a list column and an image column sharing one
 * gutter. The list and image come out as the SAME box height, so their gaps to
 * the chrome match by construction — on every page, at any font size.
 *
 * header_h is the page's full header height (tab bar; plus the sub-header where
 * present). All heights are queried live, so the layout tracks font size and the
 * on/off hint bar automatically. */
#define JW_BROWSE_PAD 12   /* base pad, unscaled (see padding rule in BOX_MODEL.md) */

static int jw__browse_base_item_h(void) {
    return TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + CAT_S(12);
}

static void jw__browse_boxes(const jw_launcher_state *state, int header_h,
                             int item_count, const cat_list_state *ls,
                             SDL_Rect *list, SDL_Rect *image, int *item_h) {
    int pad     = CAT_S(JW_BROWSE_PAD);
    int hints_h = jw__footer_height(state);
    /* The content box owns no bottom padding; the gap above the hint bar is the
       hint box's own top padding, so it disappears with the hints. */
    int hint_pad = (hints_h > 0) ? pad : 0;
    cat_box content = {
        0, header_h, cat_get_screen_width(),
        cat_get_screen_height() - header_h - hints_h - hint_pad,
        pad, pad, 0, pad
    };
    int list_w = cat_box_content(&content).w * 58 / 100;
    cat_box lb, ib;
    cat_box_split_cols(&content, list_w, pad, &lb, &ib);
    SDL_Rect lr = cat_box_content(&lb);
    SDL_Rect ir = cat_box_content(&ib);
    /* Fill the list box with whole rows (shared with the settings pickers), then
       snap the image pane to the same height so their bottoms land on one line. */
    int vis = ls ? ls->visible_rows : 0;
    int ih;
    lr = cat_box_fit_rows(&lb, jw__browse_base_item_h(), item_count, &vis, &ih);
    ir.h = lr.h;
    /* cat_draw_list_pane draws ls->visible_rows rows regardless of the pane
       rect, so the clamped count must land back in the list state — otherwise
       a count cached when the box was taller (hints off, smaller font) keeps
       drawing rows under the hint bar. Same idiom as the settings pickers. */
    if (ls) ((cat_list_state *)ls)->visible_rows = vis;
    /* The row pill is centered in its cell (pill_h = body + CAT_S(6); see
       jw__draw_rom_item), so the first/last pills sit inset from the cell edges
       by (ih - pill_h)/2. Inset the icon box by that same amount so its top and
       bottom line up with the pills, not the bare cell edges. (ih is the filled
       pitch in all cases - fit_rows keeps short lists on the same grid.) */
    int pad_v = (ih - (jw__browse_base_item_h() - CAT_S(6))) / 2;
    if (pad_v > 0) { ir.y += pad_v; ir.h -= pad_v * 2; }
    if (list)   *list   = lr;
    if (image)  *image  = ir;
    if (item_h) *item_h = ih;
}

/* Rows that fit the list column for the given header — used at list-init so the
 * stored visible_rows matches the renderer's box exactly. */
static int jw__browse_visible_rows(const jw_launcher_state *state, int header_h) {
    SDL_Rect list;
    jw__browse_boxes(state, header_h, 0, NULL, &list, NULL, NULL);
    int base = jw__browse_base_item_h();
    int v = (base > 0) ? list.h / base : 1;
    return v > 0 ? v : 1;
}

/* For footers drawn unconditionally (dialogs, the system overlay) -- unlike
   jw__draw_footer this does not consult the Button Hints setting, matching the
   direct call sites it replaces. Labels are translated; button pills are not. */
static void jw__footer_direct(cat_footer_item *items, int count) {
    for (int i = 0; i < count; i++) {
        if (items[i].label) items[i].label = T(items[i].label);
    }
    cat_draw_footer(items, count);
}

static void jw__draw_footer(const jw_launcher_state *state,
                            cat_footer_item *items, int count) {
    if (!jw_settings_show_hints(&state->settings)) return;
    /* Translated here rather than in all 36 footer arrays. The arrays are stack
       locals in their callers, so rewriting the label pointer is safe, and the
       button_text pills ("A", "L1/R1") are glyph labels that stay as they are. */
    for (int i = 0; i < count; i++) {
        if (items[i].label) items[i].label = T(items[i].label);
    }
    cat_draw_footer(items, count);
}

/* Per-screen footer for the settings UI (shared by the menu's Settings tab). The
   default (settings home + simple pages) is Tab + Select; B at home backs out via
   the menu, not a footer hint. */
static void jw__draw_settings_footer(const jw_launcher_state *state) {
    jw_settings_screen scr = jw_settings_ui_screen(&state->settings);
    if (scr == JW_SETTINGS_NETWORK) {
        cat_footer_item footer[] = {
            { CAT_BTN_X, "Rescan",  false, JW_HINT("X") },
            { CAT_BTN_Y, "Forget",  false, JW_HINT("Y") },
            { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
            { CAT_BTN_A, "Select",  true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (scr == JW_SETTINGS_BLUETOOTH) {
        cat_footer_item footer[] = {
            { CAT_BTN_X, "Scan",    false, JW_HINT("X") },
            { CAT_BTN_Y, "Unpair",  false, JW_HINT("Y") },
            { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
            { CAT_BTN_A, "Select",  true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (scr == JW_SETTINGS_SERVICES) {
        cat_footer_item footer[] = {
            { CAT_BTN_X, "Start with Leaf", false, JW_HINT("X") },
            { CAT_BTN_B, "Back",            true,  JW_HINT("B") },
            { CAT_BTN_A, "Run / Stop",      true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
    } else if (scr == JW_SETTINGS_UPDATE) {
        cat_footer_item footer[] = {
            { CAT_BTN_X, "Releases", false, JW_HINT("X") },
            { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
            { CAT_BTN_A, "Select",  true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
    } else if (scr == JW_SETTINGS_UPDATE_PICKER) {
        cat_footer_item footer[] = {
            { CAT_BTN_X, "Refresh", false, JW_HINT("X") },
            { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
            { CAT_BTN_A, "Pick",    true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
    } else if (scr == JW_SETTINGS_SCRAPE_QUEUE) {
        const jw_ipc_scrape_queue_info *q = state->settings.scrape_queue_have_cache
            ? state->settings.scrape_queue_cache : NULL;
        bool busy = q && (q->active > 0 || q->queued > 0);
        cat_footer_item footer[] = {
            { CAT_BTN_Y, "Filter",                          false, JW_HINT("Y") },
            { CAT_BTN_X, busy ? "Stop All" : "Clear Done",  false, JW_HINT("X") },
            { CAT_BTN_B, "Back",                            true,  JW_HINT("B") },
            { CAT_BTN_A, "Details",                         true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (scr == JW_SETTINGS_SCRAPE_QUEUE_DETAIL) {
        cat_footer_item footer[] = {
            { CAT_BTN_B, "Back", true, JW_HINT("B") },
        };
        jw__draw_footer(state, footer, 1);
    } else if (scr == JW_SETTINGS_SCRAPE_DOWNLOAD) {
        cat_footer_item footer[] = {
            { CAT_BTN_Y, state->settings.scrape_download_replace
                             ? "Missing Only" : "Replace All", false, JW_HINT("Y") },
            { CAT_BTN_B, "Back",   true, JW_HINT("B") },
            { CAT_BTN_A, "Scrape", true, JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
    } else if (scr == JW_SETTINGS_HOME_TABS) {
        bool grab = state->settings.home_tabs_grabbed;
        cat_footer_item footer[] = {
            { CAT_BTN_X, grab ? "Drop" : "Reorder",   false, JW_HINT("X") },
            { CAT_BTN_B, "Back",                       true,  JW_HINT("B") },
            { CAT_BTN_A, grab ? "Drop" : "Show/Hide",  true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    }
}

static void jw__set_launching_status(jw_launcher_state *state,
                                     const char *name,
                                     const char *fallback) {
    if (!state) {
        return;
    }

    const char *display = name && name[0] ? name : fallback;
    if (!display || !display[0]) {
        display = "item";
    }

    size_t max_name_len = 0;
    if (sizeof(state->status) > sizeof("Launching ...")) {
        max_name_len = sizeof(state->status) - sizeof("Launching ...");
    }
    if (max_name_len > (size_t)INT_MAX) {
        max_name_len = (size_t)INT_MAX;
    }

    snprintf(state->status, sizeof(state->status), "Launching %.*s...",
             (int)max_name_len, display);
}

/* ─── Flat list helpers ───────────────────────────────────────────────────── */

static void jw__build_flat_list(jw_launcher_state *state) {
    int n = 0;
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_RECENTLY_PLAYED, 0 };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_FAVORITES, 0 };
    for (int i = 0; i < state->system_count && n < JW_MAX_SYSTEMS + 4; i++)
        state->flat_items[n++] = (jw_flat_item){ JW_FLAT_SYSTEM, i };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_APPS, 0 };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_SETTINGS, 0 };
    state->flat_count = n;
}

static void jw__build_carousel_list(jw_launcher_state *state) {
    int n = 0;
    for (int i = 0; i < state->system_count && n < JW_MAX_SYSTEMS + 4; i++)
        state->flat_items[n++] = (jw_flat_item){ JW_FLAT_SYSTEM, i };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_TOOLS, 0 };
    state->flat_count = n;
}

static const char *jw__flat_label(const jw_launcher_state *state, int idx) {
    if (idx < 0 || idx >= state->flat_count) return "";
    const jw_flat_item *it = &state->flat_items[idx];
    switch (it->kind) {
        case JW_FLAT_RECENTLY_PLAYED: return "Recently Played";
        case JW_FLAT_FAVORITES:       return "Favorites";
        case JW_FLAT_SYSTEM:          return state->systems[it->system_idx].display_name;
        case JW_FLAT_APPS:            return "Apps";
        case JW_FLAT_SETTINGS:        return "Settings";
        case JW_FLAT_TOOLS:           return "Tools";
        default:                      return "";
    }
}

/* ─── Tabbed: list count ──────────────────────────────────────────────────── */

static int jw__tab_list_count(const jw_launcher_state *state) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:   return state->recents_count;
        case JW_TAB_FAVORITES: return state->favorites_count;
        case JW_TAB_GAMES:     return state->system_count;
        case JW_TAB_APPS:      return state->app_count;
        default:               return 0;
    }
}

/* ─── Library scan ────────────────────────────────────────────────────────── */

static int jw__reload_library_from_db(const char *db_path, jw_launcher_state *state) {
    if (!db_path || !state) {
        return -1;
    }

    int tab_cursor = state->list.cursor;
    int flat_cursor = state->list.cursor;
    int game_cursor = state->game_list.cursor;
    int app_cursor = state->app_list.cursor;
    char selected_system[64] = "";

    const cat_stylesheet *ss_before = cat_get_stylesheet();
    cat_launcher_layout layout_before = ss_before->launcher.layout;
    if (layout_before == CAT_LAUNCHER_TABBED) {
        if (state->current_tab == JW_TAB_GAMES &&
            state->list.cursor >= 0 &&
            state->list.cursor < state->system_count) {
            snprintf(selected_system, sizeof(selected_system), "%s",
                     state->systems[state->list.cursor].name);
        }
    } else if (state->list.cursor >= 0 && state->list.cursor < state->flat_count) {
        const jw_flat_item *it = &state->flat_items[state->list.cursor];
        if (it->kind == JW_FLAT_SYSTEM &&
            it->system_idx >= 0 && it->system_idx < state->system_count) {
            snprintf(selected_system, sizeof(selected_system), "%s",
                     state->systems[it->system_idx].name);
        }
    }

    if (jw_db_read_summary(db_path, &state->summary) != 0) {
        return -1;
    }

    jw_db_list_systems(db_path, state->systems, JW_MAX_SYSTEMS, &state->system_count);
    jw__resolve_system_names(db_path, state);
    jw__system_icon_memo_clear(state);
    jw_db_list_apps(db_path, state->apps, JW_MAX_APPS, &state->app_count);

    /* Dev-only layout filler: JAWAKA_FAKE_APPS=N appends N synthetic rows to
       the Apps tab to evaluate how a fuller list renders. Empty pak_dir marks
       them unlaunchable (guarded at the launch site). */
    const char *fake_apps_env = getenv("JAWAKA_FAKE_APPS");
    if (fake_apps_env && fake_apps_env[0]) {
        int want = atoi(fake_apps_env);
        for (int i = 0; i < want && state->app_count < JW_MAX_APPS; i++) {
            jw_app_entry *fake = &state->apps[state->app_count++];
            memset(fake, 0, sizeof(*fake));
            snprintf(fake->name, sizeof(fake->name), "Sample App %02d", i + 1);
        }
    }

    if (jw_db_list_recent_games(db_path, state->recents, JW_MAX_RECENTS,
                                &state->recents_count) != 0) {
        state->recents_count = 0;
    }
    if (jw_db_list_favorite_games(db_path, state->favorites, JW_MAX_FAVORITES,
                                  &state->favorites_count) != 0) {
        state->favorites_count = 0;
    }
    if (state->pakrat_open) {
        jw__load_pakrat_store(state);
    }

    if (state->games_open) {
        int rc = -1;
        char open_system[64];
        snprintf(open_system, sizeof(open_system), "%s", state->game_system);
        if (state->games_are_favorites) {
            rc = jw__load_bounded_game_browser(db_path, state,
                                               jw_db_list_favorite_games,
                                               JW_OPENED_GAME_BROWSER_LIMIT);
        } else if (strcmp(open_system, "Recently Played") == 0) {
            rc = jw__load_bounded_game_browser(db_path, state,
                                               jw_db_list_recent_games,
                                               JW_OPENED_GAME_BROWSER_LIMIT);
        } else if (open_system[0]) {
            rc = jw__load_system_games_full(db_path, open_system, state, 0);
        }
        if (rc == 0 && state->game_count > 0) {
            if (game_cursor >= state->game_count) {
                game_cursor = state->game_count - 1;
            }
            cat_list_state_jump(&state->game_list, game_cursor, state->game_count);
        } else if (rc == 1 || (rc == 0 && state->game_count <= 0)) {
            jw__close_game_browser(state);
        }
    }

    if (state->apps_open) {
        if (state->app_count <= 0) {
            state->apps_open = false;
        } else {
            if (app_cursor >= state->app_count) {
                app_cursor = state->app_count - 1;
            }
            cat_list_state_jump(&state->app_list, app_cursor, state->app_count);
        }
    }

    state->search_open = false;
    state->search_count = 0;

    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;
    if (layout == CAT_LAUNCHER_HORIZONTAL) {
        jw__build_carousel_list(state);
        int count = state->flat_count;
        if (selected_system[0]) {
            int selected_cursor = jw__flat_cursor_for_system(state, selected_system);
            if (selected_cursor >= 0) {
                flat_cursor = selected_cursor;
            }
        }
        if (flat_cursor >= count) {
            flat_cursor = count > 0 ? count - 1 : 0;
        }
        cat_list_state_jump(&state->list, flat_cursor, count);
    } else if (layout == CAT_LAUNCHER_VERTICAL) {
        jw__build_flat_list(state);
        int count = state->flat_count;
        if (selected_system[0]) {
            int selected_cursor = jw__flat_cursor_for_system(state, selected_system);
            if (selected_cursor >= 0) {
                flat_cursor = selected_cursor;
            }
        }
        if (flat_cursor >= count) {
            flat_cursor = count > 0 ? count - 1 : 0;
        }
        cat_list_state_jump(&state->list, flat_cursor, count);
    } else {
        int count = jw__tab_list_count(state);
        if (selected_system[0] && state->current_tab == JW_TAB_GAMES) {
            int selected_cursor = jw__system_index_by_id(state, selected_system);
            if (selected_cursor >= 0) {
                tab_cursor = selected_cursor;
            }
        }
        if (tab_cursor >= count) {
            tab_cursor = count > 0 ? count - 1 : 0;
        }
        cat_list_state_jump(&state->list, tab_cursor, count);
    }

    state->scan_ready = true;
    return 0;
}

static int jw__scan_library(const char *socket_path, const char *db_path,
                             jw_launcher_state *state) {
    (void)db_path;
    int rc = jw_ipc_scan_library(socket_path, state->status, sizeof(state->status));
    if (rc != 0) return -1;

    jw_ipc_library_status_info lib;
    if (jw_ipc_library_status_full(socket_path, &lib) == 0) {
        state->library_generation = lib.generation;
        state->scan_running = lib.scan_running;
        state->library_populated = lib.library_populated;
        state->scan_ready = lib.library_populated || !lib.scan_running;
    } else {
        state->library_generation = -1;
    }
    if (state->scan_running) {
        snprintf(state->status, sizeof(state->status), "%s", "Library updating...");
    }
    return 0;
}

static int jw__load_library_cache(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state) {
    if (jw__reload_library_from_db(db_path, state) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "library cache unavailable");
        return -1;
    }

    jw_ipc_library_status_info lib;
    if (jw_ipc_library_status_full(socket_path, &lib) == 0) {
        state->library_generation = lib.generation;
        state->scan_running = lib.scan_running;
        state->library_populated = lib.library_populated;
        state->scan_ready = lib.library_populated || !lib.scan_running;
    } else {
        state->library_generation = -1;
    }
    snprintf(state->status, sizeof(state->status), T("%d games, %d systems, %d apps"),
        state->summary.game_count, state->system_count, state->summary.app_count);
    return 0;
}

/* ── Background status poller ───────────────────────────────────────────────
   The status-bar volume/Wi-Fi/Bluetooth reads and the library-generation check
   all hit the daemon over IPC (the daemon forks pactl for volume) or shell out.
   Done on the render thread they stalled a frame ~once a second — visible as a
   hitch in any continuous animation (e.g. the long-name marquee).

   Threading contract: the worker never touches jw_settings_ui (the render
   thread owns it and also writes the same cached fields from the settings
   pages). All cross-thread traffic goes through the C11 atomics below — the
   render thread publishes which values to poll (poll_mask) and consumes the
   samples into the settings/status-bar caches once per loop
   (jw__status_poller_sync); the worker only reads the mask and stores samples.
   JW_STATUS_SAMPLE_NONE marks an empty mailbox slot. */
#define JW_STATUS_POLL_VOLUME (1 << 0)
#define JW_STATUS_POLL_WIFI   (1 << 1)
#define JW_STATUS_POLL_BT     (1 << 2)
#define JW_STATUS_POLL_AV     (1 << 3)   /* brightness + full audio status */
#define JW_STATUS_SAMPLE_NONE INT_MIN

typedef struct {
    pthread_t   thread;
    bool        started;
    atomic_bool stop;
    char        socket_path[PATH_MAX];
    atomic_int  poll_mask;    /* JW_STATUS_POLL_* bits the render thread wants */
    atomic_int  generation;   /* latest library generation (-1 = unknown yet) */
    atomic_int  scan_running;
    atomic_int  pending_rescan;
    atomic_int  library_populated;
    atomic_int  volume;       /* sample mailboxes, JW_STATUS_SAMPLE_NONE = empty */
    atomic_int  wifi;         /* 0..3 strength, -1 = wifi unavailable */
    atomic_int  bt;           /* 0=off, 1=on, 2=connected */
    atomic_int  battery;      /* 0..100, -1 = unknown */
    atomic_int  charging;     /* 0/1, -1 = unknown */
    uint32_t    fb_last_fast; /* render-thread fallback throttles (worker never started) */
    uint32_t    fb_last_slow;
    /* Scrape progress snapshot, refreshed at the fast cadence. Strings need
       more than an atomic: the worker copies under scrape_mu and bumps
       scrape_seq; the render thread re-copies when the seq moved. */
    pthread_mutex_t scrape_mu;
    jw_ipc_scrape_status_info scrape;
    atomic_int  scrape_seq;
    /* Display & Sound snapshot, sampled only while that page is open. Same
       mutex+seq shape as scrape above, because audio status is a struct and one
       atomic cannot carry it. Brightness rides along as a plain mailbox. */
    atomic_int  av_brightness;
    pthread_mutex_t av_mu;
    jw_ipc_audio_status av_audio;
    atomic_int  av_seq;
} jw_status_poller;

static jw_status_poller jw__status_poller;

static void *jw__status_poll_worker(void *arg) {
    jw_status_poller *P = (jw_status_poller *)arg;
    uint32_t last_slow = 0;
    while (!atomic_load(&P->stop)) {
        int mask = atomic_load(&P->poll_mask);
        /* ~1s cadence: volume + library generation (both IPC to the daemon). */
        if (mask & JW_STATUS_POLL_VOLUME) {
            int percent = -1;
            if (jw_ipc_platform_volume(P->socket_path, &percent) == 0 && percent >= 0)
                atomic_store(&P->volume, percent > 100 ? 100 : percent);
        }
        /* Display & Sound: brightness + audio status. Two blocking round trips
           that used to run on the render thread and stall cursor movement on
           that page; out here they cost nothing anyone can see. */
        if (mask & JW_STATUS_POLL_AV) {
            int percent = -1;
            if (jw_ipc_platform_brightness(P->socket_path, &percent) == 0 && percent >= 0)
                atomic_store(&P->av_brightness, percent);
            jw_ipc_audio_status audio;
            if (jw_ipc_platform_audio_status(P->socket_path, &audio) == 0) {
                pthread_mutex_lock(&P->av_mu);
                P->av_audio = audio;
                pthread_mutex_unlock(&P->av_mu);
                atomic_fetch_add(&P->av_seq, 1);
            }
        }
        jw_ipc_library_status_info lib;
        if (jw_ipc_library_status_full(P->socket_path, &lib) == 0) {
            atomic_store(&P->generation, lib.generation);
            atomic_store(&P->scan_running, lib.scan_running ? 1 : 0);
            atomic_store(&P->pending_rescan, lib.pending_rescan ? 1 : 0);
            atomic_store(&P->library_populated, lib.library_populated ? 1 : 0);
        }
        /* Scrape progress (same fast cadence; art pops in live while the
           daemon worker downloads, so the status line should track it). */
        {
            jw_ipc_scrape_status_info scrape;
            if (jw_ipc_scrape_status(P->socket_path, &scrape) == 0) {
                pthread_mutex_lock(&P->scrape_mu);
                P->scrape = scrape;
                pthread_mutex_unlock(&P->scrape_mu);
                atomic_fetch_add(&P->scrape_seq, 1);
            }
        }
        /* ~5s cadence: Wi-Fi strength, Bluetooth state, and battery/charging.
           These shell out (wpa_cli/bluetoothctl) or IPC, so the render thread
           never spawns or blocks for status. */
        uint32_t now = SDL_GetTicks();
        if (last_slow == 0 || now - last_slow >= 5000) {
            if (mask & JW_STATUS_POLL_WIFI)
                atomic_store(&P->wifi,
                             jw_wifi_available() ? jw_wifi_strength_now() : -1);
            if (mask & JW_STATUS_POLL_BT)
                atomic_store(&P->bt, jw_settings_bt_state_now());
            int batt = -1, chg = -1;
            if (jw_ipc_platform_power_status(P->socket_path, &batt, &chg) == 0) {
                atomic_store(&P->battery, batt);
                atomic_store(&P->charging, chg);
            }
            last_slow = now;
        }
        /* Sleep ~1s, waking promptly for shutdown. While Display & Sound is open
           tighten to ~300ms so its sliders keep up with the hardware keys as
           closely as the old render-thread poll did — the cost is confined to
           this thread, and only while that page is showing. */
        int slices = (mask & JW_STATUS_POLL_AV) ? 3 : 10;
        for (int i = 0; i < slices && !atomic_load(&P->stop); i++) SDL_Delay(100);
    }
    return NULL;
}

/* Fallback when the worker thread could not be created: poll inline on the
   render thread with the worker's cadences — the pre-poller behavior, periodic
   frame hitch included, but status icons and library generation stay live. */
static void jw__status_poller_fallback_poll(jw_settings_ui *s, int mask) {
    jw_status_poller *P = &jw__status_poller;
    uint32_t now = SDL_GetTicks();
    if (P->fb_last_fast == 0 || now - P->fb_last_fast >= 1000) {
        if (mask & JW_STATUS_POLL_VOLUME)
            jw_settings_ui_refresh_volume(s);
        /* Only reached when the worker thread failed to start, so this is the
           degraded path: the page stalls as it did before, but at the 1s
           fallback cadence rather than 300ms, and it beats not updating. */
        if (mask & JW_STATUS_POLL_AV)
            jw_settings_ui_refresh_av(s);
        jw_ipc_library_status_info lib;
        if (jw_ipc_library_status_full(P->socket_path, &lib) == 0) {
            atomic_store(&P->generation, lib.generation);
            atomic_store(&P->scan_running, lib.scan_running ? 1 : 0);
            atomic_store(&P->pending_rescan, lib.pending_rescan ? 1 : 0);
            atomic_store(&P->library_populated, lib.library_populated ? 1 : 0);
        }
        P->fb_last_fast = now;
    }
    if (P->fb_last_slow == 0 || now - P->fb_last_slow >= 5000) {
        if (mask & JW_STATUS_POLL_WIFI) {
            jw_settings_ui_refresh_wifi_strength(s);
            jw_cat_services_set_wifi_strength(
                jw_wifi_available() ? s->wifi_strength_cached : -1);
        }
        if (mask & JW_STATUS_POLL_BT)
            jw_settings_ui_refresh_bt_state(s);
        int batt = -1, chg = -1;
        if (jw_ipc_platform_power_status(P->socket_path, &batt, &chg) == 0)
            jw_cat_services_set_power(batt, chg);
        P->fb_last_slow = now;
    }
}

/* Which values the worker should sample: each icon's poll is wanted while the
   icon is shown, except while the matching settings page (A/V, Network,
   Bluetooth) already polls it live on the render thread. */
static int jw__status_poll_mask(const jw_settings_ui *s) {
    int mask = 0;
    /* The A/V sample carries volume too, so the two never both run. */
    if (jw_settings_ui_wants_av_poll(s))
        mask |= JW_STATUS_POLL_AV;
    else if (jw_settings_show_volume(s))
        mask |= JW_STATUS_POLL_VOLUME;
    if (jw_settings_show_wifi(s) && !jw_settings_ui_wants_wifi_poll(s))
        mask |= JW_STATUS_POLL_WIFI;
    if (s->show_bluetooth && !jw_settings_ui_wants_bluetooth_poll(s))
        mask |= JW_STATUS_POLL_BT;
    return mask;
}

/* Render-thread half of the poller, called once per loop iteration: publish
   what the worker should poll (reading jw_settings_ui is render-thread-only)
   and fold any delivered samples into the settings/status-bar caches. A page
   that polls its own values live (A/V, Network, Bluetooth) masks the matching
   poll off; a sample already in flight when a page opens is dropped here, and
   at worst the page's own refresh overwrites it a frame later. */
static void jw__status_poller_sync(jw_settings_ui *s) {
    jw_status_poller *P = &jw__status_poller;
    int mask = jw__status_poll_mask(s);

    if (!P->started) {
        jw__status_poller_fallback_poll(s, mask);
        return;
    }
    atomic_store(&P->poll_mask, mask);

    int v = atomic_exchange(&P->volume, JW_STATUS_SAMPLE_NONE);
    if (v != JW_STATUS_SAMPLE_NONE && (mask & JW_STATUS_POLL_VOLUME))
        s->volume_percent = v;
    /* Display & Sound snapshot. The audio half needs the lock (it is a struct);
       re-copy only when the worker bumped the seq, so a page sitting idle costs
       one atomic load per frame. */
    if (mask & JW_STATUS_POLL_AV) {
        static int av_seen = 0;
        int brightness = atomic_exchange(&P->av_brightness, JW_STATUS_SAMPLE_NONE);
        int seq = atomic_load(&P->av_seq);
        jw_ipc_audio_status audio;
        bool have_audio = false;
        if (seq != av_seen) {
            av_seen = seq;
            pthread_mutex_lock(&P->av_mu);
            audio = P->av_audio;
            pthread_mutex_unlock(&P->av_mu);
            have_audio = true;
        }
        if (brightness != JW_STATUS_SAMPLE_NONE || have_audio) {
            jw_settings_ui_apply_av(s,
                                    brightness == JW_STATUS_SAMPLE_NONE ? -1 : brightness,
                                    have_audio ? &audio : NULL);
        }
    }
    v = atomic_exchange(&P->wifi, JW_STATUS_SAMPLE_NONE);
    if (v != JW_STATUS_SAMPLE_NONE && (mask & JW_STATUS_POLL_WIFI)) {
        s->wifi_strength_cached = v > 0 ? v : 0;
        jw_cat_services_set_wifi_strength(v);
    }
    v = atomic_exchange(&P->bt, JW_STATUS_SAMPLE_NONE);
    if (v != JW_STATUS_SAMPLE_NONE && (mask & JW_STATUS_POLL_BT))
        s->bt_state_cached = v;
    int batt = atomic_exchange(&P->battery, JW_STATUS_SAMPLE_NONE);
    int chg  = atomic_exchange(&P->charging, JW_STATUS_SAMPLE_NONE);
    if (batt != JW_STATUS_SAMPLE_NONE || chg != JW_STATUS_SAMPLE_NONE)
        jw_cat_services_set_power(batt == JW_STATUS_SAMPLE_NONE ? -1 : batt,
                                  chg == JW_STATUS_SAMPLE_NONE ? -1 : chg);
}

static void jw__status_poller_start(const char *socket_path, jw_settings_ui *settings) {
    jw_status_poller *P = &jw__status_poller;
    if (P->started) return;
    snprintf(P->socket_path, sizeof(P->socket_path), "%s", socket_path ? socket_path : "");
    atomic_store(&P->generation, -1);
    atomic_store(&P->scan_running, 0);
    atomic_store(&P->pending_rescan, 0);
    atomic_store(&P->library_populated, 0);
    atomic_store(&P->volume, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->wifi, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->bt, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->battery, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->charging, JW_STATUS_SAMPLE_NONE);
    /* Seed the mask before the worker's first pass so startup doesn't skip a
       round of samples while waiting for the first sync. */
    atomic_store(&P->poll_mask, jw__status_poll_mask(settings));
    pthread_mutex_init(&P->scrape_mu, NULL);
    pthread_mutex_init(&P->av_mu, NULL);
    atomic_store(&P->av_brightness, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->av_seq, 0);
    atomic_store(&P->scrape_seq, 0);
    atomic_store(&P->stop, false);
    P->started = true;
    if (pthread_create(&P->thread, NULL, jw__status_poll_worker, P) != 0)
        P->started = false;   /* no thread -> jw__status_poller_sync polls inline */
}

static void jw__status_poller_shutdown(void) {
    jw_status_poller *P = &jw__status_poller;
    if (!P->started) return;
    atomic_store(&P->stop, true);
    pthread_join(P->thread, NULL);
    P->started = false;
}

static void jw__poll_library_generation(const char *socket_path,
                                        const char *db_path,
                                        jw_launcher_state *state) {
    (void)socket_path;   /* the library-status IPC now runs on the status poller */
    if (!db_path || !state) {
        return;
    }

    bool scan_running = atomic_load(&jw__status_poller.scan_running) != 0;
    bool pending_rescan = atomic_load(&jw__status_poller.pending_rescan) != 0;
    bool library_populated = atomic_load(&jw__status_poller.library_populated) != 0;
    if (state->scan_running != scan_running ||
        state->library_populated != library_populated) {
        state->scan_running = scan_running;
        state->library_populated = library_populated;
        state->scan_ready = library_populated || !scan_running;
        state->menu_scanning = scan_running;
        if (scan_running) {
            snprintf(state->status, sizeof(state->status), "%s",
                     pending_rescan ? "Library updating; another scan is queued"
                                    : "Library updating...");
        }
        cat_request_frame();
    }

    int generation = atomic_load(&jw__status_poller.generation);
    if (generation < 0) {
        return;   /* poller hasn't reported a generation yet */
    }
    if (state->library_generation < 0) {
        /* The startup status query failed, so the cached library may predate a
           scan that completed in the meantime — reload before adopting. */
        if (jw__reload_library_from_db(db_path, state) == 0) {
            state->library_generation = generation;
            cat_request_frame();
        }
        return;
    }
    if (generation == state->library_generation) {
        return;
    }

    if (jw__reload_library_from_db(db_path, state) == 0) {
        state->library_generation = generation;
        /* Drop cached cover textures: a generation bump can mean art was
           *replaced* at an unchanged path (re-scrape), and the texture cache
           is path-keyed. The on-disk thumbnail is mtime-checked on reload, so
           covers rebuild from fresh sources; untouched covers re-decode their
           small thumbnails (~15ms each) at worst. */
        cat_cache_clear();
        snprintf(state->status, sizeof(state->status), T("%d games, %d systems, %d apps"),
                 state->summary.game_count, state->system_count, state->summary.app_count);
        cat_request_frame();
    }
}

/* Surface scrape progress in the status line: live counts while the daemon
   worker runs and a one-shot summary when the batch finishes. Reads the
   poller's snapshot — no IPC on the render thread. */
static void jw__poll_scrape_status(jw_launcher_state *state) {
    jw_status_poller *P = &jw__status_poller;
    if (!P->started) {
        return;   /* rare fallback path: no scrape progress line */
    }

    static int  last_seq = 0;
    static bool was_active = false;
    int seq = atomic_load(&P->scrape_seq);
    if (seq == last_seq) {
        return;
    }
    last_seq = seq;

    jw_ipc_scrape_status_info s;
    pthread_mutex_lock(&P->scrape_mu);
    s = P->scrape;
    pthread_mutex_unlock(&P->scrape_mu);

    bool active = strcmp(s.state, "running") == 0 ||
                  strcmp(s.state, "paused-quota") == 0;
    if (active) {
        char item[224] = "";
        if (s.current_name[0]) {
            snprintf(item, sizeof(item), "%.*s",
                     (int)sizeof(item) - 1, s.current_name);
            char *dot = strrchr(item, '.');
            if (dot && dot != item) *dot = '\0';
        }
        if (strcmp(s.state, "paused-quota") == 0) {
            snprintf(state->status, sizeof(state->status),
                     "Scraping paused: %.200s",
                     s.message[0] ? s.message : "daily quota exceeded");
        } else if (item[0]) {
            int shown = s.done + 1 > s.total ? s.total : s.done + 1;
            snprintf(state->status, sizeof(state->status),
                     "Scraping %s: %d/%d - %.160s",
                     s.current_system, shown, s.total, item);
        } else {
            snprintf(state->status, sizeof(state->status),
                     "Scraping: %d/%d", s.done, s.total);
        }
        cat_request_frame();
        was_active = true;
    } else if (was_active) {
        was_active = false;
        if (s.total > 0) {
            size_t cap = sizeof(state->status);
            int n = snprintf(state->status, cap,
                             "Scrape finished: %d found", s.found);
            if (s.not_found > 0 && n > 0 && (size_t)n < cap) {
                n += snprintf(state->status + n, cap - (size_t)n,
                              ", %d not found", s.not_found);
            }
            if (s.failed > 0 && n > 0 && (size_t)n < cap) {
                n += snprintf(state->status + n, cap - (size_t)n,
                              ", %d failed", s.failed);
            }
            if (s.cancelled > 0 && n > 0 && (size_t)n < cap) {
                snprintf(state->status + n, cap - (size_t)n,
                         ", %d cancelled", s.cancelled);
            }
            cat_request_frame();
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TABBED RENDER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void jw__load_favorites_tab(const char *db_path, jw_launcher_state *state) {
    if (!db_path || jw_db_list_favorite_games(db_path, state->favorites,
                                              JW_MAX_FAVORITES,
                                              &state->favorites_count) != 0) {
        state->favorites_count = 0;
    }
}

static void jw__load_recents_tab(const char *db_path, jw_launcher_state *state) {
    if (!db_path || jw_db_list_recent_games(db_path, state->recents,
                                            JW_MAX_RECENTS,
                                            &state->recents_count) != 0) {
        state->recents_count = 0;
    }
}

static int jw__pakrat_context_from_state(const jw_launcher_state *state,
                                         jw_pakrat_context *ctx) {
    if (!state || !ctx || !state->sdcard_root[0] || !state->state_dir[0] ||
        !state->db_path[0] || !state->platform_root[0]) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->platform, sizeof(ctx->platform), "%s", jw_platform_compiled_id());
    snprintf(ctx->sdcard_root, sizeof(ctx->sdcard_root), "%s", state->sdcard_root);
    snprintf(ctx->state_dir, sizeof(ctx->state_dir), "%s", state->state_dir);
    snprintf(ctx->db_path, sizeof(ctx->db_path), "%s", state->db_path);
    snprintf(ctx->platform_root, sizeof(ctx->platform_root), "%s", state->platform_root);
    snprintf(ctx->socket_path, sizeof(ctx->socket_path), "%s", state->socket_path);
    char *runtime_dir = jw_runtime_dir();
    if (!runtime_dir ||
        snprintf(ctx->runtime_dir, sizeof(ctx->runtime_dir), "%s",
                 runtime_dir) >= (int)sizeof(ctx->runtime_dir)) {
        free(runtime_dir);
        return -1;
    }
    free(runtime_dir);
    return 0;
}

static void jw__load_pakrat_store(jw_launcher_state *state) {
    if (!state) {
        return;
    }
    state->pakrat_app_count = 0;
    state->pakrat_message[0] = '\0';

    jw_pakrat_context ctx;
    if (jw__pakrat_context_from_state(state, &ctx) != 0) {
        state->pakrat_load_rc = -1;
        snprintf(state->pakrat_message, sizeof(state->pakrat_message),
                 "%s", "Pak Rat runtime paths unavailable");
        return;
    }

    int count = 0;
    int rc = jw_pakrat_list_app_states(&ctx, state->pakrat_apps,
                                       JW_MAX_PAKRAT_APPS, &count);
    state->pakrat_load_rc = rc;
    if (rc == 0) {
        state->pakrat_app_count = count;
        snprintf(state->pakrat_message, sizeof(state->pakrat_message),
                 "%d Pak Rat app%s", count, count == 1 ? "" : "s");
    } else if (rc > 0) {
        snprintf(state->pakrat_message, sizeof(state->pakrat_message),
                 "%s", "Pak Rat catalog not configured");
    } else if (rc == JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF) {
        snprintf(state->pakrat_message, sizeof(state->pakrat_message),
                 "%s", "Pak Rat catalog requires a newer Leaf");
    } else {
        snprintf(state->pakrat_message, sizeof(state->pakrat_message),
                 "%s", "Pak Rat catalog unavailable");
    }
}

static void jw__switch_tab(jw_launcher_state *state, int direction, const char *db_path) {
    if (!state) return;
    /* The tab row is a cursor Catastrophe does not own, so it reports its own
       movement. Every caller is a button press (L1/R1, and the coverflow channel
       cube), and the row wraps, so the only way this is not a move is a single
       visible tab. */
    jw_tab from_tab = state->current_tab;
    /* Remember the cursor in the channel we're leaving so a later return lands
       where we were, not back at the start. */
    if (state->current_tab >= 0 && state->current_tab < JW_TAB_COUNT)
        state->tab_cursor[state->current_tab] = state->list.cursor;
    /* Cycle by POSITION within the visible set (tabs can be hidden/reordered),
       adding visible_tab_count before the modulo so dir = -1 wraps to the last
       visible tab rather than an out-of-range index. */
    int n = state->visible_tab_count > 0 ? state->visible_tab_count : 1;
    int pos = jw__visible_tab_pos(state, state->current_tab);
    int next_pos = (pos + direction + n) % n;
    state->current_tab = state->visible_tabs[next_pos];
    /* Favorites/Recents are reloaded on entry so newly toggled/played items appear. */
    if (state->current_tab == JW_TAB_FAVORITES)
        jw__load_favorites_tab(db_path, state);
    else if (state->current_tab == JW_TAB_RECENTS)
        jw__load_recents_tab(db_path, state);
    /* Restore the saved cursor for the channel we're entering (clamped — the list
       may have shrunk since we were last here, e.g. Recents/Favorites reloads). */
    int count   = jw__tab_list_count(state);
    int restore = (state->current_tab >= 0 && state->current_tab < JW_TAB_COUNT)
                      ? state->tab_cursor[state->current_tab] : 0;
    if (restore < 0) restore = 0;
    if (restore > count - 1) restore = count > 0 ? count - 1 : 0;
    cat_list_state_jump(&state->list, restore, count);   /* silent: a restore, not a press */

    cat_ui_feedback_emit(state->current_tab != from_tab ? CAT_UI_MOVED : CAT_UI_EDGE);
}

typedef struct { const jw_system_entry *systems; } jw__games_ctx;
typedef struct { const jw_app_entry   *apps;    } jw__apps_ctx;
typedef struct { const jw_pakrat_app_state *apps; } jw__pakrat_ctx;
typedef struct { const jw_game_entry  *games; const jw_launcher_state *st; } jw__roms_ctx;
typedef struct { const jw_search_result *results; } jw__search_ctx;

/* Is a games.id currently in the 5-Game Mode pick set? (defined with the wizard) */
static bool jw__fsetup_is_picked(const jw_launcher_state *st, int id);

/* Filled thick line segment (for the pick checkmark). */
static void jw__thick_line(float x0, float y0, float x1, float y1, float t,
                           SDL_Color c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * (t * 0.5f), ny = dx / len * (t * 0.5f);
    SDL_Vertex v[4];
    SDL_FPoint p[4] = { { x0 + nx, y0 + ny }, { x1 + nx, y1 + ny },
                        { x1 - nx, y1 - ny }, { x0 - nx, y0 - ny } };
    for (int i = 0; i < 4; i++) {
        v[i].position = p[i]; v[i].color = c;
        v[i].tex_coord.x = 0; v[i].tex_coord.y = 0;
    }
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(cat_get_renderer(), NULL, v, 4, idx, 6);
}

/* A checkmark centered at (cx,cy) fitting radius r. */
static void jw__draw_check(int cx, int cy, int r, SDL_Color c) {
    float t = r * 0.44f;
    jw__thick_line(cx - r * 0.60f, cy + r * 0.04f, cx - r * 0.12f, cy + r * 0.52f, t, c);
    jw__thick_line(cx - r * 0.12f, cy + r * 0.52f, cx + r * 0.62f, cy - r * 0.52f, t, c);
}

static void jw__draw_game_item(int idx, int ix, int iy, int iw, int ih,
                                bool selected, void *user) {
    jw__games_ctx *ctx = (jw__games_ctx *)user;
    ap_theme *theme    = cat_get_theme();
    TTF_Font *body     = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c  = selected ? theme->highlighted_text : theme->text;
    int name_max = iw - CAT_S(20);
    int text_y   = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    jw__draw_row_name(body, ctx->systems[idx].display_name,
        ix + CAT_S(10), text_y, name_c, name_max, selected);
}

static void jw__draw_app_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    jw__apps_ctx *ctx = (jw__apps_ctx *)user;
    ap_theme *theme   = cat_get_theme();
    TTF_Font *body    = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    jw__draw_row_name(body, ctx->apps[idx].name,
        ix + CAT_S(10), text_y, name_c, iw - CAT_S(20), selected);
}

static const char *jw__pakrat_status_label(jw_pakrat_app_status status) {
    switch (status) {
        case JW_PAKRAT_APP_AVAILABLE:        return "Available";
        case JW_PAKRAT_APP_INSTALLED:        return "Installed";
        case JW_PAKRAT_APP_UPDATE_AVAILABLE: return "Update";
        case JW_PAKRAT_APP_STALE:            return "Stale";
        case JW_PAKRAT_APP_UNMANAGED:        return "Manual";
        default:                             return "";
    }
}

static const char *jw__pakrat_primary_action_label(const jw_pakrat_app_state *app) {
    if (!app) {
        return "Select";
    }
    if (app->managed) {
        return "Blocked";
    }
    if (!app->primary_action_allowed) {
        return "Unavailable";
    }
    switch (app->status) {
        case JW_PAKRAT_APP_AVAILABLE:        return "Install";
        case JW_PAKRAT_APP_INSTALLED:        return "Reinstall";
        case JW_PAKRAT_APP_UPDATE_AVAILABLE: return "Update";
        case JW_PAKRAT_APP_STALE:            return "Restore";
        case JW_PAKRAT_APP_UNMANAGED:        return "Install";
        default:                             return "Select";
    }
}

static bool jw__pakrat_can_uninstall(const jw_pakrat_app_state *app) {
    return app && !app->managed && app->installed_owned;
}

static void jw__draw_pakrat_item(int idx, int ix, int iy, int iw, int ih,
                                 bool selected, void *user) {
    jw__pakrat_ctx *ctx = (jw__pakrat_ctx *)user;
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body      = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);

    const jw_pakrat_app_state *app = &ctx->apps[idx];
    const char *status = jw__pakrat_status_label(app->status);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    ap_color meta_c = selected ? theme->highlighted_text : theme->hint;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    int small_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
    int status_w = cat_measure_text(small, status);
    int name_max = iw - status_w - CAT_S(36);
    if (name_max < CAT_S(96)) {
        name_max = iw - CAT_S(20);
        status_w = 0;
    }

    jw__draw_row_name(body, app->package.name,
        ix + CAT_S(10), text_y, name_c, name_max, selected);
    if (status_w > 0) {
        cat_draw_text(small, status, ix + iw - status_w - CAT_S(14),
                      small_y, meta_c);
    }
}

static void jw__draw_rom_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    jw__roms_ctx *ctx = (jw__roms_ctx *)user;
    ap_theme *theme   = cat_get_theme();
    TTF_Font *body    = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    char name[256];
    jw__clean_rom_name(ctx->games[idx].name, name, sizeof(name));
    int name_max = iw - CAT_S(20);

    int name_x = ix + CAT_S(10);
    if (ctx->st && ctx->st->focus_pick_active) {
        /* 5-Game Mode Pick: a clear checkmark on chosen games (favorites are
           irrelevant here). */
        if (jw__fsetup_is_picked(ctx->st, ctx->games[idx].id)) {
            ap_color chk_c = selected ? theme->highlighted_text : theme->highlight;
            int body_h = TTF_FontHeight(body);
            int r = body_h * 36 / 100;
            jw__draw_check(name_x + r, text_y + body_h / 2, r, chk_c);
            int advance = r * 2 + CAT_S(8);
            name_x += advance;
            name_max -= advance;
        }
    } else if (ctx->games[idx].favorite) {
        /* Drawn star (not a font glyph — the body font lacks U+2605). Use the
           bright selection-pill color on a normal row (the accent/chrome tone is
           too dark to read on the row bg), and highlighted_text on a selected
           row so it stays legible against the pill. */
        ap_color star_c = selected ? theme->highlighted_text : theme->highlight;
        int body_h = TTF_FontHeight(body);
        int star_r = body_h * 32 / 100;
        cat_draw_star(name_x + star_r, text_y + body_h / 2, star_r, star_c);
        int advance = star_r * 2 + CAT_S(6);
        name_x += advance;
        name_max -= advance;
    }

    jw__draw_row_name(body, name, name_x, text_y, name_c, name_max, selected);
}

static void jw__draw_search_item(int idx, int ix, int iy, int iw, int ih,
                                  bool selected, void *user) {
    jw__search_ctx *ctx = (jw__search_ctx *)user;
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body      = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);

    const jw_search_result *result = &ctx->results[idx];
    const char *kind = result->kind == JW_SEARCH_APP ? "App" : "Game";
    const char *meta = result->kind == JW_SEARCH_APP ? result->pak_dir : result->system;

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    ap_color meta_c = selected ? theme->highlighted_text : theme->hint;

    int kind_w = cat_measure_text(small, kind);
    int meta_w = iw / 4;
    int name_max = iw - kind_w - meta_w - CAT_S(42);
    if (name_max < CAT_S(96)) {
        meta_w = 0;
        name_max = iw - kind_w - CAT_S(32);
    }

    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    int name_x = ix + CAT_S(10);
    char name[256];
    if (result->kind == JW_SEARCH_APP)
        snprintf(name, sizeof(name), "%s", result->name);
    else
        jw__clean_rom_name(result->name, name, sizeof(name));
    if (result->favorite) {
        ap_color star_c = selected ? theme->highlighted_text : theme->highlight;
        int body_h = TTF_FontHeight(body);
        int star_r = body_h * 32 / 100;
        cat_draw_star(name_x + star_r, text_y + body_h / 2, star_r, star_c);
        int advance = star_r * 2 + CAT_S(6);
        name_x += advance;
        name_max -= advance;
    }
    jw__draw_row_name(body, name, name_x, text_y, name_c, name_max, selected);

    int small_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
    if (meta_w > 0 && meta && meta[0]) {
        cat_draw_text_ellipsized(small, meta,
            ix + iw - kind_w - meta_w - CAT_S(24), small_y,
            meta_c, meta_w);
    }
    cat_draw_text(small, kind, ix + iw - kind_w - CAT_S(10), small_y, meta_c);
}

static void jw__render_recents(const jw_launcher_state *state,
                                int content_y, int content_h, int margin) {
    jw__render_game_list_pane(state, state->recents, state->recents_count,
                              content_y, content_h, margin,
                              "No recent games yet — play something and it'll show up here");
}

static void jw__render_games(const jw_launcher_state *state,
                              int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    (void)content_h;

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, content_y, state->system_count,
                     &state->list, &list, &image, &item_h);

    if (state->system_count > 0 && state->list.cursor < state->system_count) {
        const jw_system_entry *sys = &state->systems[state->list.cursor];
        jw__draw_system_preview(image.x, image.y, image.w, image.h,
                                sys->name, sys->game_count);
    } else {
        cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
            cat_hex_to_color("#ffffff18"));
    }

    if (state->system_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? T("No games found") : T("Scanning library..."),
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__games_ctx ctx = { state->systems };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->system_count, &state->list, item_h,
            jw__draw_game_item, &ctx);
    }
}

static void jw__render_apps(const jw_launcher_state *state,
                             int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    (void)content_h;

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, content_y, state->app_count,
                     &state->list, &list, &image, &item_h);

    if (state->app_count > 0 && state->list.cursor < state->app_count) {
        jw__draw_app_detail(state, &state->apps[state->list.cursor],
                            image.x, image.y, image.w, image.h);
    } else {
        cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
            cat_hex_to_color("#ffffff18"));
    }

    if (state->app_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? T("No apps found") : T("Scanning library..."),
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__apps_ctx ctx = { state->apps };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->app_count, &state->list, item_h,
            jw__draw_app_item, &ctx);
    }
}

/* List-page preview pane: just the pak's description. The name is already in the
   list on the left and the metadata lives on the detail page (A), so this pane
   only carries the blurb. Clipped to the box so a long description can't bleed;
   the full text is always readable on the detail page. */
static void jw__draw_pakrat_detail(const jw_launcher_state *state,
                                   const jw_pakrat_app_state *app,
                                   int detail_x, int detail_y,
                                   int detail_w, int detail_h) {
    (void)state;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    cat_draw_rounded_rect(detail_x, detail_y, detail_w, detail_h, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));
    if (!app || !app->package.summary[0]) {
        return;
    }

    int pad = CAT_S(18);
    SDL_Rect clip = { detail_x, detail_y, detail_w, detail_h };
    SDL_RenderSetClipRect(cat_get_renderer(), &clip);
    cat_draw_text_wrapped(body, app->package.summary,
                          detail_x + pad, detail_y + pad, detail_w - pad * 2,
                          theme->text, CAT_ALIGN_LEFT);
    SDL_RenderSetClipRect(cat_get_renderer(), NULL);
}

static int jw__pakrat_title_h(void) {
    return CAT_S(12) + TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + CAT_S(10);
}

static int jw__pakrat_header_h(void) {
    return cat_get_tab_bar_height() + jw__pakrat_title_h();
}

static int jw__draw_pakrat_header(const jw_launcher_state *state, const char *title) {
    int tab_h = jw__draw_menu_tab_bar(state);
    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int margin = CAT_S(12);
    int y = tab_h + margin;
    int w = cat_get_screen_width() - margin * 2;
    int large_h = TTF_FontHeight(large);
    int title_max = w - CAT_S(8);

    if (state && state->pakrat_message[0] && small) {
        int msg_w = w / 2;
        int msg_x = margin + w - msg_w - CAT_S(4);
        int msg_y = y + (large_h - TTF_FontHeight(small)) / 2;
        cat_draw_text_ellipsized(small, state->pakrat_message,
                                 msg_x, msg_y, theme->hint, msg_w);
        title_max = msg_x - margin - CAT_S(12);
        if (title_max < CAT_S(96)) {
            title_max = w - CAT_S(8);
        }
    }

    cat_draw_text_ellipsized(large, title,
                             margin + CAT_S(4), y,
                             theme->text, title_max);
    cat_draw_rect(margin, y + large_h + CAT_S(4), w, 1,
                  cat_hex_to_color("#ffffff20"));
    return tab_h + jw__pakrat_title_h();
}

typedef struct {
    const jw_pakrat_app_state *app;
    TTF_Font *body;
    TTF_Font *small;
} jw__pakrat_detail_ctx;

/* Natural height of the drilled-in detail content (status + full description +
   the version/path rows). Mirrors the layout jw__draw_pakrat_detail_content
   draws, so the scroll view bounds it correctly. */
static int jw__pakrat_detail_content_h(const jw_pakrat_app_state *app,
                                       TTF_Font *body, TTF_Font *small, int w) {
    int h = TTF_FontHeight(small) + CAT_S(14);   /* status line */
    if (app->package.summary[0]) {
        h += cat_measure_wrapped_text_height(body, app->package.summary, w) + CAT_S(18);
    }
    if (app->gated_version[0] && app->gated_min_leaf_version[0]) {
        char gate[256];
        snprintf(gate, sizeof(gate),
                 "Version %s requires Leaf v%s — update Leaf in Settings to get it.",
                 app->gated_version, app->gated_min_leaf_version);
        h += cat_measure_wrapped_text_height(body, gate, w) + CAT_S(18);
    }
    if (app->installed_version_missing_from_history &&
        app->installed_version[0]) {
        char missing[256];
        snprintf(missing, sizeof(missing),
                 "%s unavailable: version %s is missing from catalog history.",
                 app->status == JW_PAKRAT_APP_STALE ? "Restore" : "Reinstall",
                 app->installed_version);
        h += cat_measure_wrapped_text_height(body, missing, w) + CAT_S(18);
    }
    int row_h = TTF_FontHeight(small) + CAT_S(8);
    int rows = 3 + (app->managed ? 1 : 0);        /* catalog / installed / path [/ managed] */
    h += rows * row_h;
    return h;
}

/* Scroll-view content callback for the Pak Rat detail page: status, the full
   description (it scrolls, so nothing is clipped or shrunk), then the
   version/path metadata. */
static void jw__draw_pakrat_detail_content(int x, int y, int w, void *user) {
    const jw__pakrat_detail_ctx *c = (const jw__pakrat_detail_ctx *)user;
    const jw_pakrat_app_state *app = c->app;
    ap_theme *theme = cat_get_theme();
    int cy = y;

    const char *status = jw__pakrat_status_label(app->status);
    cat_draw_text(c->small, status, x, cy,
                  app->status == JW_PAKRAT_APP_STALE ? theme->highlight : theme->hint);
    cy += TTF_FontHeight(c->small) + CAT_S(14);

    if (app->package.summary[0]) {
        cat_draw_text_wrapped(c->body, app->package.summary, x, cy, w,
                              theme->text, CAT_ALIGN_LEFT);
        cy += cat_measure_wrapped_text_height(c->body, app->package.summary, w) +
              CAT_S(18);
    }

    char line[768];
    if (app->gated_version[0] && app->gated_min_leaf_version[0]) {
        snprintf(line, sizeof(line),
                 "Version %s requires Leaf v%s — update Leaf in Settings to get it.",
                 app->gated_version, app->gated_min_leaf_version);
        cat_draw_text_wrapped(c->body, line, x, cy, w,
                              theme->highlight, CAT_ALIGN_LEFT);
        cy += cat_measure_wrapped_text_height(c->body, line, w) + CAT_S(18);
    }
    if (app->installed_version_missing_from_history &&
        app->installed_version[0]) {
        snprintf(line, sizeof(line),
                 "%s unavailable: version %s is missing from catalog history.",
                 app->status == JW_PAKRAT_APP_STALE ? "Restore" : "Reinstall",
                 app->installed_version);
        cat_draw_text_wrapped(c->body, line, x, cy, w,
                              theme->highlight, CAT_ALIGN_LEFT);
        cy += cat_measure_wrapped_text_height(c->body, line, w) + CAT_S(18);
    }

    int row_h = TTF_FontHeight(c->small) + CAT_S(8);
    snprintf(line, sizeof(line), "Catalog version: %s", app->package.version);
    cat_draw_text_ellipsized(c->small, line, x, cy, theme->hint, w);
    cy += row_h;
    snprintf(line, sizeof(line), "Installed version: %s",
             app->installed_version[0] ? app->installed_version : "-");
    cat_draw_text_ellipsized(c->small, line, x, cy, theme->hint, w);
    cy += row_h;
    snprintf(line, sizeof(line), T("Path: Apps/%s"), app->package.install_path);
    cat_draw_text_ellipsized(c->small, line, x, cy, theme->hint, w);
    cy += row_h;
    if (app->managed) {
        cat_draw_text_ellipsized(c->small, T("Release-managed path"), x, cy, theme->hint, w);
    }
}

/* The drilled-in full-info page for one pak: the same tab bar + a pak-name title
   as the store header, with a scrollable body so a long description and all the
   metadata are fully readable inside the content box. */
static void jw__render_pakrat_detail_page(const jw_launcher_state *state,
                                          const jw_pakrat_app_state *app) {
    int header_h = jw__draw_pakrat_header(state, app->package.name);

    int margin = CAT_S(12);
    int hints_h = jw__footer_height(state);
    int hint_pad = (hints_h > 0) ? margin : 0;
    int x = margin;
    int w = cat_get_screen_width() - margin * 2;
    int top = header_h + margin;
    int h = cat_get_screen_height() - hints_h - hint_pad - top;

    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int content_h = jw__pakrat_detail_content_h(app, body, small, w);

    jw__pakrat_detail_ctx ctx = { app, body, small };
    cat_draw_scroll_view(x, top, w, h, content_h,
                         (cat_scroll_state *)&state->pakrat_detail_scroll,
                         jw__draw_pakrat_detail_content, &ctx);
}

static void jw__render_pakrat_store(const jw_launcher_state *state) {
    cat_clear_screen();
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    int margin = CAT_S(12);

    const jw_pakrat_app_state *selected = NULL;
    if (state->pakrat_app_count > 0 &&
        state->pakrat_list.cursor >= 0 &&
        state->pakrat_list.cursor < state->pakrat_app_count) {
        selected = &state->pakrat_apps[state->pakrat_list.cursor];
    }
    bool detail = state->pakrat_detail_open && selected != NULL;

    if (detail) {
        jw__render_pakrat_detail_page(state, selected);
    } else {
        int header_h = jw__draw_pakrat_header(state, "Pak Rat");

        SDL_Rect list, image;
        int item_h;
        jw__browse_boxes(state, header_h, state->pakrat_app_count,
                         &state->pakrat_list, &list, &image, &item_h);

        if (selected) {
            jw__draw_pakrat_detail(state, selected,
                                   image.x, image.y, image.w, image.h);
        } else {
            cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
                cat_hex_to_color("#ffffff18"));
        }

        if (state->pakrat_app_count == 0) {
            const char *msg = state->pakrat_message[0]
                ? state->pakrat_message
                : "No Pak Rat apps found";
            cat_draw_text_wrapped(body, msg,
                list.x + CAT_S(8), list.y + CAT_S(8),
                list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
        } else {
            jw__pakrat_ctx ctx = { state->pakrat_apps };
            cat_draw_list_pane(list.x, list.y, list.w, list.h,
                state->pakrat_app_count, &state->pakrat_list, item_h,
                jw__draw_pakrat_item, &ctx);
        }
    }

    cat_footer_item footer[4];
    int footer_count = 0;
    footer[footer_count++] = (cat_footer_item){ CAT_BTN_X, "Refresh", false, JW_HINT("X") };
    if (detail) {
        if (jw__pakrat_can_uninstall(selected)) {
            footer[footer_count++] = (cat_footer_item){ CAT_BTN_Y, "Uninstall", false, JW_HINT("Y") };
        }
        footer[footer_count++] = (cat_footer_item){ CAT_BTN_B, "Back", true, JW_HINT("B") };
        footer[footer_count++] = (cat_footer_item){
            CAT_BTN_A, jw__pakrat_primary_action_label(selected), true, JW_HINT("A")
        };
    } else {
        footer[footer_count++] = (cat_footer_item){ CAT_BTN_B, "Back", true, JW_HINT("B") };
        footer[footer_count++] = (cat_footer_item){
            CAT_BTN_A, "Details", selected != NULL, JW_HINT("A")
        };
    }
    jw__draw_footer(state, footer, footer_count);
    jw__present();
}

static void jw__render_settings(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    int sw = cat_get_screen_width();

    int sx = margin;
    int sy = content_y + margin;
    int sw_inner = sw - margin * 2;
    /* Same content-box height rule as jw__browse_boxes (top pad here is the
       margin already applied via sy; the gap above the hint bar is the hint
       box's own top padding) - so the Settings list sits on the same row grid
       as the browse tabs instead of a slightly denser one. */
    int hints_h = jw__footer_height(state);
    TTF_Font *status_font = cat_get_font(CAT_FONT_SMALL);
    int status_h = state->status[0]
        ? TTF_FontHeight(status_font) + CAT_S(8)
        : 0;
    int sh_inner = content_h - margin - ((hints_h > 0) ? margin : 0) - status_h;
    if (sh_inner < 0) sh_inner = 0;

    jw_settings_ui_render(&state->settings, sx, sy, sw_inner, sh_inner);

    /* Settings actions report success and failure through state->status. The
       System-menu-hosted Settings path used to reserve no place to draw it, so
       failures such as an unavailable scraper looked exactly like a dropped A
       press. Keep one line below the page only while there is feedback to show. */
    if (status_h > 0) {
        ap_theme *theme = cat_get_theme();
        int status_y = sy + sh_inner + CAT_S(4);
        cat_draw_text_ellipsized(status_font, state->status,
                                 sx + CAT_S(12), status_y, theme->hint,
                                 sw_inner - CAT_S(24));
    }
}

/* The current tab's content dispatch, factored out so it can draw to the screen
   or into an offscreen snapshot for the Glide slide. */
static void jw__render_tab_content(const jw_launcher_state *state,
                                   int content_y, int content_h, int margin) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:   jw__render_recents(state, content_y, content_h, margin);   break;
        case JW_TAB_FAVORITES: jw__render_favorites(state, content_y, content_h, margin); break;
        case JW_TAB_GAMES:     jw__render_games(state, content_y, content_h, margin);     break;
        case JW_TAB_APPS:      jw__render_apps(state, content_y, content_h, margin);       break;
        default: break;
    }
}

static void jw__tab_anim_clear(jw_launcher_state *state) {
    if (state->tab_anim_from) { SDL_DestroyTexture(state->tab_anim_from); state->tab_anim_from = NULL; }
    if (state->tab_anim_to)   { SDL_DestroyTexture(state->tab_anim_to);   state->tab_anim_to   = NULL; }
    state->tab_anim_active = false;
}

/* The tab-bar views, declared so the snapshot helper below can render whichever
   one is live. (Definitions follow further down.) */
static void jw__render_tabbed(const jw_launcher_state *state);
static void jw__render_actions(const jw_launcher_state *state);
static void jw__render_search(const jw_launcher_state *state);
static void jw__render_game_browser(const jw_launcher_state *state);

/* Snapshot whatever tab-bar view is currently on screen into a full-screen
   texture. cat_present no-ops while a render target is set, so the view's own
   render path draws into the texture without presenting. The slide clips to the
   content band, so each view's own header/footer are harmlessly captured too. */
static SDL_Texture *jw__capture_view(const jw_launcher_state *state) {
    SDL_Renderer *r = cat_get_renderer();
    if (!r) return NULL;
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, sw, sh);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_Texture *prev = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, tex);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    if (state->actions_open)     jw__render_actions(state);
    else if (state->search_open) jw__render_search(state);
    else if (state->games_open)  jw__render_game_browser(state);
    else                         jw__render_tabbed(state);
    SDL_SetRenderTarget(r, prev);
    return tex;
}

/* L1/R1 tab switch with the Glide slide. Honors the tab-bar promise: snapshots
   the current tab-bar view (home/Settings, Options, or search results), closes
   any sub-view, switches, snapshots the destination tab, and cross-slides the
   content band. Snap mode (or any failure) falls back to an instant switch. */
static void jw__switch_tab_slide(jw_launcher_state *state, int direction, const char *db_path) {
    bool glide = cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED &&
                 jw_settings_tab_glide(&state->settings);

    /* Settle any in-flight slide first, so the snapshot is the current view at
       rest — fast flips keep gliding, and old textures are freed before new. */
    if (state->tab_anim_active) jw__tab_anim_clear(state);

    SDL_Texture *from = glide ? jw__capture_view(state) : NULL;

    /* Leaving a tab-bar sub-view (Options/search/game browser) closes it; the
       destination is always a top-level tab. Centralized here so every L1/R1 path
       is uniform. */
    state->actions_open        = false;
    state->action_scope        = JW_ACTION_NONE;
    state->search_open         = false;
    jw__close_game_browser(state);
    state->status[0]           = '\0';
    jw__switch_tab(state, direction, db_path);

    if (!glide) { if (from) SDL_DestroyTexture(from); return; }

    SDL_Texture *to = jw__capture_view(state);
    if (!from || !to) {
        if (from) SDL_DestroyTexture(from);
        if (to)   SDL_DestroyTexture(to);
        return;
    }
    int content_y = cat_get_tab_bar_height();
    state->tab_anim_from = from;
    state->tab_anim_to   = to;
    state->tab_anim_dir  = direction;
    state->tab_anim_y    = content_y;
    state->tab_anim_h    = cat_get_screen_height() - content_y - jw__footer_height(state);
    state->tab_anim_start_ms = SDL_GetTicks();
    state->tab_anim_active = true;
    cat_request_frame();
}

static void jw__render_tabbed(const jw_launcher_state *state) {
    cat_clear_screen();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int header_h = jw__draw_tab_header(state);

    int content_y = header_h;
    int content_h = sh - header_h - fh;
    int margin    = CAT_S(12);

    if (state->tab_anim_active) {
        /* Cross-slide the two snapshots, eased; header/footer stay put. The two
           are always one screen-width apart, so they read as adjacent pages. */
        const uint32_t dur = 200;
        uint32_t elapsed = SDL_GetTicks() - state->tab_anim_start_ms;
        float p = (dur > 0) ? (float)elapsed / (float)dur : 1.0f;
        if (p > 1.0f) p = 1.0f;
        float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);  /* easeOutCubic */
        int sw = cat_get_screen_width();
        int from_x = (int)(-state->tab_anim_dir * sw * e);
        int to_x   = (int)( state->tab_anim_dir * sw * (1.0f - e));
        SDL_Renderer *r = cat_get_renderer();
        SDL_Rect clip = { 0, state->tab_anim_y, sw, state->tab_anim_h };
        SDL_RenderSetClipRect(r, &clip);
        SDL_Rect fr = { from_x, 0, sw, sh };
        SDL_Rect tr = { to_x,   0, sw, sh };
        SDL_RenderCopy(r, state->tab_anim_from, NULL, &fr);
        SDL_RenderCopy(r, state->tab_anim_to,   NULL, &tr);
        SDL_RenderSetClipRect(r, NULL);
        if (p >= 1.0f) jw__tab_anim_clear((jw_launcher_state *)state);
        else           cat_request_frame();
    } else {
        jw__render_tab_content(state, content_y, content_h, margin);
    }

    if (state->focus_pick_active) {
        cat_footer_item footer[] = {
            { CAT_BTN_Y,     "Clear",  false, JW_HINT("Y") },
            { CAT_BTN_B,     "Cancel", false, JW_HINT("B") },
            { CAT_BTN_START, "Done",   true,  JW_HINT("Start") },
            { CAT_BTN_A,     "Open",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (jw_settings_ui_is_open(&state->settings)) {
        /* Dead in practice now (settings is only open inside the MENU page, which
           skips this render path), but kept correct via the shared helper. */
        jw__draw_settings_footer(state);
    } else if (state->current_tab == JW_TAB_FAVORITES) {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Remove",   false, JW_HINT("Y") },
            { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (state->current_tab == JW_TAB_RECENTS) {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
            { CAT_BTN_A,  "Resume",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_L1,   "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,    state->current_tab == JW_TAB_GAMES ? "Options" : "Search",
                                            false, JW_HINT("X") },
            { CAT_BTN_A,    "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
    }
    jw__present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VERTICAL RENDER (NextUI-style)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const jw_launcher_state *state;
} jw__vert_ctx;

static void jw__draw_vert_item(int idx, int ix, int iy, int iw, int ih,
                                bool selected, void *user) {
    jw__vert_ctx *ctx = (jw__vert_ctx *)user;
    const jw_launcher_state *state = ctx->state;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    const jw_flat_item *it = &state->flat_items[idx];
    bool is_section = (it->kind == JW_FLAT_RECENTLY_PLAYED ||
                       it->kind == JW_FLAT_FAVORITES ||
                       it->kind == JW_FLAT_APPS ||
                       it->kind == JW_FLAT_SETTINGS);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text
                                : (is_section ? theme->text : theme->text);

    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    const char *label = jw__flat_label(state, idx);

    if (it->kind == JW_FLAT_SYSTEM) {
        jw__draw_row_name(body, label, ix + CAT_S(10), text_y, label_c,
                          iw - CAT_S(20), selected);
    } else {
        /* section header: slightly muted when not selected */
        if (!selected)
            label_c = theme->hint;
        cat_draw_text_ellipsized(body, label, ix + CAT_S(10), text_y, label_c,
                                 iw - CAT_S(20));
    }
}

static void jw__render_vertical_preview(const jw_launcher_state *state,
                                         int px, int py, int pw, int ph) {
    if (state->flat_count == 0 || state->list.cursor >= state->flat_count) {
        cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                              cat_hex_to_color("#ffffff10"));
        return;
    }

    const jw_flat_item *it = &state->flat_items[state->list.cursor];

    if (it->kind == JW_FLAT_SYSTEM) {
        const jw_system_entry *sys = &state->systems[it->system_idx];
        jw__draw_system_preview(px, py, pw, ph, sys->name, sys->game_count);
    } else {
        /* Non-system entries (Recents, Favorites, Apps, Settings): text only,
         * matches pre-icon behaviour. */
        ap_theme *theme  = cat_get_theme();
        TTF_Font *large  = cat_get_font(CAT_FONT_EXTRA_LARGE);
        const char *label = jw__flat_label(state, state->list.cursor);
        int large_h = TTF_FontHeight(large);
        int label_w = cat_measure_text(large, label);
        int margin  = CAT_S(16);
        cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                              cat_hex_to_color("#ffffff10"));
        cat_draw_text_ellipsized(large, label,
                                  px + (pw - label_w) / 2,
                                  py + (ph - large_h) / 2,
                                  theme->text, pw - margin * 2);
    }
}

static void jw__render_vertical(const jw_launcher_state *state) {
    cat_clear_screen();
    jw__draw_status_bar(state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int sb_h = CAT_DS(20);
    int margin = CAT_S(10);

    float split = ss->launcher.list_split;
    int list_w = (int)(sw * split);
    int list_x = 0;
    int prev_x = list_w + margin;
    int prev_w = sw - prev_x;
    int content_y = sb_h + margin;
    int content_h = sh - content_y - fh - margin;

    /* Subtle divider between list and preview */
    cat_draw_rect(list_w, content_y, 1, content_h, cat_hex_to_color("#ffffff20"));

    /* Left: nav list */
    int body_h = TTF_FontHeight(body);
    int item_h = body_h + CAT_S(12);
    jw__vert_ctx ctx = { state };
    cat_draw_list_pane(list_x, content_y, list_w, content_h,
        state->flat_count, &state->list, item_h,
        jw__draw_vert_item, &ctx);

    /* Right: preview panel */
    jw__render_vertical_preview(state, prev_x, content_y, prev_w - margin, content_h);

    /* Status line at bottom-left */
    int status_y = content_y + content_h - TTF_FontHeight(small);
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, list_w - margin);

    /* Settings overlay (dims background + draws panel) */
    if (jw_settings_ui_is_open(&state->settings)) {
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_Rect full = { 0, 0, sw, sh };
        SDL_RenderFillRect(ren, &full);

        int ox = sw / 6;
        int ow = sw - ox * 2;
        int oy = content_y;
        int oh = content_h;
        cat_draw_rounded_rect(ox, oy, ow, oh, CAT_S(8), theme->background);
        jw_settings_ui_render(&state->settings,
                               ox + CAT_S(12), oy + CAT_S(8),
                               ow - CAT_S(24), oh - CAT_S(16));

        cat_footer_item footer[] = {
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,    "Options",  false, JW_HINT("X") },
            { CAT_BTN_MENU, "Menu",     false, JW_HINT("H") },
            { CAT_BTN_Y,    "Rescan",   true,  JW_HINT("Y") },
            { CAT_BTN_A,    "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    }
    jw__present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HORIZONTAL RENDER (kUI-style carousel)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Hash a string to a deterministic hue [0..360) for placeholder tile colors */
static uint32_t jw__str_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* Fill a solid-colour quad. quad[0]=TL, quad[1]=TR, quad[2]=BR, quad[3]=BL. */
static void jw__fill_quad(const SDL_FPoint quad[4], SDL_Color c) {
    SDL_Color col[4] = { c, c, c, c };
    cat_draw_textured_quad(NULL, quad, NULL, col);
}

/* Draw a single carousel tile centered at cx, cy. */
static void jw__draw_carousel_tile(const jw_launcher_state *state, int tile_idx,
                                    int cx, int cy, int tw, int th, int skew,
                                    uint8_t alpha) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    const jw_flat_item *it = &state->flat_items[tile_idx];
    const char *label = jw__flat_label(state, tile_idx);

    /* Background color: curated per-console palette if available, otherwise
     * fall back to a muted hash-derived color so unmapped systems still get
     * deterministic-but-distinct tiles. */
    SDL_Color bg;
    bool curated = false;
    if (it->kind == JW_FLAT_SYSTEM) {
        curated = jw_console_colors_lookup(
            &state->console_colors,
            state->systems[it->system_idx].name, &bg);
    }
    if (curated) {
        bg.a = alpha;
    } else {
        uint32_t h = jw__str_hash(label);
        SDL_Color hl = theme->highlight;
        uint8_t mix = (uint8_t)((h & 0xFF) / 3);
        bg.r = (uint8_t)((hl.r * mix + 20 * (255 - mix)) / 255);
        bg.g = (uint8_t)((hl.g * mix + 10 * (255 - mix)) / 255);
        bg.b = (uint8_t)((hl.b * mix + 30 * (255 - mix)) / 255);
        bg.a = alpha;
    }

    int tx = cx - tw / 2;
    int ty = cy - th / 2;

    /* Parallelogram: top edge shifted left by skew relative to bottom */
    SDL_FPoint quad[4] = {
        { (float)(tx + skew),      (float)ty        }, /* TL */
        { (float)(tx + tw + skew), (float)ty        }, /* TR */
        { (float)(tx + tw),        (float)(ty + th) }, /* BR */
        { (float)tx,               (float)(ty + th) }, /* BL */
    };
    jw__fill_quad(quad, bg);

    /* Highlight border for active tile */
    if (alpha == 255) {
        SDL_Color border = theme->highlight;
        border.a = 200;
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, border.r, border.g, border.b, border.a);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_FPoint border_pts[5] = {
            quad[0], quad[1], quad[2], quad[3], quad[0]
        };
        SDL_RenderDrawLinesF(ren, border_pts, 5);
    }

    /* Clip text rendering to tile bounding box */
    SDL_Rect clip = {
        (int)quad[3].x, ty,
        tw + skew, th
    };
    SDL_Renderer *ren = cat_get_renderer();
    SDL_RenderSetClipRect(ren, &clip);

    ap_color text_c;
    text_c.r = 255; text_c.g = 255; text_c.b = 255; text_c.a = alpha;

    int body_h = TTF_FontHeight(body);
    int small_h = TTF_FontHeight(small);

    /* System label centered in tile */
    int lw = cat_measure_text(body, label);
    int lx = cx - lw / 2 + skew / 2;
    int ly = cy - body_h / 2;
    if (it->kind == JW_FLAT_SYSTEM) ly -= small_h / 2 + CAT_S(4);
    cat_draw_text_ellipsized(body, label, lx, ly, text_c, tw - CAT_S(16));

    /* Game count below for system tiles */
    if (it->kind == JW_FLAT_SYSTEM) {
        char cnt[24];
        snprintf(cnt, sizeof(cnt), T("%d games"),
                 state->systems[it->system_idx].game_count);
        int cw = cat_measure_text(small, cnt);
        ap_color hint_c = theme->hint;
        hint_c.a = alpha;
        cat_draw_text(small, cnt, cx - cw / 2 + skew / 2,
                      ly + body_h + CAT_S(4), hint_c);
    }

    SDL_RenderSetClipRect(ren, NULL);
}

/* Tools sub-menu drawn as a centered overlay list */
static void jw__draw_tools_menu(jw_launcher_state *state) {
    static const char *kTools[] = {
        "Recently Played", "Favorites", "Apps", "Settings"
    };
    static const int kToolsCount = 4;

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();

    int body_h = TTF_FontHeight(body);
    int item_h = body_h + CAT_S(12);
    int menu_w = sw * 40 / 100;
    int menu_h = item_h * kToolsCount + CAT_S(16);
    int mx = (sw - menu_w) / 2;
    int my = (sh - menu_h) / 2;

    /* Dim background */
    SDL_Renderer *ren = cat_get_renderer();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect full = { 0, 0, sw, sh };
    SDL_RenderFillRect(ren, &full);

    cat_draw_rounded_rect(mx, my, menu_w, menu_h, CAT_S(8),
                          theme->background);

    for (int i = 0; i < kToolsCount; i++) {
        int iy = my + CAT_S(8) + i * item_h;
        bool sel = (state->tools_list.cursor == i);
        int pill_h = body_h + CAT_S(6);
        int pill_y = iy + (item_h - pill_h) / 2;
        if (sel)
            cat_draw_pill(mx + CAT_S(4), pill_y, menu_w - CAT_S(8), pill_h,
                          theme->highlight);
        ap_color tc = sel ? theme->highlighted_text : theme->text;
        int ty = pill_y + (pill_h - body_h) / 2;
        cat_draw_text_ellipsized(body, kTools[i], mx + CAT_S(12), ty, tc,
                                 menu_w - CAT_S(24));
    }
}

static void jw__render_horizontal(jw_launcher_state *state) {
    cat_clear_screen();
    jw__draw_status_bar(state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);

    int skew = ss->launcher.carousel_skew;

    /* Carousel geometry */
    int tile_w   = CAT_S(180);
    int tile_h   = sh * 55 / 100;
    int spacing  = tile_w + CAT_S(20);
    int center_y = sh / 2 - fh / 2;
    int active   = state->list.cursor;
    int count    = state->flat_count;

    /* Draw tiles relative to active (active is centered) */
    int center_x = sw / 2;
    for (int i = 0; i < count; i++) {
        int offset = i - active;
        if (offset < -3 || offset > 3) continue;
        int cx = center_x + offset * spacing;
        uint8_t alpha;
        int w = tile_w, h = tile_h;
        if (offset == 0) {
            alpha = 255;
            w = tile_w * 12 / 10;
            h = tile_h * 12 / 10;
        } else if (abs(offset) == 1) {
            alpha = 160;
        } else {
            alpha = 80;
        }
        jw__draw_carousel_tile(state, i, cx, center_y, w, h, skew, alpha);
    }

    /* Status bar area: selected item name at top */
    int sb_h = CAT_DS(20);
    if (count > 0 && active < count) {
        TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
        const char *label = jw__flat_label(state, active);
        int lw = cat_measure_text(body, label);
        cat_draw_text(body, label, (sw - lw) / 2, sb_h / 2 - TTF_FontHeight(body) / 2,
                      theme->text);
    }

    /* Status line */
    int status_y = sh - fh - TTF_FontHeight(small);
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, CAT_S(12), status_y,
                             theme->hint, sw / 2);

    /* Tools overlay */
    if (state->tools_open)
        jw__draw_tools_menu(state);

    /* Settings overlay */
    if (jw_settings_ui_is_open(&state->settings)) {
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_Rect full = { 0, 0, sw, sh };
        SDL_RenderFillRect(ren, &full);

        int ox = sw / 6;
        int ow = sw - ox * 2;
        int oy = sb_h + CAT_S(8);
        int oh = sh - oy - fh - CAT_S(8);
        cat_draw_rounded_rect(ox, oy, ow, oh, CAT_S(8), theme->background);
        jw_settings_ui_render(&state->settings,
                               ox + CAT_S(12), oy + CAT_S(8),
                               ow - CAT_S(24), oh - CAT_S(16));

        cat_footer_item footer[] = {
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,     "Options",  false, JW_HINT("X") },
            { CAT_BTN_MENU,  "Menu",     false, JW_HINT("H") },
            { CAT_BTN_Y,     "Rescan",   true,  JW_HINT("Y") },
            { CAT_BTN_A,     "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    }
    jw__present();
}

static int jw__resolve_sdcard_path(const jw_launcher_state *state, const char *path,
                                    char *out, size_t out_size) {
    if (!state || !path || !path[0] || !out || out_size == 0) {
        return -1;
    }

    int needed = 0;
    if (path[0] == '/') {
        needed = snprintf(out, out_size, "%s", path);
    } else {
        needed = snprintf(out, out_size, "%s/%s", state->sdcard_root, path);
    }

    return needed >= 0 && needed < (int)out_size ? 0 : -1;
}

static int jw__resolve_platform_root(const char *sdcard_root,
                                     char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    const char *env = getenv("UMRK_PLATFORM_PATH");
    if (!env || !env[0]) {
        env = getenv("SYSTEM_PATH");
    }
    int needed = env && env[0]
        ? snprintf(out, out_size, "%s", env)
        : snprintf(out, out_size, "%s/.system/leaf/platforms/%s",
                   sdcard_root ? sdcard_root : ".", jw_platform_compiled_id());
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

/* Negative cache for image loads that fail (missing / unreadable file). Cold
   Cover Flow frames — search results and never-browsed systems — request the
   same absent paths on every frame: the per-game art that doesn't exist, and the
   sdcard/theme system-icon overrides that most systems don't ship. Without this,
   each miss is a fresh open() on the SD card every frame for every visible card,
   which stalls navigation. A miss is remembered for a short TTL, then re-checked
   so a file that appears later is still picked up. Keyed by FNV-1a of the path. */
#define JW_IMG_MISS_SLOTS  256
#define JW_IMG_MISS_TTL_MS 5000u
static struct { uint64_t hash; uint32_t ts; } jw__img_miss[JW_IMG_MISS_SLOTS];

static uint64_t jw__img_path_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
static bool jw__img_miss_recent(uint64_t hash, uint32_t now) {
    const size_t slot = (size_t)(hash % JW_IMG_MISS_SLOTS);
    return jw__img_miss[slot].hash == hash && jw__img_miss[slot].ts != 0 &&
           (now - jw__img_miss[slot].ts) < JW_IMG_MISS_TTL_MS;
}
static void jw__img_miss_mark(uint64_t hash, uint32_t now) {
    const size_t slot = (size_t)(hash % JW_IMG_MISS_SLOTS);
    jw__img_miss[slot].hash = hash;
    jw__img_miss[slot].ts   = now ? now : 1u;
}

static SDL_Texture *jw__load_cached_image(const char *path, int *out_w, int *out_h) {
    if (!path || !path[0]) {
        return NULL;
    }

    int tex_w = 0;
    int tex_h = 0;
    SDL_Texture *tex = cat_cache_get(path, &tex_w, &tex_h);
    if (tex) {
        if (out_w) *out_w = tex_w;
        if (out_h) *out_h = tex_h;
        return tex;
    }

    /* Skip the filesystem entirely if this path missed recently. */
    uint64_t hash = jw__img_path_hash(path);
    uint32_t now  = SDL_GetTicks();
    if (jw__img_miss_recent(hash, now)) {
        return NULL;
    }

    tex = cat_load_image(path);
    if (!tex) {
        jw__img_miss_mark(hash, now);
        return NULL;
    }

    if (SDL_QueryTexture(tex, NULL, NULL, &tex_w, &tex_h) != 0 || tex_w <= 0 || tex_h <= 0) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    cat_cache_put(path, tex, tex_w, tex_h);
    if (out_w) *out_w = tex_w;
    if (out_h) *out_h = tex_h;
    return tex;
}

static bool jw__readable_path(const char *path) {
    return path && path[0] && access(path, R_OK) == 0;
}

static bool jw__copy_path(char *out, size_t out_size, const char *path) {
    if (!out || out_size == 0 || !path) {
        return false;
    }
    int n = snprintf(out, out_size, "%s", path);
    return n >= 0 && (size_t)n < out_size;
}

/* Box-art covers are large (~1MB, ~1000px PNGs) and decoding one blocks the UI
   thread ~200ms — and the texture cache only holds a handful of entries, so
   scrolling a big system re-decodes the full image on nearly every step and the
   navigation stutters. Load covers through a persistent downscaled thumbnail cache
   instead: the first view of a cover decodes the full image once and writes a small
   thumbnail under the launcher state dir; every later view decodes that tiny
   thumbnail (~15ms), so the real art shows immediately and scrolling stays snappy. */
#define JW_COVER_THUMB_MAX 384

/* Resolve (once) the cover-thumbnail cache dir: $UMRK_INTERNAL_DATA_PATH/thumbs
   (jw_state_dir creates the state dir). Returns "" if it can't be prepared, in
   which case covers still load downscaled — just without on-disk persistence. */
static const char *jw__cover_thumb_dir(void) {
    static char dir[PATH_MAX] = "";
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        char *state = jw_state_dir();         /* creates the dir; caller frees */
        if (state && state[0]) {
            int n = snprintf(dir, sizeof(dir), "%s/thumbs", state);
            if (n > 0 && n < (int)sizeof(dir)) {
                mkdir(dir, 0775);             /* parent (state dir) already exists */
            } else {
                dir[0] = '\0';
            }
        }
        free(state);
    }
    return dir;
}

static bool jw__cover_thumb_path(const char *cover_abs, char *out, size_t out_size) {
    const char *dir = jw__cover_thumb_dir();
    if (!dir[0]) return false;
    /* FNV-1a hash of the absolute cover path -> stable, collision-resistant name. */
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)cover_abs; *p; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    int n = snprintf(out, out_size, "%s/%016llx.png", dir, (unsigned long long)h);
    return n > 0 && n < (int)out_size;
}

/* ---- Background cover decoder -----------------------------------------------
   Generating a thumbnail means decoding the full ~1MB source image, which blocks
   the UI thread ~200ms — so the *first* pass through a list (before any thumbnails
   exist) stutters, and the cover under the cursor shows blank until it is built.
   Move that decode to a worker thread with two inputs:
     - a high-priority slot = the cover under the cursor (latest wins), decoded
       first and handed back to the UI as a texture; and
     - a low-priority FIFO queue = the rest of the visible list (pre-warm), decoded
       only when the priority slot is idle. Pre-warm decodes just write the
       thumbnail to disk; the surface is discarded, so when the cursor later lands
       on that cover jw__load_cover hits the fast on-disk thumbnail path.
   Net effect: scroll freely while covers fill in a beat ahead of the cursor, in
   Favorites/Recents/search/systems alike. Existing thumbnails usually decode
   inline, but coverflow motion can route them through the worker to avoid
   mid-animation hitches. */
#define JW_COVER_QUEUE_MAX 48          /* pre-warm backlog cap (ring buffer) */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            started;
    bool            stop;

    /* High-priority request: the cover under the cursor (latest wins). */
    char            req_path[PATH_MAX];
    char            req_thumb[PATH_MAX];
    bool            has_req;

    /* Low-priority pre-warm queue (FIFO ring of covers to build ahead of time). */
    char            q_path[JW_COVER_QUEUE_MAX][PATH_MAX];
    char            q_thumb[JW_COVER_QUEUE_MAX][PATH_MAX];
    int             q_head;
    int             q_count;

    /* Result: worker -> main (only the priority request is delivered this way). */
    char            done_path[PATH_MAX];
    SDL_Surface    *done_surf;
    bool            has_done;
} jw_cover_loader;

static jw_cover_loader jw__cover_loader = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static bool jw__cf_animating;
static int  jw__cover_inline_decodes_this_frame;
/* Set for the single render that follows a button press. A cover decode is
   synchronous (~tens of ms on the A55), and while covers stream in the frame
   loop stays "active" (never idles on input) — so a decode landing on the same
   frame as a d-pad press stalls the highlight move. Skipping the inline decode
   on input frames keeps navigation instant; covers still decode on idle frames. */
static bool jw__suppress_inline_decode;

static void *jw__cover_worker(void *arg) {
    jw_cover_loader *L = (jw_cover_loader *)arg;
    pthread_mutex_lock(&L->lock);
    for (;;) {
        while (!L->stop && !L->has_req && L->q_count == 0) {
            pthread_cond_wait(&L->cond, &L->lock);
        }
        if (L->stop) break;

        char path[PATH_MAX], thumb[PATH_MAX];
        bool was_req;
        if (L->has_req) {                          /* priority: cursor cover first */
            snprintf(path, sizeof(path), "%s", L->req_path);
            snprintf(thumb, sizeof(thumb), "%s", L->req_thumb);
            L->has_req = false;
            was_req = true;
        } else {                                   /* else drain the pre-warm queue */
            snprintf(path, sizeof(path), "%s", L->q_path[L->q_head]);
            snprintf(thumb, sizeof(thumb), "%s", L->q_thumb[L->q_head]);
            L->q_head = (L->q_head + 1) % JW_COVER_QUEUE_MAX;
            L->q_count--;
            was_req = false;
        }
        pthread_mutex_unlock(&L->lock);

        SDL_Surface *surf = cat_decode_thumbnail_surface(path, thumb[0] ? thumb : NULL,
                                                         JW_COVER_THUMB_MAX);

        pthread_mutex_lock(&L->lock);
        if (was_req) {
            if (L->has_done && L->done_surf) {     /* drop a previous undelivered result */
                SDL_FreeSurface(L->done_surf);
                L->done_surf = NULL;
                L->has_done = false;
            }
            if (surf) {
                snprintf(L->done_path, sizeof(L->done_path), "%s", path);
                L->done_surf = surf;
                L->has_done = true;
            }
        } else if (surf) {
            SDL_FreeSurface(surf);                 /* pre-warm: thumbnail is on disk now */
        }
    }
    pthread_mutex_unlock(&L->lock);
    return NULL;
}

static void jw__cover_loader_ensure(void) {
    jw_cover_loader *L = &jw__cover_loader;
    if (L->started) return;            /* called on the main thread only */
    L->started = true;
    if (pthread_create(&L->thread, NULL, jw__cover_worker, L) != 0) {
        L->started = false;            /* no worker -> callers decode synchronously */
    }
}

static void jw__cover_loader_shutdown(void) {
    jw_cover_loader *L = &jw__cover_loader;
    if (!L->started) return;

    pthread_mutex_lock(&L->lock);
    L->stop = true;
    pthread_cond_signal(&L->cond);
    pthread_mutex_unlock(&L->lock);

    pthread_join(L->thread, NULL);

    pthread_mutex_lock(&L->lock);
    if (L->done_surf) {
        SDL_FreeSurface(L->done_surf);
        L->done_surf = NULL;
    }
    L->has_done = false;
    L->has_req = false;
    L->q_head = 0;
    L->q_count = 0;
    L->stop = false;
    L->started = false;
    pthread_mutex_unlock(&L->lock);
}

/* If a decoded surface for `path` is ready, consume it into *out and return true.
   Otherwise set `path` as the priority request (newest wins) and return false. */
static bool jw__cover_async_take(const char *path, const char *thumb, SDL_Surface **out) {
    jw_cover_loader *L = &jw__cover_loader;
    jw__cover_loader_ensure();
    if (!L->started) return false;
    bool got = false;
    pthread_mutex_lock(&L->lock);
    if (L->has_done && strcmp(L->done_path, path) == 0) {
        *out = L->done_surf;
        L->done_surf = NULL;
        L->has_done = false;
        got = true;
    } else {
        /* Keep an unmatched result for the rest of this draw pass. Coverflow draws
           side cards before the centre card, so dropping it here can discard the
           cover that is about to be requested later in the same frame. The worker
           still replaces stale results when a newer decode completes. */
        if (!L->has_req || strcmp(L->req_path, path) != 0) {
            snprintf(L->req_path, sizeof(L->req_path), "%s", path);
            snprintf(L->req_thumb, sizeof(L->req_thumb), "%s", thumb ? thumb : "");
            L->has_req = true;
            pthread_cond_signal(&L->cond);
        }
    }
    pthread_mutex_unlock(&L->lock);
    return got;
}

/* Append a cover to the low-priority pre-warm queue, skipping duplicates and the
   in-flight priority request. Caller has already confirmed the thumbnail is
   missing. No-op when the queue is full (we just pre-warm fewer covers). */
static void jw__cover_prewarm_enqueue(const char *path, const char *thumb) {
    jw_cover_loader *L = &jw__cover_loader;
    jw__cover_loader_ensure();
    if (!L->started) return;
    pthread_mutex_lock(&L->lock);
    bool dup = (L->has_req && strcmp(L->req_path, path) == 0);
    for (int i = 0; !dup && i < L->q_count; ++i) {
        int idx = (L->q_head + i) % JW_COVER_QUEUE_MAX;
        if (strcmp(L->q_path[idx], path) == 0) dup = true;
    }
    if (!dup && L->q_count < JW_COVER_QUEUE_MAX) {
        int tail = (L->q_head + L->q_count) % JW_COVER_QUEUE_MAX;
        snprintf(L->q_path[tail], PATH_MAX, "%s", path);
        snprintf(L->q_thumb[tail], PATH_MAX, "%s", thumb ? thumb : "");
        L->q_count++;
        pthread_cond_signal(&L->cond);
    }
    pthread_mutex_unlock(&L->lock);
}

/* Load a cover texture. Returns the texture when ready (cached, fast-thumbnail, or
   freshly-decoded by the worker). When the cover still needs the worker to build
   its thumbnail, returns NULL and sets *out_pending=true so the caller can leave
   the panel empty (rather than flashing the system icon) until the art streams in. */
static SDL_Texture *jw__load_cover(const jw_launcher_state *state, const char *cover_abs,
                                   int *out_w, int *out_h, bool *out_pending) {
    if (out_pending) *out_pending = false;
    if (!state || !cover_abs || !cover_abs[0]) return NULL;

    int w = 0, h = 0;
    SDL_Texture *cached = cat_cache_get(cover_abs, &w, &h);
    if (cached) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return cached;
    }

    char thumb[PATH_MAX];
    const char *thumb_path = jw__cover_thumb_path(cover_abs, thumb, sizeof(thumb))
                                 ? thumb : NULL;

    /* Decode entirely off the main thread. Even a small thumbnail decode + GPU
       upload is tens of ms on the A55/Mali, and this runs on the same thread that
       services input — an inline decode freezes navigation until it finishes.
       Hand the cover to the worker (priority slot, newest-wins) and only upload
       the surface it returns; the carousel keeps moving while art streams in. The
       worker decodes the on-disk thumbnail when present, else the source. */
    SDL_Surface *surf = NULL;
    if (jw__cover_async_take(cover_abs, thumb_path, &surf)) {
        SDL_Texture *tex = cat_texture_from_surface(surf);
        w = surf->w;
        h = surf->h;
        SDL_FreeSurface(surf);
        if (tex) {
            cat_cache_put(cover_abs, tex, w, h);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return tex;
        }
        return NULL;
    }

    /* Degraded fallback: if the worker thread could not be started there is no
       async path, so decode inline (bounded to one per frame, matching the cover
       budget) rather than marking the cover pending forever. Leaving out_pending
       false lets the caller show its system-icon fallback until art arrives. */
    if (!jw__cover_loader.started) {
        if (!jw__cf_animating && jw__cover_inline_decodes_this_frame < 1) {
            jw__cover_inline_decodes_this_frame++;
            SDL_Surface *isurf = cat_decode_thumbnail_surface(cover_abs, thumb_path,
                                                              JW_COVER_THUMB_MAX);
            if (isurf) {
                SDL_Texture *tex = cat_texture_from_surface(isurf);
                w = isurf->w;
                h = isurf->h;
                SDL_FreeSurface(isurf);
                if (tex) {
                    cat_cache_put(cover_abs, tex, w, h);
                    if (out_w) *out_w = w;
                    if (out_h) *out_h = h;
                    return tex;
                }
            }
        }
        cat_request_frame_in(40);              /* decode the rest over later frames */
        return NULL;
    }

    if (out_pending) *out_pending = true;
    cat_request_frame_in(40);                  /* re-check until the worker finishes */
    return NULL;
}

/* Coverflow cards share the cover loader's "no inline decode while moving"
   discipline. This is used for system/app icons as well as art, so a cold image
   miss shows the normal placeholder card instead of stalling the carousel. */
static SDL_Texture *jw__load_coverflow_image(const char *path, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!path || !path[0]) {
        return NULL;
    }

    int w = 0, h = 0;
    SDL_Texture *cached = cat_cache_get(path, &w, &h);
    if (cached) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return cached;
    }

    char thumb[PATH_MAX];
    const char *thumb_path = jw__cover_thumb_path(path, thumb, sizeof(thumb))
                                 ? thumb : NULL;

    if (!jw__cf_animating && !jw__suppress_inline_decode &&
        jw__cover_inline_decodes_this_frame < 1) {
        jw__cover_inline_decodes_this_frame++;
        SDL_Texture *tex = NULL;
        if (thumb_path) {
            tex = cat_load_image_thumbnail(path, thumb_path, JW_COVER_THUMB_MAX, &w, &h);
        } else {
            tex = cat_load_image(path);
            if (tex &&
                (SDL_QueryTexture(tex, NULL, NULL, &w, &h) != 0 || w <= 0 || h <= 0)) {
                SDL_DestroyTexture(tex);
                tex = NULL;
            }
        }
        if (tex) {
            cat_cache_put(path, tex, w, h);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return tex;
        }
    }

    SDL_Surface *surf = NULL;
    if (jw__cover_async_take(path, thumb_path, &surf)) {
        SDL_Texture *tex = cat_texture_from_surface(surf);
        w = surf->w;
        h = surf->h;
        SDL_FreeSurface(surf);
        if (tex) {
            cat_cache_put(path, tex, w, h);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return tex;
        }
    }

    cat_request_frame_in(40);
    return NULL;
}

/* Pre-warm covers around the cursor so navigating lands on art that is already
   built. Windows the work (a few behind, more ahead) so even huge systems only
   enqueue near-cursor covers, while small lists (Favorites/Recents) fill within a
   couple of cursor moves. Cheap to call every render: bounded by the window and
   skips covers already decoded or already thumbnailed. */
#define JW_PREWARM_BEHIND 2
#define JW_PREWARM_AHEAD  16
static void jw__cover_prewarm(const jw_launcher_state *state,
                              const jw_game_entry *games, int count, int cursor) {
    if (!state || !games || count <= 0) return;
    int from = cursor - JW_PREWARM_BEHIND;
    if (from < 0) from = 0;
    int to = cursor + JW_PREWARM_AHEAD;
    if (to > count - 1) to = count - 1;
    for (int i = from; i <= to; ++i) {
        const char *rel = games[i].image_path;
        if (!rel[0]) continue;
        char abs[PATH_MAX];
        if (jw__resolve_sdcard_path(state, rel, abs, sizeof(abs)) != 0) continue;
        if (cat_cache_get(abs, NULL, NULL)) continue;          /* already decoded */
        char thumb[PATH_MAX];
        const char *tp = jw__cover_thumb_path(abs, thumb, sizeof(thumb)) ? thumb : NULL;
        if (tp && cat_thumbnail_is_cached(abs, tp)) continue;  /* already built */
        jw__cover_prewarm_enqueue(abs, tp);
    }
}

/* Same windowed prefetch as jw__cover_prewarm, but over search results (game
   covers only; app icons are tiny and load inline). */
/* Cursor last swept by jw__cover_prewarm_search. The sweep stats every cover in
   its window (two SD-card stat() calls each), so re-running it on frames where
   the carousel did not move (e.g. moving key-to-key on the keyboard) needlessly
   stalls the frame. Reset to -1 whenever the result set changes. */
static int s_search_prewarm_cursor = -1;
/* Set when the search results change (open / new query) so jw__render_search_cf
   re-seats its carousel to the new cursor instead of animating from a stale
   position (which briefly shows the wrong centered card on a same-count re-query). */
static bool s_search_cf_reset = false;

static void jw__cover_prewarm_search(const jw_launcher_state *state,
                                     const jw_search_result *results,
                                     int count, int cursor) {
    if (!state || !results || count <= 0) return;
    if (cursor == s_search_prewarm_cursor) return;   /* window unchanged */
    s_search_prewarm_cursor = cursor;
    /* Wider window than the main carousels: search surfaces games the user has
       never browsed, so their thumbnails are cold and must be built ahead. */
    int from = cursor - 8;
    if (from < 0) from = 0;
    int to = cursor + 32;
    if (to > count - 1) to = count - 1;
    for (int i = from; i <= to; ++i) {
        if (results[i].kind != JW_SEARCH_GAME) continue;
        const char *rel = results[i].image_path;
        if (!rel[0]) continue;
        char abs[PATH_MAX];
        if (jw__resolve_sdcard_path(state, rel, abs, sizeof(abs)) != 0) continue;
        if (cat_cache_get(abs, NULL, NULL)) continue;
        char thumb[PATH_MAX];
        const char *tp = jw__cover_thumb_path(abs, thumb, sizeof(thumb)) ? thumb : NULL;
        if (tp && cat_thumbnail_is_cached(abs, tp)) continue;
        jw__cover_prewarm_enqueue(abs, tp);
    }
}

static void jw__draw_image_fit(SDL_Texture *tex, int tex_w, int tex_h,
                                int x, int y, int w, int h) {
    if (!tex || tex_w <= 0 || tex_h <= 0 || w <= 0 || h <= 0) {
        return;
    }

    int draw_w = w;
    int draw_h = (tex_h * draw_w) / tex_w;
    if (draw_h > h) {
        draw_h = h;
        draw_w = (tex_w * draw_h) / tex_h;
    }

    int draw_x = x + (w - draw_w) / 2;
    int draw_y = y + (h - draw_h) / 2;
    cat_draw_image(tex, draw_x, draw_y, draw_w, draw_h);
}

/* Like jw__draw_image_fit, but clips the image to the same rounded-corner shape
   as the list pills (theme pill radius + pill_corner_mask), so cover art matches
   the current List Style setting. Used ONLY for real box art — never icons. */
static void jw__draw_cover_fit(SDL_Texture *tex, int tex_w, int tex_h,
                               int x, int y, int w, int h) {
    if (!tex || tex_w <= 0 || tex_h <= 0 || w <= 0 || h <= 0) {
        return;
    }

    int draw_w = w;
    int draw_h = (tex_h * draw_w) / tex_w;
    if (draw_h > h) {
        draw_h = h;
        draw_w = (tex_w * draw_h) / tex_h;
    }
    int draw_x = x + (w - draw_w) / 2;
    int draw_y = y + (h - draw_h) / 2;

    /* Round in proportion to the cover (scaled by the List Style ratio, so a sharp
       style stays square) and keep the style's corner mask. Tied to the image size
       so the curve reads the same on big and small art. */
    const ap_theme *theme = cat_get_theme();
    int smaller = draw_w < draw_h ? draw_w : draw_h;
    int radius = (int)(theme->pill_radius_ratio * smaller * 0.26f + 0.5f);
    unsigned corners = (unsigned)theme->pill_corner_mask;
    if (corners == 0) corners = CAT_CORNER_ALL;

    cat_draw_image_rounded_ex(tex, draw_x, draw_y, draw_w, draw_h, radius, corners);
}

/* ─── 5-Game Mode (focus mode) ───────────────────────────────────────────── */

/* Resolve the focus config the daemon passed via env into state->focus_games.
   Danglers (a picked game whose ROM was removed → no DB row) are dropped so a
   deleted game can't wedge the mode; if that empties the set we fall back to the
   normal launcher (can't lock an empty focus screen). */
static void jw__focus_init(jw_launcher_state *state) {
    state->focus_active = false;
    state->focus_cursor = 0;
    state->focus_count = 0;
    state->focus_batt_pct = -1;
    state->focus_batt_chg = -1;
    state->focus_batt_next_ms = 0;
    const char *fm = getenv("JAWAKA_FOCUS_MODE");
    if (!fm || fm[0] != '1') {
        return;
    }

    const char *style = getenv("JAWAKA_FOCUS_STYLE");
    state->focus_bw = (style && strcmp(style, "bw") == 0);
    state->focus_lock = jw_focus_lock_parse(getenv("JAWAKA_FOCUS_LOCK"));
    /* PIN hash comes from the DB (never the env) so the hash isn't exposed in
       /proc/<pid>/environ. The lock type is from the env but the hash from the
       DB: a transient sqlite-busy on this one read (jw_db_get_setting returns -1
       only on a real open/busy failure, 0 for an absent key) would leave
       lock==PIN with an empty hash and disable the PIN for the whole session, so
       retry briefly when the lock is PIN. The common case succeeds first try and
       never sleeps. */
    state->focus_pin_hash[0] = '\0';
    int hrc = jw_db_get_setting(state->db_path, JW_FOCUS_KEY_PIN_HASH,
                                state->focus_pin_hash, sizeof(state->focus_pin_hash));
    for (int tries = 0; hrc != 0 && state->focus_lock == JW_FOCUS_LOCK_PIN &&
                        tries < 4; tries++) {
        usleep(40000);   /* 40ms; runs only on a genuine read failure */
        hrc = jw_db_get_setting(state->db_path, JW_FOCUS_KEY_PIN_HASH,
                                state->focus_pin_hash, sizeof(state->focus_pin_hash));
    }
    if (state->focus_lock == JW_FOCUS_LOCK_PIN && state->focus_pin_hash[0] == '\0')
        jw_log_warn("focus: PIN hash unreadable at spawn; PIN disabled this session "
                    "(failsafe chord + SD-delete still recover)");
    state->focus_unlock_open = false;
    state->focus_pin_slot = 0;
    state->focus_pin_error = false;
    for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) state->focus_pin[i] = 0;

    int ids[JW_FOCUS_MAX_GAMES];
    int n = 0;
    jw_focus_ids_parse(getenv("JAWAKA_FOCUS_IDS"), ids, &n);

    int count = 0;
    for (int i = 0; i < n && count < JW_FOCUS_SCREEN_MAX_TILES; i++) {
        jw_game_entry g;
        if (jw_db_get_game_by_id(state->db_path, ids[i], &g) == 0) {
            state->focus_games[count++] = g;
        } else {
            jw_log_warn("focus: picked game id=%d no longer resolves; dropping", ids[i]);
        }
    }
    state->focus_count = count;
    /* We only reach here because the daemon spawned us into focus mode (the env
       gate at the top), so stay locked even if some — or all — picked games no
       longer resolve. Otherwise a library change that drops every id would
       silently fall through to the full, unlocked launcher and defeat the lock.
       With count == 0 the focus screen shows a "no games" note and MENU still
       reaches the unlock/exit path. */
    state->focus_active = true;
    if (count == 0) {
        jw_log_warn("focus: no picked games resolved; showing empty locked screen");
    } else {
        jw_log_info("focus: active with %d game(s), style=%s",
                    count, state->focus_bw ? "bw" : "theme");
    }
}

/* Render one focus-mode frame: resolve each tile's cover (async cache; title
   fallback while it streams) + the live battery, then hand off to the pure
   focus-screen renderer. */
static void jw__render_focus(jw_launcher_state *state) {
    cat_clear_screen();

    jw_focus_tile tiles[JW_FOCUS_SCREEN_MAX_TILES];
    memset(tiles, 0, sizeof(tiles));
    bool any_pending = false;
    for (int i = 0; i < state->focus_count; i++) {
        const jw_game_entry *g = &state->focus_games[i];
        tiles[i].title = g->name;
        char abs[PATH_MAX];
        int w = 0, h = 0;
        bool pending = false;
        if (g->image_path[0] &&
            jw__resolve_sdcard_path(state, g->image_path, abs, sizeof(abs)) == 0) {
            SDL_Texture *t = jw__load_cover(state, abs, &w, &h, &pending);
            if (t) {
                tiles[i].art = t;
                tiles[i].art_w = w;
                tiles[i].art_h = h;
            }
        }
        if (pending) any_pending = true;
    }

    /* Refresh battery directly (~5s cadence); the main loop's 1s idle frame keeps
       this re-polling even without input. */
    uint32_t now = SDL_GetTicks();
    if (state->focus_batt_next_ms == 0 || now >= state->focus_batt_next_ms) {
        int b = -1, c = -1;
        if (jw_ipc_platform_power_status(state->socket_path, &b, &c) == 0) {
            state->focus_batt_pct = b;
            state->focus_batt_chg = c;
        }
        state->focus_batt_next_ms = now + 5000;
    }
    jw_focus_battery batt;
    batt.percent = state->focus_batt_pct;   /* <0 = unknown; drawn as "?", no blink */
    batt.charging = (state->focus_batt_chg == 1);
    /* Keep re-rendering so the critical-battery blink animates (known % only). */
    if (!batt.charging && batt.percent >= 0 && batt.percent <= 10)
        cat_request_frame_in(450);

    /* Poll for a paired-but-disconnected headset on a slow throttle (the paired
       list shells out to bluetoothctl; the focus screen is static so an
       occasional hitch is invisible). */
    if (state->focus_bt_next_ms == 0 || now >= state->focus_bt_next_ms) {
        state->focus_bt_pip = false;
        state->focus_bt_headset_mac[0] = '\0';
        if (jw_bt_available() && jw_bt_radio_is_on()) {
            jw_bt_device_t devs[JW_BT_MAX_DEVICES];
            int nd = jw_bt_list_paired(devs, JW_BT_MAX_DEVICES);
            for (int i = 0; i < nd; i++) {
                bool audio = devs[i].kind == JW_BT_DEVICE_HEADSET ||
                             devs[i].has_audio_sink || devs[i].has_a2dp;
                if (devs[i].paired && !devs[i].connected && audio) {
                    state->focus_bt_pip = true;
                    snprintf(state->focus_bt_headset_mac,
                             sizeof(state->focus_bt_headset_mac), "%s", devs[i].mac);
                    break;
                }
            }
        }
        state->focus_bt_next_ms = now + 10000;
    }

    jw_focus_screen_render(tiles, state->focus_count, state->focus_cursor,
                           state->focus_bw, batt, state->focus_bt_pip);

    /* All picked games vanished (removed/renamed since setup): the screen would be
       empty, so tell the user why and how to leave. Still fully locked. */
    if (state->focus_count == 0) {
        int sw = cat_get_screen_width(), sh = cat_get_screen_height();
        SDL_Color fg = { 235, 235, 235, 255 };
        TTF_Font *f = cat_get_font(CAT_FONT_MEDIUM);
        const char *m = "No games available";
        int w = cat_measure_text(f, m);
        cat_draw_text(f, m, (sw - w) / 2, sh / 2 - CAT_S(24), fg);
        TTF_Font *fs = cat_get_font(CAT_FONT_SMALL);
        const char *m2 = "Press MENU to exit";
        int w2 = cat_measure_text(fs, m2);
        cat_draw_text(fs, m2, (sw - w2) / 2, sh / 2 + CAT_S(16), fg);
    }

    if (state->focus_unlock_open) {
        jw_focus_unlock_view uv;
        memset(&uv, 0, sizeof(uv));
        uv.pin_mode = (state->focus_lock == JW_FOCUS_LOCK_PIN);
        uv.pin = state->focus_pin;
        uv.pin_slot = state->focus_pin_slot;
        uv.error = state->focus_pin_error;
        if (state->focus_unlock_confirm == 1) {
            uv.confirm = "Reboot?";
        } else if (state->focus_unlock_confirm == 2) {
            uv.confirm = "Shut Down?";
        } else {
            int r = 0;
            if (uv.pin_mode) {
                uv.title = "Enter PIN to exit";
                uv.rows[r++] = (jw_focus_hint_row){ "Up/Down", "Digit",
                                                    "Left/Right", "Slot", false };
            } else {
                uv.title = "Exit 5-Game Mode?";
            }
            uv.rows[r++] = (jw_focus_hint_row){ "B", "Cancel", "A", "Confirm", false };
            uv.rows[r++] = (jw_focus_hint_row){ "L1", "Reboot", "R1", "Shut Down", false };
            if (state->focus_bt_pip)
                uv.rows[r++] = (jw_focus_hint_row){ "Y", "Reconnect Bluetooth",
                                                    NULL, NULL, true };
            uv.row_count = r;
        }
        jw_focus_screen_render_unlock(state->focus_bw, &uv);
    }
    jw__present();

    /* Covers decode off-thread; nudge a re-render so art appears promptly. */
    if (any_pending) cat_request_frame_in(150);
}

/* ─── System icon loader (shared across themes) ──────────────────────────── */

static bool jw__resolve_system_icon_path(const jw_launcher_state *state,
                                         const char *system_code,
                                         char *out, size_t out_size) {
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!system_code || !system_code[0] || !out || out_size == 0) {
        return false;
    }

    const cat_stylesheet *ss = cat_get_stylesheet();
    const char *theme_dir = cat_get_active_theme_dir();
    const char *theme_name = cat_get_active_theme_name();
    const char *icon_dir = (ss && ss->launcher.coverflow_icon_dir[0])
                               ? ss->launcher.coverflow_icon_dir : "system_icons";
    char path[PATH_MAX];
    int n = 0;

    if (system_code[0] != '_' && state && state->sdcard_root[0]) {
        n = snprintf(path, sizeof(path), "%s/Roms/%s/icon.png",
                     state->sdcard_root, system_code);
        if (n >= 0 && (size_t)n < sizeof(path) && jw__readable_path(path)) {
            return jw__copy_path(out, out_size, path);
        }
    }

    if (theme_dir && theme_dir[0] && theme_name && theme_name[0]) {
        n = snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
                     theme_dir, theme_name, icon_dir, system_code);
        if (n >= 0 && (size_t)n < sizeof(path) && jw__readable_path(path)) {
            return jw__copy_path(out, out_size, path);
        }
    }

    if (theme_dir && theme_dir[0]) {
        n = snprintf(path, sizeof(path), "%s/../system_icons/%s.png",
                     theme_dir, system_code);
        if (n >= 0 && (size_t)n < sizeof(path) && jw__readable_path(path)) {
            return jw__copy_path(out, out_size, path);
        }

        n = snprintf(path, sizeof(path), "%s/../system_icons/_default.png",
                     theme_dir);
        if (n >= 0 && (size_t)n < sizeof(path) && jw__readable_path(path)) {
            return jw__copy_path(out, out_size, path);
        }
    }

    return false;
}

static const char *jw__cf_system_icon_path(jw_launcher_state *state, int idx) {
    if (!state || idx < 0 || idx >= state->system_count) {
        return NULL;
    }

    const char *code = state->systems[idx].name;
    if (!code[0]) {
        return NULL;
    }

    jw_system_icon_memo *memo = &state->system_icon_memos[idx];
    if (!memo->done || strcmp(memo->code, code) != 0) {
        memset(memo, 0, sizeof(*memo));
        memo->done = true;
        size_t i = 0;
        for (; i + 1 < sizeof(memo->code) && code[i]; ++i) {
            memo->code[i] = code[i];
        }
        memo->code[i] = '\0';
        (void)jw__resolve_system_icon_path(state, code, memo->path, sizeof(memo->path));
    }

    return memo->path[0] ? memo->path : NULL;
}

/* Loader order:
 *   1. <sdcard_root>/Roms/<SYSTEM>/icon.png       (user override; skipped for codes starting with '_')
 *   2. <theme_dir>/<theme>/<icon_dir>/<SYSTEM>.png (theme-bundled override, if any)
 *   3. <themes_dir_parent>/system_icons/<SYSTEM>.png (shared baseline)
 *   4. <themes_dir_parent>/system_icons/_default.png (final fallback)
 * Returns NULL only if all four fail.
 * Pass "_tools" as system_code for the Tools tile.
 */
static SDL_Texture *jw__load_system_icon(const char *system_code,
                                         int *out_w, int *out_h) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    const char *theme_dir    = cat_get_active_theme_dir();
    const char *theme_name   = cat_get_active_theme_name();
    char path[1024];

    /* (1) user override on the sdcard */
    if (system_code[0] != '_') {
        char *sdcard_root = jw_sdcard_root();
        if (sdcard_root) {
            snprintf(path, sizeof(path), "%s/Roms/%s/icon.png",
                     sdcard_root, system_code);
            SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
            free(sdcard_root);
            if (t) return t;
        }
    }

    /* (2) theme-bundled override, if the theme ships its own system_icons/ */
    if (theme_dir[0] && theme_name[0]) {
        int n = snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
                         theme_dir, theme_name,
                         ss->launcher.coverflow_icon_dir, system_code);
        if (n > 0 && (size_t)n < sizeof(path)) {
            SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
            if (t) return t;
        }
    }

    /* (3) shared baseline at <themes_dir_parent>/system_icons/<SYSTEM>.png.
     * The shared icons live next to the active theme root. */
    if (theme_dir[0]) {
        int n = snprintf(path, sizeof(path), "%s/../system_icons/%s.png",
                         theme_dir, system_code);
        if (n > 0 && (size_t)n < sizeof(path)) {
            SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
            if (t) return t;
        }
    }

    /* (4) shared _default.png */
    if (theme_dir[0]) {
        int n = snprintf(path, sizeof(path), "%s/../system_icons/_default.png",
                         theme_dir);
        if (n > 0 && (size_t)n < sizeof(path)) {
            SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
            if (t) return t;
        }
    }

    return NULL;
}

/* Shared preview-pane renderer: rounded backdrop + centered icon + label
 * + game-count subtitle. Used by Tabs games-tab right pane and the Vertical
 * preview pane so they stay visually consistent.
 *
 * Pass game_count < 0 to suppress the subtitle (e.g. non-system entries). */
static void jw__draw_system_preview(int px, int py, int pw, int ph,
                                     const char *system_code, int game_count) {
    ap_theme *theme   = cat_get_theme();
    TTF_Font *small   = cat_get_font(CAT_FONT_SMALL);

    cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));

    /* Icon: up to 88% of pane width or 340px, whichever is smaller, and never
       more than 72% of the pane height (leaves room for the count below). */
    int icon_max = CAT_S(340);
    int icon_box = pw * 88 / 100;
    if (icon_box > icon_max)    icon_box = icon_max;
    if (icon_box > ph * 72 / 100) icon_box = ph * 72 / 100;

    int sub_h = TTF_FontHeight(small);
    int gap   = CAT_S(12);

    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    if (system_code && system_code[0])
        tex = jw__load_system_icon(system_code, &tw, &th);

    /* Vertical stack: icon + count (no name), centered in the pane. */
    int block_h = (tex ? icon_box : 0) + ((game_count >= 0) ? (gap + sub_h) : 0);
    int top_y   = py + (ph - block_h) / 2;

    int count_y = top_y;
    if (tex) {
        jw__draw_image_fit(tex, tw, th,
                           px + (pw - icon_box) / 2, top_y, icon_box, icon_box);
        count_y = top_y + icon_box + gap;
    }

    if (game_count >= 0) {
        char sub[32];
        snprintf(sub, sizeof(sub), T("%d games"), game_count);
        int subw = cat_measure_text(small, sub);
        cat_draw_text(small, sub, px + (pw - subw) / 2, count_y, theme->hint);
    }
}

static int jw__resolve_app_icon_path(const jw_launcher_state *state,
                                     const jw_app_entry *app,
                                     char *out, size_t out_size) {
    if (!state || !app || !app->icon[0] || !out || out_size == 0) {
        return -1;
    }

    if (app->icon[0] == '/') {
        int needed = snprintf(out, out_size, "%s", app->icon);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    char pak_abs[PATH_MAX];
    if (jw__resolve_sdcard_path(state, app->pak_dir, pak_abs, sizeof(pak_abs)) != 0) {
        return -1;
    }

    int needed = snprintf(out, out_size, "%s/%s", pak_abs, app->icon);
    return needed >= 0 && needed < (int)out_size ? 0 : -1;
}

static void jw__draw_app_detail(const jw_launcher_state *state,
                                const jw_app_entry *app,
                                int detail_x, int detail_y,
                                int detail_w, int detail_h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    cat_draw_rounded_rect(detail_x, detail_y, detail_w, detail_h, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));

    if (!app) {
        return;
    }

    int metadata_lines = 1 +
                         (app->pak_version[0] ? 1 : 0) +
                         (app->min_leaf_version[0] ? 1 : 0);
    int sub_h = TTF_FontHeight(small);
    int line_gap = CAT_S(4);
    int text_h = metadata_lines * sub_h +
                 (metadata_lines - 1) * line_gap;
    int gap = CAT_S(12);

    /* Icon sizing mirrors jw__draw_system_preview, with enough space reserved
       for the runtime compatibility diagnostics below it. */
    int icon_max = CAT_S(340);
    int icon_box = detail_w * 88 / 100;
    if (icon_box > icon_max)           icon_box = icon_max;
    if (icon_box > detail_h * 72 / 100) icon_box = detail_h * 72 / 100;
    int icon_height_limit = detail_h - text_h - gap - CAT_S(16);
    if (icon_box > icon_height_limit) icon_box = icon_height_limit;
    if (icon_box < 0) icon_box = 0;

    SDL_Texture *tex = NULL;
    int icon_w = 0, icon_h = 0;
    char icon_abs[PATH_MAX];
    if (jw__resolve_app_icon_path(state, app, icon_abs, sizeof(icon_abs)) == 0) {
        tex = jw__load_cached_image(icon_abs, &icon_w, &icon_h);
    }
    if (!tex) {
        /* Apps that ship no icon fall back to the Leaf badge. */
        tex = jw__load_system_icon("_apps", &icon_w, &icon_h);
    }

    /* Vertical stack: icon + runtime metadata, centered in the pane. */
    int block_h = (tex && icon_box > 0 ? icon_box + gap : 0) + text_h;
    int top_y   = detail_y + (detail_h - block_h) / 2;

    int label_y = top_y;
    if (tex && icon_box > 0) {
        jw__draw_image_fit(tex, icon_w, icon_h,
                           detail_x + (detail_w - icon_box) / 2, top_y,
                           icon_box, icon_box);
        label_y = top_y + icon_box + gap;
    }

    int max_w  = detail_w - CAT_S(16) * 2;
    int name_w = cat_measure_text(small, app->name);
    if (name_w > max_w) name_w = max_w;
    cat_draw_text_ellipsized(small, app->name,
                             detail_x + (detail_w - name_w) / 2, label_y,
                             theme->hint, max_w);
    label_y += sub_h + line_gap;

    char metadata[160];
    if (app->pak_version[0]) {
        snprintf(metadata, sizeof(metadata), "Version %s",
                 app->pak_version);
        int metadata_w = cat_measure_text(small, metadata);
        if (metadata_w > max_w) metadata_w = max_w;
        cat_draw_text_ellipsized(
            small, metadata, detail_x + (detail_w - metadata_w) / 2,
            label_y, theme->hint, max_w);
        label_y += sub_h + line_gap;
    }
    if (app->min_leaf_version[0]) {
        snprintf(metadata, sizeof(metadata), "Requires Leaf v%s",
                 app->min_leaf_version);
        int metadata_w = cat_measure_text(small, metadata);
        if (metadata_w > max_w) metadata_w = max_w;
        cat_draw_text_ellipsized(
            small, metadata, detail_x + (detail_w - metadata_w) / 2,
            label_y, theme->highlight, max_w);
    }
}

/* ─── Coverflow: shared album-card carousel ──────────────────────────────────
 * The carousel driver (jw_cf_draw_cards), the per-section metrics table
 * (jw_cf_layouts[]), and the icon-callback typedef (jw_cf_icon_fn) live in
 * internal/launcher/coverflow.h. The icon providers below feed that callback;
 * they stay here because they reach into launcher state. Forward-declared so the
 * channel renderer (used higher up) can see them. */
static SDL_Texture *jw__cf_icon_system(void *ctx, int idx, int *tw, int *th);
static SDL_Texture *jw__cf_icon_favorite(void *ctx, int idx, int *tw, int *th);
static SDL_Texture *jw__cf_icon_recent(void *ctx, int idx, int *tw, int *th);
static SDL_Texture *jw__cf_icon_app(void *ctx, int idx, int *tw, int *th);
static void jw__cf_draw_channel(jw_launcher_state *state);
static void jw__cf_channel_begin(jw_launcher_state *state, int dir, const char *db_path);

/* ─── Coverflow (systems level): the console carousel, card-styled ──────────── */
static void jw__render_coverflow(jw_launcher_state *state) {
    SDL_Renderer *ren = cat_get_renderer();
    ap_theme *theme = cat_get_theme();
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);

    SDL_SetRenderDrawColor(ren, 6, 7, 9, 255);   /* glossy black stage (matches games) */
    SDL_RenderClear(ren);
    /* L1/R1 hold-repeat is set once per frame in jw__render_launcher (see
       jw__view_wants_shoulder_repeat); enabling it here too would re-clear/re-arm
       the deadline each frame and break held-shoulder flow. */

    /* Channel content: a cube-rotation transition while switching channels,
       otherwise the current channel's carousel. */
    if (state->cf.cube.active) {
        uint32_t el = SDL_GetTicks() - state->cf.cube.start_ms;
        if (el >= JW_CF_CUBE_MS) {
            state->cf.cube.active = false;
            jw__cf_draw_channel(state);
        } else {
            jw_cf_draw_cube(&state->cf.cube, el);
            cat_request_frame();
        }
    } else {
        jw__cf_draw_channel(state);
    }

    /* Tools overlay */
    if (state->tools_open)
        jw__draw_tools_menu(state);

    /* Settings overlay */
    if (jw_settings_ui_is_open(&state->settings)) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_Rect full = { 0, 0, sw, sh };
        SDL_RenderFillRect(ren, &full);

        int sb_h = CAT_DS(20);
        int ox = sw / 6;
        int ow = sw - ox * 2;
        int oy = sb_h + CAT_S(8);
        int oh = sh - oy - fh - CAT_S(8);
        cat_draw_rounded_rect(ox, oy, ow, oh, CAT_S(8), theme->background);
        jw_settings_ui_render(&state->settings,
                               ox + CAT_S(12), oy + CAT_S(8),
                               ow - CAT_S(24), oh - CAT_S(16));

        cat_footer_item footer[] = {
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    } else if (!state->tools_open && !state->cf.cube.active &&
               state->visible_tab_count > 1) {
        /* Chrome-light home: no footer hints; a small ▲▼ in the corner cues the
           Up/Down channel switch. */
        jw_cf_draw_channel_hint();
    }
    jw__present();
}

/* ─── Coverflow (games level): angled album cards + floor reflection ──────────
 * Prototype. Renders the drilled-in system's games as a Cover Flow carousel:
 * uniform album cards drawn as perspective quads (SDL_RenderGeometry) on a
 * glossy-black stage, each mirrored into a fading floor reflection. Reuses the
 * cover thumbnail cache + background prewarm, and a continuous exponential
 * approach on the game_list cursor. Active only when the launcher layout is
 * coverflow. */

/* CF-local math (jw_cf_clampf, jw_cf_ease_in_out_cubic, jw_cf_anim_ms), the
   carousel tween (jw_games_cf_step / jw_games_cf_renormalize) and the card
   renderer (jw_cf_draw_card) live in internal/launcher/coverflow.c. */

/* Box art for a game entry (favorites / recents / drilled games) via the thumb
   cache; NULL while it streams in or has no art. */
static SDL_Texture *jw__cf_cover(jw_launcher_state *state, const jw_game_entry *g,
                                 int *tw, int *th) {
    *tw = 0; *th = 0;
    bool pending = false;
    if (g->image_path[0]) {
        char abs[PATH_MAX];
        if (jw__resolve_sdcard_path(state, g->image_path, abs, sizeof(abs)) == 0) {
            SDL_Texture *t = jw__load_cover(state, abs, tw, th, &pending);
            if (t) return t;
        }
    }
    if (pending) return NULL;                         /* streaming in — leave blank */
    return jw__load_system_icon(g->system, tw, th);   /* no cover -> system icon */
}

/* Per-channel icon sources (Games=systems, Favorites/Recents=box art, Apps=icons).
   These match jw_cf_icon_fn: ctx is the launcher state (opaque to the CF module). */
static SDL_Texture *jw__cf_icon_game(void *ctx, int idx, int *tw, int *th) {
    jw_launcher_state *state = (jw_launcher_state *)ctx;
    return jw__cf_cover(state, &state->games[idx], tw, th);
}
static SDL_Texture *jw__cf_icon_favorite(void *ctx, int idx, int *tw, int *th) {
    jw_launcher_state *state = (jw_launcher_state *)ctx;
    return jw__cf_cover(state, &state->favorites[idx], tw, th);
}
static SDL_Texture *jw__cf_icon_recent(void *ctx, int idx, int *tw, int *th) {
    jw_launcher_state *state = (jw_launcher_state *)ctx;
    return jw__cf_cover(state, &state->recents[idx], tw, th);
}
static SDL_Texture *jw__cf_icon_system(void *ctx, int idx, int *tw, int *th) {
    jw_launcher_state *state = (jw_launcher_state *)ctx;
    /* Games channel: idx is a system index. */
    const char *path = jw__cf_system_icon_path(state, idx);
    return jw__load_coverflow_image(path, tw, th);
}
static SDL_Texture *jw__cf_icon_app(void *ctx, int idx, int *tw, int *th) {
    jw_launcher_state *state = (jw_launcher_state *)ctx;
    *tw = 0; *th = 0;
    const jw_app_entry *app = &state->apps[idx];
    char abs[PATH_MAX];
    if (jw__resolve_app_icon_path(state, app, abs, sizeof(abs)) == 0)
        return jw__load_coverflow_image(abs, tw, th);
    return NULL;
}

/* ── Coverflow channels: draw + snapshot + cube-rotation transition ─────────── */

/* Draw the current channel's carousel + labels into the active render target. */
static void jw__cf_draw_channel(jw_launcher_state *state) {
    int sw = cat_get_screen_width();
    int count = jw__tab_list_count(state);

    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    cat_draw_color white = { 236, 238, 240, 255 };
    const char *chan = T(kTabs[state->current_tab]);
    int chw = cat_measure_text(small, chan);
    int cy0 = CAT_S(6);
    cat_draw_text(small, chan, (sw - chw) / 2, cy0, white);   /* channel name, always */

    if (count <= 0) return;

    int cur = state->list.cursor;
    jw_tab tab = state->current_tab;
    jw_cf_icon_fn icon_fn = jw__cf_icon_system;
    /* Pick this channel's icon source + layout profile (see jw_cf_layouts). */
    jw_cf_section sec = JW_CF_SECTION_SYSTEMS;
    if (tab == JW_TAB_FAVORITES)    { icon_fn = jw__cf_icon_favorite; sec = JW_CF_SECTION_FAVORITES; }
    else if (tab == JW_TAB_RECENTS) { icon_fn = jw__cf_icon_recent;   sec = JW_CF_SECTION_RECENTS; }
    else if (tab == JW_TAB_APPS)    { icon_fn = jw__cf_icon_app;      sec = JW_CF_SECTION_APPS; }
    jw__cf_animating |= jw_cf_draw_cards(&state->cf.systems_cf, count, cur, icon_fn,
                                         state, jw_cf_layouts[sec]);

    char labelbuf[192];
    const char *label = "";
    if (cur >= 0 && cur < count) {
        if (tab == JW_TAB_GAMES)      label = state->systems[cur].display_name;
        else if (tab == JW_TAB_APPS)  label = state->apps[cur].name;
        else {
            const jw_game_entry *g = (tab == JW_TAB_FAVORITES)
                                         ? &state->favorites[cur] : &state->recents[cur];
            jw__clean_rom_name(g->name, labelbuf, sizeof(labelbuf));
            label = labelbuf;
        }
    }
    TTF_Font *font = cat_get_font(CAT_FONT_LARGE);
    int ty   = cy0 + TTF_FontHeight(small) + CAT_S(2);
    int maxw = sw - CAT_S(96);
    int lw   = cat_measure_text(font, label);
    int lx   = (sw - (lw < maxw ? lw : maxw)) / 2;
    if (lw > maxw) cat_draw_text_ellipsized(font, label, lx, ty, white, maxw);
    else           cat_draw_text(font, label, lx, ty, white);
}

/* Snapshot the current channel into `tex` (a render-target texture). */
static void jw__cf_capture_channel(jw_launcher_state *state, SDL_Texture *tex) {
    SDL_Renderer *ren = cat_get_renderer();
    SDL_SetRenderTarget(ren, tex);
    SDL_SetRenderDrawColor(ren, 6, 7, 9, 255);
    SDL_RenderClear(ren);
    jw__cf_draw_channel(state);
    SDL_SetRenderTarget(ren, NULL);
}

/* The cube compositor (jw_cf_draw_cube) lives in internal/launcher/coverflow.c;
   the launcher only fills its snapshots (below) and drives the transition. */

/* Begin a channel switch: snapshot outgoing, switch, snapshot incoming, and start
   the cube rotation. Falls back to an instant switch if targets are unavailable. */
static void jw__cf_channel_begin(jw_launcher_state *state, int dir, const char *db_path) {
    SDL_Renderer *ren = cat_get_renderer();
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    if (!state->cf.cube.out_tex) {
        state->cf.cube.out_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                                     SDL_TEXTUREACCESS_TARGET, sw, sh);
        state->cf.cube.in_tex  = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                                     SDL_TEXTUREACCESS_TARGET, sw, sh);
    }
    if (!state->cf.cube.out_tex || !state->cf.cube.in_tex) {
        jw__switch_tab(state, dir, db_path);           /* fallback: instant switch */
        state->cf.systems_cf.inited = false;
        return;
    }
    jw__cf_capture_channel(state, state->cf.cube.out_tex);
    jw__switch_tab(state, dir, db_path);
    state->cf.systems_cf.inited = false;
    jw__cf_capture_channel(state, state->cf.cube.in_tex);
    state->cf.cube.active   = true;
    state->cf.cube.dir      = dir;
    state->cf.cube.start_ms = SDL_GetTicks();
}

/* The ▲▼ channel-switch hint (jw_cf_draw_channel_hint) lives in the CF module. */

static bool jw__coverflow_frame_log_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("JAWAKA_FRAME_LOG");
        enabled = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return enabled != 0;
}

static void jw__coverflow_frame_log(uint32_t frame_start_ms, uint32_t render_done_ms) {
    static uint32_t last_frame_start_ms;
    static uint32_t report_start_ms;
    static int frames;
    static int slow_frames;
    static uint32_t max_start_delta_ms;
    static uint32_t max_render_ms;

    uint32_t render_ms = render_done_ms - frame_start_ms;
    uint32_t start_delta_ms = 0;
    if (last_frame_start_ms != 0) {
        start_delta_ms = frame_start_ms - last_frame_start_ms;
        if (start_delta_ms > max_start_delta_ms) max_start_delta_ms = start_delta_ms;
    }
    last_frame_start_ms = frame_start_ms;
    if (render_ms > max_render_ms) max_render_ms = render_ms;
    if (render_ms > 17 || start_delta_ms > 20) slow_frames++;
    frames++;

    if (report_start_ms == 0) report_start_ms = frame_start_ms;
    if (render_done_ms - report_start_ms >= 1000u) {
        cat_log("coverflow frame stats: frames=%d slow=%d max_delta_ms=%u max_render_ms=%u",
                frames, slow_frames, max_start_delta_ms, max_render_ms);
        report_start_ms = render_done_ms;
        frames = 0;
        slow_frames = 0;
        max_start_delta_ms = 0;
        max_render_ms = 0;
    }
}

/* Draw the Cover Flow games stage — clear, card carousel, and the centered game
   title — WITHOUT presenting. Shared by the games view and the actions overlay
   (which dims this and floats the action list on top). */
static void jw__draw_coverflow_games_stage(jw_launcher_state *state) {
    SDL_Renderer *ren = cat_get_renderer();
    int sw = cat_get_screen_width();

    SDL_SetRenderDrawColor(ren, 6, 7, 9, 255);   /* glossy black stage */
    SDL_RenderClear(ren);

    int count = state->game_count;
    if (count <= 0) {
        return;
    }
    int cur = state->game_list.cursor;

    /* Prewarm covers around the cursor (reuses the game-grid pipeline). */
    jw__cover_prewarm(state, state->games, count, cur);

    jw__cf_animating |= jw_cf_draw_cards(&state->cf.games_cf, count, cur, jw__cf_icon_game,
                                         state, jw_cf_layouts[JW_CF_SECTION_GAMES]);

    /* Centred game title, near the top. Tags like " (USA)" / " [!]" stripped. */
    if (cur >= 0 && cur < count) {
        TTF_Font *font = cat_get_font(CAT_FONT_LARGE);
        char title[192];
        jw__clean_rom_name(state->games[cur].name, title, sizeof(title));
        int maxw = sw - CAT_S(96);
        int nw   = cat_measure_text(font, title);
        int tx   = (sw - (nw < maxw ? nw : maxw)) / 2;
        int ty   = CAT_S(12);
        cat_draw_color white = { 236, 238, 240, 255 };
        if (nw > maxw) cat_draw_text_ellipsized(font, title, tx, ty, white, maxw);
        else           cat_draw_text(font, title, tx, ty, white);
    }
}

static void jw__render_coverflow_games(jw_launcher_state *state) {
    bool frame_log = jw__coverflow_frame_log_enabled();
    uint32_t frame_start_ms = frame_log ? SDL_GetTicks() : 0;

    jw__draw_coverflow_games_stage(state);

    uint32_t render_done_ms = frame_log ? SDL_GetTicks() : 0;
    jw__present();
    if (frame_log) jw__coverflow_frame_log(frame_start_ms, render_done_ms);
}

/* Top header height of the system game browser: the tab bar + system-name
   sub-header in the tabbed layout, otherwise a single title row. Shared by the
   renderer and the visible-row count so the list never reports more rows than
   actually fit (which would run the last selection pill off the bottom). */
static int jw__game_browser_header_h(const jw_launcher_state *state) {
    (void)state;
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
        return cat_get_tab_bar_height() + CAT_S(2) +
               TTF_FontHeight(cat_get_font(CAT_FONT_EXTRA_LARGE));
    return CAT_DS(30);
}

/* Header height for the search page: tab bar + "Search:" sub-header in tabbed
   mode, or the standalone title bar otherwise. Shared by the renderer and the
   visible-row count so the box model and row count never disagree. */
static int jw__search_header_h(const jw_launcher_state *state) {
    (void)state;
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
        return cat_get_tab_bar_height() + CAT_S(2) +
               TTF_FontHeight(cat_get_font(CAT_FONT_EXTRA_LARGE));
    return CAT_DS(34);
}

static void jw__render_game_browser(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int margin = CAT_S(12);

    /* In the tabbed layout, show the section tabs across the top (current
       section highlighted) so the user can tab to any section from within a
       system's game list, with the status icons inline in the tab bar — the
       same header as the tabbed home view. The system name drops to a
       sub-header beneath the tabs. Other layouts keep the standalone title +
       full status bar. */
    bool tabbed = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED);
    int header_h;
    int title_y;
    int title_max;
    if (tabbed) {
        /* Same tab bar + inline status as the home view (shared helper, so the
           chrome can't drift between this and the other tab-bar views). */
        int bar_h = jw__draw_tab_header(state);

        title_y   = bar_h + CAT_S(2);
        header_h  = jw__game_browser_header_h(state);
        title_max = sw - margin * 2;
    } else {
        jw__draw_status_bar(state);
        header_h = jw__game_browser_header_h(state);
        title_y  = CAT_S(6);
        /* Cap the title's visible width so it stops before the status bar
           (top-right). Width adapts when the user hides battery/wifi/clock. */
        cat_status_bar_opts title_sb = {0};
        jw_settings_status_bar_opts(&state->settings, &title_sb);
        title_max = sw - cat_get_status_bar_width(&title_sb) - margin * 3;
        if (title_max < CAT_S(120)) title_max = CAT_S(120);
    }

    int region_y = header_h;
    int region_h = cat_get_screen_height() - header_h - jw__footer_height(state);

    char title[96];
    if (state->focus_pick_active)
        snprintf(title, sizeof(title), "%s   %d / %d", state->game_system_display,
                 state->focus_setup_count, JW_FOCUS_SCREEN_MAX_TILES);
    else if (state->games_are_favorites)
        snprintf(title, sizeof(title), "%s", "Favorites");
    else
        snprintf(title, sizeof(title), "%s", state->game_system_display);

    /* A title longer than title_max scrolls (looping marquee) instead of
       truncating, so the full system name is always readable. State is
       function-static (one browser at a time) and resets when the title
       changes. */
    {
        static cat_marquee title_marquee;
        static char        last_title[96] = "";
        static uint32_t    last_ms = 0;
        uint32_t now = SDL_GetTicks();
        if (strcmp(title, last_title) != 0) {
            title_marquee.elapsed_ms = 0;
            snprintf(last_title, sizeof(last_title), "%s", title);
            last_ms = now;
        }
        uint32_t dt = (last_ms == 0) ? 0u : (now - last_ms);
        last_ms = now;
        if (cat_draw_text_marquee(large, title, margin, title_y, theme->text,
                                  title_max, &title_marquee, dt))
            cat_request_frame();
    }

    (void)region_h;
    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, region_y, state->game_count,
                     &state->game_list, &list, &image, &item_h);

    if (state->game_count == 0) {
        cat_draw_text_wrapped(body, T("No games found"),
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__roms_ctx ctx = { state->games, state };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->game_count, &state->game_list, item_h,
            jw__draw_rom_item, &ctx);
    }

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->game_count > 0 && state->game_list.cursor < state->game_count) {
        const jw_game_entry *game = &state->games[state->game_list.cursor];
        jw__cover_prewarm(state, state->games, state->game_count,
                          state->game_list.cursor);
        /* Show the cover centered/fit in the panel — no name (it's in the list
           on the left). When a game has no cover, fall back to the system icon
           as a placeholder so the panel is never empty. */
        int art_pad = CAT_S(16);
        char image_abs[PATH_MAX];
        int iw = 0, ih = 0;
        SDL_Texture *tex = NULL;
        bool is_cover = false;
        bool pending = false;
        if (jw__resolve_sdcard_path(state, game->image_path, image_abs, sizeof(image_abs)) == 0) {
            tex = jw__load_cover(state, image_abs, &iw, &ih, &pending);
            is_cover = (tex != NULL);
        }
        if (!tex && !pending)            /* genuine no-cover; while pending leave empty */
            tex = jw__load_system_icon(game->system, &iw, &ih);
        if (tex) {
            /* Round real box art to match the list style; leave the icon
               fallback square. */
            if (is_cover)
                jw__draw_cover_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                                   image.w - art_pad * 2, image.h - art_pad * 2);
            else
                jw__draw_image_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                                   image.w - art_pad * 2, image.h - art_pad * 2);
        }
    }

    if (state->focus_pick_active) {
        cat_footer_item footer[] = {
            { CAT_BTN_Y,     "Clear",  false, JW_HINT("Y") },
            { CAT_BTN_B,     "Systems", false, JW_HINT("B") },
            { CAT_BTN_START, "Done",   true,  JW_HINT("Start") },
            { CAT_BTN_A,     "Select", true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (tabbed) {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 5);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    }
    jw__present();
}

/* Shared tabbed-tab content for game lists (Favorites, Recents): the list with
   star markers on the left and box art for the selected game on the right. */
static void jw__render_game_list_pane(const jw_launcher_state *state,
                                      const jw_game_entry *entries, int count,
                                      int content_y, int content_h, int margin,
                                      const char *empty_msg) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    (void)content_h;

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, content_y, count, &state->list,
                     &list, &image, &item_h);

    if (count == 0) {
        cat_draw_text_wrapped(body, empty_msg,
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
        return;
    }

    jw__roms_ctx ctx = { entries, state };
    cat_draw_list_pane(list.x, list.y, list.w, list.h,
        count, &state->list, item_h, jw__draw_rom_item, &ctx);

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->list.cursor >= count) return;
    const jw_game_entry *game = &entries[state->list.cursor];
    jw__cover_prewarm(state, entries, count, state->list.cursor);

    /* Cover centered/fit in the panel — no name (it's in the list). When a game
       has no cover, fall back to its system icon so the panel is never empty. */
    int art_pad = CAT_S(16);
    char image_abs[PATH_MAX];
    int iw = 0, ih = 0;
    SDL_Texture *tex = NULL;
    bool is_cover = false;
    bool pending = false;
    if (jw__resolve_sdcard_path(state, game->image_path, image_abs, sizeof(image_abs)) == 0) {
        tex = jw__load_cover(state, image_abs, &iw, &ih, &pending);
        is_cover = (tex != NULL);
    }
    if (!tex && !pending)            /* genuine no-cover; while pending leave empty */
        tex = jw__load_system_icon(game->system, &iw, &ih);
    if (tex) {
        /* Round real box art to match the list style; icon fallback stays square. */
        if (is_cover)
            jw__draw_cover_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                               image.w - art_pad * 2, image.h - art_pad * 2);
        else
            jw__draw_image_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                               image.w - art_pad * 2, image.h - art_pad * 2);
    }
}

static void jw__render_favorites(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    jw__render_game_list_pane(state, state->favorites, state->favorites_count,
                              content_y, content_h, margin,
                              "No favorites yet — open a game and press Y to add one");
}

static void jw__render_app_browser(const jw_launcher_state *state) {
    cat_clear_screen();
    jw__draw_status_bar(state);

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int margin = CAT_S(12);
    int header_h = CAT_DS(30);
    int content_y = header_h + margin;
    int content_h = sh - content_y - fh - margin;

    cat_draw_text_ellipsized(large, T("Apps"), margin, CAT_S(6),
                             theme->text, sw - margin * 2);

    int list_x = margin;
    int list_w = sw * 58 / 100;
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int detail_x = list_x + list_w + margin;
    int detail_w = sw - detail_x - margin;

    if (state->app_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? T("No apps found") : T("Scanning library..."),
            list_x + CAT_S(8), content_y + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__apps_ctx ctx = { state->apps };
        cat_draw_list_pane(list_x, content_y, list_w, content_h,
            state->app_count, &state->app_list, item_h,
            jw__draw_app_item, &ctx);
    }

    if (state->app_count > 0 && state->app_list.cursor < state->app_count) {
        jw__draw_app_detail(state, &state->apps[state->app_list.cursor],
                            detail_x, content_y, detail_w, content_h);
    } else {
        cat_draw_rounded_rect(detail_x, content_y, detail_w, content_h, CAT_S(8),
            cat_hex_to_color("#ffffff10"));
    }

    int status_y = content_y + content_h - TTF_FontHeight(small);
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, sw - margin * 2);

    cat_footer_item footer[] = {
        { CAT_BTN_X,  "Search",   false, JW_HINT("X") },
        { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
        { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
    };
    jw__footer_direct(footer, 3);
    jw__present();
}

static void jw__render_search(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    /* TTF_Font *small = cat_get_font(CAT_FONT_SMALL); — for the commented-out results line below */
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int margin = CAT_S(12);

    /* Same header as the home tabs / game browser: in tabbed layout, the section
       tab bar with status icons inline, and "Search: <query>" as a sub-header
       beneath it (so L1/R1 can tab away). Other layouts keep the standalone
       status pill + title. */
    bool tabbed = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED);
    int title_y;
    if (tabbed) {
        title_y = jw__draw_tab_header(state) + CAT_S(2);
    } else {
        jw__draw_status_bar(state);
        title_y = CAT_S(6);
    }
    int region_y = jw__search_header_h(state);   /* shared with jw__search_visible_rows */
    (void)fh; (void)sh;

    char title[320];
    if (state->search_query[0])
        snprintf(title, sizeof(title), "Search: %s  (%d %s)", state->search_query,
                 state->search_count, state->search_count == 1 ? "result" : "results");
    else
        snprintf(title, sizeof(title), "%s", "Search: (empty)");
    cat_draw_text_ellipsized(large, title, margin, title_y, theme->text, sw - margin * 2);

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, region_y, state->search_count,
                     &state->search_list, &list, &image, &item_h);

    if (state->search_count == 0) {
        cat_draw_text_wrapped(body, T("No results"),
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__search_ctx ctx = { state->search_results };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->search_count, &state->search_list, item_h,
            jw__draw_search_item, &ctx);
    }

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->search_count > 0 && state->search_list.cursor < state->search_count) {
        const jw_search_result *result = &state->search_results[state->search_list.cursor];

        /* Cover art only — the name is already in the list rows. Game cover (its
           system icon as a fallback) or an app's icon, centered/fit in the pane,
           exactly like the Recents/Favorites detail panes. */
        int art_pad = CAT_S(16);
        int art_x   = image.x + art_pad;
        int art_y   = image.y + art_pad;
        int art_w   = image.w - art_pad * 2;
        int art_h   = image.h - art_pad * 2;

        char img_abs[PATH_MAX];
        int iw = 0, ih = 0;
        SDL_Texture *tex = NULL;
        bool is_cover = false;
        bool pending = false;
        if (result->kind == JW_SEARCH_GAME) {
            if (result->image_path[0] &&
                jw__resolve_sdcard_path(state, result->image_path, img_abs, sizeof(img_abs)) == 0) {
                tex = jw__load_cover(state, img_abs, &iw, &ih, &pending);
                is_cover = (tex != NULL);
            }
            if (!tex && !pending)        /* genuine no-cover; while pending leave empty */
                tex = jw__load_system_icon(result->system, &iw, &ih);
        } else {
            jw_app_entry app;
            memset(&app, 0, sizeof(app));
            snprintf(app.pak_dir, sizeof(app.pak_dir), "%s", result->pak_dir);
            snprintf(app.icon, sizeof(app.icon), "%s", result->icon);
            if (jw__resolve_app_icon_path(state, &app, img_abs, sizeof(img_abs)) == 0)
                tex = jw__load_cached_image(img_abs, &iw, &ih);
        }
        if (tex) {
            /* Round real box art to match the list style; icons stay square. */
            if (is_cover)
                jw__draw_cover_fit(tex, iw, ih, art_x, art_y, art_w, art_h);
            else
                jw__draw_image_fit(tex, iw, ih, art_x, art_y, art_w, art_h);
        }
    }

    /* Results-count ("N results") line at the bottom — commented out for now per request.
    int status_y = content_y + content_h - TTF_FontHeight(small);
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, sw - margin * 2);
    */

    cat_footer_item footer[] = {
        { CAT_BTN_X,  "Search",   false, JW_HINT("X") },
        { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
        { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
        { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
    };
    jw__draw_footer(state, footer, 4);
    jw__present();
}

/* Card art for a search result: game box art (system-icon fallback) or app icon.
   Matches the jw_cf_icon_fn signature so the shared CF carousel can draw it. */
static SDL_Texture *jw__cf_icon_search(void *ctx, int idx, int *tw, int *th) {
    jw_launcher_state *state = (jw_launcher_state *)ctx;
    *tw = 0; *th = 0;
    const jw_search_result *sr = &state->search_results[idx];
    char abs[PATH_MAX];
    if (sr->kind == JW_SEARCH_APP) {
        jw_app_entry app;
        memset(&app, 0, sizeof(app));
        snprintf(app.pak_dir, sizeof(app.pak_dir), "%s", sr->pak_dir);
        snprintf(app.icon, sizeof(app.icon), "%s", sr->icon);
        if (jw__resolve_app_icon_path(state, &app, abs, sizeof(abs)) == 0)
            return jw__load_cached_image(abs, tw, th);
        return NULL;
    }
    bool pending = false;
    if (sr->image_path[0] &&
        jw__resolve_sdcard_path(state, sr->image_path, abs, sizeof(abs)) == 0) {
        SDL_Texture *t = jw__load_cover(state, abs, tw, th, &pending);
        if (t) return t;
    }
    if (pending) return NULL;                       /* streaming in — leave blank */
    return jw__load_system_icon(sr->system, tw, th); /* no cover -> system icon */
}

/* ── Cover Flow inline search keyboard ─────────────────────────────────────── */
/* Lowercase only — library search is case-insensitive, so no shift is needed.
   Row 4 is the action row: a wide Space and a Del (backspace). */
static const char *const kCfKbRows[] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
#define JW_CF_KB_LETTER_ROWS 4
#define JW_CF_KB_ROWS        5
static int  s_cf_kb_row = 1, s_cf_kb_col = 0;
/* Focus zone: false = the on-screen keyboard, true = the results carousel.
   Up off the keyboard's top row jumps into the results; Down (or B) drops back. */
static bool  s_cf_focus_results = false;
/* Eased 0..1 driver for the focus transition (0 = keyboard, 1 = results): the
   keyboard slides down out of frame while the result cards grow and drop into the
   vacated space. Stepped each render toward the s_cf_focus_results target. */
static float s_cf_focus_t = 0.0f;

static float jw__lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Advance s_cf_focus_t toward its target with an ease-out approach; request another
   frame while it is still moving. Returns true while animating. */
static bool jw__cf_focus_step(void) {
    float target = s_cf_focus_results ? 1.0f : 0.0f;
    if (s_cf_focus_t == target) return false;
    s_cf_focus_t += (target - s_cf_focus_t) * 0.30f;      /* exponential ease-out */
    if (fabsf(target - s_cf_focus_t) < 0.004f) s_cf_focus_t = target;
    cat_request_frame();
    return true;
}

static int jw__perform_search(const char *db_path, jw_launcher_state *state, const char *query);
static int jw__launch_selected_search_result(const char *socket_path,
                                             jw_launcher_state *state, bool *running);
static void jw__favorite_selected_search_result(const char *db_path,
                                                 jw_launcher_state *state);

static int jw__cf_kb_row_len(int row) {
    if (row >= 0 && row < JW_CF_KB_LETTER_ROWS) return (int)strlen(kCfKbRows[row]);
    return 1;   /* action row: space (Delete is on B) */
}

/* Open the Cover Flow inline search overlay — no blocking system keyboard. */
static void jw__cf_open_search(jw_launcher_state *state, const char *db_path) {
    (void)db_path;
    state->search_query[0] = '\0';
    state->search_count    = 0;
    state->search_open     = true;
    s_cf_kb_row = 1;   /* start on 'q' */
    s_cf_kb_col = 0;
    s_cf_focus_results = false;
    s_cf_focus_t = 0.0f;
    s_search_prewarm_cursor = -1;
    s_search_cf_reset = true;
    cat_request_frame();
}

/* CF search uses a fixed selection color, not the per-theme accent, so the look
   stays consistent with Cover Flow's black/white stage across all schemes. Soft
   platinum gray: bright enough to read as "selected" on the dark keys and the
   near-black stage, used for both the highlighted key and the focused title. */
static const cat_draw_color kCfSelect = { 182, 189, 199, 255 };

static void jw__cf_draw_keyboard(const jw_launcher_state *state) {
    (void)state;
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    TTF_Font *font = cat_get_font(CAT_FONT_MEDIUM);
    cat_draw_color white   = { 236, 238, 240, 255 };
    cat_draw_color keybg   = { 30, 34, 42, 235 };
    cat_draw_color sel     = kCfSelect;
    cat_draw_color seltext = { 12, 14, 18, 255 };

    /* Slide the whole keyboard down and out of frame as focus moves to the
       results (s_cf_focus_t: 0 = seated, 1 = fully below the bottom edge). */
    if (s_cf_focus_t >= 0.995f) return;
    int kb_top = (int)(sh * 0.56f);
    int kb_bot = sh - CAT_S(14);
    int slide  = (int)(s_cf_focus_t * (float)(sh - kb_top + CAT_S(24)));
    kb_top += slide;
    kb_bot += slide;
    int gap    = CAT_S(6);
    int key_h  = (kb_bot - kb_top - gap * (JW_CF_KB_ROWS - 1)) / JW_CF_KB_ROWS;
    int side   = CAT_S(24);
    int key_w  = (sw - side * 2 - gap * 9) / 10;   /* sized to a 10-key row */
    int fh     = TTF_FontHeight(font);

    for (int rw = 0; rw < JW_CF_KB_ROWS; rw++) {
        int ky = kb_top + rw * (key_h + gap);
        if (rw < JW_CF_KB_LETTER_ROWS) {
            int rlen  = (int)strlen(kCfKbRows[rw]);
            int row_w = rlen * key_w + (rlen - 1) * gap;
            int kx    = (sw - row_w) / 2;
            for (int c = 0; c < rlen; c++, kx += key_w + gap) {
                bool k = !s_cf_focus_results && (rw == s_cf_kb_row && c == s_cf_kb_col);
                cat_draw_rounded_rect(kx, ky, key_w, key_h, CAT_S(6), k ? sel : keybg);
                char ch[2] = { kCfKbRows[rw][c], 0 };
                int cw = cat_measure_text(font, ch);
                cat_draw_text(font, ch, kx + (key_w - cw) / 2, ky + (key_h - fh) / 2,
                              k ? seltext : white);
            }
        } else {
            /* Action row: a single wide Space (Delete lives on B). */
            int w  = key_w * 7 + gap * 6;
            int kx = (sw - w) / 2;
            bool k = !s_cf_focus_results && (rw == s_cf_kb_row && s_cf_kb_col == 0);
            cat_draw_rounded_rect(kx, ky, w, key_h, CAT_S(6), k ? sel : keybg);
            int cw = cat_measure_text(font, T("space"));   /* must match the draw */
            cat_draw_text(font, T("space"), kx + (w - cw) / 2, ky + (key_h - fh) / 2,
                          k ? seltext : white);
        }
    }
}

/* One Cover Flow carousel step. A CF list only wraps when it has enough covers to
   fill the on-screen window (jw_cf_list_loops); a shorter list clamps at the ends
   so pressing past the last cover doesn't snap the row back to the other side.
   This mirrors the render-side clamp in jw_cf_draw_cards. Non-CF layouts always
   wrap (their rows have no such window). */
static void jw__cf_carousel_step(cat_list_state *s, int dir, int count, int layout) {
    if (layout == CAT_LAUNCHER_COVERFLOW && !jw_cf_list_loops(count))
        cat_list_state_jump(s, s->cursor + dir, count);   /* clamp at the ends */
    else
        cat_list_state_move(s, dir, count);               /* wrap around the ring */
}

static void jw__cf_kbd_input(const char *socket_path, const char *db_path,
                             jw_launcher_state *state, cat_button button, bool *running) {
    /* X closes search from anywhere — no need to empty the query first. */
    if (button == CAT_BTN_X) {
        state->search_open = false;
        state->status[0] = '\0';
        cat_request_frame();
        return;
    }

    /* Results can't hold focus once they're gone (e.g. after a backspace). */
    if (s_cf_focus_results && state->search_count <= 0) s_cf_focus_results = false;

    /* Focus in the results carousel: flow the cards, A launches, Down/B drops
       back to the keyboard. */
    if (s_cf_focus_results) {
        switch (button) {
            case CAT_BTN_DOWN:
            case CAT_BTN_B:
                s_cf_focus_results = false;
                cat_ui_feedback_emit(CAT_UI_MOVED);   /* zone hop back to the keys */
                break;
            case CAT_BTN_LEFT:
            case CAT_BTN_L1:
                jw__cf_carousel_step(&state->search_list, -1, state->search_count,
                                     CAT_LAUNCHER_COVERFLOW);
                break;
            case CAT_BTN_RIGHT:
            case CAT_BTN_R1:
                jw__cf_carousel_step(&state->search_list, +1, state->search_count,
                                     CAT_LAUNCHER_COVERFLOW);
                break;
            case CAT_BTN_A:
                jw__launch_selected_search_result(socket_path, state, running);
                break;
            case CAT_BTN_Y:
                jw__favorite_selected_search_result(db_path, state);
                break;
            default: break;
        }
        cat_request_frame();
        return;
    }

    /* Focus on the keyboard. */
    switch (button) {
        /* This keyboard is hand-rolled rather than a cat_keyboard, so it reports
           its own movement. Rows clamp; columns wrap within the row. */
        case CAT_BTN_UP:
            if (s_cf_kb_row > 0) {
                s_cf_kb_row--;
                cat_ui_feedback_emit(CAT_UI_MOVED);
            } else if (state->search_count > 0) {
                s_cf_focus_results = true;                /* jump to results */
                cat_ui_feedback_emit(CAT_UI_MOVED);
            } else {
                cat_ui_feedback_emit(CAT_UI_EDGE);        /* top row, no results */
            }
            break;
        case CAT_BTN_DOWN:
            cat_ui_feedback_emit(s_cf_kb_row < JW_CF_KB_ROWS - 1 ? CAT_UI_MOVED
                                                                 : CAT_UI_EDGE);
            if (s_cf_kb_row < JW_CF_KB_ROWS - 1) s_cf_kb_row++;
            break;
        case CAT_BTN_LEFT: {   /* wrap within the row */
            int rl = jw__cf_kb_row_len(s_cf_kb_row);
            s_cf_kb_col = (s_cf_kb_col - 1 + rl) % rl;
            cat_ui_feedback_emit(CAT_UI_MOVED);
            break;
        }
        case CAT_BTN_RIGHT: {
            int rl = jw__cf_kb_row_len(s_cf_kb_row);
            s_cf_kb_col = (s_cf_kb_col + 1) % rl;
            cat_ui_feedback_emit(CAT_UI_MOVED);
            break;
        }
        case CAT_BTN_A: {   /* type the selected key, then re-run the search live */
            size_t len = strlen(state->search_query);
            bool edited = false;
            if (s_cf_kb_row < JW_CF_KB_LETTER_ROWS) {
                if (len + 1 < sizeof(state->search_query)) {
                    state->search_query[len]     = kCfKbRows[s_cf_kb_row][s_cf_kb_col];
                    state->search_query[len + 1] = '\0';
                    edited = true;
                }
            } else {                                       /* space */
                if (len + 1 < sizeof(state->search_query)) {
                    state->search_query[len]     = ' ';
                    state->search_query[len + 1] = '\0';
                    edited = true;
                }
            }
            cat_ui_feedback_emit(edited ? CAT_UI_ENTERED : CAT_UI_EDGE);
            if (edited) {
                char q[256];
                snprintf(q, sizeof(q), "%s", state->search_query);
                jw__perform_search(db_path, state, q);
            }
            break;
        }
        case CAT_BTN_B: {   /* backspace, or close when the query is empty */
            size_t len = strlen(state->search_query);
            if (len > 0) {
                state->search_query[len - 1] = '\0';
                cat_ui_feedback_emit(CAT_UI_ENTERED);
                char q[256];
                snprintf(q, sizeof(q), "%s", state->search_query);
                jw__perform_search(db_path, state, q);
            } else {
                state->search_open = false;
                state->status[0] = '\0';
            }
            break;
        }
        default: break;
    }
    int rl = jw__cf_kb_row_len(s_cf_kb_row);
    if (s_cf_kb_col > rl - 1) s_cf_kb_col = rl - 1;
    if (s_cf_kb_col < 0)      s_cf_kb_col = 0;
    cat_request_frame();
}

/* Cover Flow search results: the query header + a card carousel of hits (games
   and apps) on the glossy black stage, with the inline keyboard below. */
static void jw__render_search_cf(jw_launcher_state *state) {
    static jw_games_cf s_cf;
    if (s_search_cf_reset) {
        s_cf.inited = false;   /* re-seat to the new cursor (jw__cf_draw_cards re-inits) */
        s_search_cf_reset = false;
    }
    SDL_Renderer *r = cat_get_renderer();
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    cat_draw_color white = { 236, 238, 240, 255 };
    cat_draw_color dim   = { 150, 154, 160, 255 };

    SDL_SetRenderDrawColor(r, 6, 7, 9, 255);
    SDL_RenderClear(r);

    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);

    char hdr[320];
    if (state->search_query[0])
        snprintf(hdr, sizeof(hdr), "Search: %s", state->search_query);
    else
        snprintf(hdr, sizeof(hdr), "%s", "Search");
    int maxhw = sw - CAT_S(40);
    int hw = cat_measure_text(small, hdr);
    int hx = (sw - (hw < maxhw ? hw : maxhw)) / 2;
    cat_draw_text_ellipsized(small, hdr, hx, CAT_S(6), white, maxhw);

    if (state->search_count <= 0) {
        const char *msg = state->search_query[0] ? T("No results") : T("Type to search");
        int mw = cat_measure_text(large, msg);
        cat_draw_text(large, msg, (sw - mw) / 2, (int)(sh * 0.28f), dim);
    } else {
        int cur = state->search_list.cursor;
        if (cur < 0) cur = 0;
        if (cur >= state->search_count) cur = state->search_count - 1;

        char namebuf[256];
        jw__clean_rom_name(state->search_results[cur].name, namebuf, sizeof(namebuf));
        int ty   = CAT_S(6) + TTF_FontHeight(small) + CAT_S(2);
        int maxw = sw - CAT_S(96);
        int lw   = cat_measure_text(large, namebuf);
        int lx   = (sw - (lw < maxw ? lw : maxw)) / 2;
        /* When the carousel holds focus, the selected name switches to the same
           fixed selection color as the keyboard's highlighted key. */
        cat_draw_color name_col = s_cf_focus_results ? kCfSelect : white;
        if (lw > maxw) cat_draw_text_ellipsized(large, namebuf, lx, ty, name_col, maxw);
        else           cat_draw_text(large, namebuf, lx, ty, name_col);

        jw__cover_prewarm_search(state, state->search_results, state->search_count, cur);
        /* Cards grow and drop as focus moves from the keyboard (t=0: small, seated
           high above the keyboard) to the results (t=1: large, dropped into the
           space the keyboard vacates). */
        float ch = jw__lerpf(0.30f, 0.56f, s_cf_focus_t);
        float oy = jw__lerpf(0.31f, 0.45f, s_cf_focus_t);
        jw_cf_layout search_layout = jw_cf_layouts[JW_CF_SECTION_GAMES];
        search_layout.size = ch;                  /* animated: grows as focus drops in */
        search_layout.oy   = oy;
        jw__cf_animating |= jw_cf_draw_cards(&s_cf, state->search_count, cur,
                                             jw__cf_icon_search, state, search_layout);
    }

    jw__cf_draw_keyboard(state);
    jw__cf_focus_step();
    jw__present();
}

typedef struct { const jw_launcher_state *state; } jw__actions_ctx;

static const char *jw__action_core_label(const jw_launcher_state *state,
                                         const char *core_id);
static const char *jw__action_perf_label(const char *value);

static void jw__action_row_strings(const jw_launcher_state *state,
                                   jw_action_row_kind row,
                                   char *title, size_t title_size,
                                   char *value, size_t value_size) {
    if (title && title_size > 0) title[0] = '\0';
    if (value && value_size > 0) value[0] = '\0';
    switch (row) {
        case JW_ACTION_ROW_SEARCH:
            snprintf(title, title_size, "%s", "Search This System");
            snprintf(value, value_size, "%s", "Open");
            break;
        case JW_ACTION_ROW_DISPLAY_NAME: {
            snprintf(title, title_size, "%s", "Display Name");
            if (state->action_scope == JW_ACTION_SYSTEM) {
                const char *label = state->action_system_display[0]
                    ? state->action_system_display
                    : "Default";
                if (state->action_system_display_override[0]) {
                    snprintf(value, value_size, "%s (custom)", label);
                } else {
                    snprintf(value, value_size, "%s", label);
                }
            } else {
                char name[256];
                jw__clean_rom_name(state->action_game.name, name, sizeof(name));
                jw__str_copy(value, value_size, name[0] ? name : "Scanned");
            }
            break;
        }
        case JW_ACTION_ROW_CORE:
            snprintf(title, title_size, "%s", "Core");
            if (state->action_scope == JW_ACTION_GAME &&
                state->action_core_game_override[0]) {
                snprintf(value, value_size, "%s (game)",
                         jw__action_core_label(state, state->action_core_game_override));
            } else if (state->action_core_system_override[0]) {
                snprintf(value, value_size, "%s (system)",
                         jw__action_core_label(state, state->action_core_system_override));
            } else if (state->action_core_effective[0]) {
                snprintf(value, value_size, "%s (default)",
                         jw__action_core_label(state, state->action_core_effective));
            } else {
                snprintf(value, value_size, "%s", "Unavailable");
            }
            break;
        case JW_ACTION_ROW_PERFORMANCE:
            snprintf(title, title_size, "%s", "Performance");
            if (state->action_scope == JW_ACTION_GAME &&
                state->action_perf_game_override[0]) {
                snprintf(value, value_size, "%s (game)",
                         jw__action_perf_label(state->action_perf_game_override));
            } else if (state->action_perf_system_override[0]) {
                snprintf(value, value_size, "%s (system)",
                         jw__action_perf_label(state->action_perf_system_override));
            } else {
                snprintf(value, value_size, "%s", "Auto");
            }
            break;
        case JW_ACTION_ROW_SCRAPE:
            snprintf(title, title_size, "%s", "Scrape Artwork");
            snprintf(value, value_size, "%s", "Replace");
            break;
        case JW_ACTION_ROW_SCRAPE_CANCEL:
            snprintf(title, title_size, "%s", "Cancel Scraping");
            snprintf(value, value_size, "%s", "Stop");
            break;
        case JW_ACTION_ROW_RESET:
            snprintf(title, title_size, "%s",
                     state->action_scope == JW_ACTION_GAME
                         ? "Reset Game Overrides"
                         : "Reset System Overrides");
            snprintf(value, value_size, "%s", "Clear");
            break;
        default:
            break;
    }
}

static void jw__draw_action_item(int idx, int ix, int iy, int iw, int ih,
                                 bool selected, void *user) {
    jw__actions_ctx *ctx = (jw__actions_ctx *)user;
    const jw_launcher_state *state = ctx ? ctx->state : NULL;
    if (!state || idx < 0 || idx >= state->action_row_count) {
        return;
    }

    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int pill_h = TTF_FontHeight(body) + CAT_S(8);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected) {
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);
    }

    char title[96];
    char value[160];
    jw__action_row_strings(state, state->action_rows[idx],
                           title, sizeof(title), value, sizeof(value));

    ap_color title_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    int value_w = iw * 42 / 100;
    int title_w = iw - value_w - CAT_S(28);

    cat_draw_text_ellipsized(body, title, ix + CAT_S(10), text_y,
                             title_c, title_w);
    if (value[0]) {
        int value_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
        cat_draw_text_ellipsized(small, value,
                                 ix + iw - value_w - CAT_S(12),
                                 value_y, value_c, value_w);
    }
}

/* Header height for the actions page: tab bar + name sub-header in tabbed
   mode, or the standalone title bar otherwise - same shape as search, shared
   by the renderer and the box model so they never disagree. */
static int jw__actions_header_h(void) {
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
        return cat_get_tab_bar_height() + CAT_S(2) +
               TTF_FontHeight(cat_get_font(CAT_FONT_EXTRA_LARGE));
    return CAT_DS(34);
}

/* Cover Flow styling for the game actions menu. The tabbed box-model panel
   clashes with Cover Flow's minimal look, so render the same action rows over
   the near-black CF backdrop with the platinum-gray selection — matching the CF
   search + keyboard. Full functionality, CF chrome. */
static void jw__render_actions_cf(jw_launcher_state *state) {
    SDL_Renderer *r = cat_get_renderer();
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    cat_draw_color white   = { 236, 238, 240, 255 };
    cat_draw_color dim     = { 150, 154, 160, 255 };
    cat_draw_color sel     = kCfSelect;
    cat_draw_color seltext = { 12, 14, 18, 255 };
    cat_draw_color selval  = { 58, 64, 72, 255 };

    /* Backdrop: the live Cover Flow game stage, dimmed — the menu floats over the
       game rather than replacing it. */
    if (state->action_scope == JW_ACTION_GAME) {
        jw__draw_coverflow_games_stage(state);
    } else {
        SDL_SetRenderDrawColor(r, 6, 7, 9, 255);
        SDL_RenderClear(r);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 172);
    SDL_RenderFillRect(r, NULL);

    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);

    int n = state->action_row_count;
    if (n > 0) {
        int cur = state->action_list.cursor;
        if (cur < 0) cur = 0;
        if (cur >= n) cur = n - 1;

        int row_h   = TTF_FontHeight(body) + CAT_S(16);
        int cell_h  = row_h - CAT_S(6);
        int col_w   = sw - CAT_S(72);
        int col_x   = (sw - col_w) / 2;
        int top     = CAT_S(12) + TTF_FontHeight(large) + CAT_S(16);
        int start_y = top + ((sh - top) - n * row_h) / 2;
        if (start_y < top) start_y = top;

        int val_w = col_w * 44 / 100;
        int ttl_w = col_w - val_w - CAT_S(20);

        for (int i = 0; i < n; i++) {
            char title[96], value[160];
            jw__action_row_strings(state, state->action_rows[i],
                                   title, sizeof(title), value, sizeof(value));
            bool s  = (i == cur);
            int  ry = start_y + i * row_h;
            if (s) {
                cat_draw_rounded_rect(col_x - CAT_S(14), ry,
                                      col_w + CAT_S(28), cell_h, CAT_S(8), sel);
            }
            int text_y = ry + (cell_h - TTF_FontHeight(body)) / 2;
            cat_draw_text_ellipsized(body, title, col_x, text_y,
                                     s ? seltext : white, ttl_w);
            if (value[0]) {
                int vw = cat_measure_text(small, value);
                int vx = col_x + col_w - (vw < val_w ? vw : val_w);
                int vy = ry + (cell_h - TTF_FontHeight(small)) / 2;
                cat_draw_text_ellipsized(small, value, vx, vy,
                                         s ? selval : dim, val_w);
            }
        }
    }

    jw__present();
}

static void jw__render_actions(const jw_launcher_state *state) {
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW) {
        jw__render_actions_cf((jw_launcher_state *)state);
        return;
    }
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int margin = CAT_S(12);

    /* Same header as the home tabs / game browser / search: in tabbed layout
       the section tab bar with the status icons inline, and the game or
       system name as a sub-header beneath it. The status bar never floats
       free in the tabbed world. Other layouts keep the standalone pill. */
    bool tabbed = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED);
    int title_y;
    if (tabbed) {
        title_y = jw__draw_tab_header(state) + CAT_S(2);
    } else {
        jw__draw_status_bar(state);
        title_y = CAT_S(6);
    }

    int title_max = sw - margin * 2;
    if (!tabbed) {
        cat_status_bar_opts title_sb = {0};
        jw_settings_status_bar_opts(&state->settings, &title_sb);
        title_max = sw - cat_get_status_bar_width(&title_sb) - margin * 3;
        if (title_max < CAT_S(120)) title_max = CAT_S(120);
    }

    char title[320];
    if (state->action_scope == JW_ACTION_GAME) {
        char name[256];
        jw__clean_rom_name(state->action_game.name, name, sizeof(name));
        snprintf(title, sizeof(title), "%s", name);
    } else {
        snprintf(title, sizeof(title), "System Options: %s",
                 state->action_system_display);
    }

    /* A title longer than title_max scrolls (looping marquee) instead of
       truncating, so the full game name is always readable - same treatment
       as the game browser's system sub-header. */
    {
        static cat_marquee title_marquee;
        static char        last_title[320] = "";
        static uint32_t    last_ms = 0;
        uint32_t now = SDL_GetTicks();
        if (strcmp(title, last_title) != 0) {
            title_marquee.elapsed_ms = 0;
            snprintf(last_title, sizeof(last_title), "%s", title);
            last_ms = now;
        }
        uint32_t dt = (last_ms == 0) ? 0u : (now - last_ms);
        last_ms = now;
        if (cat_draw_text_marquee(large, title, margin, title_y, theme->text,
                                  title_max, &title_marquee, dt))
            cat_request_frame();
    }

    /* Box model below the header: the rows fill the content box on the
       canonical grid - same pitch and filled geometry as every tab. */
    int header_h = jw__actions_header_h();
    int hint_pad = (fh > 0) ? margin : 0;
    cat_box page = { 0, header_h, sw, sh - header_h - fh - hint_pad,
                     margin, margin, 0, margin };

    /* No status toast in this view: a selection's feedback is the row value
       itself changing, and the name already lives in the sub-header. */
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&page, item_h, state->action_row_count,
                                   &vis, &item_h);
    ((cat_list_state *)&state->action_list)->visible_rows = vis;
    jw__actions_ctx ctx = { state };
    if (state->action_row_count > 0) {
        cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h,
                           state->action_row_count, &state->action_list,
                           item_h, jw__draw_action_item, &ctx);
    }

    /* No Left/Right hint: cycling a value row is discoverable and A cycles it
       too. The Tab hint only applies where the tab bar is shown (tabbed). */
    cat_footer_item footer[] = {
        { CAT_BTN_L1,   "Tab",    false, JW_HINT_DEVICE(";/t", "L1/R1") },
        { CAT_BTN_B,    "Back",   true,  JW_HINT("B") },
        { CAT_BTN_A,    "Select", true,  JW_HINT("A") },
    };
    cat_footer_item *footer_items = tabbed ? footer : footer + 1;
    jw__draw_footer(state, footer_items, tabbed ? 3 : 2);
    jw__present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Dedicated Select-opened switcher view: a focused recents carousel with its
   own title/footer, drawn over whatever home layout was active. */
static void jw__render_switcher(jw_launcher_state *state) {
    cat_clear_screen();

    cat_status_bar_opts sb = {0};
    jw_settings_status_bar_opts(&state->settings, &sb);
    cat_draw_screen_title(T("Switcher"), &sb);

    bool hints = jw_settings_show_hints(&state->settings);
    SDL_Rect content = cat_get_content_rect(true, hints, false);
    int margin = CAT_S(12);
    jw_game_switcher_render(&state->switcher,
                            content.x + margin, content.y,
                            content.w - margin * 2, content.h);

    /* No status line here: the switcher sets no status of its own, so drawing
       the shared state->status just leaks the last global message (e.g. a stale
       "Scrape finished" / scan count). The carousel already shows the selected
       game's title and system. */

    cat_footer_item footer[] = {
        { CAT_BTN_Y, "Remove", false, JW_HINT("Y") },
        { CAT_BTN_B, "Back",   true,  JW_HINT("B") },
        { CAT_BTN_A, "Resume", true,  JW_HINT("A") },
    };
    jw__draw_footer(state, footer, 3);
    jw__present();
}

/* ─── System menu overlay (MENU) ────────────────────────────────────────────
   Drawn over the live launcher; opening/closing is a state flag, so it's instant
   (no process respawn). Search / Rescan / power are direct calls + IPC; About and
   System Update are hosted modally via a settings UI (see jw__menu_host_setting). */
/* The System menu is split into two L1/R1 tabs: Actions (do-something items) and
   Info (read-only data pages). Search / Rescan / power are direct calls + IPC;
   Device (the About page) is hosted modally via a settings UI (jw__menu_host_setting). */
static const char *const kSysMenuTabs[] = { "Settings", "Actions", "Info" };
enum { JW_SMTAB_SETTINGS = 0, JW_SMTAB_ACTIONS, JW_SMTAB_INFO, JW_SMTAB_COUNT };

static const char *const kSysActions[] = {
    "Search", "Pak Rat", "System Update", "Rescan Library", "Start 5-Game Mode",
    "Sleep", "Exit to Stock", "Reboot", "Power Off",
};
enum { JW_SA_SEARCH = 0, JW_SA_PAKRAT, JW_SA_UPDATE, JW_SA_RESCAN, JW_SA_START_5GAME,
       JW_SA_SLEEP, JW_SA_EXIT_STOCK, JW_SA_REBOOT, JW_SA_POWEROFF, JW_SA_COUNT };

static const char *const kSysInfo[] = { "Device", "Library", "Playtime" };
enum { JW_SI_DEVICE = 0, JW_SI_LIBRARY, JW_SI_PLAYTIME, JW_SI_COUNT };

/* The active tab's item labels + count. */
static const char *const *jw__menu_tab_items(int tab, int *count) {
    if (tab == JW_SMTAB_INFO) { if (count) *count = JW_SI_COUNT; return kSysInfo; }
    if (count) *count = JW_SA_COUNT;
    return kSysActions;
}

static void jw__draw_menu_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    const char *const *items = (const char *const *)user;
    if (!items || idx < 0) return;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);
    ap_color c = selected ? theme->highlighted_text : theme->text;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, T(items[idx]), ix + CAT_S(10), text_y,
                             c, iw - CAT_S(20));
}

/* The System menu's top band: the Actions/Info tab bar drawn exactly like the
   browse pages' header (cat_draw_tab_bar fills the accent band; status icons
   inline). Drawn on the menu list AND the drilled-in Info/Update pages so the tab
   bar stays pinned at top — consistent with the content side keeping its tab bar.
   Returns the band height so callers can place content below it. */
static int jw__draw_menu_tab_bar(const jw_launcher_state *state) {
    int bar_h  = cat_get_tab_bar_height();
    int pill_h = CAT_DS(CAT__PILL_SIZE);
    cat_status_bar_opts sb = {0};
    jw_settings_status_bar_opts(&state->settings, &sb);
    sb.no_pill    = true;
    sb.use_y      = true;
    sb.y_position = (bar_h - pill_h) / 2;
    cat_set_tab_bar_reserved_right(cat_get_status_bar_width(&sb) + CAT_S(12));
    {
        /* Translated per frame into a local: the array itself must stay English
           because it is also the extraction source and T() keys. */
        const char *tabs[JW_SMTAB_COUNT];
        for (int i = 0; i < JW_SMTAB_COUNT; i++) tabs[i] = T(kSysMenuTabs[i]);
        cat_draw_tab_bar(tabs, JW_SMTAB_COUNT, state->menu_tab);
    }
    cat_draw_status_bar(&sb);
    return bar_h;
}

static void jw__render_menu(const jw_launcher_state *state) {
    cat_clear_screen();
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    /* The tab bar keeps the list/panel boxes — and the gap above them —
       pixel-aligned with Recents/Favorites/Games/etc. */
    int header_h = jw__draw_menu_tab_bar(state);

    /* Settings tab: render the real settings UI under the tab bar. It owns its own
       sub-navigation, per-screen priming (Network/Bluetooth/Display) and live
       polling (run from the main loop, gated on jw_settings_ui_wants_*), so this is
       the same machinery the old Settings home tab used — just hosted here. */
    if (state->menu_tab == JW_SMTAB_SETTINGS) {
        int margin    = CAT_S(12);
        int content_h = cat_get_screen_height() - header_h - jw__footer_height(state);
        jw__render_settings(state, header_h, content_h, margin);
        jw__draw_settings_footer(state);
        jw__present();
        return;
    }

    /* Same box geometry as the browse pages: a 58% list on the left, a panel on the
       right. The right panel is the library-counts card. */
    /* Size rows to the base-height fit count (what the browse pages use), NOT the
       item count — otherwise fit_rows divides the column by the item count and
       stretches the rows (taller pitch + a deeper panel inset via pad_v). A short
       list (either tab) then sits at the same pitch as the browse pages, top-aligned. */
    int tab_count;
    const char *const *items = jw__menu_tab_items(state->menu_tab, &tab_count);
    ((cat_list_state *)&state->menu_list)->visible_rows =
        jw__browse_visible_rows(state, header_h);
    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, header_h, tab_count,
                     &state->menu_list, &list, &image, &item_h);

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));
    {
        int line_h = TTF_FontHeight(body);
        int ty = image.y + (image.h - line_h) / 2;
        int tx = image.x + CAT_S(10);
        int tw = image.w - CAT_S(20);
        if (state->menu_scanning || state->scan_running) {
            cat_draw_text_wrapped(body, T("Scanning…"), tx, ty, tw,
                                  theme->hint, CAT_ALIGN_CENTER);
        } else {
            char counts[96];
            snprintf(counts, sizeof(counts), T("%d games, %d systems, %d apps"),
                     state->summary.game_count, state->system_count,
                     state->summary.app_count);
            cat_draw_text_wrapped(body, counts, tx, ty, tw, theme->text,
                                  CAT_ALIGN_CENTER);
        }
    }

    cat_draw_list_pane(list.x, list.y, list.w, list.h, tab_count,
                       &state->menu_list, item_h, jw__draw_menu_item, (void *)items);

    cat_footer_item footer[] = {
        { CAT_BTN_L1, "Tab",    false, JW_HINT_DEVICE(";/t", "L1/R1") },
        { CAT_BTN_A,  "Select", true,  JW_HINT("A") },
    };
    jw__draw_footer(state, footer, 2);
    jw__present();
}

/* Open the System menu fresh on the Settings tab (the leftmost/primary tab).
   Settings renders the real settings UI, so enter it; the Actions/Info list
   states are initialized lazily when L1/R1 lands on them. */
static void jw__open_menu(jw_launcher_state *state) {
    state->menu_open = true;
    state->menu_tab  = JW_SMTAB_SETTINGS;
    jw_settings_ui_enter(&state->settings);
}

static void jw__switch_system_tab(jw_launcher_state *state, int direction) {
    if (!state) return;
    /* Moving along the System tab row is movement, so it ticks like the home tab
       row does: "nav", gated on the opt-in navigation tick, rather than the
       "select" a context change gets. The row wraps, so there is no boundary to
       report blocked. */
    jw__haptic(state, "nav");
    bool was_settings = (state->menu_tab == JW_SMTAB_SETTINGS);
    state->menu_tab = (state->menu_tab + direction + JW_SMTAB_COUNT) % JW_SMTAB_COUNT;
    bool now_settings = (state->menu_tab == JW_SMTAB_SETTINGS);
    if (was_settings && !now_settings)
        jw_settings_ui_close(&state->settings);
    if (now_settings) {
        jw_settings_ui_enter(&state->settings);
    } else {
        int n;
        jw__menu_tab_items(state->menu_tab, &n);
        cat_list_state_init(&state->menu_list, n);
    }
    cat_request_frame();
}

static void jw__open_pakrat_store(jw_launcher_state *state) {
    if (!state) {
        return;
    }

    int old_cursor = state->pakrat_list.cursor;
    state->menu_open = false;
    state->pakrat_open = true;
    state->pakrat_detail_open = false;
    state->menu_tab = JW_SMTAB_ACTIONS;
    jw_settings_ui_close(&state->settings);

    jw__load_pakrat_store(state);
    cat_list_state_init(&state->pakrat_list, jw__pakrat_visible_rows(state));
    cat_list_state_jump(&state->pakrat_list, old_cursor, state->pakrat_app_count);
    snprintf(state->status, sizeof(state->status), "%s",
             state->pakrat_message[0] ? state->pakrat_message : "Pak Rat");
}

/* Views where a held L1/R1 should hold-repeat to keep the carousel flowing: the
   Cover Flow home channels and the drilled-in Cover Flow game carousel. Every
   overlay (menu/search/actions/apps/etc.) and the non-Cover-Flow layouts keep the
   shoulders single-shot. Mirrors the jw__render_launcher dispatch below. */
static bool jw__view_wants_shoulder_repeat(const jw_launcher_state *state) {
    if (cat_get_stylesheet()->launcher.layout != CAT_LAUNCHER_COVERFLOW) {
        return false;
    }
    if (state->switcher_open || state->pakrat_open || state->menu_open ||
        state->search_open || state->actions_open || state->apps_open) {
        return false;
    }
    return true;   /* home channels, or the drilled-in games carousel */
}

/* 5-Game Mode setup wizard — defined later, referenced by the render/menu paths. */
static void jw__render_focus_setup(jw_launcher_state *state);
static void jw__focus_setup_begin(jw_launcher_state *state);

static void jw__render_launcher(jw_launcher_state *state) {
    jw__cf_animating = false;
    jw__cover_inline_decodes_this_frame = 0;

    /* Set the L1/R1 hold-repeat state ONCE per frame from the active view. Toggling
       it off-then-on within a frame would clear the repeat deadline the input poll
       just armed (cat_set_shoulder_repeat(false) clears it), so a held shoulder
       would never repeat — it must be the final desired value, set a single time. */
    cat_set_shoulder_repeat(jw__view_wants_shoulder_repeat(state));

    if (state->focus_active) {
        jw__render_focus(state);
        return;
    }

    if (state->focus_setup_open) {
        jw__render_focus_setup(state);
        return;
    }

    if (state->switcher_open) {
        jw__render_switcher(state);
        return;
    }

    if (state->pakrat_open) {
        jw__render_pakrat_store(state);
        return;
    }

    if (state->menu_open) {
        jw__render_menu(state);
        return;
    }

    if (state->search_open) {
        if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW)
            jw__render_search_cf(state);
        else
            jw__render_search(state);
        return;
    }

    if (state->actions_open) {
        jw__render_actions(state);
        return;
    }

    if (state->games_open) {
        if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW) {
            /* hold-repeat already set for this frame in jw__render_launcher */
            jw__render_coverflow_games(state);
        } else {
            jw__render_game_browser(state);
        }
        return;
    }

    if (state->apps_open) {
        jw__render_app_browser(state);
        return;
    }

    const cat_stylesheet *ss = cat_get_stylesheet();
    switch (ss->launcher.layout) {
        case CAT_LAUNCHER_VERTICAL:   jw__render_vertical(state);   break;
        case CAT_LAUNCHER_HORIZONTAL: jw__render_horizontal(state); break;
        case CAT_LAUNCHER_COVERFLOW:  jw__render_coverflow(state);  break;
        default:                      jw__render_tabbed(state);     break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ACTION + INPUT
 * ═══════════════════════════════════════════════════════════════════════════ */

static int jw__game_browser_visible_rows(const jw_launcher_state *state) {
    return jw__browse_visible_rows(state, jw__game_browser_header_h(state));
}

static int jw__app_browser_visible_rows(const jw_launcher_state *state) {
    int fh = jw__footer_height(state);
    int content_h = cat_get_screen_height() - CAT_DS(30) - CAT_S(24) - fh;
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int visible = content_h / item_h;
    return visible > 0 ? visible : 1;
}

static int jw__search_visible_rows(const jw_launcher_state *state) {
    return jw__browse_visible_rows(state, jw__search_header_h(state));
}

static int jw__pakrat_visible_rows(const jw_launcher_state *state) {
    return jw__browse_visible_rows(state, jw__pakrat_header_h());
}

static const jw_platform_perf_profile kActionPerfProfiles[] = {
    JW_PLATFORM_PERF_PROFILE_AUTO,
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
};
#define JW_ACTION_PERF_COUNT ((int)(sizeof(kActionPerfProfiles) / sizeof(kActionPerfProfiles[0])))

static bool jw__runtime_cores_dir(const jw_launcher_state *state,
                                  char *out, size_t out_size) {
    const char *env = getenv("CORES_PATH");
    if (!env || !env[0]) {
        env = getenv("JAWAKA_RETROARCH_CORES_DIR");
    }
    if (env && env[0]) {
        return snprintf(out, out_size, "%s", env) < (int)out_size;
    }

    const char *system_path = getenv("SYSTEM_PATH");
    if (system_path && system_path[0]) {
        return snprintf(out, out_size, "%s/cores", system_path) < (int)out_size;
    }

    (void)state;
    return false;
}

static int jw__action_find_core(const jw_launcher_state *state, const char *core_id) {
    if (!state || !core_id || !core_id[0]) {
        return -1;
    }
    for (size_t i = 0; i < state->action_core_count; i++) {
        if (strcmp(state->action_core_choices[i].id, core_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const char *jw__action_core_label(const jw_launcher_state *state,
                                         const char *core_id) {
    int idx = jw__action_find_core(state, core_id);
    if (idx >= 0) {
        return state->action_core_choices[idx].display_name;
    }
    return core_id && core_id[0] ? core_id : "Default";
}

static int jw__action_perf_index(const char *value) {
    jw_platform_perf_profile profile;
    if (value && value[0] && jw_platform_parse_perf_profile(value, &profile)) {
        for (int i = 0; i < JW_ACTION_PERF_COUNT; i++) {
            if (kActionPerfProfiles[i] == profile) {
                return i;
            }
        }
    }
    return 0;
}

static const char *jw__action_perf_label(const char *value) {
    jw_platform_perf_profile profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    if (value && value[0]) {
        (void)jw_platform_parse_perf_profile(value, &profile);
    }
    return jw_platform_perf_profile_label(profile);
}

static void jw__action_add_row(jw_launcher_state *state, jw_action_row_kind row) {
    if (!state || state->action_row_count >= JW_MAX_ACTION_ROWS) {
        return;
    }
    state->action_rows[state->action_row_count++] = row;
}

static void jw__action_refresh_rows(jw_launcher_state *state) {
    if (!state) {
        return;
    }
    int old_cursor = state->action_list.cursor;
    state->action_row_count = 0;
    if (state->action_scope == JW_ACTION_SYSTEM) {
        jw__action_add_row(state, JW_ACTION_ROW_SEARCH);
        jw__action_add_row(state, JW_ACTION_ROW_DISPLAY_NAME);
        if (state->action_core_count > 1 || state->action_core_system_override[0]) {
            jw__action_add_row(state, JW_ACTION_ROW_CORE);
        }
        jw__action_add_row(state, JW_ACTION_ROW_PERFORMANCE);
        /* Per-system scraping moved to Settings > Game Art > Scrape Missing
           Artwork; the system X menu no longer offers it. */
        jw__action_add_row(state, JW_ACTION_ROW_RESET);
    } else if (state->action_scope == JW_ACTION_GAME) {
        jw__action_add_row(state, JW_ACTION_ROW_DISPLAY_NAME);
        if (state->action_core_count > 1 ||
            state->action_core_game_override[0] ||
            state->action_core_system_override[0]) {
            jw__action_add_row(state, JW_ACTION_ROW_CORE);
        }
        jw__action_add_row(state, JW_ACTION_ROW_PERFORMANCE);
        jw__action_add_row(state, state->action_scrape_pending
                                      ? JW_ACTION_ROW_SCRAPE_CANCEL
                                      : JW_ACTION_ROW_SCRAPE);
        jw__action_add_row(state, JW_ACTION_ROW_RESET);
    }
    cat_list_state_init(&state->action_list, 7);
    cat_list_state_jump(&state->action_list, old_cursor, state->action_row_count);
}

static void jw__action_refresh_core_choices(const char *db_path,
                                            jw_launcher_state *state) {
    (void)db_path;
    if (!state) {
        return;
    }
    state->action_core_count = 0;
    state->action_core_effective[0] = '\0';

    char cores_dir[PATH_MAX];
    if (!jw__runtime_cores_dir(state, cores_dir, sizeof(cores_dir))) {
        return;
    }

    char error[256];
    const jw_ra_catalog *catalog =
        jw_ra_catalog_get(state->sdcard_root, error, sizeof(error));
    if (!catalog) {
        snprintf(state->status, sizeof(state->status), "Core metadata unavailable: %.180s",
                 error[0] ? error : "unknown");
        return;
    }

    const char *system = state->action_scope == JW_ACTION_GAME
        ? state->action_game.system
        : state->action_system;
    (void)jw_ra_catalog_list_system_cores(catalog, system, cores_dir,
                                          state->platform_root,
                                          state->action_core_choices,
                                          JW_MAX_CORE_CHOICES,
                                          &state->action_core_count);

    const char *preferred = state->action_core_game_override[0]
        ? state->action_core_game_override
        : state->action_core_system_override;
    if (preferred && preferred[0] &&
        jw__action_find_core(state, preferred) >= 0) {
        jw__str_copy(state->action_core_effective,
                     sizeof(state->action_core_effective), preferred);
    } else if (state->action_core_count > 0) {
        jw__str_copy(state->action_core_effective,
                     sizeof(state->action_core_effective),
                     state->action_core_choices[0].id);
    }
}

static void jw__action_refresh_overrides(const char *db_path,
                                         jw_launcher_state *state) {
    if (!state || !db_path) {
        return;
    }
    state->action_core_game_override[0] = '\0';
    state->action_core_system_override[0] = '\0';
    state->action_perf_game_override[0] = '\0';
    state->action_perf_system_override[0] = '\0';
    state->action_system_display_override[0] = '\0';

    const char *system = state->action_scope == JW_ACTION_GAME
        ? state->action_game.system
        : state->action_system;

    if (state->action_scope == JW_ACTION_GAME && state->action_game.id > 0) {
        (void)jw_db_get_game_setting(db_path, state->action_game.id,
                                     JW_CONTENT_SETTING_CORE_ID,
                                     state->action_core_game_override,
                                     sizeof(state->action_core_game_override));
        (void)jw_db_get_game_setting(db_path, state->action_game.id,
                                     JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                     state->action_perf_game_override,
                                     sizeof(state->action_perf_game_override));
    }
    if (system && system[0]) {
        (void)jw_db_get_system_setting(db_path, system,
                                       JW_CONTENT_SETTING_CORE_ID,
                                       state->action_core_system_override,
                                       sizeof(state->action_core_system_override));
        (void)jw_db_get_system_setting(db_path, system,
                                       JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                       state->action_perf_system_override,
                                       sizeof(state->action_perf_system_override));
        (void)jw_db_get_system_setting(db_path, system,
                                       JW_CONTENT_SETTING_DISPLAY_NAME,
                                       state->action_system_display_override,
                                       sizeof(state->action_system_display_override));
    }
}

static void jw__action_refresh_scrape_pending(jw_launcher_state *state) {
    state->action_scrape_pending = false;
    const char *socket_path = state->settings.socket_path;
    if (!socket_path[0]) {
        return;
    }
    bool pending = false;
    const char *system = state->action_scope == JW_ACTION_GAME
        ? state->action_game.system
        : state->action_system;
    const char *rom_path = state->action_scope == JW_ACTION_GAME
        ? state->action_game.rom_path
        : NULL;
    if (jw_ipc_scrape_pending(socket_path, system, rom_path, &pending) == 0) {
        state->action_scrape_pending = pending;
    }
}

static void jw__action_refresh(const char *db_path, jw_launcher_state *state) {
    jw__action_refresh_overrides(db_path, state);
    jw__action_refresh_core_choices(db_path, state);
    jw__action_refresh_scrape_pending(state);
    jw__action_refresh_rows(state);
}

static void jw__open_system_actions(const char *db_path, jw_launcher_state *state,
                                    const char *system, const char *display_name) {
    if (!state || !system || !system[0]) {
        return;
    }
    state->actions_open = true;
    state->action_scope = JW_ACTION_SYSTEM;
    memset(&state->action_game, 0, sizeof(state->action_game));
    snprintf(state->action_system, sizeof(state->action_system), "%s", system);
    jw_system_display_name(db_path, system, state->action_system_display,
                           sizeof(state->action_system_display));
    if (!state->action_system_display[0] && display_name && display_name[0]) {
        snprintf(state->action_system_display, sizeof(state->action_system_display),
                 "%s", display_name);
    }
    jw__action_refresh(db_path, state);
    /* No "Actions: ..." status echo - the name is already the sub-header. */
    state->status[0] = '\0';
}

static void jw__open_game_actions(const char *db_path, jw_launcher_state *state,
                                  const jw_game_entry *game) {
    if (!state || !game || game->id <= 0) {
        return;
    }
    state->actions_open = true;
    state->action_scope = JW_ACTION_GAME;
    state->action_game = *game;
    snprintf(state->action_system, sizeof(state->action_system), "%s", game->system);
    jw_system_display_name(db_path, game->system, state->action_system_display,
                           sizeof(state->action_system_display));
    jw__action_refresh(db_path, state);
    /* No "Actions: ..." status echo - the name is already the sub-header. */
    state->status[0] = '\0';
}

static bool jw__selected_home_game(const jw_launcher_state *state,
                                   const jw_game_entry **out) {
    if (!state || !out) {
        return false;
    }
    *out = NULL;
    if (cat_get_stylesheet()->launcher.layout != CAT_LAUNCHER_TABBED) {
        return false;
    }
    if (state->current_tab == JW_TAB_RECENTS &&
        state->recents_count > 0 &&
        state->list.cursor < state->recents_count) {
        *out = &state->recents[state->list.cursor];
        return true;
    }
    if (state->current_tab == JW_TAB_FAVORITES &&
        state->favorites_count > 0 &&
        state->list.cursor < state->favorites_count) {
        *out = &state->favorites[state->list.cursor];
        return true;
    }
    return false;
}

static bool jw__selected_home_system(const jw_launcher_state *state,
                                     const jw_system_entry **out) {
    if (!state || !out) {
        return false;
    }
    *out = NULL;
    cat_launcher_layout layout = cat_get_stylesheet()->launcher.layout;
    if (layout == CAT_LAUNCHER_TABBED) {
        if (state->current_tab == JW_TAB_GAMES &&
            state->system_count > 0 &&
            state->list.cursor < state->system_count) {
            *out = &state->systems[state->list.cursor];
            return true;
        }
        return false;
    }
    if (state->list.cursor >= state->flat_count) {
        return false;
    }
    const jw_flat_item *it = &state->flat_items[state->list.cursor];
    if (it->kind == JW_FLAT_SYSTEM &&
        it->system_idx >= 0 && it->system_idx < state->system_count) {
        *out = &state->systems[it->system_idx];
        return true;
    }
    return false;
}

static bool jw__open_context_actions(const char *db_path, jw_launcher_state *state) {
    if (!state) {
        return false;
    }
    if (state->games_open &&
        state->game_count > 0 &&
        state->game_list.cursor < state->game_count) {
        jw__open_game_actions(db_path, state, &state->games[state->game_list.cursor]);
        return true;
    }

    const jw_game_entry *game = NULL;
    if (jw__selected_home_game(state, &game) && game) {
        jw__open_game_actions(db_path, state, game);
        return true;
    }

    const jw_system_entry *system = NULL;
    if (jw__selected_home_system(state, &system) && system) {
        jw__open_system_actions(db_path, state, system->name, system->display_name);
        return true;
    }

    return false;
}

static int jw__perform_search(const char *db_path, jw_launcher_state *state,
                              const char *query) {
    jw__str_copy(state->search_query, sizeof(state->search_query), query);
    state->search_count = 0;
    s_search_prewarm_cursor = -1;   /* results changed — force a re-sweep */
    s_search_cf_reset = true;       /* results changed — re-seat the CF carousel */

    if (jw_db_search_library(db_path, state->search_query, state->search_results,
                             JW_MAX_SEARCH_RESULTS, &state->search_count) != 0) {
        state->search_open = true;
        cat_list_state_init(&state->search_list, jw__search_visible_rows(state));
        snprintf(state->status, sizeof(state->status), "%s", "search failed");
        return -1;
    }

    state->search_open = true;
    cat_list_state_init(&state->search_list, jw__search_visible_rows(state));
    cat_list_state_jump(&state->search_list, 0, state->search_count);
    snprintf(state->status, sizeof(state->status), "%d results", state->search_count);
    return 0;
}

static void jw__open_search(const char *db_path, jw_launcher_state *state) {
    cat_keyboard_result result;
    int rc = cat_keyboard(state->search_query,
                          "Search library\nStart: Confirm\nY: Cancel",
                          CAT_KB_GENERAL, &result);
    if (rc == CAT_OK) {
        jw__perform_search(db_path, state, result.text);
    } else if (rc == CAT_ERROR) {
        snprintf(state->status, sizeof(state->status), "%s", "search keyboard failed");
    }
}

static int jw__open_system_games(const char *db_path, const char *system,
                                 jw_launcher_state *state) {
    char display_name[64];
    jw_system_display_name(db_path, system, display_name, sizeof(display_name));

    int rc = jw__load_system_games_full(db_path, system, state, 0);
    if (rc != 0) {
        if (rc < 0) {
            snprintf(state->status, sizeof(state->status), "Could not load games for %s",
                     display_name[0] ? display_name : system);
            return -1;
        }
        snprintf(state->status, sizeof(state->status), "No launchable games for %s",
                 display_name[0] ? display_name : system);
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", system);
    snprintf(state->game_system_display, sizeof(state->game_system_display), "%s",
             display_name[0] ? display_name : system);
    state->games_are_favorites = false;
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows(state));
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    snprintf(state->status, sizeof(state->status), T("%d %s games"),
             state->game_count, state->game_system_display);
    return 0;
}

static int jw__open_favorites(const char *db_path, jw_launcher_state *state) {
    if (jw__load_bounded_game_browser(db_path, state, jw_db_list_favorite_games,
                                      JW_OPENED_GAME_BROWSER_LIMIT) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Could not load favorites");
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", "Favorites");
    snprintf(state->game_system_display, sizeof(state->game_system_display),
             "%s", "Favorites");
    state->games_are_favorites = true;
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows(state));
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    if (state->game_count == 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "No favorites yet — press Y on a game to add one");
    } else {
        snprintf(state->status, sizeof(state->status), "%d favorites", state->game_count);
    }
    return 0;
}

static int jw__open_recents(const char *db_path, jw_launcher_state *state) {
    if (jw__load_bounded_game_browser(db_path, state, jw_db_list_recent_games,
                                      JW_OPENED_GAME_BROWSER_LIMIT) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Could not load recents");
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", "Recently Played");
    snprintf(state->game_system_display, sizeof(state->game_system_display),
             "%s", "Recently Played");
    state->games_are_favorites = false;
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows(state));
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    if (state->game_count == 0) {
        snprintf(state->status, sizeof(state->status), "%s", "No recent games yet");
    } else {
        snprintf(state->status, sizeof(state->status), "%d recent", state->game_count);
    }
    return 0;
}

typedef enum {
    JW_PAKRAT_UI_INSTALL = 0,
    JW_PAKRAT_UI_UNINSTALL,
    JW_PAKRAT_UI_REMOVE_RETAINED,
} jw_pakrat_ui_action;

typedef struct {
    jw_pakrat_context ctx;
    char store_id[128];
    char target_version[64];
    jw_pakrat_ui_action action;
    int allow_adopt;   /* install may replace a manually-installed pak */
    int repair_exact;
    char error_message[256];
} jw_pakrat_ui_job;

static int jw__pakrat_ui_worker(void *userdata) {
    jw_pakrat_ui_job *job = (jw_pakrat_ui_job *)userdata;
    if (!job) {
        return -1;
    }
    if (job->action == JW_PAKRAT_UI_UNINSTALL) {
        return jw_pakrat_uninstall_app(&job->ctx, job->store_id);
    }
    if (job->action == JW_PAKRAT_UI_REMOVE_RETAINED) {
        return jw_pakrat_remove_retained_data(&job->ctx, job->store_id);
    }
    if (job->repair_exact) {
        return jw_pakrat_repair_app_version(
            &job->ctx, job->store_id, job->target_version);
    }
    return jw_pakrat_install_app_target(
        &job->ctx, job->store_id, job->target_version, job->allow_adopt);
}

static void jw__pakrat_format_size(unsigned long long bytes,
                                   char *out, size_t out_size) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.1f GiB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.1f MiB",
                 (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_size, "%.1f KiB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_size, "%llu B", bytes);
    }
}

static bool jw__pakrat_append(char *message, size_t capacity,
                              size_t *used, const char *fmt, ...) {
    if (!message || !used || *used >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(message + *used, capacity - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= capacity - *used) {
        return false;
    }
    *used += (size_t)n;
    return true;
}

static bool jw__confirm_pakrat_uninstall(
    const jw_pakrat_app_state *app,
    const jw_pakrat_uninstall_info *info) {
    if (!app || !info || info->item_count < 0) {
        return false;
    }
    size_t capacity = 768u +
        (size_t)info->item_count * (size_t)(PATH_MAX + 128);
    char *message = calloc(capacity, 1u);
    if (!message) {
        return false;
    }
    size_t used = 0;
    bool complete = jw__pakrat_append(
        message, capacity, &used, "Uninstall %.180s?\n\n",
        app->package.name[0] ? app->package.name : app->package.id);
    if (complete && info->metadata.revoke_count > 0) {
        complete = jw__pakrat_append(
            message, capacity, &used,
            "Declared browser trust and credentials will be revoked.\n");
    }
    if (complete && info->item_count == 0) {
        complete = jw__pakrat_append(
            message, capacity, &used,
            "No retained data directories are declared.");
    } else if (complete) {
        complete = jw__pakrat_append(
            message, capacity, &used,
            "These retained data directories will be preserved:\n");
    }
    for (int i = 0; complete && i < info->item_count; i++) {
        const jw_pakrat_retained_item *item = &info->items[i];
        char size[64] = "unknown size";
        if (item->size_known) {
            jw__pakrat_format_size(item->size_bytes, size, sizeof(size));
        }
        complete = jw__pakrat_append(
            message, capacity, &used, "\n%s: %s (%s)", item->source_id,
            item->root,
            item->source_present
                ? size : "card absent; not inventoried");
    }
    if (!complete) {
        free(message);
        return false;
    }
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel",    .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Uninstall", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    bool confirmed = cat_confirmation(&opts, &result) == CAT_OK &&
                     result.confirmed;
    free(message);
    return confirmed;
}

static bool jw__confirm_pakrat_remove_retained(
    const jw_pakrat_app_state *app,
    const jw_pakrat_uninstall_info *info) {
    if (!app || !info || info->item_count <= 0) {
        return false;
    }
    char message[768];
    snprintf(
        message, sizeof(message),
        "Also remove retained data for %.180s?\n\nThis separately deletes every declared retained directory on cards that are present now. Absent cards remain unchanged. This cannot be undone.",
        app->package.name[0] ? app->package.name : app->package.id);
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Keep data", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Remove", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_pakrat_reinstall(const jw_pakrat_app_state *app) {
    if (!app) {
        return false;
    }
    char message[512];
    snprintf(message, sizeof(message),
             "Reinstall %.180s?\n\nThe app package will be replaced. User data is preserved.",
             app->package.name[0] ? app->package.name : app->package.id);
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel",    .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Reinstall", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

/* Confirm taking over a pak that is already on disk from a manual install.
   Replacing a file the user placed themselves should never be silent. */
static bool jw__confirm_pakrat_adopt(const jw_pakrat_app_state *app) {
    if (!app) {
        return false;
    }
    char message[512];
    snprintf(message, sizeof(message),
             "%.180s is already installed manually.\n\nReplace it with the Pak Rat "
             "version so it can be updated? User data is preserved.",
             app->package.name[0] ? app->package.name : app->package.id);
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel",  .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Replace", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static const char *jw__pakrat_running_label(const jw_pakrat_app_state *app,
                                            jw_pakrat_ui_action action) {
    if (action == JW_PAKRAT_UI_UNINSTALL) {
        return "Uninstalling Pak Rat app";
    }
    if (!app) {
        return "Installing Pak Rat app";
    }
    switch (app->status) {
        case JW_PAKRAT_APP_INSTALLED:        return "Reinstalling Pak Rat app";
        case JW_PAKRAT_APP_UPDATE_AVAILABLE: return "Updating Pak Rat app";
        case JW_PAKRAT_APP_STALE:            return "Restoring Pak Rat app";
        default:                             return "Installing Pak Rat app";
    }
}

static const char *jw__pakrat_done_label(const jw_pakrat_app_state *app,
                                         jw_pakrat_ui_action action) {
    if (action == JW_PAKRAT_UI_UNINSTALL) {
        return "Uninstalled";
    }
    if (!app) {
        return "Installed";
    }
    switch (app->status) {
        case JW_PAKRAT_APP_INSTALLED:        return "Reinstalled";
        case JW_PAKRAT_APP_UPDATE_AVAILABLE: return "Updated";
        case JW_PAKRAT_APP_STALE:            return "Restored";
        default:                             return "Installed";
    }
}

static void jw__set_pakrat_message(jw_launcher_state *state, const char *fmt, ...) {
    if (!state || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->pakrat_message, sizeof(state->pakrat_message), fmt, ap);
    va_end(ap);
    snprintf(state->status, sizeof(state->status), "%s", state->pakrat_message);
}

static void jw__run_pakrat_action(const char *db_path, jw_launcher_state *state,
                                  jw_pakrat_ui_action action) {
    if (!state || state->pakrat_app_count <= 0 ||
        state->pakrat_list.cursor < 0 ||
        state->pakrat_list.cursor >= state->pakrat_app_count) {
        jw__set_pakrat_message(state, "%s", "No Pak Rat app selected");
        return;
    }

    jw_pakrat_app_state app = state->pakrat_apps[state->pakrat_list.cursor];
    const char *name = app.package.name[0] ? app.package.name : app.package.id;
    if (app.managed) {
        jw__set_pakrat_message(state, "%.120s is release-managed", name);
        return;
    }
    if (action != JW_PAKRAT_UI_UNINSTALL &&
        !app.primary_action_allowed) {
        if (app.installed_version_missing_from_history &&
            app.installed_version[0]) {
            jw__set_pakrat_message(
                state, "Version %.40s is missing from catalog history",
                app.installed_version);
        } else if (app.gated_version[0] &&
                   app.gated_min_leaf_version[0]) {
            jw__set_pakrat_message(
                state, "Version %.40s requires Leaf v%.40s",
                app.gated_version, app.gated_min_leaf_version);
        } else {
            jw__set_pakrat_message(
                state, "%.120s has no compatible install action", name);
        }
        return;
    }

    jw_pakrat_context ctx;
    if (jw__pakrat_context_from_state(state, &ctx) != 0) {
        jw__set_pakrat_message(state, "%s", "Pak Rat runtime paths unavailable");
        return;
    }

    jw_pakrat_uninstall_info uninstall_info;
    memset(&uninstall_info, 0, sizeof(uninstall_info));
    bool uninstall_info_loaded = false;
    bool allow_adopt = false;
    if (action == JW_PAKRAT_UI_UNINSTALL) {
        if (!jw__pakrat_can_uninstall(&app)) {
            jw__set_pakrat_message(state, "%.120s is not installed by Pak Rat", name);
            return;
        }
        if (jw_pakrat_get_uninstall_info(
                &ctx, app.package.id, &uninstall_info) != 0) {
            jw__set_pakrat_message(
                state, "Could not inspect retained data for %.100s", name);
            return;
        }
        uninstall_info_loaded = true;
        if (!jw__confirm_pakrat_uninstall(&app, &uninstall_info)) {
            jw_pakrat_free_uninstall_info(&uninstall_info);
            jw__set_pakrat_message(state, "Uninstall cancelled: %.120s", name);
            return;
        }
    } else if (app.status == JW_PAKRAT_APP_UNMANAGED) {
        if (!jw__confirm_pakrat_adopt(&app)) {
            jw__set_pakrat_message(state, "Install cancelled: %.120s", name);
            return;
        }
        allow_adopt = true;
    } else if (app.status == JW_PAKRAT_APP_INSTALLED &&
               !jw__confirm_pakrat_reinstall(&app)) {
        jw__set_pakrat_message(state, "Reinstall cancelled: %.120s", name);
        return;
    }

    jw_pakrat_ui_job job;
    memset(&job, 0, sizeof(job));
    job.ctx = ctx;
    job.action = action;
    job.allow_adopt = allow_adopt ? 1 : 0;
    job.repair_exact = app.action_uses_history;
    job.ctx.error_message = job.error_message;
    job.ctx.error_message_size = sizeof(job.error_message);
    snprintf(job.store_id, sizeof(job.store_id), "%s", app.package.id);
    snprintf(job.target_version, sizeof(job.target_version), "%s",
             app.action_version);

    char detail[320];
    snprintf(detail, sizeof(detail), "%.240s", name);
    char *dynamic_message = detail;
    cat_process_opts opts = {
        .message = jw__pakrat_running_label(&app, action),
        .show_progress = false,
        .progress = NULL,
        .interrupt_signal = NULL,
        .interrupt_button = CAT_BTN_NONE,
        .dynamic_message = &dynamic_message,
        .message_lines = 1,
    };
    int old_cursor = state->pakrat_list.cursor;
    int rc = cat_process_message(&opts, jw__pakrat_ui_worker, &job);
    int retained_rc = -2;
    if (rc == 0 && action == JW_PAKRAT_UI_UNINSTALL &&
        uninstall_info.item_count > 0 &&
        jw__confirm_pakrat_remove_retained(&app, &uninstall_info)) {
        job.action = JW_PAKRAT_UI_REMOVE_RETAINED;
        job.error_message[0] = '\0';
        char *remove_detail = detail;
        cat_process_opts remove_opts = {
            .message = "Removing retained Pak Rat data",
            .show_progress = false,
            .progress = NULL,
            .interrupt_signal = NULL,
            .interrupt_button = CAT_BTN_NONE,
            .dynamic_message = &remove_detail,
            .message_lines = 1,
        };
        retained_rc = cat_process_message(
            &remove_opts, jw__pakrat_ui_worker, &job);
    }
    if (uninstall_info_loaded) {
        jw_pakrat_free_uninstall_info(&uninstall_info);
    }
    int reload_rc = jw__reload_library_from_db(db_path, state);
    if (reload_rc != 0) {
        jw__load_pakrat_store(state);
    }
    cat_list_state_jump(&state->pakrat_list, old_cursor, state->pakrat_app_count);

    if (rc == 0 && retained_rc == 0) {
        jw__set_pakrat_message(state,
                               "Uninstalled %.100s and removed retained data",
                               name);
    } else if (rc == 0 && retained_rc != -2 && retained_rc != 0) {
        jw__set_pakrat_message(
            state, "Uninstalled %.100s; retained-data removal failed", name);
    } else if (rc == 0) {
        jw__set_pakrat_message(state, "%.80s %.120s",
                               jw__pakrat_done_label(&app, action), name);
    } else {
        jw__set_pakrat_message(
            state, "%s",
            job.error_message[0] ? job.error_message
                                 : "Pak Rat action failed");
    }
    if (rc == 0 && reload_rc != 0) {
        jw__set_pakrat_message(state, "%.120s changed; refresh failed", name);
    }
    cat_request_frame();
}

static void jw__open_apps(jw_launcher_state *state) {
    state->apps_open = true;
    cat_list_state_init(&state->app_list, jw__app_browser_visible_rows(state));
    cat_list_state_jump(&state->app_list, 0, state->app_count);
    if (state->app_count > 0) {
        snprintf(state->status, sizeof(state->status), T("%d apps"), state->app_count);
    } else {
        snprintf(state->status, sizeof(state->status), "%s",
                 state->scan_ready ? T("No apps found") : T("Scanning library..."));
    }
}

/* ─── Navigation resume (breadcrumb) ─────────────────────────────────────────
   Launching a game or app exits the launcher; jawakad respawns it when the game
   returns, which would otherwise drop you at the top of the Startup Tab. So the
   current position is stashed in /tmp right before launching and restored on the
   next start. /tmp is deliberate: it survives the game round-trip but clears on
   reboot, so a cold boot still honors the Startup Tab setting. */
#define JW_RESUME_PATH "/tmp/jawaka-launcher-resume"
/* Dropped by the System menu's "Search" item; opens the search overlay on respawn. */
#define JW_OPEN_SEARCH_MARKER "/tmp/jawaka-open-search"

typedef struct {
    int  tab;
    int  list_cursor;   /* every tabbed tab list navigates state->list */
    int  games_open;
    int  games_fav;
    int  game_cursor;
    char game_system[64];
} jw_resume;

static void jw__save_resume(const jw_launcher_state *state) {
    FILE *fp = fopen(JW_RESUME_PATH, "w");
    if (!fp) return;
    fprintf(fp, "tab=%d\n", (int)state->current_tab);
    fprintf(fp, "list_cursor=%d\n", state->list.cursor);
    fprintf(fp, "games_open=%d\n", state->games_open ? 1 : 0);
    fprintf(fp, "games_fav=%d\n", state->games_are_favorites ? 1 : 0);
    fprintf(fp, "game_cursor=%d\n", state->game_list.cursor);
    fprintf(fp, "game_system=%s\n", state->game_system);
    fclose(fp);
}

/* Load and consume (delete) the resume breadcrumb. True if one was present. */
static bool jw__load_resume(jw_resume *out) {
    FILE *fp = fopen(JW_RESUME_PATH, "r");
    if (!fp) return false;
    memset(out, 0, sizeof(*out));
    out->tab = -1;
    char line[160];
    while (fgets(line, sizeof(line), fp)) {
        int v;
        if (sscanf(line, "tab=%d", &v) == 1) out->tab = v;
        else if (sscanf(line, "list_cursor=%d", &v) == 1) out->list_cursor = v;
        else if (sscanf(line, "games_open=%d", &v) == 1) out->games_open = v;
        else if (sscanf(line, "games_fav=%d", &v) == 1) out->games_fav = v;
        else if (sscanf(line, "game_cursor=%d", &v) == 1) out->game_cursor = v;
        else if (strncmp(line, "game_system=", 12) == 0) {
            jw__str_copy(out->game_system, sizeof(out->game_system), line + 12);
            out->game_system[strcspn(out->game_system, "\r\n")] = '\0';
        }
    }
    fclose(fp);
    remove(JW_RESUME_PATH);
    return out->tab >= 0;
}

/* Restore the drilled-in game browser + cursors from a loaded breadcrumb. Call
   AFTER jw__rebuild_for_layout (which re-inits state->list to cursor 0), and after
   the library + tab contents are loaded. cat_list_state_jump clamps the target, so
   stale indices (a removed game) just land on the nearest row. */
static void jw__apply_resume(const char *db_path, jw_launcher_state *state,
                             const jw_resume *r) {
    /* Only restore a drilled-in system-games browser if the Games tab is still
       visible (it's hideable). Favorites/Recents drills belong to their own tabs
       and are gated by current_tab already being reconciled onto the visible set. */
    bool restore_games = r->games_open;
    if (restore_games) {
        bool recents = (strcmp(r->game_system, "Recently Played") == 0);
        bool system_drill = !r->games_fav && !recents;
        if (system_drill && !jw__tab_is_visible(state, JW_TAB_GAMES))
            restore_games = false;
    }
    if (restore_games) {
        bool recents = (strcmp(r->game_system, "Recently Played") == 0);
        int rc;
        /* Reopening the breadcrumbed list is not a user action -- without this
           the launcher buzzes to itself every time you exit a game. */
        bool was_muted = state->haptics_muted;
        state->haptics_muted = true;
        if (r->games_fav)   rc = jw__open_favorites(db_path, state);
        else if (recents)   rc = jw__open_recents(db_path, state);
        else                rc = jw__open_system_games(db_path, r->game_system, state);
        if (rc == 0 && state->game_count > 0)
            /* Recents reorders the just-played game to the top -> land on row 0. */
            cat_list_state_jump(&state->game_list, recents ? 0 : r->game_cursor,
                                state->game_count);
        state->haptics_muted = was_muted;
    }
    /* Every tabbed tab list (Recents/Favorites/Games systems/Apps) navigates
       state->list. Recents reorders the just-played game to the top -> land on 0. */
    int count = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
                    ? jw__tab_list_count(state) : state->flat_count;
    int target = (state->current_tab == JW_TAB_RECENTS) ? 0 : r->list_cursor;
    cat_list_state_jump(&state->list, target, count);
}

static int jw__launch_app_request(const char *socket_path, const char *name,
                                  const char *pak_dir, jw_launcher_state *state,
                                  bool *running) {
    if (!pak_dir || !pak_dir[0]) {
        snprintf(state->status, sizeof(state->status), "%s", "No app selected");
        return -1;
    }

    jw__set_launching_status(state, name, "app");
    cat_request_frame();
    jw__render_launcher(state);

    if (jw_ipc_launch_app(socket_path, pak_dir, state->status, sizeof(state->status)) != 0) {
        jw__haptic(state, "blocked");   /* refused: the launcher is still here */
        return -1;
    }

    jw__save_resume(state);
    cat_hide_window();
    *running = false;
    return 0;
}

static int jw__launch_app_at(const char *socket_path, jw_launcher_state *state,
                             int cursor, bool *running) {
    if (state->app_count <= 0 || cursor < 0 || cursor >= state->app_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No app selected");
        return -1;
    }

    const jw_app_entry *app = &state->apps[cursor];
    if (!app->pak_dir[0]) {
        snprintf(state->status, sizeof(state->status),
                 "%.200s is a layout sample, not a real app", app->name);
        return -1;
    }
    return jw__launch_app_request(socket_path, app->name, app->pak_dir, state, running);
}

static int jw__launch_game_entry_with_mode(const char *socket_path,
                                           jw_launcher_state *state,
                                           const jw_game_entry *game,
                                           bool switcher_resume,
                                           bool *running) {
    if (!game) {
        snprintf(state->status, sizeof(state->status), "%s", "No game selected");
        return -1;
    }

    jw__set_launching_status(state, game->name, "game");
    cat_request_frame();
    jw__render_launcher(state);

    int rc = switcher_resume
        ? jw_ipc_launch_game_switcher(socket_path, game->system, game->rom_path,
                                      state->status, sizeof(state->status))
        : jw_ipc_launch_game(socket_path, game->system, game->rom_path,
                             state->status, sizeof(state->status));
    if (rc != 0) {
        /* The launcher stays up on a refusal, so this is the one outcome worth
           reporting by touch. */
        jw__haptic(state, "blocked");
        return -1;
    }
    /* No success pattern on the way out. A commit here was fired BEFORE the IPC,
       so it claimed success the daemon had not granted yet -- and it could not
       finish regardless: the daemon quiesces the worker to hand the motor to the
       emulator, truncating whatever was mid-flight. Ownership transfers cleanly
       instead of two writers racing for the channel. */

    jw__save_resume(state);
    cat_hide_window();
    *running = false;
    return 0;
}

static int jw__launch_game_entry(const char *socket_path, jw_launcher_state *state,
                                 const jw_game_entry *game, bool *running) {
    return jw__launch_game_entry_with_mode(socket_path, state, game, false, running);
}

static int jw__launch_selected_game(const char *socket_path, jw_launcher_state *state,
                                    bool *running) {
    if (state->game_count <= 0 || state->game_list.cursor >= state->game_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No game selected");
        return -1;
    }
    return jw__launch_game_entry(socket_path, state,
                                 &state->games[state->game_list.cursor], running);
}

static int jw__launch_selected_app(const char *socket_path, jw_launcher_state *state,
                                   bool *running) {
    return jw__launch_app_at(socket_path, state, state->list.cursor, running);
}

static int jw__launch_selected_search_result(const char *socket_path,
                                             jw_launcher_state *state,
                                             bool *running) {
    if (state->search_count <= 0 || state->search_list.cursor >= state->search_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No result selected");
        return -1;
    }

    const jw_search_result *result = &state->search_results[state->search_list.cursor];
    if (result->kind == JW_SEARCH_APP) {
        return jw__launch_app_request(socket_path, result->name, result->pak_dir, state, running);
    }

    jw__set_launching_status(state, result->name, "game");
    cat_request_frame();
    jw__render_launcher(state);

    if (jw_ipc_launch_game(socket_path, result->system, result->rom_path,
                           state->status, sizeof(state->status)) != 0) {
        jw__haptic(state, "blocked");   /* refused: the launcher is still here */
        return -1;
    }

    jw__save_resume(state);
    cat_hide_window();
    *running = false;
    return 0;
}

static void jw__refresh_action_game_from_db(const char *db_path,
                                            jw_launcher_state *state) {
    if (!state || state->action_scope != JW_ACTION_GAME ||
        state->action_game.id <= 0 || !db_path) {
        return;
    }
    jw_game_entry updated;
    if (jw_db_get_game_by_rom_path(db_path, state->action_game.rom_path,
                                   &updated) == 0) {
        state->action_game = updated;
    }
}

static void jw__refresh_action_system_display(const char *db_path,
                                              jw_launcher_state *state) {
    if (!state || !db_path || !state->action_system[0]) {
        return;
    }
    jw_system_display_name(db_path, state->action_system,
                           state->action_system_display,
                           sizeof(state->action_system_display));
    if (state->games_open && strcmp(state->game_system, state->action_system) == 0) {
        snprintf(state->game_system_display, sizeof(state->game_system_display), "%s",
                 state->action_system_display[0]
                     ? state->action_system_display
                     : state->action_system);
    }
}

static void jw__refresh_after_action_write(const char *db_path,
                                           jw_launcher_state *state) {
    if (!state) {
        return;
    }
    (void)jw__reload_library_from_db(db_path, state);
    jw__refresh_action_game_from_db(db_path, state);
    jw__refresh_action_system_display(db_path, state);
    jw__action_refresh(db_path, state);
}

static void jw__cycle_action_core(const char *db_path, jw_launcher_state *state,
                                  int direction) {
    if (!state || state->action_core_count == 0) {
        return;
    }
    int idx = jw__action_find_core(state, state->action_core_effective);
    if (idx < 0) idx = 0;
    int next = (idx + direction + (int)state->action_core_count) %
               (int)state->action_core_count;
    const char *next_id = state->action_core_choices[next].id;

    int rc;
    if (state->action_scope == JW_ACTION_GAME) {
        rc = jw_db_set_game_setting(db_path, state->action_game.id,
                                    JW_CONTENT_SETTING_CORE_ID, next_id);
    } else {
        rc = jw_db_set_system_setting(db_path, state->action_system,
                                      JW_CONTENT_SETTING_CORE_ID, next_id);
    }
    if (rc == 0) {
        snprintf(state->status, sizeof(state->status), "Core: %.160s",
                 jw__action_core_label(state, next_id));
        jw__refresh_after_action_write(db_path, state);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", "Core update failed");
    }
}

static void jw__cycle_action_performance(const char *db_path,
                                         jw_launcher_state *state,
                                         int direction) {
    if (!state) {
        return;
    }
    const char *current = "";
    if (state->action_scope == JW_ACTION_GAME &&
        state->action_perf_game_override[0]) {
        current = state->action_perf_game_override;
    } else if (state->action_perf_system_override[0]) {
        current = state->action_perf_system_override;
    }
    int idx = jw__action_perf_index(current);
    int next = (idx + direction + JW_ACTION_PERF_COUNT) % JW_ACTION_PERF_COUNT;
    const char *next_name = jw_platform_perf_profile_name(kActionPerfProfiles[next]);

    int rc;
    if (state->action_scope == JW_ACTION_GAME) {
        rc = jw_db_set_game_setting(db_path, state->action_game.id,
                                    JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                    next_name);
    } else {
        rc = jw_db_set_system_setting(db_path, state->action_system,
                                      JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                      next_name);
    }
    if (rc == 0) {
        snprintf(state->status, sizeof(state->status), "Performance: %.160s",
                 jw_platform_perf_profile_label(kActionPerfProfiles[next]));
        jw__refresh_after_action_write(db_path, state);
    } else {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Performance update failed");
    }
}

static void jw__edit_action_display_name(const char *db_path,
                                         jw_launcher_state *state) {
    if (!state ||
        (state->action_scope == JW_ACTION_GAME && state->action_game.id <= 0) ||
        (state->action_scope == JW_ACTION_SYSTEM && !state->action_system[0]) ||
        state->action_scope == JW_ACTION_NONE) {
        return;
    }

    const char *current = state->action_scope == JW_ACTION_SYSTEM
        ? state->action_system_display
        : state->action_game.name;
    cat_keyboard_result result;

    /* Cover Flow: float the keyboard over a dimmed snapshot of the game stage,
       dressed in the platinum CF palette — matching the CF actions overlay.
       Other layouts fall through to the standard themed keyboard. */
    SDL_Texture *cf_backdrop = NULL;
    cat_theme *cf_theme = NULL;
    cat_draw_color sv_hl = {0}, sv_hltxt = {0}, sv_accent = {0}, sv_hint = {0};
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW) {
        SDL_Renderer *ren = cat_get_renderer();
        int sw = cat_get_screen_width();
        int sh = cat_get_screen_height();
        cf_backdrop = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, sw, sh);
        if (cf_backdrop) {
            SDL_SetRenderTarget(ren, cf_backdrop);
            if (state->action_scope == JW_ACTION_GAME) {
                jw__draw_coverflow_games_stage(state);
            } else {
                SDL_SetRenderDrawColor(ren, 6, 7, 9, 255);
                SDL_RenderClear(ren);
            }
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 172);
            SDL_RenderFillRect(ren, NULL);
            SDL_SetRenderTarget(ren, NULL);

            cf_theme  = cat_get_theme();
            sv_hl     = cf_theme->highlight;
            sv_hltxt  = cf_theme->highlighted_text;
            sv_accent = cf_theme->accent;
            sv_hint   = cf_theme->hint;
            cat_draw_color cf_sel     = kCfSelect;               /* selected key + input pill */
            cat_draw_color cf_seltext = {  12,  14,  18, 255 };  /* dark text on the platinum pill */
            cat_draw_color cf_keybg   = {  30,  34,  42, 255 };  /* unselected key bg */
            cat_draw_color cf_keytxt  = { 236, 238, 240, 255 };  /* unselected key text */
            cf_theme->highlight        = cf_sel;
            cf_theme->highlighted_text = cf_seltext;
            cf_theme->accent           = cf_keybg;
            cf_theme->hint             = cf_keytxt;
        }
    }

    int rc = cat_keyboard_ex(current,
                          "Start: Confirm\nY: Cancel\nLeave empty to reset",
                          CAT_KB_GENERAL, cf_backdrop, &result);

    if (cf_theme) {
        cf_theme->highlight        = sv_hl;
        cf_theme->highlighted_text = sv_hltxt;
        cf_theme->accent           = sv_accent;
        cf_theme->hint             = sv_hint;
    }
    if (cf_backdrop) SDL_DestroyTexture(cf_backdrop);
    if (rc == CAT_OK) {
        if (result.text[0]) {
            int write_rc = state->action_scope == JW_ACTION_SYSTEM
                ? jw_db_set_system_setting(db_path, state->action_system,
                                           JW_CONTENT_SETTING_DISPLAY_NAME,
                                           result.text)
                : jw_db_set_game_setting(db_path, state->action_game.id,
                                         JW_CONTENT_SETTING_DISPLAY_NAME,
                                         result.text);
            if (write_rc != 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Display name update failed");
                return;
            }
            snprintf(state->status, sizeof(state->status), "Display Name: %.160s",
                     result.text);
        } else {
            int delete_rc = state->action_scope == JW_ACTION_SYSTEM
                ? jw_db_delete_system_setting(db_path, state->action_system,
                                              JW_CONTENT_SETTING_DISPLAY_NAME)
                : jw_db_delete_game_setting(db_path, state->action_game.id,
                                            JW_CONTENT_SETTING_DISPLAY_NAME);
            if (delete_rc != 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Display name reset failed");
                return;
            }
            snprintf(state->status, sizeof(state->status), "%s",
                     "Display name reset");
        }
        jw__refresh_after_action_write(db_path, state);
    } else if (rc == CAT_ERROR) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "display name keyboard failed");
    }
}

static void jw__reset_action_overrides(const char *db_path,
                                       jw_launcher_state *state) {
    if (!state) {
        return;
    }
    int rc = 0;
    if (state->action_scope == JW_ACTION_GAME) {
        rc |= jw_db_delete_game_setting(db_path, state->action_game.id,
                                        JW_CONTENT_SETTING_CORE_ID);
        rc |= jw_db_delete_game_setting(db_path, state->action_game.id,
                                        JW_CONTENT_SETTING_PERFORMANCE_PROFILE);
        rc |= jw_db_delete_game_setting(db_path, state->action_game.id,
                                        JW_CONTENT_SETTING_DISPLAY_NAME);
    } else if (state->action_scope == JW_ACTION_SYSTEM) {
        rc |= jw_db_delete_system_setting(db_path, state->action_system,
                                          JW_CONTENT_SETTING_CORE_ID);
        rc |= jw_db_delete_system_setting(db_path, state->action_system,
                                          JW_CONTENT_SETTING_PERFORMANCE_PROFILE);
        rc |= jw_db_delete_system_setting(db_path, state->action_system,
                                          JW_CONTENT_SETTING_DISPLAY_NAME);
    }
    if (rc == 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Overrides reset");
        jw__refresh_after_action_write(db_path, state);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", "Reset failed");
    }
}

static bool jw__screenscraper_account_configured(const char *db_path) {
    char username[64] = "";
    return db_path && db_path[0] &&
           jw_db_get_setting(db_path, "screenscraper_user",
                             username, sizeof(username)) == 0 &&
           username[0] != '\0';
}

static bool jw__confirm_anonymous_batch_scrape(bool missing_only) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Start",  .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = missing_only
            ? "Scrape missing artwork anonymously? ScreenScraper can be slow and quota-limited without an account."
            : "Re-scrape all artwork anonymously? ScreenScraper can be slow and quota-limited without an account.",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__start_action_scrape(const char *socket_path, const char *db_path,
                                    jw_launcher_state *state, bool missing_only) {
    bool is_game = state->action_scope == JW_ACTION_GAME;
    if (!is_game && !jw__screenscraper_account_configured(db_path) &&
        !jw__confirm_anonymous_batch_scrape(missing_only)) {
        snprintf(state->status, sizeof(state->status), "%s", "Scrape cancelled");
        return;
    }
    int enqueued = 0;
    char status[256] = "";
    int rc = jw_ipc_scrape_start(socket_path,
                                 is_game ? "game" : "system",
                                 is_game ? state->action_game.system
                                         : state->action_system,
                                 is_game ? state->action_game.rom_path : NULL,
                                 missing_only, &enqueued,
                                 status, sizeof(status));
    if (rc != 0) {
        snprintf(state->status, sizeof(state->status), "Scrape failed: %.180s",
                 status[0] ? status : "daemon unavailable");
        return;
    }
    if (is_game) {
        char name[256];
        jw__clean_rom_name(state->action_game.name, name, sizeof(name));
        snprintf(state->status, sizeof(state->status),
                 "Scraping artwork: %.180s", name);
    } else if (enqueued == 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 missing_only ? "Nothing to scrape - all games have artwork"
                              : "Nothing to scrape - no games in this system");
    } else {
        snprintf(state->status, sizeof(state->status),
                 "Scraping %d game%s in %.140s", enqueued,
                 enqueued == 1 ? "" : "s", state->action_system_display);
    }
    jw__action_refresh(db_path, state);
}

static void jw__cancel_action_scrape(const char *socket_path, const char *db_path,
                                     jw_launcher_state *state) {
    bool is_game = state->action_scope == JW_ACTION_GAME;
    int removed = 0;
    if (jw_ipc_scrape_cancel(socket_path,
                             is_game ? "game" : "system",
                             is_game ? state->action_game.system
                                     : state->action_system,
                             is_game ? state->action_game.rom_path : NULL,
                             &removed) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Cancel failed: daemon unavailable");
        return;
    }
    snprintf(state->status, sizeof(state->status),
             "Scraping cancelled (%d item%s stopped)", removed,
             removed == 1 ? "" : "s");
    jw__action_refresh(db_path, state);
}

static void jw__select_action_row(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, bool *running) {
    if (!state || state->action_list.cursor < 0 ||
        state->action_list.cursor >= state->action_row_count) {
        return;
    }
    jw_action_row_kind row = state->action_rows[state->action_list.cursor];
    switch (row) {
        case JW_ACTION_ROW_SEARCH:
            state->actions_open = false;
            jw__open_search(db_path, state);
            break;
        case JW_ACTION_ROW_DISPLAY_NAME:
            jw__edit_action_display_name(db_path, state);
            break;
        case JW_ACTION_ROW_CORE:
            jw__cycle_action_core(db_path, state, +1);
            break;
        case JW_ACTION_ROW_PERFORMANCE:
            jw__cycle_action_performance(db_path, state, +1);
            break;
        case JW_ACTION_ROW_SCRAPE:
            jw__start_action_scrape(socket_path, db_path, state, false);
            break;
        case JW_ACTION_ROW_SCRAPE_CANCEL:
            jw__cancel_action_scrape(socket_path, db_path, state);
            break;
        case JW_ACTION_ROW_RESET:
            jw__reset_action_overrides(db_path, state);
            break;
        default:
            break;
    }
}

static void jw__handle_actions_input(const char *socket_path, const char *db_path,
                                     jw_launcher_state *state,
                                     cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->action_list, -1, state->action_row_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->action_list, +1, state->action_row_count);
            break;
        case CAT_BTN_LEFT:
        case CAT_BTN_RIGHT:
            if (state->action_list.cursor >= 0 &&
                state->action_list.cursor < state->action_row_count) {
                jw_action_row_kind row = state->action_rows[state->action_list.cursor];
                int dir = button == CAT_BTN_LEFT ? -1 : +1;
                if (row == JW_ACTION_ROW_CORE) {
                    jw__cycle_action_core(db_path, state, dir);
                } else if (row == JW_ACTION_ROW_PERFORMANCE) {
                    jw__cycle_action_performance(db_path, state, dir);
                }
            }
            break;
        case CAT_BTN_A:
            jw__select_action_row(socket_path, db_path, state, running);
            break;
        case CAT_BTN_X:   /* X opened the menu; let it toggle closed too */
        case CAT_BTN_B:
            state->actions_open = false;
            state->action_scope = JW_ACTION_NONE;
            state->status[0] = '\0';
            break;
        case CAT_BTN_L1:
        case CAT_BTN_R1:
            /* The actions header shows the section tabs, so L1/R1 tabs away —
               closing the actions view and landing on the adjacent section.
               jw__switch_tab_slide closes the view and glides (Options shows the
               tab bar). Tabbed layout only. */
            if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab_slide(state, button == CAT_BTN_L1 ? -1 : +1, db_path);
            break;
        case CAT_BTN_MENU:
            jw__open_menu(state);
            break;
        default:
            break;
    }
}

static void jw__activate_tabbed(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, bool *running) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:
            if (state->recents_count > 0 && state->list.cursor < state->recents_count) {
                /* Recents resumes: the daemon loads the newest state (preferring
                   the game-switcher slot) when one exists and cold-launches
                   otherwise, so no per-entry state probe is needed here. */
                jw__launch_game_entry_with_mode(socket_path, state,
                                                &state->recents[state->list.cursor],
                                                true, running);
            }
            break;
        case JW_TAB_FAVORITES:
            if (state->favorites_count > 0 && state->list.cursor < state->favorites_count) {
                jw__launch_game_entry(socket_path, state,
                                      &state->favorites[state->list.cursor], running);
            }
            break;
        case JW_TAB_GAMES:
            if (state->system_count > 0 && state->list.cursor < state->system_count) {
                jw__open_system_games(db_path, state->systems[state->list.cursor].name, state);
            }
            break;
        case JW_TAB_APPS:
            jw__launch_selected_app(socket_path, state, running);
            break;
        default:
            break;
    }
    (void)socket_path;
    (void)running;
}

static void jw__activate_flat(const char *socket_path, const char *db_path,
                               jw_launcher_state *state, bool *running) {
    if (state->list.cursor >= state->flat_count) return;
    const jw_flat_item *it = &state->flat_items[state->list.cursor];
    switch (it->kind) {
        case JW_FLAT_SETTINGS:
            jw_settings_ui_enter(&state->settings);
            break;
        case JW_FLAT_TOOLS:
            state->tools_open = true;
            cat_list_state_init(&state->tools_list, 4);
            break;
        case JW_FLAT_FAVORITES:
            jw__open_favorites(db_path, state);
            break;
        case JW_FLAT_RECENTLY_PLAYED:
            jw__open_recents(db_path, state);
            break;
        case JW_FLAT_APPS:
            jw__open_apps(state);
            break;
        case JW_FLAT_SYSTEM:
            jw__open_system_games(db_path, state->systems[it->system_idx].name, state);
            break;
        default:
            break;
    }
    (void)socket_path;
    (void)running;
}

/* Rebuild layout-dependent state. Call after the active stylesheet's
 * launcher.layout may have changed (theme switch) or at first startup. */
static void jw__rebuild_for_layout(jw_launcher_state *state) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;

    state->tools_open = false;
    state->apps_open = false;
    jw__tab_anim_clear(state);   /* cancel any in-flight tab slide on layout/theme change */
    jw__system_icon_memo_clear(state);

    /* Refresh per-console color palette from the active theme stylesheet.
     * Empty / missing maps degrade to hash-derived colors in the carousel. */
    jw_console_colors_load(&state->console_colors,
                           cat_get_active_theme_dir(),
                           cat_get_active_theme_name());

    if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW) {
        jw__build_carousel_list(state);
    } else if (layout == CAT_LAUNCHER_VERTICAL) {
        jw__build_flat_list(state);
    } else {
        state->flat_count = 0;
    }

    int fh         = jw__footer_height(state);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h     = TTF_FontHeight(body) + CAT_S(12);
    int visible;
    if (layout == CAT_LAUNCHER_TABBED) {
        /* Tab pages use the shared box model with a tab-bar-only header (no
           sub-header), so the row count matches the renderer exactly. */
        visible = jw__browse_visible_rows(state, cat_get_tab_bar_height());
    } else {
        int sb_h      = CAT_DS(20);
        int margin    = CAT_S(10);
        int content_h = cat_get_screen_height() - sb_h - margin - fh - margin;
        visible = content_h / item_h;
    }
    if (visible < 1) visible = 1;

    int count = (layout == CAT_LAUNCHER_TABBED) ? jw__tab_list_count(state)
                                                : state->flat_count;
    cat_list_state_init(&state->list, visible);
    cat_list_state_jump(&state->list, 0, count);
}

static void jw__handle_search_input(const char *socket_path, const char *db_path,
                                    jw_launcher_state *state,
                                    cat_button button, bool *running) {
    /* Cover Flow uses the inline keyboard, not the list-nav search input. */
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW) {
        jw__cf_kbd_input(socket_path, db_path, state, button, running);
        return;
    }
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->search_list, -1, state->search_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->search_list, +1, state->search_count);
            break;
        case CAT_BTN_LEFT:
            /* Cover Flow flows one card (wrap only when the list fills the window);
               other layouts page the list. */
            if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW)
                jw__cf_carousel_step(&state->search_list, -1, state->search_count,
                                     CAT_LAUNCHER_COVERFLOW);
            else
                cat_list_state_page(&state->search_list, -1, state->search_count);
            break;
        case CAT_BTN_RIGHT:
            if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW)
                jw__cf_carousel_step(&state->search_list, +1, state->search_count,
                                     CAT_LAUNCHER_COVERFLOW);
            else
                cat_list_state_page(&state->search_list, +1, state->search_count);
            break;
        case CAT_BTN_A:
            jw__launch_selected_search_result(socket_path, state, running);
            break;
        case CAT_BTN_Y:
            jw__favorite_selected_search_result(db_path, state);
            break;
        case CAT_BTN_B:
            state->search_open = false;
            state->status[0] = '\0';
            break;
        case CAT_BTN_X:
            jw__open_search(db_path, state);
            break;
        case CAT_BTN_L1:
        case CAT_BTN_R1:
            /* Tabbed: the results header shows the section tabs, so L1/R1 tabs
               away (closing search, gliding to the adjacent section). Cover Flow:
               L1/R1 flow the carousel one card with wrap. */
            if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab_slide(state, button == CAT_BTN_L1 ? -1 : +1, db_path);
            else if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW)
                jw__cf_carousel_step(&state->search_list, button == CAT_BTN_L1 ? -1 : +1,
                                     state->search_count, CAT_LAUNCHER_COVERFLOW);
            break;
        default:
            break;
    }
}

static void jw__toggle_favorite_selected(const char *db_path, jw_launcher_state *state) {
    if (state->game_count <= 0 || state->game_list.cursor >= state->game_count) {
        return;
    }
    jw_game_entry *game = &state->games[state->game_list.cursor];
    int want_on = !game->favorite;

    if (jw_db_set_favorite(db_path, "game", game->id, want_on) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Favorite update failed");
        return;
    }
    game->favorite = want_on;

    /* When viewing the Favorites list, an unfavorite must drop the row, so
       reload the list and keep the cursor near its prior position. */
    if (state->games_are_favorites && !want_on) {
        int prev_cursor = state->game_list.cursor;
        /* Reloading the list in place is not "opened Favorites". */
        bool was_muted = state->haptics_muted;
        state->haptics_muted = true;
        jw__open_favorites(db_path, state);
        state->haptics_muted = was_muted;
        if (state->game_count > 0) {
            int c = prev_cursor >= state->game_count ? state->game_count - 1 : prev_cursor;
            cat_list_state_jump(&state->game_list, c, state->game_count);
        }
        return;
    }

    /* Bound the name so the prefix + name always fit the status buffer; the
       line is ellipsized on screen anyway. */
    snprintf(state->status, sizeof(state->status), "%s %.200s",
             want_on ? "Favorited" : "Unfavorited", game->name);
}

/* Toggle the highlighted search game's favorite state. Empty results, apps,
   and an invalid cursor are inert. */
static void jw__favorite_selected_search_result(const char *db_path,
                                                 jw_launcher_state *state) {
    if (!state || state->search_count <= 0 ||
        state->search_list.cursor < 0 ||
        state->search_list.cursor >= state->search_count) {
        return;
    }
    jw_search_result *result =
        &state->search_results[state->search_list.cursor];
    if (result->kind != JW_SEARCH_GAME) return;

    int want_on = !result->favorite;
    if (jw_db_set_favorite(db_path, "game", result->id, want_on) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Favorite update failed");
        return;
    }
    result->favorite = want_on;
    snprintf(state->status, sizeof(state->status), "%s %.200s",
             want_on ? "Favorited" : "Unfavorited", result->name);
}

/* Drop the selected game from the Recents tab's play-history and reload the list,
   keeping the cursor near its prior spot. The game and any favorite are untouched
   — it just leaves Recents. */
/* Label accessor for alphabetical letter-jump in the coverflow game carousel. */
static const char *jw__games_label_cb(int i, void *user) {
    const jw_launcher_state *st = (const jw_launcher_state *)user;
    return (i >= 0 && i < st->game_count) ? st->games[i].name : "";
}

static void jw__handle_game_browser_input(const char *socket_path, const char *db_path,
                                          jw_launcher_state *state,
                                          cat_button button, bool *running) {
    /* Coverflow reads the game list as a horizontal carousel: Left/Right (and the
       L1/R1 shoulders) step one cover with wrap-around, Up/Down jump to the next
       alphabetical letter. The grid layouts keep their row/page navigation. */
    bool cf = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW);
    switch (button) {
        case CAT_BTN_UP:
            if (cf) cat_list_state_jump_letter(&state->game_list, jw__games_label_cb,
                                               state, state->game_count, -1);
            else    cat_list_state_move(&state->game_list, -1, state->game_count);
            break;
        case CAT_BTN_DOWN:
            if (cf) cat_list_state_jump_letter(&state->game_list, jw__games_label_cb,
                                               state, state->game_count, +1);
            else    cat_list_state_move(&state->game_list, +1, state->game_count);
            break;
        case CAT_BTN_LEFT:
            if (cf) jw__cf_carousel_step(&state->game_list, -1, state->game_count,
                                         CAT_LAUNCHER_COVERFLOW);
            else    cat_list_state_page(&state->game_list, -1, state->game_count);
            break;
        case CAT_BTN_RIGHT:
            if (cf) jw__cf_carousel_step(&state->game_list, +1, state->game_count,
                                         CAT_LAUNCHER_COVERFLOW);
            else    cat_list_state_page(&state->game_list, +1, state->game_count);
            break;
        case CAT_BTN_L1:
            if (cf) jw__cf_carousel_step(&state->game_list, -1, state->game_count,
                                         CAT_LAUNCHER_COVERFLOW);
            break;
        case CAT_BTN_R1:
            if (cf) jw__cf_carousel_step(&state->game_list, +1, state->game_count,
                                         CAT_LAUNCHER_COVERFLOW);
            break;
        case CAT_BTN_A:
            jw__launch_selected_game(socket_path, state, running);
            break;
        case CAT_BTN_Y:
            jw__toggle_favorite_selected(db_path, state);
            break;
        case CAT_BTN_B:
            jw__close_game_browser(state);
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

static void jw__handle_app_browser_input(const char *socket_path,
                                         jw_launcher_state *state,
                                         cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->app_list, -1, state->app_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->app_list, +1, state->app_count);
            break;
        case CAT_BTN_LEFT:
            cat_list_state_page(&state->app_list, -1, state->app_count);
            break;
        case CAT_BTN_RIGHT:
            cat_list_state_page(&state->app_list, +1, state->app_count);
            break;
        case CAT_BTN_A:
            jw__launch_app_at(socket_path, state, state->app_list.cursor, running);
            break;
        case CAT_BTN_B:
            state->apps_open = false;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

/* Open the Select switcher: a fresh load of Recents in carousel form. The
   underlying layout state is untouched, so closing just clears the flag. */
static void jw__open_switcher(const char *db_path, jw_launcher_state *state) {
    /* RetroArch States/ root for savestate thumbnails: prefer the explicit
       STATES_PATH from env.sh, else the default <sdcard>/States layout. */
    const char *states = getenv("STATES_PATH");
    char states_buf[PATH_MAX + 16];
    if (!states || !states[0]) {
        snprintf(states_buf, sizeof(states_buf), "%s/States", state->sdcard_root);
        states = states_buf;
    }
    jw_game_switcher_reset(&state->switcher, false, state->sdcard_root, states);
    jw_game_switcher_load(&state->switcher, db_path);
    jw_game_switcher_resolve_thumbnails(&state->switcher);
    state->switcher_open = true;
    state->status[0] = '\0';
}

/* Y in the switcher: drop the selected game from Recents only (id, artwork,
   favorite, and the game itself are untouched), then refresh both the carousel
   and the Recents tab list so they stay consistent. */
static void jw__switcher_remove_selected(const char *db_path, jw_launcher_state *state) {
    const jw_game_entry *sel = jw_game_switcher_selected(&state->switcher);
    if (!sel || sel->id < 0) {
        return;
    }
    char removed_name[256];
    snprintf(removed_name, sizeof(removed_name), "%.200s", sel->name);

    if (jw_db_remove_recent(db_path, "game", sel->id) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Remove failed");
        return;
    }

    jw_game_switcher_remove_selected(&state->switcher);

    /* Keep the Recents tab in sync (it also reloads on tab entry). */
    jw__load_recents_tab(db_path, state);
    if (state->current_tab == JW_TAB_RECENTS) {
        int c = state->list.cursor;
        if (c >= state->recents_count) {
            c = state->recents_count > 0 ? state->recents_count - 1 : 0;
        }
        cat_list_state_jump(&state->list, c, jw__tab_list_count(state));
    }

    snprintf(state->status, sizeof(state->status), "Removed %.200s", removed_name);
}

static void jw__handle_switcher_input(const char *socket_path, const char *db_path,
                                      jw_launcher_state *state,
                                      cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_LEFT:
        case CAT_BTN_UP:        /* alias Up/Down to the carousel for desktop ease */
            jw_game_switcher_move(&state->switcher, -1);
            break;
        case CAT_BTN_RIGHT:
        case CAT_BTN_DOWN:
            jw_game_switcher_move(&state->switcher, +1);
            break;
        case CAT_BTN_A: {
            const jw_game_entry *sel = jw_game_switcher_selected(&state->switcher);
            if (sel) {
                state->switcher_open = false;
                jw__launch_game_entry_with_mode(socket_path, state, sel, true, running);
            }
            break;
        }
        case CAT_BTN_Y:
            jw__switcher_remove_selected(db_path, state);
            break;
        case CAT_BTN_B:
        case CAT_BTN_SELECT:   /* Select closes the switcher too (it opened it) */
            state->switcher_open = false;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

/* Host About / System Update modally over the launcher, on a dedicated settings UI
   instance (kept separate from the Settings tab's, so opening it from the menu can't
   disturb the tab). Blocks until the user backs out, then returns to the overlay. */
static void jw__menu_host_setting(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, jw_settings_screen screen) {
    static jw_settings_ui *ui = NULL;
    if (!ui) {
        ui = malloc(sizeof(*ui));
        if (!ui) return;
        char theme_name[256];
        jw_resolve_theme_name(db_path, theme_name, sizeof(theme_name));
        jw_settings_ui_init(ui, db_path, theme_name, socket_path);
    }
    jw_settings_ui_open(ui, screen);
    /* Blocking modal: it never returns to jw__handle_input, so the surface tick
       has to come from here. The matching one fires when the loop ends. */
    jw__haptic(state, "select");

    char status[256] = { 0 };
    bool hints = jw_settings_show_hints(&state->settings);
    int m = CAT_S(12);
    bool running = true;
    cat_request_frame();
    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            /* MENU exits System entirely back to Content, from any depth — B only
               steps back within System (to the Info/Actions list). */
            if (ev.button == CAT_BTN_MENU) {
                state->menu_open = false;
                state->status[0] = '\0';
                running = false;
                break;
            }
            /* L1/R1 switches the System tab from anywhere — back out to the menu
               list on the adjacent tab, mirroring how the content tabs let you
               switch sections from within a drilled-in view. */
            if (ev.button == CAT_BTN_L1 || ev.button == CAT_BTN_R1) {
                /* This intentionally primes state->settings (the Settings tab UI),
                   not the modal ui, so landing on Settings renders immediately. */
                jw__switch_system_tab(state, ev.button == CAT_BTN_L1 ? -1 : 1);
                running = false;
                break;
            }
            bool theme_changed = false;
            jw_settings_ui_handle_button(ui, ev.button, status, sizeof(status),
                                         &theme_changed);
            if (theme_changed)
                jw__rebuild_for_layout(state);
            if (!jw_settings_ui_is_open(ui) ||
                jw_settings_ui_screen(ui) == JW_SETTINGS_HOME) {
                running = false;
                break;
            }
        }
        if (!running) break;
        if (jw_settings_ui_screen(ui) == JW_SETTINGS_UPDATE)
            jw_settings_ui_refresh_update(ui);

        cat_clear_screen();
        /* Keep the Actions/Info tab bar pinned at top while a page is open, so the
           system side matches the content side (which never drops its tab bar). */
        jw__draw_menu_tab_bar(state);
        SDL_Rect cr = cat_get_content_rect(true, hints, false);
        jw_settings_ui_render(ui, cr.x + m, cr.y, cr.w - m * 2, cr.h);
        if (hints) {
            jw_settings_screen scr = jw_settings_ui_screen(ui);
            if (scr == JW_SETTINGS_ABOUT || scr == JW_SETTINGS_LIBRARY ||
                scr == JW_SETTINGS_PLAYTIME) {
                cat_footer_item f[] = { { CAT_BTN_B, "Back", true, JW_HINT("B") } };
                jw__footer_direct(f, 1);
            } else {
                cat_footer_item f[] = {
                    { CAT_BTN_X, "Releases", false, JW_HINT("X") },
                    { CAT_BTN_B, "Back",     true,  JW_HINT("B") },
                    { CAT_BTN_A, "Select",   true,  JW_HINT("A") },
                };
                jw__footer_direct(f, 3);
            }
        }
        jw__present();
    }
    jw_settings_ui_close(ui);
    jw__haptic(state, "select");   /* handed the screen back */
    cat_request_frame();
}

static void jw__menu_activate(const char *socket_path, const char *db_path,
                              jw_launcher_state *state, bool *running) {
    if (state->menu_tab == JW_SMTAB_INFO) {
        switch (state->menu_list.cursor) {
            case JW_SI_DEVICE:
                jw__menu_host_setting(socket_path, db_path, state, JW_SETTINGS_ABOUT);
                break;
            case JW_SI_LIBRARY:
                jw__menu_host_setting(socket_path, db_path, state, JW_SETTINGS_LIBRARY);
                break;
            case JW_SI_PLAYTIME:
                jw__menu_host_setting(socket_path, db_path, state, JW_SETTINGS_PLAYTIME);
                break;
            default:
                break;
        }
        return;
    }
    switch (state->menu_list.cursor) {
        case JW_SA_SEARCH:
            state->menu_open = false;
            jw__open_search(db_path, state);
            break;
        case JW_SA_START_5GAME:
            state->menu_open = false;
            jw__focus_setup_begin(state);
            break;
        case JW_SA_PAKRAT:
            jw__open_pakrat_store(state);
            break;
        case JW_SA_UPDATE:
            jw__menu_host_setting(socket_path, db_path, state, JW_SETTINGS_UPDATE);
            break;
        case JW_SA_RESCAN: {
            /* Show "Scanning…" in the right pane and let the status poller refresh
               counts when the daemon publishes the next library generation. */
            char buf[160] = { 0 };
            state->menu_scanning = true;
            cat_request_frame();
            jw__render_menu(state);
            jw_ipc_scan_library(socket_path, buf, sizeof(buf));
            jw_ipc_library_status_info lib;
            if (jw_ipc_library_status_full(socket_path, &lib) == 0) {
                state->library_generation = lib.generation;
                state->scan_running = lib.scan_running;
                state->library_populated = lib.library_populated;
                state->scan_ready = lib.library_populated || !lib.scan_running;
            }
            state->menu_scanning = state->scan_running;
            /* Neither movement nor a surface change -- an action that ran and
               came back, so it reports its own outcome. */
            jw__haptic(state, buf[0] ? "commit" : "blocked");
            break;
        }
        case JW_SA_SLEEP:
            /* Blocks until the system resumes; keep the menu open so we land back. */
            jw_ipc_platform_action(socket_path, "sleep", 0);
            break;
        case JW_SA_EXIT_STOCK:
            jw_ipc_exit_stock(socket_path);
            cat_hide_window();
            *running = false;
            break;
        case JW_SA_REBOOT:
            jw_ipc_platform_action(socket_path, "reboot", 0);
            cat_hide_window();
            *running = false;
            break;
        case JW_SA_POWEROFF:
            jw_ipc_platform_action(socket_path, "poweroff", 0);
            cat_hide_window();
            *running = false;
            break;
        default:
            break;
    }
}

static void jw__handle_menu_input(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state,
                                  cat_button button, bool *running) {
    /* L1/R1 switches the System tab from any tab. Entering Settings opens the
       settings UI at its home; leaving it closes the UI; Actions/Info reset their
       selectable list to the tab's item count. */
    if (button == CAT_BTN_L1 || button == CAT_BTN_R1) {
        jw__switch_system_tab(state, button == CAT_BTN_L1 ? -1 : 1);
        return;
    }

    /* Settings tab: forward to the real settings UI (its own sub-nav + priming).
       MENU closes the whole menu; B at settings home closes the UI (still_open
       false), which backs out of the menu to the games view. */
    if (state->menu_tab == JW_SMTAB_SETTINGS) {
        if (button == CAT_BTN_MENU) {
            jw_settings_ui_close(&state->settings);
            state->menu_open = false;
            state->status[0] = '\0';
            return;
        }
        bool theme_changed = false;
        bool still_open = jw_settings_ui_handle_button(
            &state->settings, button,
            state->status, sizeof(state->status), &theme_changed);
        if (theme_changed)
            jw__rebuild_for_layout(state);
        if (!still_open) {
            /* B at the settings home is a no-op: stay in System. Only MENU exits
               back to Content. Re-enter so the UI stays open at its home. */
            jw_settings_ui_enter(&state->settings);
        }
        return;
    }

    /* Actions / Info: simple selectable lists. */
    int tab_count;
    jw__menu_tab_items(state->menu_tab, &tab_count);
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->menu_list, -1, tab_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->menu_list, +1, tab_count);
            break;
        case CAT_BTN_A:
            jw__menu_activate(socket_path, db_path, state, running);
            break;
        case CAT_BTN_MENU:
            /* MENU exits System back to Content; B stays inside System (no-op at
               a tab's root — you leave via MENU). Ticks on the way out as well as
               in: leaving a page is the same kind of event as entering one, and a
               button that buzzes opening a screen but goes dead closing it reads
               as a dropped input. */
            state->menu_open = false;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

static void jw__handle_pakrat_input(const char *db_path, jw_launcher_state *state,
                                    cat_button button) {
    if (!state) {
        return;
    }

    /* Drilled into a pak's full detail page: the d-pad scrolls the body and the
       install/uninstall actions live here; B returns to the store list. */
    if (state->pakrat_detail_open) {
        int line_h = TTF_FontHeight(cat_get_font(CAT_FONT_SMALL)) + CAT_S(8);
        switch (button) {
            case CAT_BTN_UP:
                cat_scroll_state_move(&state->pakrat_detail_scroll, -line_h);
                break;
            case CAT_BTN_DOWN:
                cat_scroll_state_move(&state->pakrat_detail_scroll, +line_h);
                break;
            case CAT_BTN_LEFT:
                cat_scroll_state_move(&state->pakrat_detail_scroll, -line_h * 4);
                break;
            case CAT_BTN_RIGHT:
                cat_scroll_state_move(&state->pakrat_detail_scroll, +line_h * 4);
                break;
            case CAT_BTN_A:
                jw__run_pakrat_action(db_path, state, JW_PAKRAT_UI_INSTALL);
                break;
            case CAT_BTN_Y:
                jw__run_pakrat_action(db_path, state, JW_PAKRAT_UI_UNINSTALL);
                break;
            case CAT_BTN_X: {
                int old_cursor = state->pakrat_list.cursor;
                jw__load_pakrat_store(state);
                cat_list_state_jump(&state->pakrat_list, old_cursor,
                                    state->pakrat_app_count);
                snprintf(state->status, sizeof(state->status), "%s",
                         state->pakrat_message[0]
                            ? state->pakrat_message
                            : "Pak Rat refreshed");
                break;
            }
            case CAT_BTN_B:
                state->pakrat_detail_open = false;
                state->status[0] = '\0';
                break;
            case CAT_BTN_L1:
            case CAT_BTN_R1:
                state->pakrat_detail_open = false;
                state->pakrat_open = false;
                state->menu_open = true;
                state->status[0] = '\0';
                jw__switch_system_tab(state, button == CAT_BTN_L1 ? -1 : 1);
                break;
            case CAT_BTN_MENU:
                state->pakrat_detail_open = false;
                state->pakrat_open = false;
                state->menu_open = false;
                state->status[0] = '\0';
                break;
            default:
                break;
        }
        return;
    }

    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->pakrat_list, -1, state->pakrat_app_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->pakrat_list, +1, state->pakrat_app_count);
            break;
        case CAT_BTN_LEFT:
            cat_list_state_page(&state->pakrat_list, -1, state->pakrat_app_count);
            break;
        case CAT_BTN_RIGHT:
            cat_list_state_page(&state->pakrat_list, +1, state->pakrat_app_count);
            break;
        case CAT_BTN_L1:
        case CAT_BTN_R1:
            state->pakrat_open = false;
            state->menu_open = true;
            state->status[0] = '\0';
            jw__switch_system_tab(state, button == CAT_BTN_L1 ? -1 : 1);
            break;
        case CAT_BTN_A:
            if (state->pakrat_app_count > 0 &&
                state->pakrat_list.cursor >= 0 &&
                state->pakrat_list.cursor < state->pakrat_app_count) {
                state->pakrat_detail_open = true;
                cat_scroll_state_init(&state->pakrat_detail_scroll);
                state->status[0] = '\0';
            }
            break;
        case CAT_BTN_X: {
            int old_cursor = state->pakrat_list.cursor;
            jw__load_pakrat_store(state);
            cat_list_state_jump(&state->pakrat_list, old_cursor,
                                state->pakrat_app_count);
            snprintf(state->status, sizeof(state->status), "%s",
                     state->pakrat_message[0]
                        ? state->pakrat_message
                        : "Pak Rat refreshed");
            break;
        }
        case CAT_BTN_B: {
            state->pakrat_open = false;
            state->menu_open = true;
            state->menu_tab = JW_SMTAB_ACTIONS;
            int n = 0;
            jw__menu_tab_items(state->menu_tab, &n);
            cat_list_state_init(&state->menu_list, n);
            cat_list_state_jump(&state->menu_list, JW_SA_PAKRAT, n);
            state->status[0] = '\0';
            break;
        }
        case CAT_BTN_MENU:
            state->pakrat_open = false;
            state->menu_open = false;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

/* Directional move across the 2-up/3-down tile grid: pick the tile whose center
   lies furthest in `dir` with the least cross-axis drift. Returns the new cursor
   (unchanged when nothing lies that way). */
static int jw__focus_nav(int cursor, int count, cat_button dir) {
    SDL_Rect cur;
    if (!jw_focus_screen_tile_rect(cursor, count, &cur)) return cursor;
    int cx = cur.x + cur.w / 2;
    int cy = cur.y + cur.h / 2;

    int best = cursor;
    long best_cost = -1;
    for (int i = 0; i < count; i++) {
        if (i == cursor) continue;
        SDL_Rect r;
        if (!jw_focus_screen_tile_rect(i, count, &r)) continue;
        int ix = r.x + r.w / 2;
        int iy = r.y + r.h / 2;
        int dx = ix - cx;
        int dy = iy - cy;
        long cost;
        switch (dir) {
            case CAT_BTN_RIGHT: if (dx <= 0) continue; cost = dx + labs(dy) * 4; break;
            case CAT_BTN_LEFT:  if (dx >= 0) continue; cost = -dx + labs(dy) * 4; break;
            case CAT_BTN_DOWN:  if (dy <= 0) continue; cost = dy + labs(dx) * 4; break;
            case CAT_BTN_UP:    if (dy >= 0) continue; cost = -dy + labs(dx) * 4; break;
            default: return cursor;
        }
        if (best_cost < 0 || cost < best_cost) { best_cost = cost; best = i; }
    }
    return best;
}

/* Leave focus mode: clear the persisted active flag + the SD recovery lock file,
   then drop to the normal launcher in place. The daemon re-reads focus state on
   each launcher spawn, so a later respawn won't re-enter focus. */
static void jw__focus_unlock_exit(jw_launcher_state *state) {
    /* Restore the Wi-Fi radio to its pre-focus state (only re-enable if it was on
       when the mode was turned on; if it was off, leave it off). */
    char wprev[8] = "";
    if (jw_wifi_available() &&
        jw_db_get_setting(state->db_path, JW_FOCUS_KEY_WIFI_PREV,
                          wprev, sizeof(wprev)) == 0 && wprev[0] == '1') {
        jw_wifi_set_radio(true);
    }
    int rc_db   = jw_db_set_setting(state->db_path, JW_FOCUS_KEY_ACTIVE, "0");
    int rc_lock = jw_focus_lock_remove(state->sdcard_root);
    sync();   /* flush both writes so the exit survives a later power-off, and so a
                 read-only-flipped card surfaces as a failed write above */
    if (rc_db != 0 || rc_lock != 0)
        jw_log_warn("focus: exit did not fully persist (db=%d lock=%d); the SD card "
                    "may be read-only — focus mode could re-lock on next boot",
                    rc_db, rc_lock);
    state->focus_active = false;
    state->focus_unlock_open = false;
    jw_log_info("focus: unlocked; returning to the normal launcher");
    cat_request_frame();
}

/* Commit the setup wizard's result: persist the config + write the SD lock file,
   then enter focus mode in place (no reboot — the daemon re-resolves focus on
   each launcher spawn, so the persisted state carries forward). */
static void jw__focus_setup_commit(jw_launcher_state *state) {
    jw_focus_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.active = true;
    cfg.id_count = state->focus_setup_count;
    for (int i = 0; i < cfg.id_count; i++) cfg.ids[i] = state->focus_setup_ids[i];
    cfg.lock = state->focus_setup_lock;
    cfg.style = state->focus_setup_style;
    /* Capture the current Wi-Fi radio state so it can be restored on exit, then
       turn Wi-Fi off (v1: helps BT coexistence + battery; RetroAchievements,
       the only thing that needs Wi-Fi here, is deferred). */
    cfg.wifi_prev = jw_wifi_radio_is_on() ? 1 : 0;
    if (cfg.lock == JW_FOCUS_LOCK_PIN) {
        char pin[JW_FOCUS_PIN_LEN + 1];
        for (int i = 0; i < JW_FOCUS_PIN_LEN; i++)
            pin[i] = (char)('0' + (state->focus_setup_pin[i] % 10));
        pin[JW_FOCUS_PIN_LEN] = '\0';
        jw_focus_pin_hash(pin, cfg.pin_hash);
    }

    char ids_csv[128];
    jw_focus_ids_to_csv(cfg.ids, cfg.id_count, ids_csv, sizeof(ids_csv));
    char wifi_prev[4];
    snprintf(wifi_prev, sizeof(wifi_prev), "%d", cfg.wifi_prev);
    const char *keys[] = {
        JW_FOCUS_KEY_IDS, JW_FOCUS_KEY_LOCK, JW_FOCUS_KEY_PIN_HASH,
        JW_FOCUS_KEY_STYLE, JW_FOCUS_KEY_WIFI_PREV, JW_FOCUS_KEY_ACTIVE,
    };
    const char *vals[] = {
        ids_csv, jw_focus_lock_name(cfg.lock), cfg.pin_hash,
        jw_focus_style_name(cfg.style), wifi_prev, "1",
    };
    int rc_db   = jw_db_set_settings(state->db_path, keys, vals,
                                     (int)(sizeof(keys) / sizeof(keys[0])));
    int rc_lock = jw_focus_lock_write(state->sdcard_root, &cfg);
    sync();   /* persist the config + lock file before we start locking the UI, so
                 a power-off can't leave the mode half-enabled */
    if (rc_db != 0 || rc_lock != 0)
        jw_log_warn("focus: setup did not fully persist (db=%d lock=%d); the SD card "
                    "may be read-only — the lock may not survive a reboot",
                    rc_db, rc_lock);
    if (jw_wifi_available()) jw_wifi_set_radio(false);

    /* Enter focus mode in place: mirror what jw__focus_init would resolve. */
    state->focus_lock = cfg.lock;
    state->focus_bw = (cfg.style == JW_FOCUS_STYLE_BW);
    strncpy(state->focus_pin_hash, cfg.pin_hash, sizeof(state->focus_pin_hash) - 1);
    state->focus_pin_hash[sizeof(state->focus_pin_hash) - 1] = '\0';
    state->focus_count = 0;
    for (int i = 0; i < cfg.id_count; i++) {
        jw_game_entry g;
        if (jw_db_get_game_by_id(state->db_path, cfg.ids[i], &g) == 0)
            state->focus_games[state->focus_count++] = g;
    }
    state->focus_cursor = 0;
    state->focus_unlock_open = false;
    state->focus_setup_open = false;
    state->focus_active = true;   /* wizard guarantees >=1 pick; enter focus mode */
    jw_log_info("focus: setup committed; entering focus mode (%d game(s), lock=%s, style=%s)",
                state->focus_count, jw_focus_lock_name(cfg.lock),
                jw_focus_style_name(cfg.style));
    cat_request_frame();
}

/* ─── 5-Game Mode setup wizard ───────────────────────────────────────────── */

static bool jw__fsetup_is_picked(const jw_launcher_state *st, int id) {
    for (int i = 0; i < st->focus_setup_count; i++)
        if (st->focus_setup_ids[i] == id) return true;
    return false;
}

static void jw__fsetup_toggle(jw_launcher_state *st, int id) {
    st->focus_setup_note[0] = '\0';
    for (int i = 0; i < st->focus_setup_count; i++) {
        if (st->focus_setup_ids[i] == id) {
            for (int j = i; j < st->focus_setup_count - 1; j++)
                st->focus_setup_ids[j] = st->focus_setup_ids[j + 1];
            st->focus_setup_count--;
            jw__haptic(st, "select");            /* deselected */
            return;
        }
    }
    if (st->focus_setup_count >= JW_FOCUS_SCREEN_MAX_TILES) {
        snprintf(st->focus_setup_note, sizeof(st->focus_setup_note),
                 "5 games max — deselect one first");
        jw__haptic(st, "blocked");               /* refused: the set is full */
        return;
    }
    st->focus_setup_ids[st->focus_setup_count++] = id;
    jw__haptic(st, "select");                    /* selected */
}

/* Force the Games-tab systems list (the base of Pick). */
static void jw__pick_show_systems(jw_launcher_state *state) {
    if (state->games_open) jw__close_game_browser(state);
    state->current_tab = JW_TAB_GAMES;
    cat_list_state_jump(&state->list, 0, state->system_count);
}

static void jw__focus_setup_begin(jw_launcher_state *state) {
    state->focus_setup_open = false;
    state->focus_setup_count = 0;
    state->focus_setup_lock = JW_FOCUS_LOCK_NONE;
    state->focus_setup_style = JW_FOCUS_STYLE_THEME;
    state->focus_setup_choice = 0;
    state->focus_setup_note[0] = '\0';

    /* Pre-fill the remembered set (last chosen games that still resolve). */
    char csv[128] = "";
    if (jw_db_get_setting(state->db_path, JW_FOCUS_KEY_IDS, csv, sizeof(csv)) == 0 &&
        csv[0]) {
        int ids[JW_FOCUS_MAX_GAMES], n = 0;
        jw_focus_ids_parse(csv, ids, &n);
        for (int i = 0; i < n &&
             state->focus_setup_count < JW_FOCUS_SCREEN_MAX_TILES; i++) {
            jw_game_entry g;
            if (jw_db_get_game_by_id(state->db_path, ids[i], &g) == 0)
                state->focus_setup_ids[state->focus_setup_count++] = ids[i];
        }
    }

    /* Overlay pick mode on the real Games-tab browser. */
    jw__pick_show_systems(state);
    state->focus_pick_active = true;
    cat_request_frame();
}

/* Re-enter Pick from a later step (Arrange), keeping the current selection. */
static void jw__focus_pick_reenter(jw_launcher_state *state) {
    state->focus_setup_open = false;
    jw__pick_show_systems(state);
    state->focus_pick_active = true;
    cat_request_frame();
}

static void jw__pick_clear(jw_launcher_state *state) {
    state->focus_setup_count = 0;
    for (int i = 0; i < JW_FOCUS_SCREEN_MAX_TILES; i++) state->focus_setup_ids[i] = 0;
    snprintf(state->focus_setup_note, sizeof(state->focus_setup_note), "Cleared");
    jw__haptic(state, "select");
}

static void jw__pick_done(jw_launcher_state *state) {
    if (state->focus_setup_count <= 0) {
        snprintf(state->focus_setup_note, sizeof(state->focus_setup_note),
                 "Pick at least one game");
        return;
    }
    if (state->games_open) jw__close_game_browser(state);
    state->focus_pick_active = false;
    state->focus_setup_open = true;
    state->focus_setup_step = JW_FSETUP_ARRANGE;
    state->focus_setup_arrange_cursor = 0;
    state->focus_setup_grabbed = false;
}

static void jw__pick_cancel(jw_launcher_state *state) {
    if (state->games_open) jw__close_game_browser(state);
    state->focus_pick_active = false;
    state->status[0] = '\0';
}

/* Pick input: reuse the real browser's systems->games navigation, but A toggles
   a game into the set (not launch), Y clears, Start advances, and everything
   else is constrained (handled in jw__handle_input's pick branch). */
static void jw__handle_pick_input(const char *db_path, jw_launcher_state *state,
                                  cat_button button) {
    state->focus_setup_note[0] = '\0';
    if (state->games_open) {
        switch (button) {
            case CAT_BTN_UP:    cat_list_state_move(&state->game_list, -1, state->game_count); break;
            case CAT_BTN_DOWN:  cat_list_state_move(&state->game_list, +1, state->game_count); break;
            case CAT_BTN_LEFT:  cat_list_state_page(&state->game_list, -1, state->game_count); break;
            case CAT_BTN_RIGHT: cat_list_state_page(&state->game_list, +1, state->game_count); break;
            case CAT_BTN_A:
                if (state->game_count > 0 && state->game_list.cursor < state->game_count)
                    jw__fsetup_toggle(state, state->games[state->game_list.cursor].id);
                break;
            case CAT_BTN_Y:     jw__pick_clear(state); break;
            case CAT_BTN_START: jw__pick_done(state); break;
            case CAT_BTN_B:     jw__close_game_browser(state); state->status[0] = '\0'; break;
            default: break;
        }
    } else {
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&state->list, -1, state->system_count); break;
            case CAT_BTN_DOWN: cat_list_state_move(&state->list, +1, state->system_count); break;
            case CAT_BTN_A:
                if (state->system_count > 0 && state->list.cursor < state->system_count)
                    jw__open_system_games(db_path, state->systems[state->list.cursor].name, state);
                break;
            case CAT_BTN_Y:     jw__pick_clear(state); break;
            case CAT_BTN_START: jw__pick_done(state); break;
            case CAT_BTN_B:     jw__pick_cancel(state); break;
            default: break;
        }
    }
    cat_request_frame();
}

/* ---- shared chrome ---- */

static int jw__fsetup_font_h(TTF_Font *f) {
    int h = 0;
    TTF_SizeUTF8(f, "Ay", NULL, &h);
    return h;
}

static void jw__fsetup_header(const char *title, const char *right) {
    const cat_theme *th = cat_get_theme();
    int sw = cat_get_screen_width();
    TTF_Font *f = cat_get_font(CAT_FONT_LARGE);
    int y = CAT_S(24);
    cat_draw_text(f, title, CAT_S(28), y, th->text);
    if (right && right[0]) {
        int w = cat_measure_text(f, right);
        cat_draw_text(f, right, sw - CAT_S(28) - w, y, th->emphasis);
    }
}

static void jw__fsetup_footer(jw_launcher_state *state, const char *hint) {
    const cat_theme *th = cat_get_theme();
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    TTF_Font *f = cat_get_font(CAT_FONT_SMALL);
    if (state->focus_setup_note[0]) {
        int w = cat_measure_text(f, state->focus_setup_note);
        cat_draw_text(f, state->focus_setup_note, (sw - w) / 2, sh - CAT_S(84),
                      th->emphasis);
    }
    jw_focus_draw_hint_kv(f, hint, sw / 2, sh - CAT_S(56), th->emphasis, th->text);
}

/* option-menu (Lock / Style): highlight the cursor row. */
static void jw__fsetup_options(const char *const *opts, int n, int cursor) {
    const cat_theme *th = cat_get_theme();
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    TTF_Font *f = cat_get_font(CAT_FONT_MEDIUM);
    int ih = CAT_S(60), gap = CAT_S(14), w = CAT_S(380), x = (sw - w) / 2;
    int total = n * ih + (n - 1) * gap;
    int y0 = (sh - total) / 2;
    int fh = jw__fsetup_font_h(f);
    for (int i = 0; i < n; i++) {
        int y = y0 + i * (ih + gap);
        bool sel = (i == cursor);
        if (sel) cat_draw_pill(x, y, w, ih, th->highlight);
        int tw = cat_measure_text(f, opts[i]);
        cat_draw_text(f, opts[i], x + (w - tw) / 2, y + (ih - fh) / 2,
                      sel ? th->highlighted_text : th->text);
    }
}

/* 4 PIN slots centered at y; active slot highlighted. */
static void jw__fsetup_pin_slots(const int *pin, int slot) {
    const cat_theme *th = cat_get_theme();
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    TTF_Font *f = cat_get_font(CAT_FONT_LARGE);
    int s = CAT_S(72), gap = CAT_S(20);
    int total = JW_FOCUS_PIN_LEN * s + (JW_FOCUS_PIN_LEN - 1) * gap;
    int x0 = (sw - total) / 2, y = (sh - s) / 2;
    int bw = CAT_S(4);
    for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) {
        int x = x0 + i * (s + gap);
        bool active = (i == slot);
        cat_draw_rounded_rect(x, y, s, s, CAT_S(12), active ? th->emphasis : th->hint);
        cat_draw_rounded_rect(x + bw, y + bw, s - 2 * bw, s - 2 * bw, CAT_S(9),
                              th->background);
        char d[2] = { (char)('0' + (pin[i] % 10)), '\0' };
        int dw = cat_measure_text(f, d), dh = jw__fsetup_font_h(f);
        cat_draw_text(f, d, x + (s - dw) / 2, y + (s - dh) / 2, th->text);
    }
}

/* ---- per-step render ---- */

static void jw__fsetup_render_arrange(jw_launcher_state *state) {
    const cat_theme *th = cat_get_theme();
    jw__fsetup_header("Arrange", NULL);
    TTF_Font *f = cat_get_font(CAT_FONT_SMALL);
    int fh = jw__fsetup_font_h(f);
    for (int i = 0; i < state->focus_setup_count; i++) {
        SDL_Rect r;
        if (!jw_focus_screen_tile_rect(i, state->focus_setup_count, &r)) continue;
        bool sel = (i == state->focus_setup_arrange_cursor);
        bool grab = sel && state->focus_setup_grabbed;
        SDL_Color bc = grab ? th->accent : (sel ? th->emphasis : th->hint);
        int bw = grab ? CAT_S(7) : (sel ? CAT_S(5) : CAT_S(3));
        cat_draw_rounded_rect(r.x, r.y, r.w, r.h, CAT_S(14), bc);
        cat_draw_rounded_rect(r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw,
                              CAT_S(12), th->background);
        jw_game_entry g;
        const char *nm = "";
        if (state->focus_setup_ids[i] > 0 &&
            jw_db_get_game_by_id(state->db_path, state->focus_setup_ids[i], &g) == 0)
            nm = g.name;
        int pad = CAT_S(10);
        int ty = r.y + (r.h - fh) / 2;
        cat_draw_text_wrapped(f, nm, r.x + pad, ty, r.w - 2 * pad,
                              th->text, CAT_ALIGN_CENTER);
    }
    jw__fsetup_footer(state,
        state->focus_setup_grabbed ? "X: Drop   D-pad: Move"
                                   : "X: Grab   Y: Remove   B: Back   A: Next");
}

static void jw__fsetup_render_lock(jw_launcher_state *state) {
    static const char *const opts[] = { "No lock", "PIN" };
    jw__fsetup_header("Lock", NULL);
    jw__fsetup_options(opts, 2, state->focus_setup_choice);
    jw__fsetup_footer(state, "B: Back   A: Next");
}

static void jw__fsetup_render_style(jw_launcher_state *state) {
    static const char *const opts[] = { "Theme colors", "Black & white" };
    jw__fsetup_header("Style", NULL);
    jw__fsetup_options(opts, 2, state->focus_setup_choice);
    jw__fsetup_footer(state, "B: Back   A: Next");
}

static void jw__fsetup_render_pin(jw_launcher_state *state) {
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    jw__fsetup_header(state->focus_setup_pin_confirming ? "Re-enter PIN"
                                                        : "Set a PIN", NULL);
    jw__fsetup_pin_slots(state->focus_setup_pin_confirming ? state->focus_setup_pin2
                                                           : state->focus_setup_pin,
                         state->focus_setup_pin_slot);
    if (state->focus_setup_pin_mismatch) {
        TTF_Font *f = cat_get_font(CAT_FONT_SMALL);
        const char *m = "PINs did not match — try again";
        int w = cat_measure_text(f, m);
        cat_draw_text(f, m, (sw - w) / 2, sh / 2 + CAT_S(70),
                      (SDL_Color){ 0xFF, 0x3B, 0x30, 0xFF });
    }
    jw__fsetup_footer(state, "D-pad: Value/Position   B: Back   A: Next");
}

static void jw__fsetup_render_confirm(jw_launcher_state *state) {
    /* True preview: draw the focus screen exactly as it will appear. */
    jw_focus_tile tiles[JW_FOCUS_SCREEN_MAX_TILES];
    memset(tiles, 0, sizeof(tiles));
    static jw_game_entry games[JW_FOCUS_SCREEN_MAX_TILES];
    int n = 0;
    for (int i = 0; i < state->focus_setup_count; i++) {
        if (jw_db_get_game_by_id(state->db_path, state->focus_setup_ids[i],
                                 &games[n]) != 0)
            continue;
        tiles[n].title = games[n].name;
        char abs[PATH_MAX];
        int w = 0, h = 0; bool pending = false;
        if (games[n].image_path[0] &&
            jw__resolve_sdcard_path(state, games[n].image_path, abs, sizeof(abs)) == 0) {
            SDL_Texture *t = jw__load_cover(state, abs, &w, &h, &pending);
            if (t) { tiles[n].art = t; tiles[n].art_w = w; tiles[n].art_h = h; }
        }
        n++;
    }
    bool bw = state->focus_setup_style == JW_FOCUS_STYLE_BW;
    jw_focus_battery batt = { state->focus_batt_pct < 0 ? 100 : state->focus_batt_pct,
                              state->focus_batt_chg == 1 };
    jw_focus_screen_render(tiles, n, -1, bw, batt, false);

    /* Confirmation popup over the dimmed preview — same panel as the exit overlay. */
    jw_focus_unlock_view uv;
    memset(&uv, 0, sizeof(uv));
    uv.confirm      = "Start 5-Game Mode?";
    uv.confirm_hint = "B: Back      A: Start";
    jw_focus_screen_render_unlock(bw, &uv);
}

static void jw__render_focus_setup(jw_launcher_state *state) {
    cat_clear_screen();
    switch (state->focus_setup_step) {
        case JW_FSETUP_PICK:    break;   /* Pick renders via the real browser */
        case JW_FSETUP_ARRANGE: jw__fsetup_render_arrange(state); break;
        case JW_FSETUP_LOCK:    jw__fsetup_render_lock(state); break;
        case JW_FSETUP_PIN:     jw__fsetup_render_pin(state); break;
        case JW_FSETUP_STYLE:   jw__fsetup_render_style(state); break;
        case JW_FSETUP_CONFIRM: jw__fsetup_render_confirm(state); break;
    }
    jw__present();
}

/* ---- input ---- */

static void jw__fsetup_pin_reset(jw_launcher_state *state) {
    state->focus_setup_pin_slot = 0;
    state->focus_setup_pin_confirming = false;
    state->focus_setup_pin_mismatch = false;
    for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) {
        state->focus_setup_pin[i] = 0;
        state->focus_setup_pin2[i] = 0;
    }
}

static void jw__handle_focus_setup_input(const char *db_path,
                                         jw_launcher_state *state,
                                         cat_button button) {
    state->focus_setup_note[0] = '\0';
    switch (state->focus_setup_step) {

    case JW_FSETUP_PICK:   /* Pick is a mode outside the wizard now; never here. */
        break;

    case JW_FSETUP_ARRANGE: {
        int n = state->focus_setup_count;
        switch (button) {
            case CAT_BTN_LEFT: case CAT_BTN_RIGHT:
            case CAT_BTN_UP:   case CAT_BTN_DOWN: {
                /* jw__focus_nav is a grid cursor Catastrophe does not own, so
                   this reports its own movement. Grabbed or not, the cursor
                   lands in the same place; only the tiles differ. */
                int cur = state->focus_setup_arrange_cursor;
                int dst = jw__focus_nav(cur, n, button);
                if (state->focus_setup_grabbed) {
                    /* move the grabbed tile in the arranged order */
                    if (dst != cur) {
                        int v = state->focus_setup_ids[cur];
                        if (cur < dst)
                            for (int i = cur; i < dst; i++) state->focus_setup_ids[i] = state->focus_setup_ids[i + 1];
                        else
                            for (int i = cur; i > dst; i--) state->focus_setup_ids[i] = state->focus_setup_ids[i - 1];
                        state->focus_setup_ids[dst] = v;
                        state->focus_setup_arrange_cursor = dst;
                    }
                } else {
                    state->focus_setup_arrange_cursor = dst;
                }
                cat_ui_feedback_emit(dst != cur ? CAT_UI_MOVED : CAT_UI_EDGE);
                break;
            }
            case CAT_BTN_X:
                if (n > 0) state->focus_setup_grabbed = !state->focus_setup_grabbed;
                break;
            case CAT_BTN_Y:   /* remove the highlighted game from the set */
                if (!state->focus_setup_grabbed && n > 0) {
                    int cur = state->focus_setup_arrange_cursor;
                    for (int i = cur; i < state->focus_setup_count - 1; i++)
                        state->focus_setup_ids[i] = state->focus_setup_ids[i + 1];
                    state->focus_setup_count--;
                    if (state->focus_setup_count == 0) {
                        jw__focus_pick_reenter(state);   /* nothing left -> back to Pick */
                    } else if (state->focus_setup_arrange_cursor >= state->focus_setup_count) {
                        state->focus_setup_arrange_cursor = state->focus_setup_count - 1;
                    }
                }
                break;
            case CAT_BTN_A:
                if (state->focus_setup_grabbed) { state->focus_setup_grabbed = false; break; }
                state->focus_setup_step = JW_FSETUP_LOCK;
                state->focus_setup_choice = (state->focus_setup_lock == JW_FOCUS_LOCK_PIN) ? 1 : 0;
                break;
            case CAT_BTN_B:
                if (state->focus_setup_grabbed) { state->focus_setup_grabbed = false; break; }
                jw__focus_pick_reenter(state);   /* back to Pick (keep selection) */
                break;
            default: break;
        }
        break;
    }

    case JW_FSETUP_LOCK:
        switch (button) {
            /* Two options, so the choice always flips: never a boundary. */
            case CAT_BTN_UP:   state->focus_setup_choice = (state->focus_setup_choice + 1) % 2;
                               cat_ui_feedback_emit(CAT_UI_MOVED); break;
            case CAT_BTN_DOWN: state->focus_setup_choice = (state->focus_setup_choice + 1) % 2;
                               cat_ui_feedback_emit(CAT_UI_MOVED); break;
            case CAT_BTN_A:
                if (state->focus_setup_choice == 1) {
                    state->focus_setup_lock = JW_FOCUS_LOCK_PIN;
                    jw__fsetup_pin_reset(state);
                    state->focus_setup_step = JW_FSETUP_PIN;
                } else {
                    state->focus_setup_lock = JW_FOCUS_LOCK_NONE;
                    state->focus_setup_step = JW_FSETUP_STYLE;
                    state->focus_setup_choice = (state->focus_setup_style == JW_FOCUS_STYLE_BW) ? 1 : 0;
                }
                break;
            case CAT_BTN_B: state->focus_setup_step = JW_FSETUP_ARRANGE; break;
            default: break;
        }
        break;

    case JW_FSETUP_PIN: {
        int *pin = state->focus_setup_pin_confirming ? state->focus_setup_pin2
                                                     : state->focus_setup_pin;
        int *slot = &state->focus_setup_pin_slot;
        switch (button) {
            /* Digits wrap so they always move; the slot clamps at both ends. */
            case CAT_BTN_UP:   pin[*slot] = (pin[*slot] + 1) % 10; state->focus_setup_pin_mismatch = false;
                               cat_ui_feedback_emit(CAT_UI_MOVED); break;
            case CAT_BTN_DOWN: pin[*slot] = (pin[*slot] + 9) % 10; state->focus_setup_pin_mismatch = false;
                               cat_ui_feedback_emit(CAT_UI_MOVED); break;
            case CAT_BTN_LEFT:
                cat_ui_feedback_emit(*slot > 0 ? CAT_UI_MOVED : CAT_UI_EDGE);
                if (*slot > 0) (*slot)--;
                break;
            case CAT_BTN_RIGHT:
                cat_ui_feedback_emit(*slot < JW_FOCUS_PIN_LEN - 1 ? CAT_UI_MOVED : CAT_UI_EDGE);
                if (*slot < JW_FOCUS_PIN_LEN - 1) (*slot)++;
                break;
            case CAT_BTN_A:
                if (!state->focus_setup_pin_confirming) {
                    state->focus_setup_pin_confirming = true;
                    state->focus_setup_pin_slot = 0;
                } else {
                    bool match = true;
                    for (int i = 0; i < JW_FOCUS_PIN_LEN; i++)
                        if (state->focus_setup_pin[i] != state->focus_setup_pin2[i]) match = false;
                    if (match) {
                        state->focus_setup_step = JW_FSETUP_STYLE;
                        state->focus_setup_choice = (state->focus_setup_style == JW_FOCUS_STYLE_BW) ? 1 : 0;
                    } else {
                        state->focus_setup_pin_mismatch = true;
                        state->focus_setup_pin_confirming = false;
                        state->focus_setup_pin_slot = 0;
                        for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) {
                            state->focus_setup_pin[i] = 0;
                            state->focus_setup_pin2[i] = 0;
                        }
                    }
                }
                break;
            case CAT_BTN_B:
                if (state->focus_setup_pin_confirming) {
                    state->focus_setup_pin_confirming = false;
                    state->focus_setup_pin_slot = 0;
                    state->focus_setup_pin_mismatch = false;
                } else {
                    state->focus_setup_step = JW_FSETUP_LOCK;
                }
                break;
            default: break;
        }
        break;
    }

    case JW_FSETUP_STYLE:
        switch (button) {
            /* Two options, so the choice always flips: never a boundary. */
            case CAT_BTN_UP:   state->focus_setup_choice = (state->focus_setup_choice + 1) % 2;
                               cat_ui_feedback_emit(CAT_UI_MOVED); break;
            case CAT_BTN_DOWN: state->focus_setup_choice = (state->focus_setup_choice + 1) % 2;
                               cat_ui_feedback_emit(CAT_UI_MOVED); break;
            case CAT_BTN_A:
                state->focus_setup_style = state->focus_setup_choice == 1 ? JW_FOCUS_STYLE_BW
                                                                          : JW_FOCUS_STYLE_THEME;
                state->focus_setup_step = JW_FSETUP_CONFIRM;
                break;
            case CAT_BTN_B:
                state->focus_setup_step = (state->focus_setup_lock == JW_FOCUS_LOCK_PIN)
                                          ? JW_FSETUP_PIN : JW_FSETUP_LOCK;
                if (state->focus_setup_step == JW_FSETUP_PIN) jw__fsetup_pin_reset(state);
                break;
            default: break;
        }
        break;

    case JW_FSETUP_CONFIRM:
        switch (button) {
            case CAT_BTN_A:
                jw__focus_setup_commit(state);
                break;
            case CAT_BTN_B:
                state->focus_setup_step = JW_FSETUP_STYLE;
                state->focus_setup_choice = (state->focus_setup_style == JW_FOCUS_STYLE_BW) ? 1 : 0;
                break;
            default: break;
        }
        break;
    }
    cat_request_frame();
}

/* Input while the MENU unlock overlay is open. */
static void jw__focus_unlock_input(const char *socket_path, jw_launcher_state *state,
                                   cat_button button, bool *running) {
    /* Reboot / Power off confirm sub-prompt (open to everyone — powering off or
       rebooting doesn't escape the lock, it comes right back into focus mode). */
    if (state->focus_unlock_confirm != 0) {
        switch (button) {
            case CAT_BTN_A: {
                /* Tick on the answer, not the intent: if the daemon refuses,
                   nothing is rebooting and the buzz should say so. */
                int prc = jw_ipc_platform_action(socket_path,
                    state->focus_unlock_confirm == 1 ? "reboot" : "poweroff", 0);
                jw__haptic(state, prc == 0 ? "commit" : "blocked");
                if (prc != 0) break;
                cat_hide_window();
                *running = false;
                break;
            }
            case CAT_BTN_B:
            case CAT_BTN_MENU:
                jw__haptic(state, "select");
                state->focus_unlock_confirm = 0; cat_request_frame(); break;
            default: break;
        }
        return;
    }
    /* L1/R1 open the reboot / power-off confirms. */
    if (button == CAT_BTN_L1) {
        jw__haptic(state, "select");
        state->focus_unlock_confirm = 1; cat_request_frame(); return;
    }
    if (button == CAT_BTN_R1) {
        jw__haptic(state, "select");
        state->focus_unlock_confirm = 2; cat_request_frame(); return;
    }

    /* Y reconnects a paired-but-disconnected headset (works in both PIN and
       confirm modes). The BT stack does the connect asynchronously; we just kick
       it off — the corner pip clears on the next poll if it succeeds. */
    if (button == CAT_BTN_Y && state->focus_bt_pip &&
        state->focus_bt_headset_mac[0]) {
        jw_bt_connect_start(state->focus_bt_headset_mac, false);
        state->focus_bt_next_ms = 0;   /* re-poll soon to refresh the pip */
        cat_request_frame();
        return;
    }
    if (state->focus_lock == JW_FOCUS_LOCK_PIN) {
        int *slot = &state->focus_pin[state->focus_pin_slot];
        switch (button) {
            case CAT_BTN_UP:
                *slot = (*slot + 1) % 10;
                jw__haptic(state, "nav");   /* digits wrap, so never blocked */
                state->focus_pin_error = false; cat_request_frame(); break;
            case CAT_BTN_DOWN:
                *slot = (*slot + 9) % 10;
                jw__haptic(state, "nav");
                state->focus_pin_error = false; cat_request_frame(); break;
            case CAT_BTN_LEFT:
                jw__haptic(state, state->focus_pin_slot > 0 ? "nav" : "blocked");
                if (state->focus_pin_slot > 0) state->focus_pin_slot--;
                cat_request_frame(); break;
            case CAT_BTN_RIGHT:
                jw__haptic(state, state->focus_pin_slot < JW_FOCUS_PIN_LEN - 1
                                      ? "nav" : "blocked");
                if (state->focus_pin_slot < JW_FOCUS_PIN_LEN - 1) state->focus_pin_slot++;
                cat_request_frame(); break;
            case CAT_BTN_A: {
                char pin[JW_FOCUS_PIN_LEN + 1];
                for (int i = 0; i < JW_FOCUS_PIN_LEN; i++)
                    pin[i] = (char)('0' + (state->focus_pin[i] % 10));
                pin[JW_FOCUS_PIN_LEN] = '\0';
                if (jw_focus_pin_verify(pin, state->focus_pin_hash)) {
                    /* The one place the blocked/commit distinction really earns
                       its keep: right PIN and wrong PIN feel different without
                       having to read the screen. */
                    jw__haptic(state, "commit");
                    jw__focus_unlock_exit(state);
                } else {
                    jw__haptic(state, "blocked");
                    state->focus_pin_error = true;
                    state->focus_pin_slot = 0;
                    for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) state->focus_pin[i] = 0;
                    cat_request_frame();
                }
                break;
            }
            case CAT_BTN_B:
            case CAT_BTN_MENU:
                state->focus_unlock_open = false; cat_request_frame(); break;
            default: break;
        }
    } else {
        /* lock == none: a plain Exit? confirm. PIN is the only other lock type;
           there is no free-exit path for anything but an explicit no-lock. */
        switch (button) {
            case CAT_BTN_A:
                jw__haptic(state, "commit");
                jw__focus_unlock_exit(state); break;
            case CAT_BTN_B:
            case CAT_BTN_MENU:
                state->focus_unlock_open = false; cat_request_frame(); break;
            default: break;
        }
    }
}

/* Focus-mode input: grid navigation + launch, and MENU opens the unlock overlay
   (the only way out). Everything else is swallowed so the mode stays a locked,
   minimal surface. */
static void jw__focus_handle_input(const char *socket_path, jw_launcher_state *state,
                                   cat_button button, bool *running) {
    if (state->focus_unlock_open) {
        jw__focus_unlock_input(socket_path, state, button, running);
        return;
    }

    switch (button) {
        case CAT_BTN_LEFT:
        case CAT_BTN_RIGHT:
        case CAT_BTN_UP:
        case CAT_BTN_DOWN: {
            int next = jw__focus_nav(state->focus_cursor, state->focus_count, button);
            bool moved = next != state->focus_cursor;
            if (moved) {
                state->focus_cursor = next;
                cat_request_frame();
            }
            /* Same boundary feedback as the home grid: a tick when the cursor
               moves, the blocked pattern at the edge of the tile set. */
            jw__haptic(state, moved ? "nav" : "blocked");
            break;
        }
        case CAT_BTN_A:
            if (state->focus_count > 0) {
                /* Launch reports only failure by touch -- see the helper.
                   Focus mode resumes, exactly as Recents does: the daemon loads
                   the newest state (preferring the game-switcher slot) when one
                   exists and cold-launches otherwise, so no per-tile state probe
                   is needed here. Picking up where you left off matters more in
                   focus mode than anywhere else, since these five games are the
                   whole device and whoever is using it is least equipped to
                   navigate a title screen and a load menu. */
                jw__launch_game_entry_with_mode(socket_path, state,
                                                &state->focus_games[state->focus_cursor],
                                                true, running);
            } else {
                jw__haptic(state, "blocked");
            }
            break;
        case CAT_BTN_MENU:
            /* Open the unlock overlay, fresh. */
            state->focus_unlock_open = true;
            state->focus_unlock_confirm = 0;
            state->focus_pin_error = false;
            state->focus_pin_slot = 0;
            for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) state->focus_pin[i] = 0;
            cat_request_frame();
            break;
        default:
            break;   /* swallow B/X/Y/Select/shoulders */
    }
}

/* Which surface owns input right now, folded to one number.
 *
 * The chain below mirrors the overlay precedence in jw__handle_input_inner
 * exactly -- read them together, and add to both when a surface is added. That
 * is what makes the surface tick symmetric: opening a screen and backing out of
 * it are the same event to this function, so both feel the same. They used to
 * be written by hand at each open site, which is why nearly every close was
 * silent -- someone had to remember twice and only ever remembered once.
 *
 * Two deliberate omissions. Settings sub-pages are absent because
 * jw_settings_ui_handle_button owns that tick; counting them here would double
 * it. The System menu tab row is absent because moving along a tab row is
 * navigation, not a surface change -- jw__switch_system_tab ticks it as "nav",
 * matching the home tab row. */
static long jw__surface_id(const jw_launcher_state *state) {
    if (!state)                   return 0;
    if (state->focus_active)      return state->focus_unlock_open ? 101 : 100;
    if (state->focus_setup_open)  return 200 + (long)state->focus_setup_step;
    if (state->focus_pick_active) return state->games_open ? 301 : 300;
    if (state->pakrat_open)       return state->pakrat_detail_open ? 401 : 400;
    if (state->menu_open)         return 500;
    if (state->switcher_open)     return 600;
    if (state->actions_open)      return 700;
    if (state->search_open)       return 800;
    if (state->games_open)        return 900;
    if (state->apps_open)         return 1000;
    if (state->tools_open)        return 1100;
    if (jw_settings_ui_is_open(&state->settings)) return 1200;
    return 0;   /* home */
}

static void jw__handle_input_inner(const char *socket_path, const char *db_path,
                              jw_launcher_state *state, cat_button button, bool *running);

/* Public entry: run the real dispatcher, then tick once if the press changed
   which surface is on screen. */
static void jw__handle_input(const char *socket_path, const char *db_path,
                             jw_launcher_state *state, cat_button button, bool *running) {
    long surf0 = jw__surface_id(state);
    jw__handle_input_inner(socket_path, db_path, state, button, running);
    if (jw__surface_id(state) != surf0)
        jw__haptic(state, "select");
}

static void jw__handle_input_inner(const char *socket_path, const char *db_path,
                              jw_launcher_state *state, cat_button button, bool *running) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;

    /* Desktop-only: Q exercises the IPC shutdown path. Catastrophe only
       emits CAT_BTN_QUIT off-device, so this is inert on real hardware. */
    if (button == CAT_BTN_QUIT) {
        jw_ipc_shutdown(socket_path);
        *running = false;
        return;
    }

    /* 5-Game Mode swallows all normal input: only the focus grid + launch (and,
       from Phase 3, the MENU unlock) are reachable. Kept above every overlay and
       the LED shortcut so nothing leaks the device out of the locked surface. */
    if (state->focus_active) {
        jw__focus_handle_input(socket_path, state, button, running);
        return;
    }

    if (state->focus_setup_open) {
        jw__handle_focus_setup_input(db_path, state, button);
        return;
    }

    /* Pick mode overlays the real Games-tab browser but stays locked to it:
       fully intercept input (no tab-switch / system menu / context / switcher). */
    if (state->focus_pick_active) {
        jw__handle_pick_input(db_path, state, button);
        return;
    }

    /* Analog-stick click is a global shortcut: toggle the LED ring on/off. */
    if (button == CAT_BTN_STICK) {
        jw_settings_toggle_led(&state->settings);
        return;
    }

    if (state->pakrat_open) {
        jw__handle_pakrat_input(db_path, state, button);
        return;
    }

    /* The system-menu overlay captures all input while open. */
    if (state->menu_open) {
        jw__handle_menu_input(socket_path, db_path, state, button, running);
        return;
    }

    /* The switcher overlay captures all input while open. */
    if (state->switcher_open) {
        jw__handle_switcher_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->actions_open) {
        jw__handle_actions_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->search_open) {
        jw__handle_search_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->games_open) {
        /* In the tabbed layout the section tabs sit above the game list, so
           L1/R1 tabs away from the system — closing the browser and gliding to
           the adjacent section (the game list shows the tab bar). */
        if (layout == CAT_LAUNCHER_TABBED &&
            (button == CAT_BTN_L1 || button == CAT_BTN_R1)) {
            jw__switch_tab_slide(state, button == CAT_BTN_L1 ? -1 : +1, db_path);
            return;
        }
        if (button == CAT_BTN_X) {
            if (!jw__open_context_actions(db_path, state)) {
                jw__open_search(db_path, state);
            }
            return;
        }
        /* MENU opens the System page from inside a system's game list too, so it
           works the same everywhere (the browser stays open underneath). */
        if (button == CAT_BTN_MENU) {
            jw__open_menu(state);
            return;
        }
        jw__handle_game_browser_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->apps_open) {
        if (button == CAT_BTN_X) {
            jw__open_search(db_path, state);
            return;
        }
        if (button == CAT_BTN_MENU) {
            jw__open_menu(state);
            return;
        }
        jw__handle_app_browser_input(socket_path, state, button, running);
        return;
    }

    /* Tools overlay captures all input first.
       Tools entries: 0=Recently Played, 1=Favorites, 2=Apps, 3=Settings */
    if (state->tools_open) {
        static const int kToolsCount = 4;
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&state->tools_list, -1, kToolsCount);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&state->tools_list, +1, kToolsCount);
                break;
            case CAT_BTN_A:
                state->tools_open = false;
                if (state->tools_list.cursor == 2) {
                    jw__open_apps(state);
                } else if (state->tools_list.cursor == 3) {
                    jw_settings_ui_enter(&state->settings);
                } else {
                    snprintf(state->status, sizeof(state->status), "%s", "Coming soon");
                }
                break;
            case CAT_BTN_B:
            case CAT_BTN_MENU:
                state->tools_open = false;
                break;
            default:
                break;
        }
        return;
    }

    /* Settings UI captures input when open. */
    if (jw_settings_ui_is_open(&state->settings)) {
        /* MENU always opens the main menu, from any settings sub-screen and
           any layout — it is a global action, not consumed by settings. */
        if (button == CAT_BTN_MENU) {
            jw__open_menu(state);
            return;
        }
        /* Tabbed mode: Settings is a tab, not an app. Triggers must escape
           it cleanly from any sub-screen, and B at Settings home is a no-op
           (the user leaves via L1/R1). jw__switch_tab closes Settings as a
           side effect when moving off the tab. */
        if (layout == CAT_LAUNCHER_TABBED) {
            if (button == CAT_BTN_L1) {
                jw__switch_tab_slide(state, -1, db_path);
                return;
            }
            if (button == CAT_BTN_R1) {
                jw__switch_tab_slide(state, +1, db_path);
                return;
            }
        }
        bool theme_changed = false;
        bool still_open = jw_settings_ui_handle_button(
            &state->settings, button,
            state->status, sizeof(state->status), &theme_changed);
        if (theme_changed)
            jw__rebuild_for_layout(state);
        if (!still_open && layout == CAT_LAUNCHER_TABBED) {
            /* B at Settings home in tabbed mode is a no-op: re-open so the
               user stays in Settings until they use the triggers to leave. */
            jw_settings_ui_enter(&state->settings);
        }
        return;
    }

    if (button == CAT_BTN_X) {
        /* Cover Flow: X opens the inline search overlay (no per-item Options menu,
           no blocking system keyboard). */
        if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_COVERFLOW) {
            jw__cf_open_search(state, db_path);
            return;
        }
        if (!jw__open_context_actions(db_path, state)) {
            jw__open_search(db_path, state);
        }
        return;
    }

    /* Select opens the dedicated switcher from any home layout/tab (desktop:
       Space). It is launcher-local; the daemon stays out of it. */
    if (button == CAT_BTN_SELECT) {
        jw__open_switcher(db_path, state);
        return;
    }

    /* Coverflow shares the tabbed data model (current_tab + per-tab lists); the
       carousel is that tab's content, and Up/Down switches channel. */
    bool cf_channels = (layout == CAT_LAUNCHER_COVERFLOW);
    int count = (layout == CAT_LAUNCHER_TABBED || cf_channels)
                    ? jw__tab_list_count(state) : state->flat_count;

    switch (button) {
        case CAT_BTN_UP:
            if (cf_channels) {
                jw__cf_channel_begin(state, -1, db_path);   /* previous channel (cube up) */
            } else if (layout == CAT_LAUNCHER_HORIZONTAL) {
                break;
            } else {
                cat_list_state_move(&state->list, -1, count);
            }
            break;
        case CAT_BTN_DOWN:
            if (cf_channels) {
                jw__cf_channel_begin(state, +1, db_path);   /* next channel (cube down) */
            } else if (layout == CAT_LAUNCHER_HORIZONTAL) {
                break;
            } else {
                cat_list_state_move(&state->list, +1, count);
            }
            break;
        case CAT_BTN_LEFT:
            /* Coverflow flows one item (wrap only when the list fills the window);
               Horizontal wraps; tabbed pages. */
            if (layout == CAT_LAUNCHER_COVERFLOW || layout == CAT_LAUNCHER_HORIZONTAL)
                jw__cf_carousel_step(&state->list, -1, count, layout);
            else
                cat_list_state_page(&state->list, -1, count);
            break;
        case CAT_BTN_RIGHT:
            if (layout == CAT_LAUNCHER_COVERFLOW || layout == CAT_LAUNCHER_HORIZONTAL)
                jw__cf_carousel_step(&state->list, +1, count, layout);
            else
                cat_list_state_page(&state->list, +1, count);
            break;
        case CAT_BTN_L1:   /* Tabbed: tab between sections. Coverflow: flow one item. */
            if (layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab_slide(state, -1, db_path);
            else if (layout == CAT_LAUNCHER_COVERFLOW)
                jw__cf_carousel_step(&state->list, -1, count, layout);
            break;
        case CAT_BTN_R1:
            if (layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab_slide(state, +1, db_path);
            else if (layout == CAT_LAUNCHER_COVERFLOW)
                jw__cf_carousel_step(&state->list, +1, count, layout);
            break;
        case CAT_BTN_A:
            if (layout == CAT_LAUNCHER_TABBED || cf_channels)
                jw__activate_tabbed(socket_path, db_path, state, running);
            else
                jw__activate_flat(socket_path, db_path, state, running);
            break;
        /* B is intentionally unmapped on the home tabs: there is no "back" at the
           top level, and B stays the universal cancel everywhere else. (Recents
           self-curates as you play, so it needs no per-entry remove.) */
        case CAT_BTN_MENU:
            jw__open_menu(state);
            break;
        case CAT_BTN_Y: {
            /* On the Recents tab, Y toggles the favorite of the selected game
               (the row's star updates in place); on the Favorites tab, Y removes
               the selected favorite and reloads the list; everywhere else Y
               rescans the library. */
            if (jw__layout_uses_channels(layout) && state->current_tab == JW_TAB_RECENTS) {
                if (state->recents_count > 0 && state->list.cursor < state->recents_count) {
                    jw_game_entry *rec = &state->recents[state->list.cursor];
                    int want_on = !rec->favorite;
                    if (jw_db_set_favorite(db_path, "game", rec->id, want_on) == 0) {
                        rec->favorite = want_on;
                        snprintf(state->status, sizeof(state->status), "%s %.200s",
                                 want_on ? "Favorited" : "Unfavorited", rec->name);
                    } else {
                        snprintf(state->status, sizeof(state->status), "%s",
                                 "Favorite update failed");
                    }
                }
                break;
            }
            if (jw__layout_uses_channels(layout) && state->current_tab == JW_TAB_FAVORITES) {
                if (state->favorites_count > 0 && state->list.cursor < state->favorites_count) {
                    const jw_game_entry *fav = &state->favorites[state->list.cursor];
                    if (jw_db_set_favorite(db_path, "game", fav->id, 0) == 0) {
                        int prev_cursor = state->list.cursor;
                        jw__load_favorites_tab(db_path, state);
                        int c = prev_cursor >= state->favorites_count
                                    ? state->favorites_count - 1 : prev_cursor;
                        if (c < 0) c = 0;
                        cat_list_state_jump(&state->list, c, jw__tab_list_count(state));
                        snprintf(state->status, sizeof(state->status), "%s",
                                 "Removed from favorites");
                    } else {
                        snprintf(state->status, sizeof(state->status), "%s",
                                 "Favorite update failed");
                    }
                }
                break;
            }
            snprintf(state->status, sizeof(state->status), "%s", "rescanning...");
            cat_request_frame();
            jw__render_launcher(state);
            jw__scan_library(socket_path, db_path, state);
            if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW)
                jw__build_carousel_list(state);
            else if (layout == CAT_LAUNCHER_VERTICAL)
                jw__build_flat_list(state);
            else
                cat_list_state_jump(&state->list, 0, jw__tab_list_count(state));
            break;
        }
        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Blocking "keep this TV mode?" prompt for the 1080p120 auto-revert. Shown when
   the daemon reports a pending revert (after a deliberate switch to 1080p120).
   Keeping needs a deliberate two-shoulder squeeze (L1 + R1 together): a blank TV
   invites panic-mashing of face buttons, and that must never cancel the rescue.
   B - or simply leaving it - reverts to a safe 720p60 at the daemon's deadline. */
static void jw__hdmi_keep_prompt(const char *socket_path) {
    if (!socket_path || !socket_path[0]) {
        return;
    }
    TTF_Font *font = cat_get_font(CAT_FONT_MEDIUM);
    if (!font) {
        return;
    }
    ap_theme *theme = cat_get_theme();
    int max_w = cat_get_screen_width() - CAT_S(80);
    if (max_w < 1) {
        max_w = 1;
    }
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Revert", .is_confirm = false },
    };
    static const char *const body =
        "Keep this TV display mode?\n\n"
        "Sending 1080p120 over HDMI. If your TV stays blank it can't show this "
        "mode - leave it and Leaf reverts to a safe picture.";
    static const char *const keep_hint = "Press L1 then R1 to keep";

    /* Drive the visible countdown from a local deadline so the bar animates every
       frame; re-sync with the daemon every couple of seconds. */
    int total = 0;
    if (jw_ipc_hdmi_revert_status(socket_path, &total) != 0 || total <= 0) {
        return;
    }
    uint32_t deadline = SDL_GetTicks() + (uint32_t)total * 1000u;
    uint32_t last_sync = SDL_GetTicks();
    uint32_t l1_at = 0;

    for (;;) {
        uint32_t now = SDL_GetTicks();
        if (now - last_sync >= 2000) {
            int s = 0;
            if (jw_ipc_hdmi_revert_status(socket_path, &s) != 0 || s <= 0) {
                return;   /* reverted at the deadline, or kept from elsewhere */
            }
            deadline = now + (uint32_t)s * 1000u;
            last_sync = now;
        }
        long remaining_ms = (long)deadline - (long)now;
        if (remaining_ms <= 0) {
            return;   /* about to revert */
        }
        int remaining_s = (int)((remaining_ms + 999) / 1000);

        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) {
                continue;
            }
            if (ev.button == CAT_BTN_B) {
                return;   /* revert now (always safe) */
            }
            if (ev.button == CAT_BTN_L1) {
                l1_at = SDL_GetTicks();
            } else if (ev.button == CAT_BTN_R1) {
                /* Keep on the L1 -> R1 sequence (R1 within 1s of L1). A sequence is
                   reliable where a simultaneous shoulder squeeze drops a press on
                   this pad, and a single panic tap still can't keep an unshown mode. */
                if (l1_at && SDL_GetTicks() - l1_at <= 1000u) {
                    jw_ipc_hdmi_revert_keep(socket_path);
                    return;
                }
            }
        }

        cat_draw_background();
        int body_h  = cat_measure_wrapped_text_height(font, body, max_w);
        int bar_w   = sw * 3 / 5;
        int bar_h   = CAT_S(10);
        int gap     = CAT_S(24);
        int line_h  = TTF_FontHeight(font);
        int block_h = body_h + gap + bar_h + CAT_S(12) + line_h + CAT_S(10) + line_h;
        int y = (sh - block_h - cat_get_footer_height()) / 2;
        if (y < CAT_S(20)) {
            y = CAT_S(20);
        }
        cat_draw_text_wrapped(font, body, CAT_S(40), y, max_w, theme->text,
                              CAT_ALIGN_CENTER);
        int by = y + body_h + gap;
        int bx = (sw - bar_w) / 2;
        float frac = (float)remaining_ms / ((float)total * 1000.0f);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        cat_draw_rect(bx, by, bar_w, bar_h, theme->hint);
        cat_draw_rect(bx, by, (int)(bar_w * frac), bar_h, theme->highlight);
        char cd[48];
        snprintf(cd, sizeof(cd), "Reverting in %d second%s", remaining_s,
                 remaining_s == 1 ? "" : "s");
        int cw = cat_measure_text(font, cd);
        cat_draw_text(font, cd, (sw - cw) / 2, by + bar_h + CAT_S(12), theme->text);
        int kw = cat_measure_text(font, keep_hint);
        cat_draw_text(font, keep_hint, (sw - kw) / 2,
                      by + bar_h + CAT_S(12) + line_h + CAT_S(10), theme->highlight);
        jw__footer_direct(footer, 1);
        /* Request a frame so cat_present takes the active 60fps-paced path instead
           of its idle sleep — otherwise the countdown bar freezes on frame one. */
        cat_request_frame();
        jw__present();
    }
}

/* Screenshot flash: jawakad sends SIGUSR1 after saving a UI/focus screenshot; the
   main loop covers the screen white briefly so the capture is visibly acknowledged.
   The handler only sets a flag (async-signal-safe); the loop does the drawing. */
#define JW_SCREENSHOT_FLASH_MS        130
#define JW_SCREENSHOT_FLASH_ALPHA     175  /* starting white opacity (0-255); fades out */
#define JW_SCREENSHOT_FLASH_MIN_ALPHA 18   /* end the flash here so the faint tail can't linger */
static volatile sig_atomic_t g_screenshot_flash = 0;
static void jw__screenshot_flash_handler(int sig) {
    (void)sig;
    g_screenshot_flash = 1;
}

typedef struct {
    bool blocked;
    bool override_allowed;
    bool requires_verified_stop;
    bool sync_pending;
    int pending_items;
    long long pending_bytes;
    char service_id[129];
    char reason[64];
} jw_blocked_game_launch;

static bool jw__blocked_game_launch_query(const char *socket_path,
                                          jw_blocked_game_launch *out) {
    if (!socket_path || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const char *request = "{\"type\":\"game-launch-blocked-status\"}";
    char *reply = NULL;
    size_t reply_len = 0;
    if (jw_ipc_request_timeout(socket_path, request, strlen(request),
                               &reply, &reply_len, 250) != 0) {
        free(reply);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(reply, reply_len);
    free(reply);
    if (!root) {
        return false;
    }
    cJSON *blocked = cJSON_GetObjectItemCaseSensitive(root, "blocked");
    cJSON *allowed =
        cJSON_GetObjectItemCaseSensitive(root, "override_allowed");
    cJSON *sync_pending =
        cJSON_GetObjectItemCaseSensitive(root, "sync_pending");
    cJSON *requires_verified_stop =
        cJSON_GetObjectItemCaseSensitive(root, "requires_verified_stop");
    cJSON *pending_items =
        cJSON_GetObjectItemCaseSensitive(root, "pending_items");
    cJSON *pending_bytes =
        cJSON_GetObjectItemCaseSensitive(root, "pending_bytes");
    cJSON *service = cJSON_GetObjectItemCaseSensitive(root, "service_id");
    cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    out->blocked = cJSON_IsTrue(blocked);
    out->override_allowed = cJSON_IsTrue(allowed);
    out->requires_verified_stop = cJSON_IsTrue(requires_verified_stop);
    out->sync_pending = cJSON_IsTrue(sync_pending);
    if (cJSON_IsNumber(pending_items) && pending_items->valuedouble >= 0.0 &&
        pending_items->valuedouble <= (double)INT_MAX) {
        out->pending_items = pending_items->valueint;
    }
    if (cJSON_IsNumber(pending_bytes) && pending_bytes->valuedouble >= 0.0 &&
        pending_bytes->valuedouble <= (double)LLONG_MAX) {
        out->pending_bytes = (long long)pending_bytes->valuedouble;
    }
    if (cJSON_IsString(service) && service->valuestring) {
        snprintf(out->service_id, sizeof(out->service_id), "%s",
                 service->valuestring);
    }
    if (cJSON_IsString(reason) && reason->valuestring) {
        snprintf(out->reason, sizeof(out->reason), "%s", reason->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

static bool jw__blocked_game_launch_action(const char *socket_path,
                                           const char *type) {
    char request[128];
    if (!socket_path || !type) {
        return false;
    }
    int n = snprintf(request, sizeof(request), "{\"type\":\"%s\"}", type);
    if (n < 0 || (size_t)n >= sizeof(request)) {
        return false;
    }
    char *reply = NULL;
    size_t reply_len = 0;
    if (jw_ipc_request_timeout(socket_path, request, (size_t)n,
                               &reply, &reply_len, 500) != 0) {
        free(reply);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(reply, reply_len);
    free(reply);
    cJSON *type_json = root
        ? cJSON_GetObjectItemCaseSensitive(root, "type") : NULL;
    bool ok = cJSON_IsString(type_json) && type_json->valuestring &&
              strcmp(type_json->valuestring, "ok") == 0;
    cJSON_Delete(root);
    return ok;
}

static bool jw__surface_blocked_game_launch(
        const char *socket_path, const jw_blocked_game_launch *blocked) {
    if (!blocked || !blocked->blocked) {
        return false;
    }
    if (blocked->sync_pending) {
        char size[64];
        char title[192];
        jw__pakrat_format_size((unsigned long long)blocked->pending_bytes,
                               size, sizeof(size));
        snprintf(title, sizeof(title), "Sync before play — %d item%s, %s",
                 blocked->pending_items,
                 blocked->pending_items == 1 ? "" : "s", size);
        cat_list_item items[] = {
            CAT_LIST_ITEM("Wait for sync", "wait"),
            CAT_LIST_ITEM("Play anyway", "play"),
            CAT_LIST_ITEM("Cancel", "cancel"),
        };
        cat_footer_item footer[] = {
            { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
            { .button = CAT_BTN_A, .label = "Choose", .is_confirm = true },
        };
        cat_list_opts opts = cat_list_default_opts(title, items, 3);
        opts.footer = footer;
        opts.footer_count = 2;
        opts.help_text = "Syncthing still has Saves or States to transfer. Wait keeps syncing for up to 15 seconds. Play anyway stops Syncthing first. Cancel leaves syncing active.";
        cat_list_result result;
        int rc = cat_list(&opts, &result);
        const char *action = "game-check-cancel";
        bool leaves_launcher = false;
        if (rc == CAT_OK && result.selected_index == 0) {
            action = "game-check-wait";
            leaves_launcher = true;
        } else if (rc == CAT_OK && result.selected_index == 1) {
            action = "game-check-play-anyway";
            leaves_launcher = true;
        }
        bool accepted = jw__blocked_game_launch_action(socket_path, action);
        return accepted && leaves_launcher;
    }
    char message[640];
    if (blocked->override_allowed) {
        bool unsafe_card_binding =
            strcmp(blocked->reason, "unsafe-card-binding") == 0;
        if (unsafe_card_binding) {
            snprintf(message, sizeof(message),
                     "Syncthing card setup needs attention.\n\nLeaf could not "
                     "match this game's card to its Saves and States folders. "
                     "Review Cards and Folders in Syncthing.");
        } else if (blocked->pending_items > 0 || blocked->pending_bytes > 0) {
            char size[64];
            jw__pakrat_format_size(
                (unsigned long long)blocked->pending_bytes,
                size, sizeof(size));
            snprintf(message, sizeof(message),
                     "Sync needs attention.\n\n%d item%s (%s) remained when "
                     "waiting ended. Play anyway will stop Syncthing first; "
                     "Cancel leaves syncing active.",
                     blocked->pending_items,
                     blocked->pending_items == 1 ? "" : "s", size);
        } else if (strcmp(blocked->reason, "raofflineproxy-not-ready") == 0) {
            snprintf(message, sizeof(message),
                     "Offline achievements unavailable.\n\nRAOfflineProxy is "
                     "enabled but not responding. You can play now without "
                     "offline achievements, or cancel and check the service "
                     "in Settings > Services.");
        } else if (blocked->requires_verified_stop) {
            snprintf(message, sizeof(message),
                     "Sync needs attention.\n\nSyncthing could not be verified "
                     "stopped. Play anyway will retry the safe stop; Leaf "
                     "will not launch until Syncthing is stopped.");
        } else {
            snprintf(message, sizeof(message),
                     "Game launch blocked.\n\n%.128s may still be writing Saves "
                     "or States. Play anyway only if you accept possible data "
                     "corruption.",
                     blocked->service_id[0] ? blocked->service_id
                                            : "A background service");
        }
        bool rop_not_ready =
            strcmp(blocked->reason, "raofflineproxy-not-ready") == 0;
        cat_footer_item footer[] = {
            { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
            { .button = CAT_BTN_A,
              .label = unsafe_card_binding ? "Stop & Play"
                      : rop_not_ready  ? "Play without achievements"
                                       : "Play Anyway",
              .is_confirm = true },
        };
        cat_message_opts opts = {
            .message = message,
            .footer = footer,
            .footer_count = 2,
        };
        cat_confirm_result result;
        bool confirmed = cat_confirmation(&opts, &result) == CAT_OK &&
                         result.confirmed;
        if (confirmed) {
            return jw__blocked_game_launch_action(
                socket_path, "game-launch-override");
        }
        (void)jw__blocked_game_launch_action(
            socket_path, "game-launch-blocked-dismiss");
        return false;
    }

    snprintf(message, sizeof(message),
             "Game launch blocked.\n\nA previous game may still be writing "
             "Saves or States. Restart the device before launching another "
             "game.");
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "OK", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    cat_confirm_result result;
    (void)cat_confirmation(&opts, &result);
    return false;
}

/* SDL installs its own SIGTERM handler that pushes SDL_QUIT, which Catastrophe
   maps to CAT_BTN_B -- so a plain SIGTERM reads as a "back" press and the
   launcher stays up. The daemon uses SIGTERM to ask us to exit (a language
   change, a game launch), so we need a real one. Installed AFTER cat_init, or
   SDL's would win. */
static volatile sig_atomic_t g_term_requested = 0;

static void jw__term_handler(int sig) {
    (void)sig;
    g_term_requested = 1;
}

int main(void) {
    /* jawakad uses SIGTERM to replace this frontend with a ready game. SDL's
       handler turns it into a Back press instead of terminating the child. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Install before anything can signal us — SIGUSR1's default disposition is to
       terminate the process. */
    signal(SIGUSR1, jw__screenshot_flash_handler);
    long long process_start_ms = jw__monotonic_ms();
    char *socket_path = jw_socket_path();
    char *db_path     = jw_db_path();
    char *sdcard_root = jw_sdcard_root();
    if (!socket_path || !db_path || !sdcard_root) {
        jw_log_error("could not resolve runtime paths");
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }

    long long hello_start_ms = jw__monotonic_ms();
    if (jw_ipc_hello(socket_path, "launcher") != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?",
                     socket_path);
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }
    long long hello_done_ms = jw__monotonic_ms();

    /* Load the UI language before anything draws. The daemon already resolved
       the matching font into this process's environment when it spawned us, so
       there is nothing to reconcile here -- read the setting, load the table,
       and every T() from this point on is translated.

       A missing or damaged table simply leaves lookups returning their English
       keys, so a failure here is not worth aborting startup over. */
    {
        char lang[16];
        if (jw_db_get_setting(db_path, "language", lang, sizeof(lang)) != 0 || !lang[0])
            snprintf(lang, sizeof(lang), "%s", "en");
        jw_i18n_load(lang);
    }

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title       = "Jawaka Launcher";
    cfg.disable_background = true;
    /* The joystick scan costs ~200ms on MLP1; run it after the first frame is
       on screen (below, right after frontend-ready) instead of before it. */
    cfg.defer_input_init   = true;

    long long cat_start_ms = jw__monotonic_ms();
    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }
    /* After cat_init: SDL_Init installs a SIGTERM handler of its own, and the
       last one registered wins. */
    signal(SIGTERM, jw__term_handler);
    jw_cat_services_install(socket_path);
    /* Register the failsafe unlock chord (acted on only while focus mode is
       active — see the main loop). */
    {
        cat_button chord[] = { CAT_BTN_MENU, CAT_BTN_SELECT, CAT_BTN_START,
                               CAT_BTN_L2, CAT_BTN_R2 };
        cat_register_chord("focus-unlock", chord,
                           (int)(sizeof(chord) / sizeof(chord[0])),
                           JW_FOCUS_CHORD_WINDOW_MS);
    }
    long long cat_done_ms = jw__monotonic_ms();

    /* Resolve theme: env > DB > default Jawaka-Tabs */
    long long theme_start_ms = jw__monotonic_ms();
    char theme_name_buf[256];
    jw_resolve_theme_name(db_path, theme_name_buf, sizeof(theme_name_buf));
    const char *theme_name = theme_name_buf;
    {
        cat_stylesheet ss;
        if (cat_stylesheet_load_theme(&ss, theme_name) == CAT_OK)
            cat_stylesheet_apply(&ss);
        else
            jw_log_error("theme '%s' not found, using defaults", theme_name);
    }
    long long theme_done_ms = jw__monotonic_ms();

    cat_activate_window();

    jw_launcher_state state;
    memset(&state, 0, sizeof(state));
    state.library_generation = -1;
    /* A resume breadcrumb (left by the last game/app launch, cleared on reboot)
       restores the exact position; otherwise honor the persisted Startup Tab
       (Settings > Behavior > Startup Tab), default Games. Index mirrors jw_tab. */
    jw_resume resume;
    bool have_resume = jw__load_resume(&resume);
    /* Build the visible-tab set first (tabs can be hidden/reordered), so the
       resume/startup choice below can fall back to the first visible tab. */
    jw__load_visible_tabs(&state, db_path);
    state.current_tab = JW_TAB_GAMES;
    if (have_resume && resume.tab >= 0 && resume.tab < JW_TAB_COUNT) {
        state.current_tab = (jw_tab)resume.tab;
    } else {
        char startup_buf[16];
        if (jw_db_get_setting(db_path, "startup_tab_index", startup_buf,
                              sizeof(startup_buf)) == 0 && startup_buf[0]) {
            int idx = atoi(startup_buf);
            if (idx >= 0 && idx < JW_TAB_COUNT)
                state.current_tab = (jw_tab)idx;
        }
    }
    /* If the chosen tab is hidden, fall back to the first visible tab. */
    jw__reconcile_current_tab(&state);
    snprintf(state.sdcard_root, sizeof(state.sdcard_root), "%s", sdcard_root);
    snprintf(state.socket_path, sizeof(state.socket_path), "%s", socket_path);
    snprintf(state.db_path, sizeof(state.db_path), "%s", db_path);
    /* From here on every list, scroll view and keyboard reports its own movement
       (see jw__ui_feedback); screens no longer have to remember to. */
    cat_ui_feedback_set(jw__ui_feedback, &state);
    char *state_dir = jw_state_dir();
    if (state_dir && state_dir[0]) {
        snprintf(state.state_dir, sizeof(state.state_dir), "%s", state_dir);
    }
    free(state_dir);
    if (jw__resolve_platform_root(sdcard_root, state.platform_root,
                                  sizeof(state.platform_root)) != 0) {
        state.platform_root[0] = '\0';
    }

    /* 5-Game Mode: if the daemon spawned us into focus mode, resolve the curated
       set now (before the first frame) so the very first render is the focus
       screen, not a flash of the normal launcher. */
    jw__focus_init(&state);

    snprintf(state.status, sizeof(state.status), "%s", "loading library...");

    long long cache_start_ms = jw__monotonic_ms();
    if (jw__load_library_cache(socket_path, db_path, &state) != 0) {
        snprintf(state.status, sizeof(state.status), "%s", "scanning library...");
        state.scan_ready = false;
        state.scan_running = true;
    }
    long long cache_done_ms = jw__monotonic_ms();

    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;
    const char *layout_name = (layout == CAT_LAUNCHER_VERTICAL)   ? "vertical"
                            : (layout == CAT_LAUNCHER_HORIZONTAL) ? "horizontal"
                            : (layout == CAT_LAUNCHER_COVERFLOW)  ? "coverflow"
                            : "tabbed";
    jw_log_info("launcher layout: %s (theme=%s)", layout_name, theme_name);

    /* Init settings UI with the currently-active theme */
    long long settings_start_ms = jw__monotonic_ms();
    jw_settings_ui_init(&state.settings, db_path, theme_name, socket_path);
    long long settings_done_ms = jw__monotonic_ms();

    /* Prime the startup tab's lazily-loaded contents so the first frame is
       correct (Favorites/Recents are normally loaded on tab entry, and the
       Settings tab is owned by jw_settings_ui). Tabbed layout only — other
       layouts use flat/carousel lists, not per-tab state. */
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED) {
        if (state.current_tab == JW_TAB_FAVORITES)
            jw__load_favorites_tab(db_path, &state);
        else if (state.current_tab == JW_TAB_RECENTS)
            jw__load_recents_tab(db_path, &state);
    }

    jw__rebuild_for_layout(&state);

    /* Restore the drilled-in browser + cursors from the resume breadcrumb. Must run
       AFTER jw__rebuild_for_layout, which re-inits state->list to cursor 0. */
    if (have_resume)
        jw__apply_resume(db_path, &state, &resume);

    /* A standalone Menu+Select asked jawakad to reopen us straight into the
       switcher carousel, seeded on the just-exited game (env set by the daemon
       on respawn). Open it after the library/layout/resume are settled but
       before the first frame so it shows immediately. */
    if (getenv("JAWAKA_OPEN_SWITCHER")) {
        jw__open_switcher(db_path, &state);
        const char *sel_system = getenv("JAWAKA_SWITCHER_SELECT_SYSTEM");
        const char *sel_rom = getenv("JAWAKA_SWITCHER_SELECT_ROM");
        if (sel_rom && sel_rom[0])
            jw_game_switcher_select(&state.switcher, sel_system, sel_rom);
        unsetenv("JAWAKA_OPEN_SWITCHER");
        unsetenv("JAWAKA_SWITCHER_SELECT_SYSTEM");
        unsetenv("JAWAKA_SWITCHER_SELECT_ROM");
    }

    jw_autodemo demo;
    jw_autodemo_init(&demo);
    bool running = true;

    long long first_frame_start_ms = jw__monotonic_ms();
    cat_request_frame();
    jw__render_launcher(&state);
    long long first_frame_done_ms = jw__monotonic_ms();

    /* First frame is on screen; initialize input before any platform readiness
     * IPC so a slow daemon reply cannot postpone joystick setup. */
    long long input_start_ms = jw__monotonic_ms();
    cat_init_input();
    long long input_done_ms = jw__monotonic_ms();

    /* jawakad owns any platform-specific readiness side effects such as
     * dismissing the MLP1 stock boot transition. */
    long long ready_start_ms = jw__monotonic_ms();
    if (jw_ipc_frontend_ready(socket_path, "launcher") != 0) {
        jw_log_warn("frontend-ready notification failed");
    }
    long long ready_done_ms = jw__monotonic_ms();
    jw_log_info("launcher startup timings: hello_ms=%lld cat_ms=%lld theme_ms=%lld cache_ms=%lld settings_ms=%lld first_frame_ms=%lld input_ms=%lld ready_ms=%lld total_ms=%lld",
                hello_done_ms - hello_start_ms,
                cat_done_ms - cat_start_ms,
                theme_done_ms - theme_start_ms,
                cache_done_ms - cache_start_ms,
                settings_done_ms - settings_start_ms,
                first_frame_done_ms - first_frame_start_ms,
                input_done_ms - input_start_ms,
                ready_done_ms - ready_start_ms,
                ready_done_ms - process_start_ms);

    /* A fail-closed LIFE-1 stop happens after the launcher that requested the
       game has exited. The replacement launcher is therefore the first place
       that can visibly name the possible writer and collect the contract's
       explicit override. */
    jw_blocked_game_launch blocked_launch;
    if (jw__blocked_game_launch_query(socket_path, &blocked_launch) &&
        blocked_launch.blocked &&
        jw__surface_blocked_game_launch(socket_path, &blocked_launch)) {
        running = false;
    }

    /* Move the status-bar/library polls off the render thread (see jw__status_poller). */
    jw__status_poller_start(socket_path, &state.settings);

    /* The System menu's "Search" item left a marker — open the search overlay now
       that input is up (jw__open_search runs a blocking keyboard). */
    {
        FILE *mf = fopen(JW_OPEN_SEARCH_MARKER, "r");
        if (mf) {
            fclose(mf);
            remove(JW_OPEN_SEARCH_MARKER);
            jw__open_search(db_path, &state);
        }
    }

    while (running) {
        if (g_term_requested) {
            jw_log_info("SIGTERM received; exiting for respawn");
            break;
        }
        cat_input_event ev;
        bool had_input = false;
        bool was_menu_open = state.menu_open;
        bool was_settings_open = jw_settings_ui_is_open(&state.settings);
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            had_input = true;
            jw__handle_input(socket_path, db_path, &state, ev.button, &running);
        }

        /* Failsafe unlock chord: drain combo events every frame (act only in
           focus mode); once the chord has been held for JW_FOCUS_CHORD_HOLD_MS,
           exit focus mode. Keep waking while it is held so the hold timer runs. */
        {
            cat_combo_event ce;
            while (cat_poll_combo(&ce)) {
                if (state.focus_active && ce.id &&
                    strcmp(ce.id, "focus-unlock") == 0) {
                    if (ce.triggered) {
                        state.focus_chord_held = true;
                        state.focus_chord_start_ms = SDL_GetTicks();
                    } else {
                        state.focus_chord_held = false;
                    }
                }
            }
            if (state.focus_active && state.focus_chord_held) {
                if (SDL_GetTicks() - state.focus_chord_start_ms >=
                        JW_FOCUS_CHORD_HOLD_MS) {
                    state.focus_chord_held = false;
                    jw_log_info("focus: failsafe unlock chord held; exiting");
                    jw__focus_unlock_exit(&state);
                } else {
                    cat_request_frame_in(120);
                }
            }
        }

        /* The Home Tabs editor lives on the Settings UI (reached via the System
           menu's Settings tab). When the menu / settings overlay closes back to
           Content, re-read the visible-tab set from the DB so any hide/reorder
           takes effect live, and snap current_tab back onto it if it was pointing
           at a now-hidden tab. */
        if ((was_menu_open && !state.menu_open) ||
            (was_settings_open && !jw_settings_ui_is_open(&state.settings))) {
            jw_tab prev_tab = state.current_tab;
            jw__load_visible_tabs(&state, db_path);
            jw__reconcile_current_tab(&state);
            /* If reconciling moved us off a now-hidden tab, load the new tab's
               contents (Favorites/Recents load lazily) and reset the cursor —
               mirroring jw__switch_tab's on-entry refresh. */
            if (state.current_tab != prev_tab &&
                jw__layout_uses_channels(cat_get_stylesheet()->launcher.layout)) {
                if (state.current_tab == JW_TAB_FAVORITES)
                    jw__load_favorites_tab(db_path, &state);
                else if (state.current_tab == JW_TAB_RECENTS)
                    jw__load_recents_tab(db_path, &state);
                cat_list_state_jump(&state.list, 0, jw__tab_list_count(&state));
            }
        }

        if (demo.enabled && !demo.fired) {
            uint32_t rem = jw_autodemo_remaining_ms(&demo);
            if (jw_autodemo_should_fire(&demo)) {
                cat_hide_window();
                jw_ipc_open_menu(socket_path);
                running = false;
            } else {
                cat_request_frame_in(rem);
            }
        }

        /* While the Display & Sound page is open, keep repainting so the samples
           the background poller delivers (brightness + audio status, folded in by
           jw__status_poller_sync) actually reach the screen. The sampling itself
           is NOT done here on purpose: each round trip to jawakad costs ~110ms of
           latency and this page needs two, which is what used to make its cursor
           movement drag. Nothing on the render thread may block on IPC. */
        if (jw_settings_ui_wants_av_poll(&state.settings)) {
            cat_request_frame_in(300);
        }

        /* While the Network page is open, keep the Wi-Fi status live (the refresh
           self-throttles so platform Wi-Fi is not polled every frame). */
        if (jw_settings_ui_wants_wifi_poll(&state.settings)) {
            jw_settings_ui_refresh_wifi(&state.settings);
            cat_request_frame_in(2000);
        }

        if (jw_settings_ui_wants_bluetooth_poll(&state.settings)) {
            jw_settings_ui_refresh_bluetooth(&state.settings);
            cat_request_frame_in(250);
        }

        if (jw_settings_ui_wants_update_poll(&state.settings)) {
            jw_settings_ui_refresh_update(&state.settings);
            cat_request_frame_in(500);
        }

        if (jw_settings_ui_wants_services_poll(&state.settings)) {
            jw_settings_ui_refresh_services(&state.settings);
            cat_request_frame_in(1000);
        }

        /* The library-generation IPC and the status-bar volume/Wi-Fi/Bluetooth
           reads all run on the background status poller now (jw__status_poller);
           the sync publishes what to poll and folds delivered samples into the
           settings/status-bar caches, and the generation poll adopts the latest
           generation. Keeping the polls off the render thread is what removes
           the ~1s hitch in continuous animations. */
        jw__status_poller_sync(&state.settings);
        jw__poll_library_generation(socket_path, db_path, &state);
        jw__poll_scrape_status(&state);

        /* 1080p120 auto-revert: when the daemon has armed a revert (after a
           deliberate switch to a 120Hz TV mode), show the blocking keep-or-revert
           prompt so the user can confirm the TV actually lit up. Throttled so a
           held d-pad doesn't spam the IPC. Suppressed in focus mode: the device is
           a locked kiosk there (no way to reach the mode switch), a blocking prompt
           would swallow the failsafe unlock chord, and if a revert was somehow
           armed the daemon still auto-reverts to the safe mode on its own timer. */
        if (!state.focus_active) {
            static uint32_t s_revert_poll = 0;
            uint32_t rn = SDL_GetTicks();
            if (s_revert_poll == 0 || rn - s_revert_poll >= 700) {
                s_revert_poll = rn;
                int rsecs = 0;
                if (jw_ipc_hdmi_revert_status(socket_path, &rsecs) == 0 && rsecs > 0) {
                    jw__hdmi_keep_prompt(socket_path);
                    cat_request_frame();
                }
            }
        }

        /* Keep the status bar live while idle: the launcher only renders on
           input or a requested frame, so without this a wifi connect / charger
           plug-in / clock tick wouldn't show until the next button press. ~1s
           is responsive and cheap (the earliest pending frame request wins, so
           shorter ticks above still fire sooner). */
        cat_request_frame_in(1000);

        /* Keep navigation instant while cover art streams in: skip the one
           synchronous cover decode on the frame that handled a button press, so
           the highlight/carousel moves without waiting on a decode. Covers keep
           decoding on the idle frames between presses. */
        /* Screenshot flash (SIGUSR1 from jawakad after a UI/focus capture): a
           translucent white that fades out over the live scene, like RetroArch's.
           Render the scene with its present deferred, lay the fading white on top,
           then present once. Covers focus mode too — same process. */
        if (g_screenshot_flash) {
            g_screenshot_flash = 0;
            /* Arm only when not already flashing: a burst of SIGUSR1 must not keep
               re-extending the fade (which would hold the screen white). */
            if (state.flash_until_ms == 0)
                state.flash_until_ms = SDL_GetTicks() + JW_SCREENSHOT_FLASH_MS;
        }
        if (state.flash_until_ms != 0) {
            int32_t rem = (int32_t)(state.flash_until_ms - SDL_GetTicks());
            int alpha = (rem > 0)
                ? JW_SCREENSHOT_FLASH_ALPHA * rem / JW_SCREENSHOT_FLASH_MS
                : 0;
            /* Time-based, so a dropped frame never lengthens it; end at the min
               alpha so the near-invisible tail can't sit on screen an extra frame. */
            if (alpha > JW_SCREENSHOT_FLASH_MIN_ALPHA) {
                /* Schedule the next fade frame BEFORE presenting: the idle-wait
                   lives inside cat_present() and sleeps until the earliest pending
                   frame request, so requesting after it would let the frame linger
                   until unrelated input. */
                cat_request_frame_in(16);
                g_defer_present = true;
                jw__suppress_inline_decode = true;   /* no cover decodes during the fade */
                jw__render_launcher(&state);   /* draw the scene, hold the present */
                jw__suppress_inline_decode = false;
                g_defer_present = false;
                SDL_Renderer *r = cat_get_renderer();
                SDL_BlendMode prev;
                SDL_GetRenderDrawBlendMode(r, &prev);
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, 255, 255, 255, (Uint8)alpha);
                SDL_RenderFillRect(r, NULL);
                SDL_SetRenderDrawBlendMode(r, prev);
                cat_present();
                continue;   /* skip the normal render below */
            }
            state.flash_until_ms = 0;
            cat_request_frame();   /* resume the normal scene */
        }

        jw__suppress_inline_decode = had_input;
        jw__render_launcher(&state);
        jw__suppress_inline_decode = false;
    }

    jw__status_poller_shutdown();
    jw__cover_loader_shutdown();
    jw__close_game_browser(&state);
    /* Hand-off exit. The launcher only ever exits to be respawned (into the menu /
       an app / a game) or for shutdown, so the OS reclaims everything — memory, the
       cover-art texture cache, the Wayland surface — the instant we exit. Running
       cat_quit() first (cat_cache_clear over a warm cover cache + SDL/TTF teardown)
       costs ~1s of dead time before the next screen appears, with no benefit. Skip
       it: hide the surface and _Exit. Logs are line-flushed; the resume + clean-exit
       markers are already on disk; the worker threads above were joined cleanly. */
    /* Coverage dump, when a tester asked for one. It goes here rather than in an
       atexit handler because this path _Exit()s deliberately, and it is safe to
       put a file write in front of a fast exit only because it is gated: with
       JAWAKA_I18N_COVERAGE unset this is a single branch on a cold global. */
    if (jw_i18n_coverage_enabled()) {
        const char *logs = getenv("LOGS_PATH");
        char cov[PATH_MAX];
        if (logs && logs[0] &&
            snprintf(cov, sizeof(cov), "%s/i18n-coverage-launcher.txt", logs) <
                (int)sizeof(cov)) {
            jw_i18n_coverage_dump(cov);
        }
    }
    cat_hide_window();
    _Exit(0);
}
