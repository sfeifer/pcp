/*
 * Copyright (c) 2020,2022 Red Hat.
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
#ifndef SEARCH_SCHEMA_H
#define SEARCH_SCHEMA_H

#include <pmapi.h>
#include <mmv_stats.h>
#include "keys.h"
#include "private.h"
#include "schema.h"
#include "slots.h"

#define FT_TEXT_KEY	"pcp:text"
#define FT_TEXT_KEY_LEN	(sizeof(FT_TEXT_KEY)-1)
#define FT_TEXT_KEY_PFX	"pcp:text:"
#define FT_TEXT_KEY_PFX_LEN (sizeof(FT_TEXT_KEY_PFX)-1)

#define RESP_HSET	"HSET"
#define RESP_HSET_LEN	(sizeof(RESP_HSET)-1)

#define FT_CREATE	"FT.CREATE"
#define FT_CREATE_LEN	(sizeof(FT_CREATE)-1)
#define FT_SEARCH	"FT.SEARCH"
#define FT_SEARCH_LEN	(sizeof(FT_SEARCH)-1)
#define FT_INFO		"FT.INFO"
#define FT_INFO_LEN	(sizeof(FT_INFO)-1)

#define FT_ON		"ON"
#define FT_ON_LEN	(sizeof(FT_ON)-1)
#define FT_HASH		"HASH"
#define FT_HASH_LEN	(sizeof(FT_HASH)-1)
#define FT_PREFIX	"PREFIX"
#define FT_PREFIX_LEN	(sizeof(FT_PREFIX)-1)

#define FT_ASC		"ASC"
#define FT_ASC_LEN	(sizeof(FT_ASC)-1)
#define FT_FIELDS	"FIELDS"
#define FT_FIELDS_LEN	(sizeof(FT_FIELDS)-1)
#define FT_HELPTEXT	"HELPTEXT"
#define FT_HELPTEXT_LEN	(sizeof(FT_HELPTEXT)-1)
#define FT_INDOM	"INDOM"
#define FT_INDOM_LEN	(sizeof(FT_INDOM)-1)
#define FT_INORDER	"INORDER"
#define FT_INORDER_LEN	(sizeof(FT_INORDER)-1)
#define FT_LIMIT	"LIMIT"
#define FT_LIMIT_LEN	(sizeof(FT_LIMIT)-1)
#define FT_NAME		"NAME"
#define FT_NAME_LEN	(sizeof(FT_NAME)-1)
#define FT_ONELINE	"ONELINE"
#define FT_ONELINE_LEN	(sizeof(FT_ONELINE)-1)
#define FT_RETURN	"RETURN"
#define FT_RETURN_LEN	(sizeof(FT_RETURN)-1)
#define FT_SCHEMA	"SCHEMA"
#define FT_SCHEMA_LEN	(sizeof(FT_SCHEMA)-1)
#define FT_SORTABLE	"SORTABLE"
#define FT_SORTABLE_LEN	(sizeof(FT_SORTABLE)-1)
#define FT_SORTBY	"SORTBY"
#define FT_SORTBY_LEN	(sizeof(FT_SORTBY)-1)
#define FT_TAG		"TAG"
#define FT_TAG_LEN	(sizeof(FT_TAG)-1)
#define FT_TEXT		"TEXT"
#define FT_TEXT_LEN	(sizeof(FT_TEXT)-1)
#define FT_TYPE		"TYPE"
#define FT_TYPE_LEN	(sizeof(FT_TYPE)-1)

extern void keysSearchInit(struct dict *);
extern void keysSearchClose(void);
extern void keys_load_search_schema(void *);
extern void keys_search_text_add(keySlots *, pmSearchTextType,
		const char *, const char *, const char *, const char *, void *);

/*
 * Asynchronous search baton structures
 */
typedef struct keysSearchBaton {
    seriesBatonMagic	header;		/* MAGIC_SEARCH */

    keySlots		*slots;		/* key server slots */
    pmSearchFlags	flags;
    int			error;
    void		*module;
    pmSearchCallBacks	*callbacks;
    pmLogInfoCallBack	info;
    struct timespec	started;
    void		*userdata;
    void		*arg;
} keysSearchBaton;

#endif	/* SEARCH_SCHEMA_H */
