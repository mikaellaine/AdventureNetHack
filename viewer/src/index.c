#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#include "index.h"

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (!r) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(r, s, n);
    return r;
}

static int cmp_article(const void *a, const void *b) {
    const Article *aa = a, *bb = b;
    return strcasecmp(aa->title, bb->title);
}

/* Does the actual directory scan; wiki_index_load() below is a thin
 * wrapper that also loads a templates sub-index afterward -- kept as a
 * separate, non-recursing function so that inner load (for pages_templates/
 * itself) can't try to load a templates-of-templates sub-index in turn. */
static int wiki_index_load_flat(WikiIndex *idx, const char *pages_dir) {
    memset(idx, 0, sizeof *idx);
    idx->pages_dir = xstrdup(pages_dir);

    DIR *d = opendir(pages_dir);
    if (!d) return -1;

    int cap = 256;
    idx->articles = malloc(cap * sizeof(Article));

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        static const char suffix[] = ".wiki";
        size_t suflen = sizeof(suffix) - 1;
        if (len <= suflen) continue;
        if (strcmp(name + len - suflen, suffix) != 0) continue;

        char path[4096];
        snprintf(path, sizeof path, "%s/%s", pages_dir, name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (idx->n_articles == cap) {
            cap *= 2;
            idx->articles = realloc(idx->articles, cap * sizeof(Article));
        }
        Article *a = &idx->articles[idx->n_articles++];
        a->title = malloc(len - suflen + 1);
        memcpy(a->title, name, len - suflen);
        a->title[len - suflen] = '\0';
        a->path = xstrdup(path);
    }
    closedir(d);

    qsort(idx->articles, idx->n_articles, sizeof(Article), cmp_article);

    /* Optional redirects map, written by scripts/02_extract_pages.py */
    char redir_path[4096];
    snprintf(redir_path, sizeof redir_path, "%s/_redirects.tsv", pages_dir);
    FILE *f = fopen(redir_path, "r");
    if (f) {
        int rcap = 64;
        idx->redirects = malloc(rcap * sizeof(Redirect));
        char *line = NULL;
        size_t linecap = 0;
        ssize_t n;
        while ((n = getline(&line, &linecap, f)) != -1) {
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            const char *from = line;
            const char *to = tab + 1;
            if (idx->n_redirects == rcap) {
                rcap *= 2;
                idx->redirects = realloc(idx->redirects, rcap * sizeof(Redirect));
            }
            idx->redirects[idx->n_redirects].from = xstrdup(from);
            idx->redirects[idx->n_redirects].to = xstrdup(to);
            idx->n_redirects++;
        }
        free(line);
        fclose(f);
    }

    return 0;
}

int wiki_index_load(WikiIndex *idx, const char *pages_dir) {
    if (wiki_index_load_flat(idx, pages_dir) != 0) return -1;

    /* pages_templates/ (produced by 02_extract_pages.py --namespace 10) is
     * optional -- a checkout that hasn't run that extraction step yet just
     * gets idx->templates == NULL, and template transclusions stay
     * unexpanded in wikitext.c, same as before this existed. */
    char templates_dir[4096];
    snprintf(templates_dir, sizeof templates_dir, "%s/../pages_templates", pages_dir);
    DIR *td = opendir(templates_dir);
    if (td) {
        closedir(td);
        WikiIndex *templates = malloc(sizeof(WikiIndex));
        if (wiki_index_load_flat(templates, templates_dir) == 0) {
            idx->templates = templates;
        } else {
            free(templates);
        }
    }

    return 0;
}

void wiki_index_free(WikiIndex *idx) {
    if (idx->templates) {
        wiki_index_free(idx->templates);
        free(idx->templates);
        idx->templates = NULL;
    }
    for (int i = 0; i < idx->n_articles; i++) {
        free(idx->articles[i].title);
        free(idx->articles[i].path);
    }
    free(idx->articles);
    for (int i = 0; i < idx->n_redirects; i++) {
        free(idx->redirects[i].from);
        free(idx->redirects[i].to);
    }
    free(idx->redirects);
    free(idx->pages_dir);
    memset(idx, 0, sizeof *idx);
}

static void normalize_target(const char *raw, char *buf, size_t bufsz) {
    size_t o = 0;
    for (const char *p = raw; *p && *p != '#'; p++) {
        char c = (*p == '_') ? ' ' : *p;
        if (o + 1 < bufsz) buf[o++] = c;
    }
    buf[o] = '\0';
    /* trim surrounding whitespace */
    size_t start = 0;
    while (buf[start] == ' ') start++;
    size_t end = o;
    while (end > start && buf[end - 1] == ' ') end--;
    size_t len = end - start;
    memmove(buf, buf + start, len);
    buf[len] = '\0';
}

static int cmp_title_key(const void *key, const void *elem) {
    const Article *a = elem;
    return strcasecmp((const char *)key, a->title);
}

/* idx->articles is kept sorted by cmp_article (strcasecmp), so lookups can
 * binary search instead of scanning linearly -- this matters because it's
 * called once per wikilink across every article. */
static int find_article_ci(const WikiIndex *idx, const char *title) {
    Article *found = bsearch(title, idx->articles, (size_t)idx->n_articles, sizeof(Article), cmp_title_key);
    return found ? (int)(found - idx->articles) : -1;
}

int wiki_index_find(const WikiIndex *idx, const char *title) {
    return find_article_ci(idx, title);
}

int wiki_index_resolve(const WikiIndex *idx, const char *raw_target) {
    char norm[1024];
    normalize_target(raw_target, norm, sizeof norm);
    if (norm[0] == '\0') return -1;

    int found = find_article_ci(idx, norm);
    if (found >= 0) return found;

    /* Follow at most a couple of redirect hops. */
    char cur[1024];
    strncpy(cur, norm, sizeof cur - 1);
    cur[sizeof cur - 1] = '\0';
    for (int hop = 0; hop < 4; hop++) {
        int matched = -1;
        for (int i = 0; i < idx->n_redirects; i++) {
            if (strcasecmp(idx->redirects[i].from, cur) == 0) { matched = i; break; }
        }
        if (matched < 0) break;
        char next[1024];
        normalize_target(idx->redirects[matched].to, next, sizeof next);
        found = find_article_ci(idx, next);
        if (found >= 0) return found;
        strncpy(cur, next, sizeof cur - 1);
        cur[sizeof cur - 1] = '\0';
    }
    return -1;
}

int str_ci_contains(const char *haystack, const char *needle) {
    if (!*needle) return 1;
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (strncasecmp(haystack + i, needle, nn) == 0) return 1;
    }
    return 0;
}

char *wiki_index_read(const WikiIndex *idx, int article_index) {
    if (article_index < 0 || article_index >= idx->n_articles) return NULL;
    FILE *f = fopen(idx->articles[article_index].path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}
