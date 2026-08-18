#ifndef JW_I18N_H
#define JW_I18N_H

#include <stdbool.h>
#include <stddef.h>

/* User-facing string lookup.
 *
 * The English string is the key. `T("Settings")` returns the translation when
 * one is loaded and the literal itself otherwise, so a missing or partial
 * translation degrades to English with no call-site handling. That property is
 * the whole reason the key is the source string rather than an enum: a
 * community translation lands incomplete and stays incomplete for a while.
 *
 * Wrap ONLY user-facing text. Log messages, IPC message types, DB column names,
 * file paths and env var names must stay unwrapped -- they look like UI strings
 * to a regex, which is why the wrapping pass cannot be automated.
 *
 * Identical English needing different translations takes a context prefix that
 * the compiler strips from the displayed text: T("verb|Open") and T("noun|Open")
 * are distinct keys, both rendering "Open" when untranslated.
 *
 * The returned pointer is owned by the table and stays valid until
 * jw_i18n_shutdown(). Do not free it, and do not hold it across a language
 * change -- the launcher relaunches on a language change, so in practice that
 * means do not stash it in a long-lived global.
 */

#define T(s) jw_i18n(s)

/* Load the table for `lang` ("en" or a code like "zh_CN"). "en" (or NULL/empty)
 * loads nothing and every lookup returns its key, which is the no-op default.
 *
 * Sources, in order:
 *   1. $UMRK_INTERNAL_DATA_PATH/i18n/<lang>.tsv  -- a live override, tab
 *      separated "english<TAB>translation" with # comments. This is what a
 *      translator exports from a spreadsheet, so they can iterate on device
 *      without a build. Lines that fail to parse are skipped, not fatal.
 *   2. $UMRK_PLATFORM_PATH/i18n/<lang>.jwi       -- the shipped compiled table.
 *
 * Returns true when a table loaded. Returns false for "en", for a missing
 * table, and for a corrupt one -- all three leave lookups returning their keys,
 * so a caller that ignores the result still behaves correctly.
 */
bool jw_i18n_load(const char *lang);

void jw_i18n_shutdown(void);

/* Never returns NULL for a non-NULL argument; returns `english` on any miss. */
const char *jw_i18n(const char *english);

/* The language actually loaded, "en" when none. Never NULL. */
const char *jw_i18n_language(void);

/* True when `lang` needs the CJK face rather than a themed Latin family. Takes
 * a language code rather than reading global state so callers that resolve a
 * language before loading a table (the fork() path that builds a child's
 * environment) can ask too. NULL, "" and "en" are all false. */
bool jw_i18n_language_is_cjk(const char *lang);

/* Language codes with a table present, newest-first is not meaningful so the
 * order is whatever the directory yields. `out` receives up to `max` pointers
 * into static storage, valid until the next call. Returns the count.
 *
 * The Settings row is hidden when this returns 0: shipping a language picker
 * with nothing to pick reads as broken, and it means a translator can make the
 * row appear simply by dropping their .tsv on the card. */
size_t jw_i18n_available(const char **out, size_t max);

/* Entry count in the loaded table, 0 when none. For diagnostics and the
 * coverage gate; not needed for normal use. */
size_t jw_i18n_count(void);

/* Coverage recording. A missing translation is otherwise silent -- T() returns
 * its key and the screen renders English -- so the only thing that has ever
 * detected one is a native speaker reading the whole device. This makes the gap
 * measurable instead, in any language, and catches keys assembled at runtime
 * that no source-scanning extractor can see.
 *
 * Enabled by JAWAKA_I18N_COVERAGE=1 in the environment, read once when a table
 * loads. Off, the cost is one predictable branch on a cold global. It records
 * distinct keys in memory only and never touches the filesystem during a
 * lookup: T() sits on the render path, and misses are the COMMON case there
 * because values (game names, scraper status) flow through it too.
 *
 * Meaningful only with a table loaded; running in English records nothing.
 */
bool jw_i18n_coverage_enabled(void);

/* Write the recorded keys, one per line, and return how many. Call from a
 * deliberate teardown or a signal handler's follow-up, never per frame.
 * Returns 0 when recording is off. A dumped key that already has a translation
 * in the .po is a format-conversion mismatch rather than a coverage gap. */
size_t jw_i18n_coverage_dump(const char *path);

#endif /* JW_I18N_H */
