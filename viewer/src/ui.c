#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <ncurses.h>

#include "ui.h"
#include "doc.h"
#include "wikitext.h"

#define CP_LINK      1
#define CP_LINK_SEL  2
#define CP_LINK_DEAD 3
#define CP_HEADER    4
#define CP_STATUS    5
#define CP_SELROW    6
#define CP_SEARCH    7

/* article_view() return values; a value >= 0 means "follow this article
 * index" (already resolved to a local page -- nothing here ever names a
 * URL or touches the network). */
#define AV_QUIT   (-1)
#define AV_BACK   (-2)
#define AV_SEARCH (-3)

#define HSCROLL_STEP 8

typedef struct { int article_index; int scroll; } HistEntry;

static void init_colors(void) {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_LINK, COLOR_CYAN, -1);
    init_pair(CP_LINK_SEL, COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_LINK_DEAD, COLOR_RED, -1);
    init_pair(CP_HEADER, COLOR_YELLOW, -1);
    init_pair(CP_STATUS, COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_SELROW, COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_SEARCH, COLOR_BLACK, COLOR_YELLOW);
}

/* Waits briefly for a second key after a prefix key (Esc for Alt-<key>,
 * Ctrl-X for the Ctrl-X C exit chord). Returns ERR if nothing arrives in
 * time, so a bare Esc or Ctrl-X still behaves as before.
 *
 * The wait only needs to be long enough to catch a chord's second byte,
 * which a terminal emits as part of the same burst as the prefix key (Alt
 * is typically implemented client-side as literally sending Esc followed
 * immediately by the plain key) -- not long enough to cover a human's
 * reaction time, since nothing here is asking the user to press a second
 * key on purpose. 150ms was ergonomically noticeable as a hang on plain
 * Esc-to-quit, the overwhelmingly more common case; 25ms is still ample
 * for a same-burst chord byte over a local terminal or any reasonably
 * responsive SSH link. */
#define CHORD_WAIT_MS 25
static int wait_second_key(void) {
    timeout(CHORD_WAIT_MS);
    int c = getch();
    timeout(-1);
    return c;
}

/* Next/previous line whose text contains `term` case-insensitively,
 * searching from `from` inclusive. Empty `term` never matches -- an
 * incremental search with nothing typed yet has nothing to find.
 *
 * `wrap` controls what happens at the document's edge: typing a fresh
 * query (or editing one with backspace) still wraps, so the live preview
 * can land on a match anywhere in the document, however far from the
 * current scroll position -- but Ctrl-S/Ctrl-R cycling through matches
 * one at a time passes wrap=0, so running out of matches in that
 * direction is reported as reaching the end/beginning rather than
 * silently looping back around. */
static int find_line_forward(const RenderedPage *rp, const char *term, int from, int wrap) {
    if (!term[0] || rp->n_lines == 0) return -1;
    if (wrap) {
        for (int i = 0; i < rp->n_lines; i++) {
            int ln = ((from + i) % rp->n_lines + rp->n_lines) % rp->n_lines;
            if (str_ci_contains(rp->lines[ln], term)) return ln;
        }
    } else {
        for (int ln = from; ln < rp->n_lines; ln++) {
            if (str_ci_contains(rp->lines[ln], term)) return ln;
        }
    }
    return -1;
}

static int find_line_backward(const RenderedPage *rp, const char *term, int from, int wrap) {
    if (!term[0] || rp->n_lines == 0) return -1;
    if (wrap) {
        for (int i = 0; i < rp->n_lines; i++) {
            int ln = ((from - i) % rp->n_lines + rp->n_lines) % rp->n_lines;
            if (str_ci_contains(rp->lines[ln], term)) return ln;
        }
    } else {
        for (int ln = from; ln >= 0; ln--) {
            if (str_ci_contains(rp->lines[ln], term)) return ln;
        }
    }
    return -1;
}

/* ---------------- article view ---------------- */

