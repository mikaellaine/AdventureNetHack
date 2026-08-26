#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (!r) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(r, s, n);
    return r;
}

Doc *doc_new(const char *title) {
    Doc *d = calloc(1, sizeof(Doc));
    d->title = xstrdup(title);
    return d;
}

void doc_free(Doc *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->n_blocks; i++) {
        Block *b = &doc->blocks[i];
        for (int j = 0; j < b->text.n; j++) free(b->text.items[j].text);
        free(b->text.items);
        if (b->cells) {
            for (int c = 0; c < b->n_cells; c++) {
                for (int j = 0; j < b->cells[c].n; j++) free(b->cells[c].items[j].text);
                free(b->cells[c].items);
            }
            free(b->cells);
        }
    }
    free(doc->blocks);
    for (int i = 0; i < doc->n_links; i++) {
        free(doc->links[i].label);
        free(doc->links[i].target);
    }
    free(doc->links);
    free(doc->title);
    free(doc);
}

Block *doc_add_block(Doc *doc, BlockType type) {
    if (doc->n_blocks == doc->cap_blocks) {
        doc->cap_blocks = doc->cap_blocks ? doc->cap_blocks * 2 : 32;
        doc->blocks = realloc(doc->blocks, doc->cap_blocks * sizeof(Block));
    }
    Block *b = &doc->blocks[doc->n_blocks++];
    memset(b, 0, sizeof *b);
    b->type = type;
    return b;
}

void runlist_push(RunList *rl, const char *text, unsigned attr, int link_index) {
    if (!text || !*text) return;
    if (rl->n == rl->cap) {
        rl->cap = rl->cap ? rl->cap * 2 : 8;
        rl->items = realloc(rl->items, rl->cap * sizeof(Run));
    }
    Run *r = &rl->items[rl->n++];
    r->text = xstrdup(text);
    r->attr = attr;
    r->link_index = link_index;
}

int doc_add_link(Doc *doc, const char *label, const char *target, int resolved, int article_index) {
    if (doc->n_links == doc->cap_links) {
        doc->cap_links = doc->cap_links ? doc->cap_links * 2 : 16;
        doc->links = realloc(doc->links, doc->cap_links * sizeof(PageLink));
    }
    PageLink *l = &doc->links[doc->n_links];
    l->label = xstrdup(label);
    l->target = target ? xstrdup(target) : NULL;
    l->resolved = resolved;
    l->article_index = article_index;
    return doc->n_links++;
}

/* ---------------- word wrap ---------------- */

#define MAX_TRACKED_LINKS 4096

typedef struct {
    char **lines;
    int n_lines, cap_lines;
    Span *spans;
    int n_spans, cap_spans;
    LinkPos *link_pos;
    int n_link_pos, cap_link_pos;
    int seen_link[MAX_TRACKED_LINKS];
} WrapCtx;

static int wc_new_line(WrapCtx *w) {
    if (w->n_lines == w->cap_lines) {
        w->cap_lines = w->cap_lines ? w->cap_lines * 2 : 64;
        w->lines = realloc(w->lines, w->cap_lines * sizeof(char *));
    }
    w->lines[w->n_lines] = xstrdup("");
    return w->n_lines++;
}

static void wc_line_append(WrapCtx *w, int line, const char *text) {
    size_t old = strlen(w->lines[line]);
    size_t add = strlen(text);
    w->lines[line] = realloc(w->lines[line], old + add + 1);
    memcpy(w->lines[line] + old, text, add + 1);
}

static void wc_add_span(WrapCtx *w, int line, int start, int end, unsigned attr, int link_index) {
    if (end <= start) return;
    if (w->n_spans == w->cap_spans) {
        w->cap_spans = w->cap_spans ? w->cap_spans * 2 : 128;
        w->spans = realloc(w->spans, w->cap_spans * sizeof(Span));
    }
    Span *s = &w->spans[w->n_spans++];
    s->line = line; s->start = start; s->end = end; s->attr = attr; s->link_index = link_index;

    if (link_index >= 0 && link_index < MAX_TRACKED_LINKS && !w->seen_link[link_index]) {
        w->seen_link[link_index] = 1;
        if (w->n_link_pos == w->cap_link_pos) {
            w->cap_link_pos = w->cap_link_pos ? w->cap_link_pos * 2 : 64;
            w->link_pos = realloc(w->link_pos, w->cap_link_pos * sizeof(LinkPos));
        }
        LinkPos *lp = &w->link_pos[w->n_link_pos++];
        lp->line = line; lp->col = start; lp->page_link_idx = link_index;
    }
}

