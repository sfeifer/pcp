/*
 * Copyright (c) 2026 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#ifndef NEWHELP_SEARCHINDEX_H
#define NEWHELP_SEARCHINDEX_H

/*
 * Include the shared on-disk format definitions from libpcp_web.
 * The search index file format is defined there since both the
 * writer (newhelp) and reader (libpcp_web search engine) need it.
 */
#include "../../libpcp_web/src/searchindex.h"

extern void search_index_add_doc(const char *, const char *,
				 const char *, const char *, uint8_t);
extern int search_index_write(const char *);
extern void search_index_build_free(void);

#endif	/* NEWHELP_SEARCHINDEX_H */