static void span_style(const Span *sp, int is_selected, int resolved, attr_t *attr_out, short *pair_out) {
    attr_t a = A_NORMAL;
    short pair = 0;

    if (sp->attr & ATTR_BOLD) a |= A_BOLD;
    if (sp->attr & ATTR_ITALIC) a |= A_UNDERLINE;
    if (sp->attr & ATTR_HEAD1) { a |= A_BOLD | A_UNDERLINE; pair = CP_HEADER; }
    else if (sp->attr & ATTR_HEAD2) { a |= A_BOLD; pair = CP_HEADER; }
    else if (sp->attr & ATTR_HEAD3P) { a |= A_BOLD; }
    if (sp->attr & ATTR_DIM) a |= A_DIM;

    if (sp->link_index >= 0) {
        if (is_selected) {
            a = A_BOLD;
            pair = resolved ? CP_LINK_SEL : CP_LINK_DEAD;
            a |= A_REVERSE;
        } else if (resolved) {
            pair = CP_LINK;
            a |= A_UNDERLINE;
        } else {
            pair = CP_LINK_DEAD;
            a |= A_DIM;
        }
    }

    *attr_out = a;
    *pair_out = pair;
}

/* Draws text[s:e) (COLUMN offsets into the logical line, matching how
 * Span.start/end are defined in doc.h) at screen row y, shifted left by
 * hoff columns and clipped to the [0, cols) screen window -- this is what
 * lets draw_line() render a horizontally panned slice of a line that's
 * wider than the terminal.
 *
 * `text` itself is a UTF-8 byte string, so a column range is only good for
 * cursor positioning (move() takes a real screen column) -- addnstr()
 * needs a byte range. The two coincide for plain ASCII text, which is why
 * this used to work by treating s/e as byte offsets directly, but that
 * silently misaligns any line containing a multi-byte glyph (price tables'
 * "<=" / ">=" headers, "-"-dashes, ...): every span after the glyph lands
 * (bytes-per-glyph - 1) columns too far right on screen even though the
 * printed string itself is correct. Converting column -> byte only at the
 * point of slicing keeps that entire column-space/byte-space distinction
 * local to this one function. */
static void put_range(int y, int cols, int hoff, const char *text, int s, int e) {
    int vs = s > hoff ? s : hoff;
    int ve = e < hoff + cols ? e : hoff + cols;
    if (vs >= ve) return;
    int byte_s = doc_utf8_byte_offset(text, vs);
    int byte_e = doc_utf8_byte_offset(text, ve);
    move(y, vs - hoff);
    addnstr(text + byte_s, byte_e - byte_s);
}

static void draw_line(int y, int cols, int hoff, const char *text, const RenderedPage *rp, int ln, int cur_link) {
    int len = doc_utf8_width(text); /* column width, matching put_range's column-space args below */
    int col = 0;
    for (int si = 0; si < rp->n_spans; si++) {
        const Span *sp = &rp->spans[si];
        if (sp->line != ln) continue;
        if (sp->start < col) continue;
        if (sp->start > col) {
            put_range(y, cols, hoff, text, col, sp->start);
            col = sp->start;
        }
        int is_sel = cur_link >= 0 &&
                     rp->link_pos[cur_link].line == ln &&
                     rp->link_pos[cur_link].col == sp->start &&
                     rp->link_pos[cur_link].page_link_idx == sp->link_index;
        int resolved = sp->link_index >= 0 && rp->doc->links[sp->link_index].resolved;
        attr_t a; short pair;
        span_style(sp, is_sel, resolved, &a, &pair);
        if (pair) attron(COLOR_PAIR(pair));
        attron(a);
        put_range(y, cols, hoff, text, sp->start, sp->end);
        col = sp->end;
        attroff(a);
        if (pair) attroff(COLOR_PAIR(pair));
    }
    if (col < len) {
        put_range(y, cols, hoff, text, col, len);
    }
}

static void ensure_link_visible(const RenderedPage *rp, int cur_link, int *scroll, int body_h) {
    if (cur_link < 0) return;
    int ln = rp->link_pos[cur_link].line;
    if (ln < *scroll) *scroll = ln;
    else if (ln >= *scroll + body_h) *scroll = ln - body_h + 1;
    if (*scroll < 0) *scroll = 0;
}

/* The inverse of ensure_link_visible(): called after the viewport itself
 * moves (line/page scroll, g/G, ...) rather than after deliberate link
 * navigation, so the selected link never goes stale off-screen -- it always
 * points at whatever link is now closest to the middle of the visible page.
 * Left at -1 if no link is visible at all (e.g. scrolled to a link-less
 * stretch of prose), which is what makes the status line correctly fall
 * back to the plain keybinding hint instead of describing a link the user
 * can no longer see. */
