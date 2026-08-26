#ifndef WIKITEXT_H
#define WIKITEXT_H

#include "doc.h"
#include "index.h"

/* Parses raw MediaWiki-style wikitext (as extracted by
 * scripts/02_extract_pages.py) into a Doc. Internal [[links]] are resolved
 * against `idx` at parse time -- only local pages can ever become
 * navigable links; everything else (external URLs, bracket links,
 * unresolved links) is rendered as inert text. */
Doc *wikitext_parse(const char *title, const char *raw, const WikiIndex *idx);

#endif
