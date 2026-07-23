/*
 * Copyright (c) 2020-2022,2024,2026 Red Hat.
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
 *
 * Local search engine using binary index files built by newhelp -S.
 * Replaces the previous RediSearch (FT.*) backend.
 */
#include <math.h>
#include <ctype.h>
#include "pmapi.h"
#include "libpcp.h"
#include "search.h"
#include "searchindex.h"

typedef struct searchModuleData {
    search_index_t	index;
    struct dict		*config;
    unsigned int	loaded;
    unsigned int	resultcount;
} searchModuleData;

static int		search_enabled;
static unsigned int	default_resultcount = 10;

/* --- index file load/free --- */

int
search_index_load(search_index_t *idx, const char *path)
{
    FILE			*fp;
    search_index_header_t	hdr;

    memset(idx, 0, sizeof(*idx));

    if ((fp = fopen(path, "rb")) == NULL)
	return -oserror();

    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
	fclose(fp);
	return -EINVAL;
    }
    if (hdr.magic != SEARCH_INDEX_MAGIC || hdr.version != SEARCH_INDEX_VERSION) {
	fclose(fp);
	return -EINVAL;
    }

    idx->ndocs = hdr.ndocs;
    idx->nterms = hdr.nterms;
    idx->npostings = hdr.npostings;
    idx->strings_len = hdr.strings_len;

    idx->docs = calloc(hdr.ndocs, sizeof(search_doc_t));
    idx->strings = malloc(hdr.strings_len);
    idx->terms = calloc(hdr.nterms, sizeof(search_term_t));
    idx->postings = calloc(hdr.npostings, sizeof(search_posting_t));

    if (!idx->docs || !idx->strings || !idx->terms || !idx->postings) {
	search_index_free(idx);
	fclose(fp);
	return -ENOMEM;
    }

    if (fread(idx->docs, sizeof(search_doc_t), hdr.ndocs, fp) != hdr.ndocs ||
	fread(idx->strings, 1, hdr.strings_len, fp) != hdr.strings_len ||
	fread(idx->terms, sizeof(search_term_t), hdr.nterms, fp) != hdr.nterms ||
	fread(idx->postings, sizeof(search_posting_t), hdr.npostings, fp) != hdr.npostings) {
	search_index_free(idx);
	fclose(fp);
	return -EINVAL;
    }

    fclose(fp);
    return 0;
}

void
search_index_free(search_index_t *idx)
{
    free(idx->docs);
    free(idx->strings);
    free(idx->terms);
    free(idx->postings);
    memset(idx, 0, sizeof(*idx));
}

/* --- tokenizer (mirrors build-time tokenizer in newhelp/searchindex.c) --- */

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
    int		lo = 0, hi = nstopwords - 1;

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

/*
 * Tokenize a query string: lowercase, split on delimiters,
 * remove stopwords, strip trailing 's'.
 * Returns a dynamically allocated array of tokens.
 */
static char **
search_tokenize(const char *text, int *ntokens)
{
    char	*copy, *p, *tok;
    char	**tokens = NULL;
    int		count = 0, max = 0;
    size_t	len;
    char	word[256];

    *ntokens = 0;
    if (text == NULL || *text == '\0')
	return NULL;

    copy = strdup(text);
    if (copy == NULL)
	return NULL;

    for (p = copy; *p; p++) {
	if (strchr(delimiters, *p) != NULL)
	    *p = ' ';
    }

    tok = strtok(copy, " \t\n\r");
    while (tok != NULL) {
	len = strlen(tok);
	if (len == 0 || len >= sizeof(word)) {
	    tok = strtok(NULL, " \t\n\r");
	    continue;
	}
	for (p = tok; *p; p++)
	    *p = tolower((unsigned char)*p);

	if (len > 2 && tok[len - 1] == 's')
	    tok[--len] = '\0';

	if (is_stopword(tok) || len < 2) {
	    tok = strtok(NULL, " \t\n\r");
	    continue;
	}

	if (count >= max) {
	    max = max ? max * 2 : 8;
	    tokens = realloc(tokens, max * sizeof(char *));
	    if (tokens == NULL) {
		free(copy);
		return NULL;
	    }
	}
	tokens[count] = strdup(tok);
	count++;

	tok = strtok(NULL, " \t\n\r");
    }

    free(copy);
    *ntokens = count;
    return tokens;
}