static void resync_link_to_viewport(const RenderedPage *rp, int scroll, int body_h, int *cur_link) {
    if (rp->n_link_pos == 0) { *cur_link = -1; return; }
    int mid = scroll + body_h / 2;
    int best = -1, best_dist = body_h + 1;
    for (int i = 0; i < rp->n_link_pos; i++) {
        int ln = rp->link_pos[i].line;
        if (ln < scroll || ln >= scroll + body_h) continue;
        int dist = ln > mid ? ln - mid : mid - ln;
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    *cur_link = best;
}

/* Widest rendered line, in columns -- table rows aren't word-wrapped (see
 * doc.c) so this can be much wider than the terminal, which is what makes
 * horizontal panning necessary. */
static int max_line_width(const RenderedPage *rp) {
    int w = 0;
    for (int i = 0; i < rp->n_lines; i++) {
        int l = doc_utf8_width(rp->lines[i]);
        if (l > w) w = l;
    }
    return w;
}

/* Case-insensitive substring search that also reports *where* the match
 * is, in the same column units Span.start/end and doc_utf8_width() use
 * everywhere else in this app (not byte offsets) -- so a match landing
 * after a multi-byte glyph earlier on the line (an accented character, a
 * price table's "<=" / ">=" sign, ...) still highlights at the right
 * screen column instead of drifting right by however many extra bytes
 * that glyph took. Returns 1 and fills out_start and out_end (a [start,end)
 * column range) on success, 0 if `needle` doesn't occur in `haystack` at
 * all (the caller already knows it does via str_ci_contains() before
 * reaching here, so that should never happen in practice, but the
 * fallback is "don't highlight anything" rather than a wild column). */
static int find_match_columns(const char *haystack, const char *needle, int *out_start, int *out_end) {
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn == 0 || nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (strncasecmp(haystack + i, needle, nn) != 0) continue;
        char before[4096];
        size_t bl = i < sizeof(before) - 1 ? i : sizeof(before) - 1;
        memcpy(before, haystack, bl);
        before[bl] = '\0';
        int start_col = doc_utf8_width(before);

        char matched[4096];
        size_t ml = nn < sizeof(matched) - 1 ? nn : sizeof(matched) - 1;
        memcpy(matched, haystack + i, ml);
        matched[ml] = '\0';

        *out_start = start_col;
        *out_end = start_col + doc_utf8_width(matched);
        return 1;
    }
    return 0;
}

/* Interactive incremental search within the article already on screen,
 * modeled on Emacs/readline isearch: typing narrows the search live,
 * jumping *scroll to the first match found searching down from wherever
 * the search started, and Ctrl-S/Ctrl-R step to the next match further
 * down/up from wherever the view currently is -- so repeated Ctrl-S cycles
 * forward through every occurrence, Ctrl-R backward. Redraws the article
 * body itself on every change (the caller's own redraw loop is paused
 * while this runs) so the jump is visible immediately, not just after
 * confirming. On Esc, *scroll and buf are both restored to what they held
 * on entry -- same "cancel means nothing happened" contract the old static
 * prompt_line() had. Returns 1 if confirmed with Enter, 0 if cancelled.
 *
 * Once the user has pressed Ctrl-S/Ctrl-R at least once (`cycled`), they've
 * moved from composing a query to browsing its matches, and h/j/k/l/Ctrl-N/
 * Ctrl-P take on their normal article-view meaning (scroll etc.) instead of
 * being typed into the search box -- confirming the search (keeping the
 * match just landed on) and pushing the key back with ungetch() so the
 * caller's own getch() sees it next and handles it exactly as if search
 * mode had never been entered. Before that first cycle, those same keys
 * are still ordinary characters, so a query like "jelly" can be typed.
 *
 * A terminal resize mid-search is handled locally, the same way
 * article_view()'s own loop handles one: getmaxyx() is polled every
 * iteration (not just reactively on KEY_RESIZE, matching that loop's own
 * belt-and-braces approach), and the page is re-wrapped when the width
 * actually changed. The re-wrapped copy is kept in `owned_rp` and freed
 * before being replaced or on return -- `rp_in` itself is the caller's
 * RenderedPage, still mid-use by article_view()'s own loop once this
 * function returns, and must never be freed here. */
