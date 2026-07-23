/*
 * Copyright (c) 2026 Red Hat.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#ifndef SEARCH_INDEX_H
#define SEARCH_INDEX_H

#include <stdint.h>

/*
 * PCP Search Index - on-disk binary format
 *
 * A search index file contains an inverted index over PCP metric names,
 * instance domain names, oneline help text, and long help text.  It is
 * built at package build time by newhelp(1) and read at search time by
 * the libpcp_web search engine.
 *
 * File layout:
 *   [header]
 *   [document table - ndocs entries]
 *   [string pool - null-terminated strings referenced by offsets]
 *   [term dictionary - nterms entries, sorted by term string]
 *   [postings pool - (doc_id, field_mask, term_freq) tuples]
 */

#define SEARCH_INDEX_MAGIC	0x50435053	/* "PCPS" */
#define SEARCH_INDEX_VERSION	1

/* field identifiers used in field_mask bitmask */
#define SEARCH_FIELD_NAME	(1 << 0)
#define SEARCH_FIELD_ONELINE	(1 << 1)
#define SEARCH_FIELD_HELPTEXT	(1 << 2)

/* field weights for TF-IDF scoring */
#define SEARCH_WEIGHT_NAME	9
#define SEARCH_WEIGHT_ONELINE	4
#define SEARCH_WEIGHT_HELPTEXT	2

/* document type values - matches pmSearchTextType in pmwebapi.h */
#define SEARCH_DOC_METRIC	1
#define SEARCH_DOC_INDOM	2
#define SEARCH_DOC_INST		3

typedef struct {
    uint32_t	magic;		/* SEARCH_INDEX_MAGIC */
    uint32_t	version;	/* SEARCH_INDEX_VERSION */
    uint32_t	ndocs;		/* number of documents */
    uint32_t	nterms;		/* number of unique terms */
    uint32_t	npostings;	/* total number of postings entries */
    uint32_t	strings_len;	/* total size of string pool in bytes */
} search_index_header_t;

typedef struct {
    uint32_t	name_off;	/* offset into string pool */
    uint32_t	oneline_off;	/* offset into string pool (0 = none) */
    uint32_t	helptext_off;	/* offset into string pool (0 = none) */
    uint32_t	indom_off;	/* offset into string pool (0 = none) */
    uint8_t	type;		/* SEARCH_DOC_METRIC/INDOM/INST */
    uint8_t	pad[3];
} search_doc_t;

typedef struct {
    uint32_t	term_off;	/* offset into string pool */
    uint32_t	postings_off;	/* index into postings pool */
    uint32_t	npostings;	/* number of postings for this term */
    uint32_t	doc_freq;	/* number of distinct docs containing term */
} search_term_t;

typedef struct {
    uint32_t	doc_id;		/* index into document table */
    uint8_t	field_mask;	/* which fields contain this term */
    uint8_t	tf_name;	/* term frequency in name field */
    uint8_t	tf_oneline;	/* term frequency in oneline field */
    uint8_t	tf_helptext;	/* term frequency in helptext field */
} search_posting_t;

/*
 * In-memory search index - built by reading the on-disk format.
 * Strings are pointers into the loaded string pool.
 */
typedef struct {
    uint32_t		ndocs;
    uint32_t		nterms;
    uint32_t		npostings;
    search_doc_t	*docs;		/* document table */
    search_term_t	*terms;		/* term dictionary (sorted) */
    search_posting_t	*postings;	/* postings pool */
    char		*strings;	/* string pool */
    uint32_t		strings_len;
} search_index_t;

extern int search_index_load(search_index_t *, const char *);
extern void search_index_free(search_index_t *);

#endif	/* SEARCH_INDEX_H */
