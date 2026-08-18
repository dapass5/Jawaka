#ifndef JW_SEARCH_PINYIN_H
#define JW_SEARCH_PINYIN_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The table is generated from the Han mapping used by emulationstation-next.
 * Each value contains the possible pinyin initials for one code point. */
#include "pinyin_table.inc"

static const char *jw__pinyin_initials(uint32_t code) {
    size_t lo = 0;
    size_t hi = sizeof(jw_pinyin_table) / sizeof(jw_pinyin_table[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (jw_pinyin_table[mid].code < code) {
            lo = mid + 1u;
        } else if (jw_pinyin_table[mid].code > code) {
            hi = mid;
        } else {
            return jw_pinyin_table[mid].initials;
        }
    }
    return NULL;
}

static int jw__utf8_next(const unsigned char **cursor, uint32_t *out_code) {
    const unsigned char *p = *cursor;
    uint32_t code;
    size_t length;
    if (!*p) return 0;
    if (*p < 0x80) {
        code = *p;
        length = 1;
    } else if ((*p & 0xe0) == 0xc0 && p[1] != 0 && (p[1] & 0xc0) == 0x80) {
        code = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        length = 2;
    } else if ((*p & 0xf0) == 0xe0 && p[1] != 0 && p[2] != 0 &&
               (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        code = ((uint32_t)(p[0] & 0x0f) << 12) |
               ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        length = 3;
    } else if ((*p & 0xf8) == 0xf0 && p[1] != 0 && p[2] != 0 && p[3] != 0 &&
               (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        code = ((uint32_t)(p[0] & 0x07) << 18) |
               ((uint32_t)(p[1] & 0x3f) << 12) |
               ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        length = 4;
    } else {
        code = *p;
        length = 1;
    }
    *cursor = p + length;
    *out_code = code;
    return 1;
}

/* Matches a UTF-8 name against either its literal UTF-8 substring or the
 * contiguous pinyin-initial sequence. The caller keeps existing FTS/LIKE
 * results ahead of this fallback, so this function only decides membership. */
static int jw_pinyin_match(const char *name, const char *query) {
    if (!name || !query || !*query) return 0;
    if (strstr(name, query) != NULL) return 1;

    char wanted[128];
    size_t wanted_count = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p; ++p) {
        if (*p >= 0x80 || !isalnum(*p)) return 0;
        if (wanted_count + 1u >= sizeof(wanted)) return 0;
        wanted[wanted_count++] = (char)tolower(*p);
    }
    if (wanted_count == 0) return 0;

    const char *candidates[512];
    char ascii_candidates[512][2];
    size_t candidate_count = 0;
    const unsigned char *cursor = (const unsigned char *)name;
    uint32_t code;
    while (jw__utf8_next(&cursor, &code)) {
        const char *initials = NULL;
        if (code < 0x80) {
            if (isalnum((int)code)) {
                if (candidate_count >= sizeof(candidates) / sizeof(candidates[0])) {
                    break;
                }
                ascii_candidates[candidate_count][0] = (char)tolower((int)code);
                ascii_candidates[candidate_count][1] = '\0';
                initials = ascii_candidates[candidate_count];
            }
        } else {
            initials = jw__pinyin_initials(code);
        }
        if (initials && *initials && candidate_count < sizeof(candidates) / sizeof(candidates[0])) {
            candidates[candidate_count++] = initials;
        }
    }

    for (size_t start = 0; start < candidate_count; ++start) {
        int matched = 1;
        for (size_t i = 0; i < wanted_count; ++i) {
            size_t at = start + i;
            if (at >= candidate_count) { matched = 0; break; }
            int one = 0;
            for (const char *p = candidates[at]; *p; ++p) {
                if (tolower((unsigned char)*p) == wanted[i]) { one = 1; break; }
            }
            if (!one) { matched = 0; break; }
        }
        if (matched) return 1;
    }
    return 0;
}

#endif