static int search_in_article(const Doc *doc, const RenderedPage *rp_in, int hscroll, int cur_link,
                              int *scroll, char *buf, size_t bufsz) {
    char orig_buf[256];
    snprintf(orig_buf, sizeof orig_buf, "%s", buf);
    int orig_scroll = *scroll;
    int start_scroll = *scroll; /* re-anchor here as the typed term changes */
    const char *status_msg = NULL; /* NULL = show the "[ctrl-s/r]" hint instead */
    int cycled = 0;

    static const char *label = "Search article: ";

    const RenderedPage *rp = rp_in;
    RenderedPage *owned_rp = NULL;
    int width = -1; /* forces a wrap check on the very first iteration */

    int result;
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if (cols != width) {
            width = cols;
            RenderedPage *new_rp = doc_wrap(doc, width);
            if (owned_rp) rendered_page_free(owned_rp);
            rp = owned_rp = new_rp;
            cur_link = rp->n_link_pos > 0 ? 0 : -1;
        }
        int body_h = rows - 2;
        if (body_h < 1) body_h = 1;
        int max_scroll = rp->n_lines - body_h;
        if (max_scroll < 0) max_scroll = 0;
        if (*scroll > max_scroll) *scroll = max_scroll;
        if (*scroll < 0) *scroll = 0;

        erase();
        attron(A_BOLD);
        attron(COLOR_PAIR(CP_STATUS));
        mvprintw(0, 0, "%-*.*s", cols, cols, doc->title);
        attroff(COLOR_PAIR(CP_STATUS));
        attroff(A_BOLD);
        for (int row = 0; row < body_h; row++) {
            int ln = *scroll + row;
            if (ln >= rp->n_lines) break;
            draw_line(1 + row, cols, hscroll, rp->lines[ln], rp, ln, cur_link);
        }

        /* Highlight the actual matched text, not just scroll to its line
         * -- *scroll only ever lands on a line that genuinely contains
         * buf (find_line_forward()/find_line_backward() guarantee that),
         * and that line is always the first visible row (row 0 above,
         * screen row 1), so this only needs to run once per redraw, not
         * once per row. */
        if (buf[0] && *scroll < rp->n_lines && str_ci_contains(rp->lines[*scroll], buf)) {
            int mstart, mend;
            if (find_match_columns(rp->lines[*scroll], buf, &mstart, &mend)) {
                attron(COLOR_PAIR(CP_SEARCH));
                put_range(1, cols, hscroll, rp->lines[*scroll], mstart, mend);
                attroff(COLOR_PAIR(CP_SEARCH));
            }
        }

        attron(COLOR_PAIR(CP_STATUS));
        mvprintw(rows - 1, 0, "%-*.*s", cols, cols, "");
        mvprintw(rows - 1, 0, "%s%s  %s", label, buf, status_msg ? status_msg : "[ctrl-s/r]");
        attroff(COLOR_PAIR(CP_STATUS));
        curs_set(1);
        move(rows - 1, (int)(strlen(label) + strlen(buf)));
        refresh();

        int ch = getch();
        status_msg = NULL;
        if (ch == 27) {
            *scroll = orig_scroll;
            snprintf(buf, bufsz, "%s", orig_buf);
            result = 0;
            break;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) { result = 1; break; }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t l = strlen(buf);
            if (l > 0) buf[l - 1] = '\0';
            int found = find_line_forward(rp, buf, start_scroll, 1);
            if (found >= 0) *scroll = found;
            else if (!buf[0]) *scroll = start_scroll;
            else status_msg = "(not found)";
        } else if (ch == 21) { /* Ctrl-U: clear */
            buf[0] = '\0';
            *scroll = start_scroll;
        } else if (ch == 19) { /* Ctrl-S: next match, searching down (no wraparound) */
            cycled = 1;
            if (buf[0]) {
                int found = find_line_forward(rp, buf, *scroll + 1, 0);
                if (found >= 0) *scroll = found; else status_msg = "[Search reached end]";
            }
        } else if (ch == 18) { /* Ctrl-R: previous match, searching up (no wraparound) */
            cycled = 1;
            if (buf[0]) {
                int found = find_line_backward(rp, buf, *scroll - 1, 0);
                if (found >= 0) *scroll = found; else status_msg = "[Search reached beginning]";
            }
        } else if (cycled && (ch == 'h' || ch == 'j' || ch == 'k' || ch == 'l' || ch == 14 || ch == 16)) {
            /* h/j/k/l/Ctrl-N/Ctrl-P: done browsing matches -- confirm and
             * let the caller's own key handling take it from here. */
            ungetch(ch);
            result = 1;
            break;
        } else if (ch >= 32 && ch < 127) {
            size_t l = strlen(buf);
            if (l < bufsz - 1) { buf[l] = (char)ch; buf[l + 1] = '\0'; }
            int found = find_line_forward(rp, buf, start_scroll, 1);
            if (found >= 0) *scroll = found; else status_msg = "(not found)";
        }
        /* KEY_RESIZE: no explicit case needed -- getmaxyx() at the top of
         * the next iteration picks up the change unconditionally. */
    }
    if (owned_rp) rendered_page_free(owned_rp);
    curs_set(0);
    return result;
}