static void
free_tokens(char **tokens, int ntokens)
{
    int		i;

    if (tokens) {
	for (i = 0; i < ntokens; i++)
	    free(tokens[i]);
	free(tokens);
    }
}

/* --- term dictionary binary search --- */

static int
search_find_term(search_index_t *idx, const char *term)
{
    int		lo = 0, hi = (int)idx->nterms - 1;

    while (lo <= hi) {
	int mid = (lo + hi) / 2;
	const char *s = idx->strings + idx->terms[mid].term_off;
	int cmp = strcmp(term, s);
	if (cmp == 0)
	    return mid;
	if (cmp < 0)
	    hi = mid - 1;
	else
	    lo = mid + 1;
    }
    return -1;
}

/*
 * Find the insertion point for prefix searching.
 * Returns the index of the first term >= prefix.
 */
static int
search_find_term_lower(search_index_t *idx, const char *prefix)
{
    int		lo = 0, hi = (int)idx->nterms;

    while (lo < hi) {
	int mid = (lo + hi) / 2;
	const char *s = idx->strings + idx->terms[mid].term_off;
	if (strcmp(s, prefix) < 0)
	    lo = mid + 1;
	else
	    hi = mid;
    }
    return lo;
}

/* --- scoring --- */

typedef struct {
    uint32_t	doc_id;
    double	score;
} search_hit_t;

static int
hit_score_cmp(const void *a, const void *b)
{
    const search_hit_t	*ha = a, *hb = b;
    if (hb->score > ha->score) return 1;
    if (hb->score < ha->score) return -1;
    return 0;
}

/*
 * Score a single posting against a query term.
 * Uses weighted TF-IDF: sum over fields of tf * idf * field_weight.
 */
static double
score_posting(search_posting_t *post, uint32_t ndocs, uint32_t doc_freq,
	      int use_name, int use_oneline, int use_helptext)
{
    double	idf, score = 0.0;

    idf = log(1.0 + (double)ndocs / (double)(doc_freq > 0 ? doc_freq : 1));

    if (use_name && (post->field_mask & SEARCH_FIELD_NAME))
	score += (double)post->tf_name * idf * SEARCH_WEIGHT_NAME;
    if (use_oneline && (post->field_mask & SEARCH_FIELD_ONELINE))
	score += (double)post->tf_oneline * idf * SEARCH_WEIGHT_ONELINE;
    if (use_helptext && (post->field_mask & SEARCH_FIELD_HELPTEXT))
	score += (double)post->tf_helptext * idf * SEARCH_WEIGHT_HELPTEXT;

    return score;
}

/* --- highlight helper --- */

static sds
highlight_text(const char *text, char **tokens, int ntokens)
{
    sds		result;
    char	lower_buf[256];
    const char	*p;
    int		i, matched;
    size_t	word_start, len;

    if (text == NULL || *text == '\0')
	return sdsnew("");

    result = sdsempty();
    p = text;
    while (*p) {
	if (isalnum((unsigned char)*p)) {
	    word_start = sdslen(result);
	    len = 0;
	    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
		if (len < sizeof(lower_buf) - 1)
		    lower_buf[len] = tolower((unsigned char)*p);
		result = sdscatlen(result, p, 1);
		p++;
		len++;
	    }
	    lower_buf[len > sizeof(lower_buf) - 1 ? sizeof(lower_buf) - 1 : len] = '\0';

	    /* strip trailing 's' for matching */
	    if (len > 2 && lower_buf[len - 1] == 's')
		lower_buf[len - 1] = '\0';

	    matched = 0;
	    for (i = 0; i < ntokens; i++) {
		if (strcmp(lower_buf, tokens[i]) == 0) {
		    matched = 1;
		    break;
		}
	    }
	    if (matched) {
		sds word = sdsnewlen(result + word_start, sdslen(result) - word_start);
		sdsrange(result, 0, word_start - 1);
		result = sdscatfmt(result, "<b>%S</b>", word);
		sdsfree(word);
	    }
	} else {
	    result = sdscatlen(result, p, 1);
	    p++;
	}
    }

    return result;
}

