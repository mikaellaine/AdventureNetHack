#ifndef UI_H
#define UI_H

#include "index.h"

/* Runs the ncurses UI. `initial_filter` (may be "") pre-fills the search
 * box shown on startup. Blocks until the user quits. */
void run_app(WikiIndex *idx, const char *initial_filter);

#endif