/* Word-wraps a run list into WrapCtx, starting at the given line/column and
 * re-indenting to `indent` on every wrapped line. Space runs (produced when
 * a paragraph's raw source lines are joined) carry a pending-space count
 * across run boundaries so gaps at run edges aren't lost. */
static int wrap_runlist(WrapCtx *w, const RunList *rl, int width, int indent, int start_line, int col) {
    int line = start_line;
    int pending_space = 0;

    for (int i = 0; i < rl->n; i++) {
        const Run *r = &rl->items[i];
        const char *p = r->text;
        while (*p) {
            if (*p == ' ') { pending_space++; p++; continue; }

            const char *we = p;
            while (*we && *we != ' ') we++;
            int word_bytes = (int)(we - p);

            char word[1024];
            int wl = word_bytes < (int)sizeof(word) - 1 ? word_bytes : (int)sizeof(word) - 1;
            memcpy(word, p, wl);
            word[wl] = '\0';
            /* Column width, not byte length -- `width`/`col` are both
             * column quantities (Span.start/end are documented as columns
             * in doc.h), so a multi-byte word must never be measured in
             * bytes here or every line-fit decision after it drifts. */
            int wordlen = doc_utf8_width(word);

            int gap = (col > indent) ? pending_space : 0;
            if (col > indent && col + gap + wordlen > width) {
                line = wc_new_line(w);
                col = indent;
                for (int k = 0; k < indent; k++) wc_line_append(w, line, " ");
                gap = 0;
            }
            if (gap > 0) {
                for (int g = 0; g < gap; g++) wc_line_append(w, line, " ");
                col += gap;
            }
            pending_space = 0;

            /* hard-wrap a single word too long to ever fit on a line */
            while (doc_utf8_width(word) > width - indent && width - indent > 4) {
                int chunk_cols = width - indent;
                int chunk_bytes = doc_utf8_byte_offset(word, chunk_cols);
                char piece[1024];
                memcpy(piece, word, chunk_bytes);
                piece[chunk_bytes] = '\0';
                int piece_cols = doc_utf8_width(piece);
                wc_line_append(w, line, piece);
                wc_add_span(w, line, col, col + piece_cols, r->attr, r->link_index);
                col += piece_cols;
                memmove(word, word + chunk_bytes, strlen(word) - (size_t)chunk_bytes + 1);
                line = wc_new_line(w);
                col = indent;
                for (int k = 0; k < indent; k++) wc_line_append(w, line, " ");
            }

            int wlen2 = doc_utf8_width(word);
            if (wlen2 > 0) {
                wc_line_append(w, line, word);
                wc_add_span(w, line, col, col + wlen2, r->attr, r->link_index);
                col += wlen2;
            }

            p = we;
        }
    }
    return line;
}

/* Appends a run list to `line` verbatim (no word-wrap, no width limit --
 * draw_line() clips to the terminal at draw time), preserving internal
 * spacing exactly. Used for BLK_PRE so ASCII maps keep their alignment. */
static void append_runlist_raw(WrapCtx *w, const RunList *rl, int line) {
    int col = doc_utf8_width(w->lines[line]);
    for (int i = 0; i < rl->n; i++) {
        const Run *r = &rl->items[i];
        wc_line_append(w, line, r->text);
        int len = doc_utf8_width(r->text);
        wc_add_span(w, line, col, col + len, r->attr, r->link_index);
        col += len;
    }
}

