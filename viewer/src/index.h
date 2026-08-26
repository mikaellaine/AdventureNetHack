#ifndef INDEX_H
#define INDEX_H

/* Scans the local pages/ directory (produced by scripts/02_extract_pages.py)
 * and builds an in-memory index of article titles, used both to populate
 * the search/browse list and to resolve [[wiki links]] to local files.
 * Everything here is local filesystem access only -- no networking. */

typedef struct {
    char *title; /* article title, equals the .wiki filename without extension */
    char *path;  /* full path to the .wiki file */
} Article;

typedef struct {
    char *from;
    char *to;
} Redirect;

typedef struct WikiIndex {
    Article *articles;
    int n_articles;
    Redirect *redirects;
    int n_redirects;
    char *pages_dir;

    /* Template: namespace pages (scripts/02_extract_pages.py --namespace 10),
     * used only for expanding {{template}} transclusions in wikitext.c --
     * never shown in the browse list or navigable like `articles` above.
     * NULL if pages_templates/ (the sibling of pages_dir) doesn't exist,
     * e.g. on a checkout where that extraction step hasn't been run yet;
     * template transclusions just stay unexpanded in that case, same as
     * before this field existed. */
    struct WikiIndex *templates;
} WikiIndex;

int wiki_index_load(WikiIndex *idx, const char *pages_dir);
void wiki_index_free(WikiIndex *idx);

/* Resolve a raw link target as it appears in wikitext (e.g.
 * "Aligned_cleric#Notes" or "Aligned cleric") to an index into
 * idx->articles, or -1 if no local page matches. */
int wiki_index_resolve(const WikiIndex *idx, const char *raw_target);

/* Exact (case-insensitive) title lookup, e.g. for matching cached data
 * back up to an article index. -1 if no such title exists. */
int wiki_index_find(const WikiIndex *idx, const char *title);

/* Case-insensitive substring test: 1 if needle occurs in haystack. */
int str_ci_contains(const char *haystack, const char *needle);

/* Load the full wikitext of an article by index. Caller frees the result. */
char *wiki_index_read(const WikiIndex *idx, int article_index);

#endif