/* --- docid helper --- */

static sds
make_docid(search_index_t *idx, uint32_t doc_id)
{
    const char	*name = idx->strings + idx->docs[doc_id].name_off;
    char	buf[32];

    pmsprintf(buf, sizeof(buf), "%u", doc_id);
    return sdscatfmt(sdsempty(), "%s:%s", buf, name);
}

/* --- type string helper --- */

const char *
pmSearchTextTypeStr(pmSearchTextType type)
{
    switch (type) {
    case PM_SEARCH_TYPE_UNKNOWN:
	return "unknown";
    case PM_SEARCH_TYPE_METRIC:
	return "metric";
    case PM_SEARCH_TYPE_INDOM:
	return "indom";
    case PM_SEARCH_TYPE_INST:
	return "instance";
    }
    return "unknown";
}

/* --- get search module data --- */

static searchModuleData *
getSearchModuleData(pmSearchModule *module)
{
    if (module->privdata == NULL)
	module->privdata = calloc(1, sizeof(searchModuleData));
    return (searchModuleData *)module->privdata;
}

/* --- query implementations --- */

static void
search_do_text_query(searchModuleData *smd, pmSearchTextRequest *request,
		     pmSearchCallBacks *callbacks, void *userdata)
{
    search_index_t	*idx = &smd->index;
    pmSearchTextResult	result;
    struct timespec	started, finished;
    search_hit_t	*hits = NULL;
    int			nhits = 0, maxhits = 0;
    char		**tokens;
    int			ntokens, t, i, j;
    int			use_name, use_oneline, use_helptext;
    unsigned int	count, offset;
    double		timer;
    uint8_t		doc_type;

    pmtimespecNow(&started);

    tokens = search_tokenize(request->query, &ntokens);
    if (ntokens == 0) {
	free_tokens(tokens, ntokens);
	callbacks->on_done(0, userdata);
	return;
    }

    /* determine which fields to search */
    use_name = request->infields_name;
    use_oneline = request->infields_oneline;
    use_helptext = request->infields_helptext;
    if (!use_name && !use_oneline && !use_helptext) {
	use_name = use_oneline = use_helptext = 1;
    }

    /* allocate hits array - one per doc, zeroed */
    maxhits = idx->ndocs;
    hits = calloc(maxhits, sizeof(search_hit_t));
    if (hits == NULL) {
	free_tokens(tokens, ntokens);
	callbacks->on_done(-ENOMEM, userdata);
	return;
    }
    for (i = 0; i < maxhits; i++)
	hits[i].doc_id = i;

    /* score documents for each query term */
    for (t = 0; t < ntokens; t++) {
	int term_idx = search_find_term(idx, tokens[t]);
	if (term_idx < 0)
	    continue;

	search_term_t *term = &idx->terms[term_idx];
	for (j = 0; j < (int)term->npostings; j++) {
	    search_posting_t *post = &idx->postings[term->postings_off + j];
	    hits[post->doc_id].score += score_posting(post, idx->ndocs,
				term->doc_freq, use_name, use_oneline, use_helptext);
	}
    }

    /* filter by type and collect non-zero scoring hits */
    nhits = 0;
    for (i = 0; i < maxhits; i++) {
	if (hits[i].score <= 0.0)
	    continue;
	doc_type = idx->docs[i].type;
	if (request->type_metric || request->type_indom || request->type_inst) {
	    if (request->type_metric && doc_type == SEARCH_DOC_METRIC)
		;
	    else if (request->type_indom && doc_type == SEARCH_DOC_INDOM)
		;
	    else if (request->type_inst && doc_type == SEARCH_DOC_INST)
		;
	    else
		continue;
	}
	hits[nhits] = hits[i];
	nhits++;
    }

    qsort(hits, nhits, sizeof(search_hit_t), hit_score_cmp);

    pmtimespecNow(&finished);
    timer = pmtimespecSub(&finished, &started);

    /* pagination */
    offset = request->offset;
    count = request->count ? request->count : smd->resultcount;

    /* determine which fields to return */
    if (!request->return_name && !request->return_indom &&
	!request->return_oneline && !request->return_helptext &&
	!request->return_type) {
	request->return_name = 1;
	request->return_indom = 1;
	request->return_oneline = 1;
	request->return_helptext = 1;
	request->return_type = 1;
    }

    for (i = offset; i < nhits && i < (int)(offset + count); i++) {
	uint32_t did = hits[i].doc_id;
	search_doc_t *doc = &idx->docs[did];

	memset(&result, 0, sizeof(result));
	result.total = nhits;
	result.count = (i - offset) + 1;
	result.timer = timer;
	result.score = hits[i].score;
	result.docid = make_docid(idx, did);

	if (request->return_type)
	    result.type = doc->type;

	if (request->return_name) {
	    const char *name = idx->strings + doc->name_off;
	    if (request->highlight_name)
		result.name = highlight_text(name, tokens, ntokens);
	    else
		result.name = sdsnew(name);
	}
	if (request->return_indom && doc->indom_off)
	    result.indom = sdsnew(idx->strings + doc->indom_off);
	if (request->return_oneline && doc->oneline_off) {
	    const char *oneline = idx->strings + doc->oneline_off;
	    if (request->highlight_oneline)
		result.oneline = highlight_text(oneline, tokens, ntokens);
	    else
		result.oneline = sdsnew(oneline);
	}
	if (request->return_helptext && doc->helptext_off) {
	    const char *helptext = idx->strings + doc->helptext_off;
	    if (request->highlight_helptext)
		result.helptext = highlight_text(helptext, tokens, ntokens);
	    else
		result.helptext = sdsnew(helptext);
	}

	callbacks->on_text_result(&result, userdata);

	sdsfree(result.docid);
	sdsfree(result.name);
	sdsfree(result.indom);
	sdsfree(result.oneline);
	sdsfree(result.helptext);
    }

    free(hits);
    free_tokens(tokens, ntokens);
    callbacks->on_done(0, userdata);
}