static int article_view(WikiIndex *idx, int article_index, int *scroll_io) {
    char *raw = wiki_index_read(idx, article_index);
    if (!raw) return AV_BACK;

    Doc *doc = wikitext_parse(idx->articles[article_index].title, raw, idx);
    free(raw);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int width = cols;
    RenderedPage *rp = doc_wrap(doc, width);
    int line_width = max_line_width(rp);

    int scroll = *scroll_io;
    int hscroll = 0;
    int cur_link = rp->n_link_pos > 0 ? 0 : -1;
    int resync_link = 0; /* set by viewport-scroll keys; consumed right after scroll is clamped below */
    char status[256] = "";
    char search_term[256] = "";

    int result = AV_QUIT;
    for (;;) {
        getmaxyx(stdscr, rows, cols);
        if (cols != width) {
            width = cols;
            rendered_page_free(rp);
            rp = doc_wrap(doc, width);
            line_width = max_line_width(rp);
            if (scroll >= rp->n_lines) scroll = rp->n_lines > 0 ? rp->n_lines - 1 : 0;
            cur_link = rp->n_link_pos > 0 ? 0 : -1;
        }

        int body_h = rows - 2;
        if (body_h < 1) body_h = 1;
        int max_scroll = rp->n_lines - body_h;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;
        if (scroll < 0) scroll = 0;

        if (resync_link) {
            resync_link_to_viewport(rp, scroll, body_h, &cur_link);
            resync_link = 0;
        }

        int max_hscroll = line_width - cols;
        if (max_hscroll < 0) max_hscroll = 0;
        if (hscroll > max_hscroll) hscroll = max_hscroll;
        if (hscroll < 0) hscroll = 0;

        erase();
        attron(A_BOLD);
        attron(COLOR_PAIR(CP_STATUS));
        mvprintw(0, 0, "%-*.*s", cols, cols, doc->title);
        attroff(COLOR_PAIR(CP_STATUS));
        attroff(A_BOLD);

        for (int row = 0; row < body_h; row++) {
            int ln = scroll + row;
            if (ln >= rp->n_lines) break;
            draw_line(1 + row, cols, hscroll, rp->lines[ln], rp, ln, cur_link);
        }

        move(rows - 1, 0);
        attron(COLOR_PAIR(CP_STATUS));
        if (status[0]) {
            mvprintw(rows - 1, 0, "%-*.*s", cols, cols, status);
        } else if (cur_link >= 0) {
            const PageLink *pl = &doc->links[rp->link_pos[cur_link].page_link_idx];
            char line[256];
            if (pl->resolved)
                snprintf(line, sizeof line, "-> %s   [Enter follow] [Tab/p next/prev-link] [Backspace back] [ctrl-s search]", pl->target);
            else
                snprintf(line, sizeof line, "-> %s (N/A)   [Tab next link] [Backspace back] [ctrl-s search]", pl->target);
            mvprintw(rows - 1, 0, "%-*.*s", cols, cols, line);
        } else {
            mvprintw(rows - 1, 0, "%-*.*s", cols, cols,
                     "[hjkl/yubn move] [Tab next link] [Backspace back] [ctrl-s search]");
        }
        attroff(COLOR_PAIR(CP_STATUS));
        status[0] = '\0';

        refresh();

        int ch = getch();
        switch (ch) {
        case 'q':
            result = AV_QUIT;
            goto out;
        case 27: { /* Esc, or Alt-v/Alt-</Alt-> (ESC v / ESC < / ESC >) */
            int c2 = wait_second_key();
            if (c2 == 'v' || c2 == 'V') { scroll -= body_h; resync_link = 1; break; }
            if (c2 == '<') { scroll = 0; resync_link = 1; break; }
            if (c2 == '>') { scroll = max_scroll; resync_link = 1; break; }
            result = AV_QUIT;
            goto out;
        }
        case KEY_BACKSPACE: case 127: case 8:
            result = AV_BACK;
            goto out;
        case KEY_LEFT: case 2: case 'h': /* Left / Ctrl-B / h: pan left */
            hscroll -= HSCROLL_STEP;
            break;
        case KEY_RIGHT: case 6: case 'l': /* Right / Ctrl-F / l: pan right */
            hscroll += HSCROLL_STEP;
            break;
        case 1: /* Ctrl-A: pan all the way left */
            hscroll = 0;
            break;
        case 5: /* Ctrl-E: pan all the way right */
            hscroll = max_hscroll;
            break;
        case '/': /* search the article listing */
            result = AV_SEARCH;
            goto out;
        case 19: { /* Ctrl-S: incremental search within this article's text */
            char buf[256];
            snprintf(buf, sizeof buf, "%s", search_term);
            if (search_in_article(doc, rp, hscroll, cur_link, &scroll, buf, sizeof buf)
                && buf[0]) {
                snprintf(search_term, sizeof search_term, "%s", buf);
                resync_link = 1;
            }
            break;
        }
        case '\n': case '\r': case KEY_ENTER:
            if (cur_link >= 0) {
                const PageLink *pl = &doc->links[rp->link_pos[cur_link].page_link_idx];
                if (pl->resolved) { result = pl->article_index; goto out; }
                snprintf(status, sizeof status, "N/A: %s", pl->target);
            }
            break;
        case '\t': case KEY_DOWN:
            if (cur_link >= 0 && cur_link + 1 < rp->n_link_pos) cur_link++;
            ensure_link_visible(rp, cur_link, &scroll, body_h);
            break;
        case KEY_BTAB: case KEY_UP: case 'p':
            if (cur_link > 0) cur_link--;
            ensure_link_visible(rp, cur_link, &scroll, body_h);
            break;
        case 'j': case 14: /* j / Ctrl-N: next line */
            scroll++;
            resync_link = 1;
            break;
        case 'k': case 16: /* k / Ctrl-P: previous line */
            scroll--;
            resync_link = 1;
            break;
        /* NetHack-style diagonal movement: y/u/b/n combine one step of the
         * pan (h/l) with one step of the scroll (j/k) they sit between on
         * a keyboard, same as the number-pad layout NetHack itself uses
         * (y k u / h . l / b j n) -- y=northwest, u=northeast,
         * b=southwest, n=southeast, applied here to pan+scroll instead of
         * dungeon-map movement. */
        case 'y': /* y: pan left + scroll up */
            hscroll -= HSCROLL_STEP;
            scroll--;
            resync_link = 1;
            break;
        case 'u': /* u: pan right + scroll up */
            hscroll += HSCROLL_STEP;
            scroll--;
            resync_link = 1;
            break;
        case 'b': /* b: pan left + scroll down */
            hscroll -= HSCROLL_STEP;
            scroll++;
            resync_link = 1;
            break;
        case 'n': /* n: pan right + scroll down */
            hscroll += HSCROLL_STEP;
            scroll++;
            resync_link = 1;
            break;
        case ' ': case KEY_NPAGE: case 22: /* space / PgDn / Ctrl-V: next page */
            scroll += body_h;
            resync_link = 1;
            break;
        case KEY_PPAGE: /* PgUp */
            scroll -= body_h;
            resync_link = 1;
            break;
        case 'g': case KEY_HOME:
            scroll = 0;
            resync_link = 1;
            break;
        case 'G': case KEY_END:
            scroll = max_scroll;
            resync_link = 1;
            break;
        case 24: { /* Ctrl-X: start of the Ctrl-X C "exit program" chord */
            int c2 = wait_second_key();
            if (c2 == 'c' || c2 == 'C' || c2 == 3) { rendered_page_free(rp); doc_free(doc); endwin(); exit(0); }
            break;
        }
        case KEY_RESIZE:
            break;
        default:
            break;
        }
    }

out:
    *scroll_io = scroll;
    rendered_page_free(rp);
    doc_free(doc);
    return result;
}

