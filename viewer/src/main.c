#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "index.h"
#include "ui.h"

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Locates the pages/ directory produced by scripts/02_extract_pages.py.
 * Tries, in order: $WIKI_PAGES_DIR, ./pages, ../pages, and the path
 * relative to this executable's own location (so `wikiview` works from any
 * working directory once built in place). Everything here is local
 * filesystem access -- no networking anywhere in this program. */
static int find_pages_dir(char *out, size_t outsz) {
    const char *env = getenv("WIKI_PAGES_DIR");
    if (env && dir_exists(env)) {
        snprintf(out, outsz, "%s", env);
        return 1;
    }
    if (dir_exists("pages")) { snprintf(out, outsz, "pages"); return 1; }
    if (dir_exists("../pages")) { snprintf(out, outsz, "../pages"); return 1; }

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/'); /* .../viewer/bin/wikiview -> .../viewer/bin */
        if (slash) {
            *slash = '\0';
            slash = strrchr(exe, '/'); /* .../viewer/bin -> .../viewer */
            if (slash) {
                *slash = '\0';
                slash = strrchr(exe, '/'); /* .../viewer -> project root
                                             * (pages/ is viewer/'s sibling,
                                             * not its child -- two strips
                                             * above only reach .../viewer
                                             * itself, so this third one is
                                             * what actually gets to the
                                             * root pages/ lives in) */
                if (slash) *slash = '\0';
                char candidate[PATH_MAX];
                snprintf(candidate, sizeof(candidate), "%s/pages", exe);
                if (dir_exists(candidate)) { snprintf(out, outsz, "%s", candidate); return 1; }
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    /* Required for ncursesw to correctly render multi-byte UTF-8 glyphs
     * (NetHackWiki's dungeon maps use non-ASCII placeholder characters
     * that get corrupted without this). */
    setlocale(LC_ALL, "");

    char pages_dir[PATH_MAX];
    if (!find_pages_dir(pages_dir, sizeof(pages_dir))) {
        fprintf(stderr,
                "Could not find a pages/ directory (checked $WIKI_PAGES_DIR, ./pages, ../pages).\n"
                "Run scripts/download_all.sh from the repo root first.\n");
        return 1;
    }

    WikiIndex idx;
    if (wiki_index_load(&idx, pages_dir) != 0) {
        fprintf(stderr, "Failed to read pages directory: %s\n", pages_dir);
        return 1;
    }
    if (idx.n_articles == 0) {
        fprintf(stderr, "No .wiki pages found in %s\n", pages_dir);
        wiki_index_free(&idx);
        return 1;
    }

    char filter[256] = "";
    if (argc > 1) {
        size_t used = 0;
        for (int i = 1; i < argc; i++) {
            if (used > 0 && used < sizeof(filter) - 1) filter[used++] = ' ';
            size_t l = strlen(argv[i]);
            if (used + l >= sizeof(filter)) l = sizeof(filter) - 1 - used;
            memcpy(filter + used, argv[i], l);
            used += l;
        }
        filter[used] = '\0';
    }

    run_app(&idx, filter);

    wiki_index_free(&idx);
    return 0;
}