static void
search_do_text_suggest(searchModuleData *smd, pmSearchTextRequest *request,
		       pmSearchCallBacks *callbacks, void *userdata)
{
    search_index_t	*idx = &smd->index;
    pmSearchTextResult	result;
    struct timespec	started, finished;
    search_hit_t	*hits = NULL;
    int			nhits = 0, maxhits = 0;
    char		**tokens;
    int			ntokens, t, i, j;
    unsigned int	count;
    double		timer;

    pmtimespecNow(&started);

    tokens = search_tokenize(request->query, &ntokens);
    if (ntokens == 0) {
	free_tokens(tokens, ntokens);
	callbacks->on_done(0, userdata);
	return;
    }

    maxhits = idx->ndocs;
    hits = calloc(maxhits, sizeof(search_hit_t));
    if (hits == NULL) {
	free_tokens(tokens, ntokens);
	callbacks->on_done(-ENOMEM, userdata);
	return;
    }
    for (i = 0; i < maxhits; i++)
	hits[i].doc_id = i;

    for (t = 0; t < ntokens; t++) {
	size_t tlen = strlen(tokens[t]);
	int start = search_find_term_lower(idx, tokens[t]);

	/* prefix matches on NAME field */
	for (i = start; i < (int)idx->nterms; i++) {
	    const char *s = idx->strings + idx->terms[i].term_off;
	    if (strncmp(s, tokens[t], tlen) != 0)
		break;
	    search_term_t *term = &idx->terms[i];
	    for (j = 0; j < (int)term->npostings; j++) {
		search_posting_t *post = &idx->postings[term->postings_off + j];
		if (!(post->field_mask & SEARCH_FIELD_NAME))
		    continue;
		double idf = log(1.0 + (double)idx->ndocs /
			    (double)(term->doc_freq > 0 ? term->doc_freq : 1));
		hits[post->doc_id].score +=
			(double)post->tf_name * idf * SEARCH_WEIGHT_NAME;
	    }
	}

	/* fuzzy matches (edit distance 1) on NAME field */
	for (i = 0; i < (int)idx->nterms; i++) {
	    const char *s = idx->strings + idx->terms[i].term_off;
	    size_t slen = strlen(s);
	    int diff;

	    if (slen > tlen + 1 || slen + 1 < tlen)
		continue;

	    /* simple edit distance 1 check */
	    diff = 0;
	    if (slen == tlen) {
		size_t k;
		for (k = 0; k < tlen; k++)
		    if (s[k] != tokens[t][k] && ++diff > 1)
			break;
	    } else {
		/* insertion or deletion */
		size_t si = 0, ti = 0;
		diff = 0;
		while (si < slen && ti < tlen) {
		    if (s[si] != tokens[t][ti]) {
			diff++;
			if (diff > 1) break;
			if (slen > tlen) si++;
			else ti++;
		    } else {
			si++; ti++;
		    }
		}
		diff += (slen - si) + (tlen - ti);
	    }

	    if (diff != 1)
		continue;

	    search_term_t *term = &idx->terms[i];
	    for (j = 0; j < (int)term->npostings; j++) {
		search_posting_t *post = &idx->postings[term->postings_off + j];
		if (!(post->field_mask & SEARCH_FIELD_NAME))
		    continue;
		double idf = log(1.0 + (double)idx->ndocs /
			    (double)(term->doc_freq > 0 ? term->doc_freq : 1));
		hits[post->doc_id].score +=
			(double)post->tf_name * idf * SEARCH_WEIGHT_NAME * 0.25;
	    }
	}
    }

    /* filter: only metrics and instances, non-zero score */
    nhits = 0;
    for (i = 0; i < maxhits; i++) {
	if (hits[i].score <= 0.0)
	    continue;
	if (idx->docs[i].type != SEARCH_DOC_METRIC &&
	    idx->docs[i].type != SEARCH_DOC_INST)
	    continue;
	hits[nhits] = hits[i];
	nhits++;
    }

    qsort(hits, nhits, sizeof(search_hit_t), hit_score_cmp);

    pmtimespecNow(&finished);
    timer = pmtimespecSub(&finished, &started);

    count = request->count ? request->count : smd->resultcount;

    for (i = 0; i < nhits && i < (int)count; i++) {
	uint32_t did = hits[i].doc_id;
	search_doc_t *doc = &idx->docs[did];

	memset(&result, 0, sizeof(result));
	result.total = nhits;
	result.count = i + 1;
	result.timer = timer;
	result.score = hits[i].score;
	result.type = doc->type;
	result.docid = make_docid(idx, did);
	result.name = sdsnew(idx->strings + doc->name_off);

	callbacks->on_text_result(&result, userdata);

	sdsfree(result.docid);
	sdsfree(result.name);
    }

    free(hits);
    free_tokens(tokens, ntokens);
    callbacks->on_done(0, userdata);
}