/* ---------------- search / browse list ---------------- */

/* Persists across repeated visits to the same browse list (filter text,
 * selection, scroll) so that backing out of an article with 'h' returns to
 * exactly where the list was left, instead of resetting it. */
typedef struct { char filter[256]; int sel, top; } ListState;

/* Articles whose rendered body has no visible content (e.g. pages that are
 * only an infobox template with no prose, like "A'salom") are hidden from
 * the browse list. Checking this means fully parsing and word-wrapping
 * every article, which is too slow to redo on every launch, so the result
 * is cached to <pages_dir>/_empty.txt (one title per line) after the first
 * (slow) run; later runs just read the cache back in. */
static int *compute_empty_flags(WikiIndex *idx) {
    int *flags = calloc((size_t)idx->n_articles, sizeof(int));

    char cache_path[4096];
    snprintf(cache_path, sizeof cache_path, "%s/_empty.txt", idx->pages_dir);

    FILE *cf = fopen(cache_path, "r");
    if (cf) {
        char line[1024];
        while (fgets(line, sizeof line, cf)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
            if (!line[0]) continue;
            int i = wiki_index_find(idx, line);
            if (i >= 0) flags[i] = 1;
        }
        fclose(cf);
        return flags;
    }

    erase();
    mvprintw(0, 0, "Indexing articles (first run only)...");
    refresh();

    for (int i = 0; i < idx->n_articles; i++) {
        char *raw = wiki_index_read(idx, i);
        if (!raw) { flags[i] = 1; continue; }
        Doc *doc = wikitext_parse(idx->articles[i].title, raw, idx);
        free(raw);
        RenderedPage *rp = doc_wrap(doc, 80);
        if (rp->n_spans == 0) flags[i] = 1;
        rendered_page_free(rp);
        doc_free(doc);
    }

    cf = fopen(cache_path, "w");
    if (cf) {
        for (int i = 0; i < idx->n_articles; i++) {
            if (flags[i]) fprintf(cf, "%s\n", idx->articles[i].title);
        }
        fclose(cf);
    }

    return flags;
}

