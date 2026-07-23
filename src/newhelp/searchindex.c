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
 *
 * Search index builder for newhelp -S mode.
 * Reads parsed help text entries and produces a binary search index file.
 */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "pmapi.h"
#include "libpcp.h"
#include "searchindex.h"

/* in-memory document during index building */
typedef struct {
    char	*name;
    char	*oneline;
    char	*helptext;
    char	*indom;
    uint8_t	type;		/* SEARCH_DOC_METRIC/INDOM/INST */
} search_build_doc_t;

/* in-memory term → postings during index building */
typedef struct {
    char		*term;
    search_posting_t	*postings;
    int			npostings;
    int			maxpostings;
} search_build_term_t;

static search_build_doc_t	*docs;
static int			ndocs;
static int			maxdocs;

static search_build_term_t	*terms;
static int			nterms;
static int			maxterms;

static const char * const stopwords[] = {
    "a", "an", "and", "are", "as", "at", "be", "but", "by", "for",
    "if", "in", "into", "is", "it", "no", "not", "of", "on", "or",
    "such", "that", "the", "their", "then", "there", "these", "they",
    "this", "to", "was", "will", "with",
};
static const int nstopwords = sizeof(stopwords) / sizeof(stopwords[0]);

static const char *delimiters = ",.<>{}[]\"':;!@#$%^&*()-+=~/";

static int
is_stopword(const char *word)
{
    int lo = 0, hi = nstopwords - 1;

    while (lo <= hi) {
	int mid = (lo + hi) / 2;
	int cmp = strcmp(word, stopwords[mid]);
	if (cmp == 0)
	    return 1;
	if (cmp < 0)
	    hi = mid - 1;
	else
	    lo = mid + 1;
    }
    return 0;
}

static search_build_term_t *
find_or_add_term(const char *word)
{
    int		lo = 0, hi = nterms - 1, mid, cmp;

    while (lo <= hi) {
	mid = (lo + hi) / 2;
	cmp = strcmp(word, terms[mid].term);
	if (cmp == 0)
	    return &terms[mid];
	if (cmp < 0)
	    hi = mid - 1;
	else
	    lo = mid + 1;
    }

    /* not found - insert at position lo */
    if (nterms >= maxterms) {
	maxterms = maxterms ? maxterms * 2 : 256;
	terms = realloc(terms, maxterms * sizeof(terms[0]));
	if (terms == NULL)
	    pmNoMem("find_or_add_term", maxterms * sizeof(terms[0]), PM_FATAL_ERR);
    }

    if (lo < nterms)
	memmove(&terms[lo + 1], &terms[lo],
		(nterms - lo) * sizeof(terms[0]));

    memset(&terms[lo], 0, sizeof(terms[0]));
    terms[lo].term = strdup(word);
    if (terms[lo].term == NULL)
	pmNoMem("find_or_add_term strdup", strlen(word) + 1, PM_FATAL_ERR);
    nterms++;

    return &terms[lo];
}

static void
add_posting(search_build_term_t *term, uint32_t doc_id,
	    uint8_t field_mask, uint8_t field)
{
    int		i;

    /* look for existing posting for this doc */
    for (i = 0; i < term->npostings; i++) {
	if (term->postings[i].doc_id == doc_id) {
	    term->postings[i].field_mask |= field_mask;
	    switch (field) {
	    case SEARCH_FIELD_NAME:
		if (term->postings[i].tf_name < 255)
		    term->postings[i].tf_name++;
		break;
	    case SEARCH_FIELD_ONELINE:
		if (term->postings[i].tf_oneline < 255)
		    term->postings[i].tf_oneline++;
		break;
	    case SEARCH_FIELD_HELPTEXT:
		if (term->postings[i].tf_helptext < 255)
		    term->postings[i].tf_helptext++;
		break;
	    }
	    return;
	}
    }

    /* new posting */
    if (term->npostings >= term->maxpostings) {
	term->maxpostings = term->maxpostings ? term->maxpostings * 2 : 4;
	term->postings = realloc(term->postings,
				 term->maxpostings * sizeof(term->postings[0]));
	if (term->postings == NULL)
	    pmNoMem("add_posting", term->maxpostings * sizeof(term->postings[0]),
		    PM_FATAL_ERR);
    }

    memset(&term->postings[term->npostings], 0, sizeof(search_posting_t));
    term->postings[term->npostings].doc_id = doc_id;
    term->postings[term->npostings].field_mask = field_mask;
    switch (field) {
    case SEARCH_FIELD_NAME:
	term->postings[term->npostings].tf_name = 1;
	break;
    case SEARCH_FIELD_ONELINE:
	term->postings[term->npostings].tf_oneline = 1;
	break;
    case SEARCH_FIELD_HELPTEXT:
	term->postings[term->npostings].tf_helptext = 1;
	break;
    }
    term->npostings++;
}