static void
search_do_text_indom(searchModuleData *smd, pmSearchTextRequest *request,
		     pmSearchCallBacks *callbacks, void *userdata)
{
    search_index_t	*idx = &smd->index;
    pmSearchTextResult	result;
    struct timespec	started, finished;
    search_hit_t	*hits = NULL;
    int			nhits = 0;
    unsigned int	i, count, offset;
    double		timer;

    pmtimespecNow(&started);

    hits = calloc(idx->ndocs, sizeof(search_hit_t));
    if (hits == NULL) {
	callbacks->on_done(-ENOMEM, userdata);
	return;
    }

    /* find documents matching the requested indom */
    for (i = 0; i < idx->ndocs; i++) {
	if (idx->docs[i].indom_off == 0)
	    continue;
	if (strcmp(idx->strings + idx->docs[i].indom_off, request->query) == 0) {
	    hits[nhits].doc_id = i;
	    hits[nhits].score = (idx->docs[i].type == SEARCH_DOC_INDOM) ? 2.0 :
				(idx->docs[i].type == SEARCH_DOC_METRIC) ? 1.0 : 0.5;
	    nhits++;
	}
    }

    /* sort by type (indom first via score) */
    qsort(hits, nhits, sizeof(search_hit_t), hit_score_cmp);

    pmtimespecNow(&finished);
    timer = pmtimespecSub(&finished, &started);

    offset = request->offset;
    count = request->count ? request->count : smd->resultcount;

    for (i = offset; (int)i < nhits && i < offset + count; i++) {
	uint32_t did = hits[i].doc_id;
	search_doc_t *doc = &idx->docs[did];

	memset(&result, 0, sizeof(result));
	result.total = nhits;
	result.count = (i - offset) + 1;
	result.timer = timer;
	result.score = hits[i].score;
	result.type = doc->type;
	result.docid = make_docid(idx, did);
	result.name = sdsnew(idx->strings + doc->name_off);
	if (doc->indom_off)
	    result.indom = sdsnew(idx->strings + doc->indom_off);
	if (doc->oneline_off)
	    result.oneline = sdsnew(idx->strings + doc->oneline_off);
	if (doc->helptext_off)
	    result.helptext = sdsnew(idx->strings + doc->helptext_off);

	callbacks->on_text_result(&result, userdata);

	sdsfree(result.docid);
	sdsfree(result.name);
	sdsfree(result.indom);
	sdsfree(result.oneline);
	sdsfree(result.helptext);
    }

    free(hits);
    callbacks->on_done(0, userdata);
}