static int list_view(WikiIndex *idx, const int *is_empty, ListState *st) {
    int *matches = malloc((size_t)idx->n_articles * sizeof(int));
    int n_matches = 0;
    int picked = -1;

    for (;;) {
        n_matches = 0;
        for (int i = 0; i < idx->n_articles; i++) {
            if (is_empty[i]) continue;
            if (str_ci_contains(idx->articles[i].title, st->filter)) matches[n_matches++] = i;
        }
        if (st->sel >= n_matches) st->sel = n_matches > 0 ? n_matches - 1 : 0;
        if (st->sel < 0) st->sel = 0;

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int list_h = rows - 3;
        if (list_h < 1) list_h = 1;
        if (st->sel < st->top) st->top = st->sel;
        if (st->sel >= st->top + list_h) st->top = st->sel - list_h + 1;
        if (st->top < 0) st->top = 0;

        erase();
        attron(A_BOLD);
        attron(COLOR_PAIR(CP_STATUS));
        mvprintw(0, 0, "%-*.*s", cols, cols, "");
        mvprintw(0, 0, "NetHackWiki -- search: %s", st->filter);
        attroff(COLOR_PAIR(CP_STATUS));
        attroff(A_BOLD);
        mvhline(1, 0, '-', cols);

        for (int row = 0; row < list_h; row++) {
            int mi = st->top + row;
            if (mi >= n_matches) break;
            int art = matches[mi];
            if (mi == st->sel) attron(COLOR_PAIR(CP_SELROW) | A_BOLD);
            mvprintw(2 + row, 0, "%-*.*s", cols, cols, idx->articles[art].title);
            if (mi == st->sel) attroff(COLOR_PAIR(CP_SELROW) | A_BOLD);
        }

        char status[256];
        snprintf(status, sizeof status, "%d/%d articles  [type to filter] [Up/Down] [Enter open] [Esc quit]",
                  n_matches, idx->n_articles);
        attron(COLOR_PAIR(CP_STATUS));
        mvprintw(rows - 1, 0, "%-*.*s", cols, cols, status);
        attroff(COLOR_PAIR(CP_STATUS));

        refresh();

        int ch = getch();
        if (ch == 27) { /* Esc, or Alt-v/Alt-</Alt-> (ESC v / ESC < / ESC >) */
            int c2 = wait_second_key();
            if (c2 == 'v' || c2 == 'V') { st->sel -= list_h; if (st->sel < 0) st->sel = 0; continue; }
            if (c2 == '<') { st->sel = 0; continue; }
            if (c2 == '>') { st->sel = n_matches > 0 ? n_matches - 1 : 0; continue; }
            picked = -1; break;
        }
        if (ch == 24) { /* Ctrl-X: start of the Ctrl-X C "exit program" chord */
            int c2 = wait_second_key();
            if (c2 == 'c' || c2 == 'C' || c2 == 3) { free(matches); endwin(); exit(0); }
            continue;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (n_matches > 0) picked = matches[st->sel];
            break;
        }
        if (ch == KEY_UP || ch == 16) { if (st->sel > 0) st->sel--; } /* Up / Ctrl-P: previous line */
        else if (ch == KEY_DOWN || ch == 14) { if (st->sel < n_matches - 1) st->sel++; } /* Down / Ctrl-N: next line */
        else if (ch == KEY_NPAGE || ch == 22) { st->sel += list_h; if (st->sel > n_matches - 1) st->sel = n_matches > 0 ? n_matches - 1 : 0; } /* PgDn / Ctrl-V: next page */
        else if (ch == KEY_PPAGE) { st->sel -= list_h; if (st->sel < 0) st->sel = 0; }
        else if (ch == KEY_HOME) st->sel = 0;
        else if (ch == KEY_END) st->sel = n_matches > 0 ? n_matches - 1 : 0;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t l = strlen(st->filter);
            if (l > 0) st->filter[l - 1] = '\0';
            st->sel = 0; st->top = 0;
        } else if (ch == 21) { /* Ctrl-U */
            st->filter[0] = '\0'; st->sel = 0; st->top = 0;
        } else if (ch == KEY_RESIZE) {
            /* redraw next loop */
        } else if (ch >= 32 && ch < 127) {
            size_t l = strlen(st->filter);
            if (l < sizeof(st->filter) - 1) { st->filter[l] = (char)ch; st->filter[l + 1] = '\0'; st->sel = 0; st->top = 0; }
        }
    }

    free(matches);
    return picked;
}