/* Display width of a UTF-8 string, counting codepoints rather than bytes
 * (a continuation byte is any byte matching 10xxxxxx). Table cells commonly
 * hold price-table glyphs like the multi-byte "<=" / ">=" signs, and sizing
 * column padding in bytes would overcount those and misalign every column
 * that follows one. Every codepoint this wiki's tables actually use is
 * single-width in a monospace terminal, so codepoint count is exact here
 * (no need for a full wcwidth pass to handle double-width CJK, etc).
 *
 * Exported (declared in doc.h) because ui.c needs the exact same notion of
 * "column" to convert Span.start/end -- which are column offsets, per the
 * struct comment in doc.h -- back to byte offsets when it slices the UTF-8
 * line buffer for addnstr(). Keeping one implementation shared between the
 * wrapper and the renderer is what keeps those two views of a line from
 * drifting apart. */
int doc_utf8_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) w++;
    return w;
}

/* Byte offset of the start of the `ncols`-th codepoint in `s` (i.e. the
 * byte length of a prefix of `s` that is exactly `ncols` columns wide), or
 * strlen(s) if `s` has fewer than `ncols` codepoints. The column-to-byte
 * counterpart of doc_utf8_width(). */
int doc_utf8_byte_offset(const char *s, int ncols) {
    const unsigned char *p = (const unsigned char *)s;
    int col = 0;
    while (*p && col < ncols) {
        p++;
        while ((*p & 0xC0) == 0x80) p++;
        col++;
    }
    return (int)((const char *)p - s);
}

static int runlist_width(const RunList *rl) {
    int w = 0;
    for (int i = 0; i < rl->n; i++) w += doc_utf8_width(rl->items[i].text);
    return w;
}