/* --- public API --- */

int
pmSearchInfo(pmSearchSettings *settings, sds key, void *arg)
{
    searchModuleData	*smd = (searchModuleData *)settings->module.privdata;
    pmSearchMetrics	metrics;

    (void)key;

    if (smd == NULL || !smd->loaded) {
	settings->callbacks.on_done(-ENOENT, arg);
	return 0;
    }

    memset(&metrics, 0, sizeof(metrics));
    metrics.docs = smd->index.ndocs;
    metrics.terms = smd->index.nterms;
    metrics.records = smd->index.npostings;

    settings->callbacks.on_metrics(&metrics, arg);
    settings->callbacks.on_done(0, arg);
    return 0;
}

int
pmSearchTextQuery(pmSearchSettings *settings, pmSearchTextRequest *request, void *arg)
{
    searchModuleData	*smd = (searchModuleData *)settings->module.privdata;

    if (smd == NULL || !smd->loaded) {
	settings->callbacks.on_done(-ENOENT, arg);
	return 0;
    }

    search_do_text_query(smd, request, &settings->callbacks, arg);
    return 0;
}

int
pmSearchTextSuggest(pmSearchSettings *settings, pmSearchTextRequest *request, void *arg)
{
    searchModuleData	*smd = (searchModuleData *)settings->module.privdata;

    if (smd == NULL || !smd->loaded) {
	settings->callbacks.on_done(-ENOENT, arg);
	return 0;
    }

    search_do_text_suggest(smd, request, &settings->callbacks, arg);
    return 0;
}

int
pmSearchTextInDom(pmSearchSettings *settings, pmSearchTextRequest *request, void *arg)
{
    searchModuleData	*smd = (searchModuleData *)settings->module.privdata;

    if (smd == NULL || !smd->loaded) {
	settings->callbacks.on_done(-ENOENT, arg);
	return 0;
    }

    search_do_text_indom(smd, request, &settings->callbacks, arg);
    return 0;
}

/* --- module setup / teardown --- */

int
pmSearchSetSlots(pmSearchModule *module, void *slots)
{
    (void)module;
    (void)slots;
    return 0;
}