/*
 * Tokenize text and add to the inverted index.
 * Lowercases, splits on delimiters, removes stopwords,
 * strips trailing 's' for basic plural stemming.
 */
static void
index_text(const char *text, uint32_t doc_id, uint8_t field)
{
    char		*copy, *p, *token;
    char		word[256];
    size_t		len;
    search_build_term_t	*term;

    if (text == NULL || *text == '\0')
	return;

    copy = strdup(text);
    if (copy == NULL)
	pmNoMem("index_text", strlen(text) + 1, PM_FATAL_ERR);

    /* replace delimiters with spaces */
    for (p = copy; *p; p++) {
	if (strchr(delimiters, *p) != NULL)
	    *p = ' ';
    }

    /* tokenize on whitespace */
    token = strtok(copy, " \t\n\r");
    while (token != NULL) {
	/* lowercase */
	len = strlen(token);
	if (len == 0 || len >= sizeof(word)) {
	    token = strtok(NULL, " \t\n\r");
	    continue;
	}
	for (p = token; *p; p++)
	    *p = tolower((unsigned char)*p);

	/* basic plural stemming - strip trailing 's' */
	if (len > 2 && token[len - 1] == 's')
	    token[--len] = '\0';

	/* skip stopwords */
	if (is_stopword(token)) {
	    token = strtok(NULL, " \t\n\r");
	    continue;
	}

	/* skip single-character tokens */
	if (len < 2) {
	    token = strtok(NULL, " \t\n\r");
	    continue;
	}

	term = find_or_add_term(token);
	add_posting(term, doc_id, field, field);

	token = strtok(NULL, " \t\n\r");
    }

    free(copy);
}

void
search_index_add_doc(const char *name, const char *oneline,
		     const char *helptext, const char *indom, uint8_t type)
{
    search_build_doc_t	*doc;
    uint32_t		doc_id;

    if (ndocs >= maxdocs) {
	maxdocs = maxdocs ? maxdocs * 2 : 256;
	docs = realloc(docs, maxdocs * sizeof(docs[0]));
	if (docs == NULL)
	    pmNoMem("search_index_add_doc", maxdocs * sizeof(docs[0]), PM_FATAL_ERR);
    }

    doc = &docs[ndocs];
    doc_id = ndocs;
    ndocs++;

    doc->name = strdup(name);
    doc->oneline = (oneline && *oneline) ? strdup(oneline) : NULL;
    doc->helptext = (helptext && *helptext) ? strdup(helptext) : NULL;
    doc->indom = (indom && *indom) ? strdup(indom) : NULL;
    doc->type = type;

    /* index each field */
    index_text(name, doc_id, SEARCH_FIELD_NAME);
    index_text(oneline, doc_id, SEARCH_FIELD_ONELINE);
    index_text(helptext, doc_id, SEARCH_FIELD_HELPTEXT);
}

static uint32_t _add_string(char **, uint32_t *, uint32_t *, const char *);

