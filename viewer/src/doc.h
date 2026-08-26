#ifndef DOC_H
#define DOC_H

/* Shared document model produced by the wikitext parser (wikitext.c) and
 * consumed by the word-wrapper (wrap.c) and the ncurses UI (ui.c).
 *
 * Parsing is split into two stages on purpose:
 *   1. wikitext_parse()  -> Doc: a list of logical blocks (paragraphs,
 *      headings, list items, table rows, ...), each holding a list of
 *      inline "runs" (text + style + optional link). No line breaking.
 *   2. doc_wrap()        -> RenderedPage: the Doc word-wrapped to a given
 *      terminal width, as concrete screen lines with style/link spans.
 * Keeping the Doc around lets the UI re-wrap on terminal resize without
 * re-parsing the wikitext.
 */

typedef enum {
    ATTR_NONE   = 0,
    ATTR_BOLD   = 1 << 0,
    ATTR_ITALIC = 1 << 1,
    ATTR_HEAD1  = 1 << 2,
    ATTR_HEAD2  = 1 << 3,
    ATTR_HEAD3P = 1 << 4,
    ATTR_DIM    = 1 << 5,
} RunAttr;

/* One contiguous run of text sharing the same style/link. Text never
 * contains '\n'; word-wrapping happens later. */
typedef struct {
    char *text;
    unsigned attr;
    int link_index; /* index into Doc.links, or -1 */
} Run;

typedef struct {
    Run *items;
    int n, cap;
} RunList;

typedef enum {
    BLK_PARAGRAPH,
    BLK_HEADING,
    BLK_LIST_ITEM,
    BLK_TABLE_ROW,
    BLK_TABLE_CAPTION, /* `|+ caption` inside a {| ... |} table, e.g. distinguishing
                         * two revisions of the same price table ("... 3.4.3 and
                         * earlier" / "... 3.6.0 and later"). Uses `text` like
                         * PARAGRAPH/HEADING. */
    BLK_HR,
    BLK_BLANK,
    BLK_PRE, /* preformatted: MediaWiki leading-space lines, <pre>, ASCII maps */
} BlockType;

typedef struct {
    BlockType type;
    int level;       /* heading level (1-6) or list indent level */
    int ordered;      /* list item: 1 if numbered ('#'), 0 if bulleted */
    RunList text;     /* PARAGRAPH / HEADING / LIST_ITEM */
    RunList *cells;   /* TABLE_ROW: one run-list per cell. PRE: one run-list
                        * per source line, rendered verbatim with no word
                        * wrap or line-joining (both share this field since
                        * neither needs the single `text` run-list above). */
    int n_cells;
    int header_row;   /* TABLE_ROW: 1 if this row is a header row ('!') */
} Block;

/* A link target as it appears in the rendered page. Everything here is
 * resolved against the local WikiIndex at parse time -- article_index is
 * the only thing needed to actually navigate, and is -1 whenever the link
 * doesn't point to a page that exists locally (nothing is ever fetched
 * over the network). */
typedef struct {
    char *label;      /* display text, for reference/debugging */
    char *target;     /* the raw link target title, for status messages */
    int resolved;     /* 1 if target exists locally and can be followed */
    int article_index; /* index into WikiIndex.articles, or -1 */
} PageLink;

typedef struct {
    char *title;
    Block *blocks;
    int n_blocks, cap_blocks;
    PageLink *links;
    int n_links, cap_links;
} Doc;

/* --- Rendered (word-wrapped) page, ready to draw --- */

typedef struct {
    int line;        /* 0-based screen line within the page */
    int start, end;  /* column range [start,end) */
    unsigned attr;
    int link_index;  /* index into RenderedPage.links, or -1 for plain style */
} Span;

typedef struct {
    int line;         /* screen line the link's first word appears on */
    int col;          /* column of the link's first word */
    int page_link_idx; /* index into Doc.links */
} LinkPos;

typedef struct {
    char **lines;
    int n_lines;
    Span *spans;
    int n_spans, cap_spans;
    LinkPos *link_pos; /* one entry per Doc link that actually got rendered */
    int n_link_pos, cap_link_pos;
    const Doc *doc; /* borrowed, for link target lookups */
} RenderedPage;

Doc *doc_new(const char *title);
void doc_free(Doc *doc);
int doc_add_link(Doc *doc, const char *label, const char *target, int resolved, int article_index);
Block *doc_add_block(Doc *doc, BlockType type);
void runlist_push(RunList *rl, const char *text, unsigned attr, int link_index);

RenderedPage *doc_wrap(const Doc *doc, int width);
void rendered_page_free(RenderedPage *p);

/* UTF-8 column <-> byte conversions. Span.start/end and LinkPos.col above
 * are column offsets (see their comments); a renderer slicing the raw
 * UTF-8 bytes of a RenderedPage line -- e.g. ui.c drawing a horizontally
 * panned slice -- needs these to turn a column range back into the byte
 * range that actually spans it. */
int doc_utf8_width(const char *s);
int doc_utf8_byte_offset(const char *s, int ncols);

#endif