int
pmSearchSetConfiguration(pmSearchModule *module, struct dict *config)
{
    searchModuleData	*smd = getSearchModuleData(module);

    if (smd == NULL)
	return -ENOMEM;
    smd->config = config;
    return 0;
}

int
pmSearchSetEventLoop(pmSearchModule *module, void *events)
{
    (void)module;
    (void)events;
    return 0;
}

int
pmSearchSetMetricRegistry(pmSearchModule *module, struct mmv_registry *registry)
{
    (void)module;
    (void)registry;
    return 0;
}

int
pmSearchSetup(pmSearchModule *module, void *arg)
{
    searchModuleData	*smd = getSearchModuleData(module);
    char		path[MAXPATHLEN];
    sds			option;
    int			sts;

    if (smd == NULL)
	return -ENOMEM;

    smd->resultcount = default_resultcount;

    /* check if search is enabled in config */
    if (smd->config) {
	option = pmIniFileLookup(smd->config, "pmsearch", "enabled");
	if (option && strcmp(option, "false") == 0)
	    return -ENOTSUP;

	option = pmIniFileLookup(smd->config, "pmsearch", "result.count");
	if (option)
	    smd->resultcount = atoi(option);
    }

    /* try configured index path, then default location */
    option = smd->config ?
	     pmIniFileLookup(smd->config, "pmsearch", "index.path") : NULL;
    if (option) {
	pmsprintf(path, sizeof(path), "%s", option);
    } else {
	pmsprintf(path, sizeof(path), "%s/lib/pcp.search",
		  pmGetConfig("PCP_SHARE_DIR"));
    }

    sts = search_index_load(&smd->index, path);
    if (sts < 0) {
	if (pmDebugOptions.search)
	    fprintf(stderr, "pmSearchSetup: failed to load index %s: %s\n",
		    path, pmErrStr(sts));
	smd->loaded = 0;
    } else {
	smd->loaded = 1;
	search_enabled = 1;
	if (pmDebugOptions.search)
	    fprintf(stderr, "pmSearchSetup: loaded index %s "
		    "(%u docs, %u terms)\n",
		    path, smd->index.ndocs, smd->index.nterms);
    }

    if (module->on_setup)
	module->on_setup(arg);
    return 0;
}

int
pmSearchEnabled(void *arg)
{
    (void)arg;
    return search_enabled;
}

void
pmSearchClose(pmSearchModule *module)
{
    searchModuleData	*smd = (searchModuleData *)module->privdata;

    if (smd) {
	if (smd->loaded)
	    search_index_free(&smd->index);
	memset(smd, 0, sizeof(*smd));
	free(smd);
	module->privdata = NULL;
    }
    search_enabled = 0;
}

/* --- stubs for schema.c / keys.c compatibility --- */

extern void keys_slots_end_phase(void *);

void
keysSearchInit(struct dict *config)
{
    sds		option;

    if (config) {
	if ((option = pmIniFileLookup(config, "pmsearch", "result.count")))
	    default_resultcount = atoi(option);
    }
}

void
keysSearchClose(void)
{
    default_resultcount = 10;
}

void
keys_load_search_schema(void *arg)
{
    keys_slots_end_phase(arg);
}

void
keys_search_text_add(struct keySlots *slots, pmSearchTextType type,
		const char *name, const char *indom,
		const char *oneline, const char *helptext, void *arg)
{
    (void)slots; (void)type; (void)name; (void)indom;
    (void)oneline; (void)helptext; (void)arg;
}

/* --- discover no-ops (declared in discover.h) --- */

void
pmSearchDiscoverMetric(pmDiscoverEvent *event,
		pmDesc *desc, int numnames, char **names, void *arg)
{
    (void)event; (void)desc; (void)numnames; (void)names; (void)arg;
}

void
pmSearchDiscoverInDom(pmDiscoverEvent *event, pmInResult *in, void *arg)
{
    (void)event; (void)in; (void)arg;
}

void
pmSearchDiscoverText(pmDiscoverEvent *event,
		int ident, int type, char *text, void *arg)
{
    (void)event; (void)ident; (void)type; (void)text; (void)arg;
}