int
search_index_write(const char *path)
{
    FILE			*fp;
    search_index_header_t	hdr;
    uint32_t			total_postings = 0;
    uint32_t			post_offset = 0;
    char			*strings = NULL;
    uint32_t			strings_len = 0;
    uint32_t			strings_max = 0;
    search_doc_t		*on_disk_docs;
    search_term_t		*on_disk_terms;
    search_posting_t		*on_disk_postings;
    int				i, j;

    #define ADD_STRING(s) _add_string(&strings, &strings_len, &strings_max, (s))

    /* count total postings */
    for (i = 0; i < nterms; i++)
	total_postings += terms[i].npostings;

    /* build string pool and on-disk document table */
    on_disk_docs = calloc(ndocs, sizeof(search_doc_t));
    if (on_disk_docs == NULL)
	pmNoMem("search_index_write docs", ndocs * sizeof(search_doc_t), PM_FATAL_ERR);

    for (i = 0; i < ndocs; i++) {
	on_disk_docs[i].name_off = ADD_STRING(docs[i].name);
	on_disk_docs[i].oneline_off = docs[i].oneline ? ADD_STRING(docs[i].oneline) : 0;
	on_disk_docs[i].helptext_off = docs[i].helptext ? ADD_STRING(docs[i].helptext) : 0;
	on_disk_docs[i].indom_off = docs[i].indom ? ADD_STRING(docs[i].indom) : 0;
	on_disk_docs[i].type = docs[i].type;
    }

    /* build on-disk term dictionary and postings pool */
    on_disk_terms = calloc(nterms, sizeof(search_term_t));
    on_disk_postings = calloc(total_postings, sizeof(search_posting_t));
    if (on_disk_terms == NULL || on_disk_postings == NULL)
	pmNoMem("search_index_write terms", nterms * sizeof(search_term_t), PM_FATAL_ERR);

    post_offset = 0;
    for (i = 0; i < nterms; i++) {
	on_disk_terms[i].term_off = ADD_STRING(terms[i].term);
	on_disk_terms[i].postings_off = post_offset;
	on_disk_terms[i].npostings = terms[i].npostings;

	/* count distinct docs for this term */
	on_disk_terms[i].doc_freq = terms[i].npostings;

	for (j = 0; j < terms[i].npostings; j++)
	    on_disk_postings[post_offset++] = terms[i].postings[j];
    }

    /* write the file */
    if ((fp = fopen(path, "wb")) == NULL) {
	fprintf(stderr, "%s: cannot create search index %s: %s\n",
		pmGetProgname(), path, osstrerror());
	return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SEARCH_INDEX_MAGIC;
    hdr.version = SEARCH_INDEX_VERSION;
    hdr.ndocs = ndocs;
    hdr.nterms = nterms;
    hdr.npostings = total_postings;
    hdr.strings_len = strings_len;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 ||
	fwrite(on_disk_docs, sizeof(search_doc_t), ndocs, fp) != ndocs ||
	fwrite(strings, 1, strings_len, fp) != strings_len ||
	fwrite(on_disk_terms, sizeof(search_term_t), nterms, fp) != nterms ||
	fwrite(on_disk_postings, sizeof(search_posting_t), total_postings, fp) != total_postings) {
	fprintf(stderr, "%s: write failed for search index %s: %s\n",
		pmGetProgname(), path, osstrerror());
	fclose(fp);
	free(on_disk_docs);
	free(on_disk_terms);
	free(on_disk_postings);
	free(strings);
	return -1;
    }

    fclose(fp);
    free(on_disk_docs);
    free(on_disk_terms);
    free(on_disk_postings);
    free(strings);
    return 0;

    #undef ADD_STRING
}

void
search_index_build_free(void)
{
    int		i;

    for (i = 0; i < ndocs; i++) {
	free(docs[i].name);
	free(docs[i].oneline);
	free(docs[i].helptext);
	free(docs[i].indom);
    }
    free(docs);
    docs = NULL;
    ndocs = maxdocs = 0;

    for (i = 0; i < nterms; i++) {
	free(terms[i].term);
	free(terms[i].postings);
    }
    free(terms);
    terms = NULL;
    nterms = maxterms = 0;
}

static uint32_t
_add_string(char **pool, uint32_t *len, uint32_t *max, const char *s)
{
    uint32_t	offset;
    size_t	slen;

    if (s == NULL || *s == '\0') {
	/* empty string at offset 0 - ensure pool starts with a null byte */
	if (*len == 0) {
	    if (*max == 0) {
		*max = 4096;
		*pool = malloc(*max);
		if (*pool == NULL)
		    pmNoMem("_add_string", *max, PM_FATAL_ERR);
	    }
	    (*pool)[0] = '\0';
	    *len = 1;
	}
	return 0;
    }

    slen = strlen(s) + 1;
    if (*len + slen > *max) {
	while (*len + slen > *max)
	    *max = *max ? *max * 2 : 4096;
	*pool = realloc(*pool, *max);
	if (*pool == NULL)
	    pmNoMem("_add_string realloc", *max, PM_FATAL_ERR);
    }

    /* ensure offset 0 is a null byte */
    if (*len == 0) {
	(*pool)[0] = '\0';
	*len = 1;
    }

    offset = *len;
    memcpy(*pool + offset, s, slen);
    *len += slen;
    return offset;
}