RenderedPage *doc_wrap(const Doc *doc, int width) {
    if (width < 20) width = 20;
    WrapCtx w; memset(&w, 0, sizeof w);
    wc_new_line(&w); /* line 0 */
    int line = 0;

    for (int bi = 0; bi < doc->n_blocks; bi++) {
        const Block *b = &doc->blocks[bi];
        switch (b->type) {
        case BLK_BLANK:
            line = wc_new_line(&w);
            break;
        case BLK_HR: {
            line = wc_new_line(&w);
            char rule[512];
            int n = width < (int)sizeof(rule) - 1 ? width : (int)sizeof(rule) - 1;
            memset(rule, '-', n); rule[n] = '\0';
            wc_line_append(&w, line, rule);
            wc_add_span(&w, line, 0, n, ATTR_DIM, -1);
            line = wc_new_line(&w);
            break;
        }
        case BLK_HEADING: {
            if (strlen(w.lines[line]) > 0) line = wc_new_line(&w);
            unsigned hattr = b->level <= 1 ? ATTR_HEAD1 : (b->level == 2 ? ATTR_HEAD2 : ATTR_HEAD3P);
            RunList tmp = b->text;
            for (int i = 0; i < tmp.n; i++) tmp.items[i].attr |= hattr;
            line = wrap_runlist(&w, &tmp, width, 0, line, (int)strlen(w.lines[line]));
            for (int i = 0; i < tmp.n; i++) tmp.items[i].attr &= ~hattr;
            line = wc_new_line(&w);
            break;
        }
        case BLK_TABLE_CAPTION: {
            /* Distinguishes tables that would otherwise look identical --
             * e.g. the same price table duplicated for "NetHack 3.4.3 and
             * earlier" vs "3.6.0 and later". Forced onto its own line
             * before and after, same as a heading, so it reads as a label
             * for the table that follows rather than running into it. */
            if (strlen(w.lines[line]) > 0) line = wc_new_line(&w);
            RunList tmp = b->text;
            for (int i = 0; i < tmp.n; i++) tmp.items[i].attr |= ATTR_BOLD;
            line = wrap_runlist(&w, &tmp, width, 0, line, 0);
            for (int i = 0; i < tmp.n; i++) tmp.items[i].attr &= ~ATTR_BOLD;
            line = wc_new_line(&w);
            break;
        }
        case BLK_LIST_ITEM: {
            if (strlen(w.lines[line]) > 0) line = wc_new_line(&w);
            int indent = b->level * 2;
            for (int k = 0; k < indent; k++) wc_line_append(&w, line, " ");
            const char *bullet = b->ordered ? "# " : "* ";
            wc_line_append(&w, line, bullet);
            wc_add_span(&w, line, indent, indent + 2, ATTR_DIM, -1);
            line = wrap_runlist(&w, &b->text, width, indent + 2, line, indent + 2);
            break;
        }
        case BLK_TABLE_ROW: {
            /* Table rows are never word-wrapped -- wrapping a row splits it
             * across screen lines and destroys column alignment. Instead
             * each row becomes one (possibly very wide) logical line that
             * the UI lets the user pan across horizontally.
             *
             * Alignment still needs every row in the table to agree on how
             * wide each column is, so a whole table (a maximal run of
             * consecutive BLK_TABLE_ROW blocks) is measured up front and
             * every row but the last column is right-padded to that
             * table-wide width -- otherwise column N only lines up with the
             * row above it when both happen to have equal-length text. */
            int table_start = bi;
            int table_end = bi;
            while (table_end + 1 < doc->n_blocks && doc->blocks[table_end + 1].type == BLK_TABLE_ROW)
                table_end++;

            int max_cols = 0;
            for (int ri = table_start; ri <= table_end; ri++)
                if (doc->blocks[ri].n_cells > max_cols) max_cols = doc->blocks[ri].n_cells;

            int *colw = calloc((size_t)max_cols, sizeof(int));
            for (int ri = table_start; ri <= table_end; ri++) {
                const Block *rb = &doc->blocks[ri];
                for (int c = 0; c < rb->n_cells; c++) {
                    int cw = runlist_width(&rb->cells[c]);
                    if (cw > colw[c]) colw[c] = cw;
                }
            }

            for (int ri = table_start; ri <= table_end; ri++) {
                const Block *rb = &doc->blocks[ri];
                if (strlen(w.lines[line]) > 0) line = wc_new_line(&w);
                for (int c = 0; c < rb->n_cells; c++) {
                    if (c > 0) {
                        wc_line_append(&w, line, " | ");
                        int pcol = doc_utf8_width(w.lines[line]);
                        wc_add_span(&w, line, pcol - 3, pcol, ATTR_DIM, -1);
                    }
                    RunList tmp = rb->cells[c];
                    if (rb->header_row) for (int i = 0; i < tmp.n; i++) tmp.items[i].attr |= ATTR_BOLD;
                    append_runlist_raw(&w, &tmp, line);
                    if (rb->header_row) for (int i = 0; i < tmp.n; i++) tmp.items[i].attr &= ~ATTR_BOLD;
                    if (c < rb->n_cells - 1) {
                        int pad = colw[c] - runlist_width(&tmp);
                        for (int k = 0; k < pad; k++) wc_line_append(&w, line, " ");
                    }
                }
            }
            free(colw);
            bi = table_end;
            break;
        }
        case BLK_PRE: {
            for (int c = 0; c < b->n_cells; c++) {
                if (strlen(w.lines[line]) > 0) line = wc_new_line(&w);
                append_runlist_raw(&w, &b->cells[c], line);
            }
            break;
        }
        case BLK_PARAGRAPH:
        default: {
            if (strlen(w.lines[line]) > 0) line = wc_new_line(&w);
            line = wrap_runlist(&w, &b->text, width, 0, line, 0);
            break;
        }
        }
    }

    RenderedPage *rp = calloc(1, sizeof(RenderedPage));
    rp->lines = w.lines;
    rp->n_lines = w.n_lines;
    rp->spans = w.spans;
    rp->n_spans = w.n_spans;
    rp->link_pos = w.link_pos;
    rp->n_link_pos = w.n_link_pos;
    rp->doc = doc;
    return rp;
}

void rendered_page_free(RenderedPage *p) {
    if (!p) return;
    for (int i = 0; i < p->n_lines; i++) free(p->lines[i]);
    free(p->lines);
    free(p->spans);
    free(p->link_pos);
    free(p);
}