/* ---------------- top-level app loop ---------------- */

void run_app(WikiIndex *idx, const char *initial_filter) {
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    /* Without this, the tty driver eats Ctrl-S/Ctrl-Q as XOFF/XON software
     * flow control before ncurses ever sees them, so Ctrl-S as a keybinding
     * would just freeze terminal output instead of doing anything. */
    struct termios term;
    if (tcgetattr(STDIN_FILENO, &term) == 0) {
        term.c_iflag &= ~(tcflag_t)IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    }

    int *is_empty = compute_empty_flags(idx);

    int cap = 64, n = 0;
    HistEntry *hist = malloc((size_t)cap * sizeof(HistEntry));

    ListState lst = { .sel = 0, .top = 0 };
    strncpy(lst.filter, initial_filter ? initial_filter : "", sizeof(lst.filter) - 1);
    lst.filter[sizeof(lst.filter) - 1] = '\0';

    /* Open straight on the "Featured" page (the front-page featured
     * article, kept up to date by scripts/fetch_featured_article.sh) when
     * launched with no arguments -- but not when the user passed an
     * explicit search filter on the command line, since that's a clear
     * request to land on the browse list instead. Falls through to the
     * ordinary list_view() if that page hasn't been fetched yet (a fresh
     * checkout before the script has ever been run), so a missing
     * Featured.wiki degrades to the previous default behavior rather than
     * failing outright. */
    int current = -1;
    if (!lst.filter[0]) current = wiki_index_find(idx, "Featured");
    if (current < 0) current = list_view(idx, is_empty, &lst);
    while (current >= 0) {
        int scroll = 0;
        for (;;) {
            int action = article_view(idx, current, &scroll);

            if (action == AV_QUIT) { current = -1; break; }

            if (action == AV_BACK) {
                if (n > 0) {
                    HistEntry hprev = hist[--n];
                    current = hprev.article_index;
                    scroll = hprev.scroll;
                    continue;
                }
                current = -2; /* sentinel: fall through to list */
                break;
            }

            if (action == AV_SEARCH) {
                if (n == cap) { cap *= 2; hist = realloc(hist, (size_t)cap * sizeof(HistEntry)); }
                hist[n++] = (HistEntry){ current, scroll };
                ListState search_state = { .sel = 0, .top = 0 };
                int picked = list_view(idx, is_empty, &search_state);
                if (picked < 0) {
                    HistEntry hprev = hist[--n];
                    current = hprev.article_index;
                    scroll = hprev.scroll;
                    continue;
                }
                current = picked;
                scroll = 0;
                continue;
            }

            /* action >= 0: a resolved local link was followed */
            if (n == cap) { cap *= 2; hist = realloc(hist, (size_t)cap * sizeof(HistEntry)); }
            hist[n++] = (HistEntry){ current, scroll };
            current = action;
            scroll = 0;
        }

        /* sentinel: back out to the same browse-list state the user left */
        if (current == -2) current = list_view(idx, is_empty, &lst);
    }

    free(is_empty);
    free(hist);
    endwin();
}
