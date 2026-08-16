#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "pinyin-search-test: %s\n", message);
    exit(1);
}

int main(void) {
    char db_path[] = "/tmp/jawaka-pinyin.XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) fail("mkstemp failed");
    close(fd);
    unlink(db_path);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "NES", "超级马里奥", "Roms/NES/mario.nes", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "三国志", "Roms/PS/sangoku.cue", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "Black Jewel Reborn", "Roms/PS/jewel.cue", NULL) != 0) {
        fail("could not prepare fixture database");
    }
    jw_db_close(db);

    jw_search_result results[8];
    int count = 0;
    if (jw_db_search_library(db_path, "cjmla", results, 8, &count) != 0 ||
        count != 1 || strcmp(results[0].name, "超级马里奥") != 0) {
        fail("cjmla did not match Chinese title");
    }
    if (jw_db_search_library(db_path, "sgz", results, 8, &count) != 0 ||
        count != 1 || strcmp(results[0].name, "三国志") != 0) {
        fail("sgz did not match Chinese title");
    }
    if (jw_db_search_library(db_path, "Black Jewel", results, 8, &count) != 0 ||
        count != 1 || strcmp(results[0].name, "Black Jewel Reborn") != 0) {
        fail("ordinary search regressed");
    }

    unlink(db_path);
    puts("PASS pinyin-search-test");
    return 0;
}
