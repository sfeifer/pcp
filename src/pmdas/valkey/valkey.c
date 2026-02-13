/*
 * Valkey PMDA
 *
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

#include <pcp/pmapi.h>
#include <pcp/pmda.h>
#include "domain.h"
#include <pcp/valkey.h>
#include <pcp/read.h>
#include <pcp/alloc.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/*
 * Valkey PMDA
 *
 * This PMDA collects metrics from Valkey servers using the INFO command
 * via the libvalkey library. It supports multiple Valkey instances configured
 * in valkey.conf.
 *
 * Metrics are organized into clusters:
 *   Cluster 0: Server information
 *   Cluster 1: Client statistics
 *   Cluster 2: Memory metrics
 *   Cluster 3: General statistics
 *   Cluster 4: Replication information
 *   Cluster 5: CPU usage
 */

#define SERVER_INDOM 0  /* Instance domain for Valkey servers */
#define DATABASE_INDOM 1  /* Instance domain for databases */

/* Maximum cache age in seconds */
#define MAX_CACHE_AGE 0.5

/* Connection timeout */
#define CONNECT_TIMEOUT_SEC 1
#define CONNECT_TIMEOUT_USEC 500000

/* Maximum config line length */
#define MAX_LINE_LEN 256

/* Server instance data structure */
typedef struct {
    char *hostname;
    int port;
    valkeyContext *context;
    time_t last_update;

    /* Cached metric values */
    char *version;
    char *git_sha1;
    unsigned int git_dirty;
    char *build_id;
    char *mode;
    char *os;
    unsigned int arch_bits;
    char *multiplexing_api;
    char *gcc_version;
    unsigned int process_id;
    char *run_id;
    unsigned int tcp_port;
    unsigned int uptime_in_seconds;
    unsigned int uptime_in_days;
    unsigned int hz;
    unsigned int lru_clock;
    char *config_file;

    unsigned int connected_clients;
    unsigned int client_longest_output_list;
    unsigned int client_biggest_input_buf;
    unsigned int blocked_clients;

    unsigned long long used_memory;
    char *used_memory_human;
    unsigned long long used_memory_rss;
    unsigned long long used_memory_peak;
    char *used_memory_peak_human;
    unsigned int used_memory_lua;
    double mem_fragmentation_ratio;
    char *mem_allocator;

    unsigned int loading;
    unsigned long long rdb_changes_since_last_save;
    unsigned int rdb_bgsave_in_progress;
    unsigned int rdb_last_save_time;
    char *rdb_last_bgsave_status;
    int rdb_last_bgsave_time_sec;
    int rdb_current_bgsave_time_sec;
    char *aof_enabled;
    int aof_rewrite_in_progress;
    int aof_rewrite_scheduled;
    int aof_last_rewrite_time_sec;
    int aof_current_rewrite_time_sec;
    char *aof_last_bgrewrite_status;
    char *aof_last_write_status;

    unsigned long long total_connections_received;
    unsigned long long total_commands_processed;
    unsigned int instantaneous_ops_per_sec;
    unsigned long long total_net_input_bytes;
    unsigned long long total_net_output_bytes;
    unsigned int rejected_connections;
    unsigned int sync_full;
    unsigned int sync_partial_ok;
    unsigned int sync_partial_err;
    unsigned long long expired_keys;
    unsigned long long evicted_keys;
    unsigned long long keyspace_hits;
    unsigned long long keyspace_misses;
    unsigned int pubsub_channels;
    unsigned int pubsub_patterns;
    unsigned long long latest_fork_usec;

    char *role;
    unsigned int connected_slaves;
    int master_repl_offset;
    int repl_backlog_active;
    unsigned long long repl_backlog_size;
    char *repl_backlog_first_byte_offset;
    unsigned long long repl_backlog_histlen;

    double used_cpu_sys;
    double used_cpu_user;
    double used_cpu_sys_children;
    double used_cpu_user_children;
} server_data_t;

/* Database instance data structure */
typedef struct {
    char *dbname;               /* e.g., "db0" */
    unsigned long long keys;
    unsigned long long expires;
    unsigned long long avg_ttl;
} database_data_t;

/*
 * Instance domains
 */
static pmdaIndom indomtab[] = {
    { SERVER_INDOM, 0, NULL },
    { DATABASE_INDOM, 0, NULL },
};

static pmInDom *server_indom = &indomtab[SERVER_INDOM].it_indom;
static pmInDom *database_indom = &indomtab[DATABASE_INDOM].it_indom;

/*
 * All metrics supported in this PMDA
 */
static pmdaMetric metrictab[] = {
    /* valkey.version - id 0 */
    { NULL,
      { PMDA_PMID(0,0), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.git_sha1 - id 1 */
    { NULL,
      { PMDA_PMID(0,1), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.git_dirty - id 2 */
    { NULL,
      { PMDA_PMID(0,2), PM_TYPE_U32, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.build_id - id 3 */
    { NULL,
      { PMDA_PMID(0,3), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.mode - id 4 */
    { NULL,
      { PMDA_PMID(0,4), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.os - id 5 */
    { NULL,
      { PMDA_PMID(0,5), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.arch_bits - id 6 */
    { NULL,
      { PMDA_PMID(0,6), PM_TYPE_U32, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.multiplexing_api - id 7 */
    { NULL,
      { PMDA_PMID(0,7), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.gcc_version - id 8 */
    { NULL,
      { PMDA_PMID(0,8), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.process_id - id 9 */
    { NULL,
      { PMDA_PMID(0,9), PM_TYPE_U32, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.run_id - id 10 */
    { NULL,
      { PMDA_PMID(0,10), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.tcp_port - id 11 */
    { NULL,
      { PMDA_PMID(0,11), PM_TYPE_U32, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.uptime_in_seconds - id 12 */
    { NULL,
      { PMDA_PMID(0,12), PM_TYPE_U32, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.uptime_in_days - id 13 */
    { NULL,
      { PMDA_PMID(0,13), PM_TYPE_U32, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.hz - id 14 */
    { NULL,
      { PMDA_PMID(0,14), PM_TYPE_U32, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.lru_clock - id 15 */
    { NULL,
      { PMDA_PMID(0,15), PM_TYPE_U32, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.config_file - id 16 */
    { NULL,
      { PMDA_PMID(0,16), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.connected_clients - id 17 */
    { NULL,
      { PMDA_PMID(0,17), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.client_longest_output_list - id 18 */
    { NULL,
      { PMDA_PMID(0,18), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.client_biggest_input_buf - id 19 */
    { NULL,
      { PMDA_PMID(0,19), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.blocked_clients - id 20 */
    { NULL,
      { PMDA_PMID(0,20), PM_TYPE_U32, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.used_memory - id 21 */
    { NULL,
      { PMDA_PMID(0,21), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
    /* valkey.used_memory_human - id 22 */
    { NULL,
      { PMDA_PMID(0,22), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.used_memory_rss - id 23 */
    { NULL,
      { PMDA_PMID(0,23), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
    /* valkey.used_memory_peak - id 24 */
    { NULL,
      { PMDA_PMID(0,24), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
    /* valkey.used_memory_peak_human - id 25 */
    { NULL,
      { PMDA_PMID(0,25), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.used_memory_lua - id 26 */
    { NULL,
      { PMDA_PMID(0,26), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
    /* valkey.mem_fragmentation_ratio - id 27 */
    { NULL,
      { PMDA_PMID(0,27), PM_TYPE_FLOAT, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.mem_allocator - id 28 */
    { NULL,
      { PMDA_PMID(0,28), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_DISCRETE,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.loading - id 29 */
    { NULL,
      { PMDA_PMID(0,29), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.rdb_changes_since_last_save - id 30 */
    { NULL,
      { PMDA_PMID(0,30), PM_TYPE_U64, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.rdb_bgsave_in_progress - id 31 */
    { NULL,
      { PMDA_PMID(0,31), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.rdb_last_save_time - id 32 */
    { NULL,
      { PMDA_PMID(0,32), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.rdb_last_bgsave_status - id 33 */
    { NULL,
      { PMDA_PMID(0,33), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.rdb_last_bgsave_time_sec - id 34 */
    { NULL,
      { PMDA_PMID(0,34), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.rdb_current_bgsave_time_sec - id 35 */
    { NULL,
      { PMDA_PMID(0,35), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.aof_enabled - id 36 */
    { NULL,
      { PMDA_PMID(0,36), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.aof_rewrite_in_progress - id 37 */
    { NULL,
      { PMDA_PMID(0,37), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.aof_rewrite_scheduled - id 38 */
    { NULL,
      { PMDA_PMID(0,38), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.aof_last_rewrite_time_sec - id 39 */
    { NULL,
      { PMDA_PMID(0,39), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.aof_current_rewrite_time_sec - id 40 */
    { NULL,
      { PMDA_PMID(0,40), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.aof_last_bgrewrite_status - id 41 */
    { NULL,
      { PMDA_PMID(0,41), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.aof_last_write_status - id 42 */
    { NULL,
      { PMDA_PMID(0,42), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.total_connections_received - id 43 */
    { NULL,
      { PMDA_PMID(0,43), PM_TYPE_32, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.total_commands_processed - id 44 */
    { NULL,
      { PMDA_PMID(0,44), PM_TYPE_U64, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.instantaneous_ops_per_sec - id 45 */
    { NULL,
      { PMDA_PMID(0,45), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,-1,1,0,PM_TIME_SEC,PM_COUNT_ONE) }, },
    /* valkey.rejected_connections - id 46 */
    { NULL,
      { PMDA_PMID(0,46), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.sync_full - id 47 */
    { NULL,
      { PMDA_PMID(0,47), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.sync_partial_ok - id 48 */
    { NULL,
      { PMDA_PMID(0,48), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.sync_partial_err - id 49 */
    { NULL,
      { PMDA_PMID(0,49), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.expired_keys - id 50 */
    { NULL,
      { PMDA_PMID(0,50), PM_TYPE_U64, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.evicted_keys - id 51 */
    { NULL,
      { PMDA_PMID(0,51), PM_TYPE_U64, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.keyspace_hits - id 52 */
    { NULL,
      { PMDA_PMID(0,52), PM_TYPE_U64, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.keyspace_misses - id 53 */
    { NULL,
      { PMDA_PMID(0,53), PM_TYPE_U64, SERVER_INDOM, PM_SEM_COUNTER,
        PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
    /* valkey.pubsub_channels - id 54 */
    { NULL,
      { PMDA_PMID(0,54), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.pubsub_patterns - id 55 */
    { NULL,
      { PMDA_PMID(0,55), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.latest_fork_usec - id 56 */
    { NULL,
      { PMDA_PMID(0,56), PM_TYPE_U64, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_USEC,0) }, },
    /* valkey.role - id 57 */
    { NULL,
      { PMDA_PMID(0,57), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.connected_slaves - id 58 */
    { NULL,
      { PMDA_PMID(0,58), PM_TYPE_U32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.master_repl_offset - id 59 */
    { NULL,
      { PMDA_PMID(0,59), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.repl_backlog_active - id 60 */
    { NULL,
      { PMDA_PMID(0,60), PM_TYPE_32, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.repl_backlog_size - id 61 */
    { NULL,
      { PMDA_PMID(0,61), PM_TYPE_U64, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
    /* valkey.repl_backlog_first_byte_offset - id 62 */
    { NULL,
      { PMDA_PMID(0,62), PM_TYPE_STRING, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.repl_backlog_histlen - id 63 */
    { NULL,
      { PMDA_PMID(0,63), PM_TYPE_U64, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
    /* valkey.used_cpu_sys - id 64 */
    { NULL,
      { PMDA_PMID(0,64), PM_TYPE_FLOAT, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.used_cpu_user - id 65 */
    { NULL,
      { PMDA_PMID(0,65), PM_TYPE_FLOAT, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.used_cpu_sys_children - id 66 */
    { NULL,
      { PMDA_PMID(0,66), PM_TYPE_FLOAT, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.used_cpu_user_children - id 67 */
    { NULL,
      { PMDA_PMID(0,67), PM_TYPE_FLOAT, SERVER_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_SEC,0) }, },
    /* valkey.db.keys - id 68 */
    { NULL,
      { PMDA_PMID(0,68), PM_TYPE_U64, DATABASE_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.db.expires - id 69 */
    { NULL,
      { PMDA_PMID(0,69), PM_TYPE_U64, DATABASE_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,0,0,0,0,0) }, },
    /* valkey.db.avg_ttl - id 70 */
    { NULL,
      { PMDA_PMID(0,70), PM_TYPE_U64, DATABASE_INDOM, PM_SEM_INSTANT,
        PMDA_PMUNITS(0,1,0,0,PM_TIME_MSEC,0) }, },
};

static int isDSO = 1;
static char *username;
static char mypath[MAXPATHLEN];

/*
 * Parse a key:value pair from INFO response
 */
static void
parse_info_line(const char *line, server_data_t *data)
{
    char key[MAX_LINE_LEN];
    char value[MAX_LINE_LEN];

    if (sscanf(line, "%255[^:]:%255[^\r\n]", key, value) != 2)
        return;

    /* Server metrics */
    if (strcmp(key, "valkey_version") == 0) {
        if (data->version) free(data->version);
        data->version = strdup(value);
    }
    else if (strcmp(key, "valkey_git_sha1") == 0) {
        if (data->git_sha1) free(data->git_sha1);
        data->git_sha1 = strdup(value);
    }
    else if (strcmp(key, "valkey_git_dirty") == 0) {
        data->git_dirty = atoi(value);
    }
    else if (strcmp(key, "valkey_build_id") == 0) {
        if (data->build_id) free(data->build_id);
        data->build_id = strdup(value);
    }
    else if (strcmp(key, "valkey_mode") == 0) {
        if (data->mode) free(data->mode);
        data->mode = strdup(value);
    }
    else if (strcmp(key, "multiplexing_api") == 0) {
        if (data->multiplexing_api) free(data->multiplexing_api);
        data->multiplexing_api = strdup(value);
    }
    else if (strcmp(key, "gcc_version") == 0) {
        if (data->gcc_version) free(data->gcc_version);
        data->gcc_version = strdup(value);
    }
    else if (strcmp(key, "run_id") == 0) {
        if (data->run_id) free(data->run_id);
        data->run_id = strdup(value);
    }
    else if (strcmp(key, "tcp_port") == 0) {
        data->tcp_port = atoi(value);
    }
    else if (strcmp(key, "uptime_in_days") == 0) {
        data->uptime_in_days = atoi(value);
    }
    else if (strcmp(key, "hz") == 0) {
        data->hz = atoi(value);
    }
    else if (strcmp(key, "lru_clock") == 0) {
        data->lru_clock = atoi(value);
    }
    else if (strcmp(key, "config_file") == 0) {
        if (data->config_file) free(data->config_file);
        data->config_file = strdup(value);
    }
    else if (strcmp(key, "os") == 0) {
        if (data->os) free(data->os);
        data->os = strdup(value);
    }
    else if (strcmp(key, "arch_bits") == 0) {
        data->arch_bits = atoi(value);
    }
    else if (strcmp(key, "process_id") == 0) {
        data->process_id = atoi(value);
    }
    else if (strcmp(key, "uptime_in_seconds") == 0) {
        data->uptime_in_seconds = atoi(value);
    }
    /* Client metrics */
    else if (strcmp(key, "connected_clients") == 0) {
        data->connected_clients = atoi(value);
    }
    else if (strcmp(key, "client_longest_output_list") == 0) {
        data->client_longest_output_list = atoi(value);
    }
    else if (strcmp(key, "client_biggest_input_buf") == 0) {
        data->client_biggest_input_buf = atoi(value);
    }
    else if (strcmp(key, "blocked_clients") == 0) {
        data->blocked_clients = atoi(value);
    }
    /* Memory metrics */
    else if (strcmp(key, "used_memory") == 0) {
        data->used_memory = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "used_memory_human") == 0) {
        if (data->used_memory_human) free(data->used_memory_human);
        data->used_memory_human = strdup(value);
    }
    else if (strcmp(key, "used_memory_rss") == 0) {
        data->used_memory_rss = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "used_memory_peak") == 0) {
        data->used_memory_peak = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "used_memory_peak_human") == 0) {
        if (data->used_memory_peak_human) free(data->used_memory_peak_human);
        data->used_memory_peak_human = strdup(value);
    }
    else if (strcmp(key, "used_memory_lua") == 0) {
        data->used_memory_lua = atoi(value);
    }
    else if (strcmp(key, "mem_fragmentation_ratio") == 0) {
        data->mem_fragmentation_ratio = atof(value);
    }
    else if (strcmp(key, "mem_allocator") == 0) {
        if (data->mem_allocator) free(data->mem_allocator);
        data->mem_allocator = strdup(value);
    }
    /* Persistence metrics */
    else if (strcmp(key, "loading") == 0) {
        data->loading = atoi(value);
    }
    else if (strcmp(key, "rdb_changes_since_last_save") == 0) {
        data->rdb_changes_since_last_save = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "rdb_bgsave_in_progress") == 0) {
        data->rdb_bgsave_in_progress = atoi(value);
    }
    else if (strcmp(key, "rdb_last_save_time") == 0) {
        data->rdb_last_save_time = atoi(value);
    }
    else if (strcmp(key, "rdb_last_bgsave_status") == 0) {
        if (data->rdb_last_bgsave_status) free(data->rdb_last_bgsave_status);
        data->rdb_last_bgsave_status = strdup(value);
    }
    else if (strcmp(key, "rdb_last_bgsave_time_sec") == 0) {
        data->rdb_last_bgsave_time_sec = atoi(value);
    }
    else if (strcmp(key, "rdb_current_bgsave_time_sec") == 0) {
        data->rdb_current_bgsave_time_sec = atoi(value);
    }
    else if (strcmp(key, "aof_enabled") == 0) {
        if (data->aof_enabled) free(data->aof_enabled);
        data->aof_enabled = strdup(value);
    }
    else if (strcmp(key, "aof_rewrite_in_progress") == 0) {
        data->aof_rewrite_in_progress = atoi(value);
    }
    else if (strcmp(key, "aof_rewrite_scheduled") == 0) {
        data->aof_rewrite_scheduled = atoi(value);
    }
    else if (strcmp(key, "aof_last_rewrite_time_sec") == 0) {
        data->aof_last_rewrite_time_sec = atoi(value);
    }
    else if (strcmp(key, "aof_current_rewrite_time_sec") == 0) {
        data->aof_current_rewrite_time_sec = atoi(value);
    }
    else if (strcmp(key, "aof_last_bgrewrite_status") == 0) {
        if (data->aof_last_bgrewrite_status) free(data->aof_last_bgrewrite_status);
        data->aof_last_bgrewrite_status = strdup(value);
    }
    else if (strcmp(key, "aof_last_write_status") == 0) {
        if (data->aof_last_write_status) free(data->aof_last_write_status);
        data->aof_last_write_status = strdup(value);
    }
    /* Stats metrics */
    else if (strcmp(key, "total_connections_received") == 0) {
        data->total_connections_received = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "total_commands_processed") == 0) {
        data->total_commands_processed = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "instantaneous_ops_per_sec") == 0) {
        data->instantaneous_ops_per_sec = atoi(value);
    }
    else if (strcmp(key, "total_net_input_bytes") == 0) {
        data->total_net_input_bytes = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "total_net_output_bytes") == 0) {
        data->total_net_output_bytes = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "rejected_connections") == 0) {
        data->rejected_connections = atoi(value);
    }
    else if (strcmp(key, "sync_full") == 0) {
        data->sync_full = atoi(value);
    }
    else if (strcmp(key, "sync_partial_ok") == 0) {
        data->sync_partial_ok = atoi(value);
    }
    else if (strcmp(key, "sync_partial_err") == 0) {
        data->sync_partial_err = atoi(value);
    }
    else if (strcmp(key, "expired_keys") == 0) {
        data->expired_keys = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "evicted_keys") == 0) {
        data->evicted_keys = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "keyspace_hits") == 0) {
        data->keyspace_hits = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "keyspace_misses") == 0) {
        data->keyspace_misses = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "pubsub_channels") == 0) {
        data->pubsub_channels = atoi(value);
    }
    else if (strcmp(key, "pubsub_patterns") == 0) {
        data->pubsub_patterns = atoi(value);
    }
    else if (strcmp(key, "latest_fork_usec") == 0) {
        data->latest_fork_usec = strtoull(value, NULL, 10);
    }
    /* Replication metrics */
    else if (strcmp(key, "role") == 0) {
        if (data->role) free(data->role);
        data->role = strdup(value);
    }
    else if (strcmp(key, "connected_slaves") == 0) {
        data->connected_slaves = atoi(value);
    }
    else if (strcmp(key, "master_repl_offset") == 0) {
        data->master_repl_offset = atoi(value);
    }
    else if (strcmp(key, "repl_backlog_active") == 0) {
        data->repl_backlog_active = atoi(value);
    }
    else if (strcmp(key, "repl_backlog_size") == 0) {
        data->repl_backlog_size = strtoull(value, NULL, 10);
    }
    else if (strcmp(key, "repl_backlog_first_byte_offset") == 0) {
        if (data->repl_backlog_first_byte_offset) free(data->repl_backlog_first_byte_offset);
        data->repl_backlog_first_byte_offset = strdup(value);
    }
    else if (strcmp(key, "repl_backlog_histlen") == 0) {
        data->repl_backlog_histlen = strtoull(value, NULL, 10);
    }
    /* CPU metrics */
    else if (strcmp(key, "used_cpu_sys") == 0) {
        data->used_cpu_sys = atof(value);
    }
    else if (strcmp(key, "used_cpu_user") == 0) {
        data->used_cpu_user = atof(value);
    }
    else if (strcmp(key, "used_cpu_sys_children") == 0) {
        data->used_cpu_sys_children = atof(value);
    }
    else if (strcmp(key, "used_cpu_user_children") == 0) {
        data->used_cpu_user_children = atof(value);
    }
}

/*
 * Parse database keyspace line like "db0:keys=123,expires=10,avg_ttl=5000"
 */
static void
parse_database_line(const char *line)
{
    char dbname[64];
    unsigned long long keys = 0, expires = 0, avg_ttl = 0;
    database_data_t *dbdata;
    int inst;

    /* Parse format: db0:keys=123,expires=10,avg_ttl=5000 */
    if (sscanf(line, "%63[^:]:keys=%llu,expires=%llu,avg_ttl=%llu",
               dbname, &keys, &expires, &avg_ttl) >= 2) {

        /* Look up or create database instance */
        if (pmdaCacheLookupName(*database_indom, dbname, &inst, (void **)&dbdata) == PMDA_CACHE_ACTIVE) {
            /* Update existing */
            dbdata->keys = keys;
            dbdata->expires = expires;
            dbdata->avg_ttl = avg_ttl;
        } else {
            /* Create new */
            dbdata = (database_data_t *)calloc(1, sizeof(database_data_t));
            if (dbdata) {
                dbdata->dbname = strdup(dbname);
                dbdata->keys = keys;
                dbdata->expires = expires;
                dbdata->avg_ttl = avg_ttl;
                pmdaCacheStore(*database_indom, PMDA_CACHE_ADD, dbname, dbdata);
            }
        }
    }
}

/*
 * Free database data
 */
static void
free_database_data(database_data_t *data)
{
    if (data->dbname) free(data->dbname);
    free(data);
}

/*
 * Fetch INFO from Valkey server and update cached data
 */
static int
update_server_data(server_data_t *data)
{
    valkeyReply *reply;
    struct timeval timeout = { CONNECT_TIMEOUT_SEC, CONNECT_TIMEOUT_USEC };
    char *line, *next;
    time_t now = time(NULL);

    /* Check cache validity */
    if (data->last_update > 0 && (now - data->last_update) < MAX_CACHE_AGE) {
        return 0;  /* Cache is still fresh */
    }

    /* Free existing database instances before refresh */
    pmdaCacheOp(*database_indom, PMDA_CACHE_WALK_REWIND);
    while (1) {
        database_data_t *db_data;
        char *name;
        if (pmdaCacheOp(*database_indom, PMDA_CACHE_WALK_NEXT) < 0)
            break;
        if (pmdaCacheLookup(*database_indom, PM_IN_NULL, &name, (void **)&db_data) == PMDA_CACHE_ACTIVE) {
            if (db_data)
                free_database_data(db_data);
        }
    }
    pmdaCacheOp(*database_indom, PMDA_CACHE_INACTIVE);

    /* Connect if needed */
    if (data->context == NULL) {
        data->context = valkeyConnectWithTimeout(data->hostname, data->port, timeout);
        if (data->context == NULL) {
            pmNotifyErr(LOG_ERR, "valkeyConnect failed: out of memory");
            return PM_ERR_AGAIN;
        }
        if (data->context->err) {
            pmNotifyErr(LOG_ERR, "Connection error to %s:%d: %s",
                       data->hostname, data->port, data->context->errstr);
            valkeyFree(data->context);
            data->context = NULL;
            return PM_ERR_AGAIN;
        }
    }

    /* Execute INFO command */
    reply = valkeyCommand(data->context, "INFO");
    if (reply == NULL) {
        if (data->context->err) {
            pmNotifyErr(LOG_ERR, "INFO command error to %s:%d: %s",
                       data->hostname, data->port, data->context->errstr);
            valkeyFree(data->context);
            data->context = NULL;
        }
        return PM_ERR_AGAIN;
    }

    if (reply->type == VALKEY_REPLY_ERROR) {
        pmNotifyErr(LOG_ERR, "INFO command returned error: %s", reply->str);
        freeReplyObject(reply);
        return PM_ERR_AGAIN;
    }

    if (reply->type != VALKEY_REPLY_STRING) {
        pmNotifyErr(LOG_ERR, "INFO command returned unexpected type: %d", reply->type);
        freeReplyObject(reply);
        return PM_ERR_AGAIN;
    }

    /* Parse INFO response line by line */
    line = reply->str;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }

        /* Remove trailing \r if present */
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        /* Skip empty lines and comments */
        if (*line && *line != '#') {
            /* Check if it's a database keyspace line */
            if (strncmp(line, "db", 2) == 0 && strchr(line, ':')) {
                parse_database_line(line);
            } else {
                parse_info_line(line, data);
            }
        }

        line = next;
    }

    freeReplyObject(reply);
    data->last_update = now;

    return 0;
}

/*
 * Free server data
 */
static void
free_server_data(server_data_t *data)
{
    if (data->hostname) free(data->hostname);
    if (data->context) valkeyFree(data->context);
    if (data->version) free(data->version);
    if (data->git_sha1) free(data->git_sha1);
    if (data->build_id) free(data->build_id);
    if (data->mode) free(data->mode);
    if (data->os) free(data->os);
    if (data->multiplexing_api) free(data->multiplexing_api);
    if (data->gcc_version) free(data->gcc_version);
    if (data->run_id) free(data->run_id);
    if (data->config_file) free(data->config_file);
    if (data->used_memory_human) free(data->used_memory_human);
    if (data->used_memory_peak_human) free(data->used_memory_peak_human);
    if (data->mem_allocator) free(data->mem_allocator);
    if (data->rdb_last_bgsave_status) free(data->rdb_last_bgsave_status);
    if (data->aof_enabled) free(data->aof_enabled);
    if (data->aof_last_bgrewrite_status) free(data->aof_last_bgrewrite_status);
    if (data->aof_last_write_status) free(data->aof_last_write_status);
    if (data->role) free(data->role);
    if (data->repl_backlog_first_byte_offset) free(data->repl_backlog_first_byte_offset);
    free(data);
}

/*
 * Callback for fetching metric values
 */
static int
valkey_fetchCallBack(pmdaMetric *mdesc, unsigned int inst, pmAtomValue *atom)
{
    int sts;
    server_data_t *server_data;
    database_data_t *db_data;
    unsigned int cluster = pmID_cluster(mdesc->m_desc.pmid);
    unsigned int item = pmID_item(mdesc->m_desc.pmid);

    /* Database metrics use DATABASE_INDOM */
    if (mdesc->m_desc.indom == *database_indom) {
        /* Lookup database instance */
        if ((sts = pmdaCacheLookup(*database_indom, inst, NULL, (void *)&db_data)) != PMDA_CACHE_ACTIVE) {
            return PM_ERR_INST;
        }

        /* Return database metric value */
        switch (item) {
        case 68:  /* db.keys */
            atom->ull = db_data->keys;
            return PMDA_FETCH_STATIC;
        case 69:  /* db.expires */
            atom->ull = db_data->expires;
            return PMDA_FETCH_STATIC;
        case 70:  /* db.avg_ttl */
            atom->ull = db_data->avg_ttl;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }
    }

    /* Server metrics use SERVER_INDOM */
    if ((sts = pmdaCacheLookup(*server_indom, inst, NULL, (void *)&server_data)) != PMDA_CACHE_ACTIVE) {
        if (sts < 0)
            pmNotifyErr(LOG_ERR, "pmdaCacheLookup failed: inst=%d: %s", inst, pmErrStr(sts));
        return PM_ERR_INST;
    }

    /* Update server data if needed */
    if ((sts = update_server_data(server_data)) < 0)
        return sts;

    /* Return requested metric value */
    switch (cluster) {
    case 0:  /* Server metrics */
        switch (item) {
        case 0:  /* version */
            if (server_data->version == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->version;
            return PMDA_FETCH_STATIC;
        case 1:  /* git_sha1 */
            if (server_data->git_sha1 == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->git_sha1;
            return PMDA_FETCH_STATIC;
        case 2:  /* git_dirty */
            atom->ul = server_data->git_dirty;
            return PMDA_FETCH_STATIC;
        case 3:  /* build_id */
            if (server_data->build_id == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->build_id;
            return PMDA_FETCH_STATIC;
        case 4:  /* mode */
            if (server_data->mode == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->mode;
            return PMDA_FETCH_STATIC;
        case 5:  /* os */
            if (server_data->os == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->os;
            return PMDA_FETCH_STATIC;
        case 6:  /* arch_bits */
            atom->ul = server_data->arch_bits;
            return PMDA_FETCH_STATIC;
        case 7:  /* multiplexing_api */
            if (server_data->multiplexing_api == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->multiplexing_api;
            return PMDA_FETCH_STATIC;
        case 8:  /* gcc_version */
            if (server_data->gcc_version == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->gcc_version;
            return PMDA_FETCH_STATIC;
        case 9:  /* process_id */
            atom->ul = server_data->process_id;
            return PMDA_FETCH_STATIC;
        case 10:  /* run_id */
            if (server_data->run_id == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->run_id;
            return PMDA_FETCH_STATIC;
        case 11:  /* tcp_port */
            atom->ul = server_data->tcp_port;
            return PMDA_FETCH_STATIC;
        case 12:  /* uptime_in_seconds */
            atom->ul = server_data->uptime_in_seconds;
            return PMDA_FETCH_STATIC;
        case 13:  /* uptime_in_days */
            atom->ul = server_data->uptime_in_days;
            return PMDA_FETCH_STATIC;
        case 14:  /* hz */
            atom->ul = server_data->hz;
            return PMDA_FETCH_STATIC;
        case 15:  /* lru_clock */
            atom->ul = server_data->lru_clock;
            return PMDA_FETCH_STATIC;
        case 16:  /* config_file */
            if (server_data->config_file == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->config_file;
            return PMDA_FETCH_STATIC;
        case 17:  /* connected_clients */
            atom->ul = server_data->connected_clients;
            return PMDA_FETCH_STATIC;
        case 18:  /* client_longest_output_list */
            atom->ul = server_data->client_longest_output_list;
            return PMDA_FETCH_STATIC;
        case 19:  /* client_biggest_input_buf */
            atom->ul = server_data->client_biggest_input_buf;
            return PMDA_FETCH_STATIC;
        case 20:  /* blocked_clients */
            atom->ul = server_data->blocked_clients;
            return PMDA_FETCH_STATIC;
        case 21:  /* used_memory */
            atom->ul = server_data->used_memory;
            return PMDA_FETCH_STATIC;
        case 22:  /* used_memory_human */
            if (server_data->used_memory_human == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->used_memory_human;
            return PMDA_FETCH_STATIC;
        case 23:  /* used_memory_rss */
            atom->ul = server_data->used_memory_rss;
            return PMDA_FETCH_STATIC;
        case 24:  /* used_memory_peak */
            atom->ul = server_data->used_memory_peak;
            return PMDA_FETCH_STATIC;
        case 25:  /* used_memory_peak_human */
            if (server_data->used_memory_peak_human == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->used_memory_peak_human;
            return PMDA_FETCH_STATIC;
        case 26:  /* used_memory_lua */
            atom->ul = server_data->used_memory_lua;
            return PMDA_FETCH_STATIC;
        case 27:  /* mem_fragmentation_ratio */
            atom->f = server_data->mem_fragmentation_ratio;
            return PMDA_FETCH_STATIC;
        case 28:  /* mem_allocator */
            if (server_data->mem_allocator == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->mem_allocator;
            return PMDA_FETCH_STATIC;
        case 29:  /* loading */
            atom->ul = server_data->loading;
            return PMDA_FETCH_STATIC;
        case 30:  /* rdb_changes_since_last_save */
            atom->ull = server_data->rdb_changes_since_last_save;
            return PMDA_FETCH_STATIC;
        case 31:  /* rdb_bgsave_in_progress */
            atom->ul = server_data->rdb_bgsave_in_progress;
            return PMDA_FETCH_STATIC;
        case 32:  /* rdb_last_save_time */
            atom->ul = server_data->rdb_last_save_time;
            return PMDA_FETCH_STATIC;
        case 33:  /* rdb_last_bgsave_status */
            if (server_data->rdb_last_bgsave_status == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->rdb_last_bgsave_status;
            return PMDA_FETCH_STATIC;
        case 34:  /* rdb_last_bgsave_time_sec */
            atom->l = server_data->rdb_last_bgsave_time_sec;
            return PMDA_FETCH_STATIC;
        case 35:  /* rdb_current_bgsave_time_sec */
            atom->l = server_data->rdb_current_bgsave_time_sec;
            return PMDA_FETCH_STATIC;
        case 36:  /* aof_enabled */
            if (server_data->aof_enabled == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->aof_enabled;
            return PMDA_FETCH_STATIC;
        case 37:  /* aof_rewrite_in_progress */
            atom->l = server_data->aof_rewrite_in_progress;
            return PMDA_FETCH_STATIC;
        case 38:  /* aof_rewrite_scheduled */
            atom->l = server_data->aof_rewrite_scheduled;
            return PMDA_FETCH_STATIC;
        case 39:  /* aof_last_rewrite_time_sec */
            atom->l = server_data->aof_last_rewrite_time_sec;
            return PMDA_FETCH_STATIC;
        case 40:  /* aof_current_rewrite_time_sec */
            atom->l = server_data->aof_current_rewrite_time_sec;
            return PMDA_FETCH_STATIC;
        case 41:  /* aof_last_bgrewrite_status */
            if (server_data->aof_last_bgrewrite_status == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->aof_last_bgrewrite_status;
            return PMDA_FETCH_STATIC;
        case 42:  /* aof_last_write_status */
            if (server_data->aof_last_write_status == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->aof_last_write_status;
            return PMDA_FETCH_STATIC;
        case 43:  /* total_connections_received */
            atom->l = server_data->total_connections_received;
            return PMDA_FETCH_STATIC;
        case 44:  /* total_commands_processed */
            atom->ull = server_data->total_commands_processed;
            return PMDA_FETCH_STATIC;
        case 45:  /* instantaneous_ops_per_sec */
            atom->ul = server_data->instantaneous_ops_per_sec;
            return PMDA_FETCH_STATIC;
        case 46:  /* rejected_connections */
            atom->ul = server_data->rejected_connections;
            return PMDA_FETCH_STATIC;
        case 47:  /* sync_full */
            atom->ul = server_data->sync_full;
            return PMDA_FETCH_STATIC;
        case 48:  /* sync_partial_ok */
            atom->ul = server_data->sync_partial_ok;
            return PMDA_FETCH_STATIC;
        case 49:  /* sync_partial_err */
            atom->ul = server_data->sync_partial_err;
            return PMDA_FETCH_STATIC;
        case 50:  /* expired_keys */
            atom->ull = server_data->expired_keys;
            return PMDA_FETCH_STATIC;
        case 51:  /* evicted_keys */
            atom->ull = server_data->evicted_keys;
            return PMDA_FETCH_STATIC;
        case 52:  /* keyspace_hits */
            atom->ull = server_data->keyspace_hits;
            return PMDA_FETCH_STATIC;
        case 53:  /* keyspace_misses */
            atom->ull = server_data->keyspace_misses;
            return PMDA_FETCH_STATIC;
        case 54:  /* pubsub_channels */
            atom->ul = server_data->pubsub_channels;
            return PMDA_FETCH_STATIC;
        case 55:  /* pubsub_patterns */
            atom->ul = server_data->pubsub_patterns;
            return PMDA_FETCH_STATIC;
        case 56:  /* latest_fork_usec */
            atom->ull = server_data->latest_fork_usec;
            return PMDA_FETCH_STATIC;
        case 57:  /* role */
            if (server_data->role == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->role;
            return PMDA_FETCH_STATIC;
        case 58:  /* connected_slaves */
            atom->ul = server_data->connected_slaves;
            return PMDA_FETCH_STATIC;
        case 59:  /* master_repl_offset */
            atom->l = server_data->master_repl_offset;
            return PMDA_FETCH_STATIC;
        case 60:  /* repl_backlog_active */
            atom->l = server_data->repl_backlog_active;
            return PMDA_FETCH_STATIC;
        case 61:  /* repl_backlog_size */
            atom->ull = server_data->repl_backlog_size;
            return PMDA_FETCH_STATIC;
        case 62:  /* repl_backlog_first_byte_offset */
            if (server_data->repl_backlog_first_byte_offset == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->repl_backlog_first_byte_offset;
            return PMDA_FETCH_STATIC;
        case 63:  /* repl_backlog_histlen */
            atom->ull = server_data->repl_backlog_histlen;
            return PMDA_FETCH_STATIC;
        case 64:  /* used_cpu_sys */
            atom->f = server_data->used_cpu_sys;
            return PMDA_FETCH_STATIC;
        case 65:  /* used_cpu_user */
            atom->f = server_data->used_cpu_user;
            return PMDA_FETCH_STATIC;
        case 66:  /* used_cpu_sys_children */
            atom->f = server_data->used_cpu_sys_children;
            return PMDA_FETCH_STATIC;
        case 67:  /* used_cpu_user_children */
            atom->f = server_data->used_cpu_user_children;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    case 1:  /* Client metrics */
        switch (item) {
        case 0:  /* connected_clients */
            atom->ul = server_data->connected_clients;
            return PMDA_FETCH_STATIC;
        case 1:  /* blocked_clients */
            atom->ul = server_data->blocked_clients;
            return PMDA_FETCH_STATIC;
        case 2:  /* client_longest_output_list */
            atom->ul = server_data->client_longest_output_list;
            return PMDA_FETCH_STATIC;
        case 3:  /* client_biggest_input_buf */
            atom->ul = server_data->client_biggest_input_buf;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    case 2:  /* Memory metrics */
        switch (item) {
        case 0:  /* used_memory */
            atom->ull = server_data->used_memory;
            return PMDA_FETCH_STATIC;
        case 1:  /* used_memory_rss */
            atom->ull = server_data->used_memory_rss;
            return PMDA_FETCH_STATIC;
        case 2:  /* used_memory_peak */
            atom->ull = server_data->used_memory_peak;
            return PMDA_FETCH_STATIC;
        case 3:  /* mem_fragmentation_ratio */
            atom->d = server_data->mem_fragmentation_ratio;
            return PMDA_FETCH_STATIC;
        case 4:  /* used_memory_human */
            if (server_data->used_memory_human == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->used_memory_human;
            return PMDA_FETCH_STATIC;
        case 5:  /* used_memory_peak_human */
            if (server_data->used_memory_peak_human == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->used_memory_peak_human;
            return PMDA_FETCH_STATIC;
        case 6:  /* used_memory_lua */
            atom->ul = server_data->used_memory_lua;
            return PMDA_FETCH_STATIC;
        case 7:  /* mem_allocator */
            if (server_data->mem_allocator == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->mem_allocator;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    case 3:  /* Stats metrics */
        switch (item) {
        case 0:  /* total_connections_received */
            atom->ull = server_data->total_connections_received;
            return PMDA_FETCH_STATIC;
        case 1:  /* total_commands_processed */
            atom->ull = server_data->total_commands_processed;
            return PMDA_FETCH_STATIC;
        case 2:  /* instantaneous_ops_per_sec */
            atom->ul = server_data->instantaneous_ops_per_sec;
            return PMDA_FETCH_STATIC;
        case 3:  /* total_net_input_bytes */
            atom->ull = server_data->total_net_input_bytes;
            return PMDA_FETCH_STATIC;
        case 4:  /* total_net_output_bytes */
            atom->ull = server_data->total_net_output_bytes;
            return PMDA_FETCH_STATIC;
        case 5:  /* rejected_connections */
            atom->ul = server_data->rejected_connections;
            return PMDA_FETCH_STATIC;
        case 6:  /* keyspace_hits */
            atom->ull = server_data->keyspace_hits;
            return PMDA_FETCH_STATIC;
        case 7:  /* keyspace_misses */
            atom->ull = server_data->keyspace_misses;
            return PMDA_FETCH_STATIC;
        case 8:  /* sync_full */
            atom->ul = server_data->sync_full;
            return PMDA_FETCH_STATIC;
        case 9:  /* sync_partial_ok */
            atom->ul = server_data->sync_partial_ok;
            return PMDA_FETCH_STATIC;
        case 10:  /* sync_partial_err */
            atom->ul = server_data->sync_partial_err;
            return PMDA_FETCH_STATIC;
        case 11:  /* expired_keys */
            atom->ull = server_data->expired_keys;
            return PMDA_FETCH_STATIC;
        case 12:  /* evicted_keys */
            atom->ull = server_data->evicted_keys;
            return PMDA_FETCH_STATIC;
        case 13:  /* pubsub_channels */
            atom->ul = server_data->pubsub_channels;
            return PMDA_FETCH_STATIC;
        case 14:  /* pubsub_patterns */
            atom->ul = server_data->pubsub_patterns;
            return PMDA_FETCH_STATIC;
        case 15:  /* latest_fork_usec */
            atom->ull = server_data->latest_fork_usec;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    case 4:  /* Replication metrics */
        switch (item) {
        case 0:  /* role */
            if (server_data->role == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->role;
            return PMDA_FETCH_STATIC;
        case 1:  /* connected_slaves */
            atom->ul = server_data->connected_slaves;
            return PMDA_FETCH_STATIC;
        case 2:  /* master_repl_offset */
            atom->l = server_data->master_repl_offset;
            return PMDA_FETCH_STATIC;
        case 3:  /* repl_backlog_active */
            atom->l = server_data->repl_backlog_active;
            return PMDA_FETCH_STATIC;
        case 4:  /* repl_backlog_size */
            atom->ull = server_data->repl_backlog_size;
            return PMDA_FETCH_STATIC;
        case 5:  /* repl_backlog_first_byte_offset */
            if (server_data->repl_backlog_first_byte_offset == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->repl_backlog_first_byte_offset;
            return PMDA_FETCH_STATIC;
        case 6:  /* repl_backlog_histlen */
            atom->ull = server_data->repl_backlog_histlen;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    case 5:  /* CPU metrics */
        switch (item) {
        case 0:  /* used_cpu_sys */
            atom->d = server_data->used_cpu_sys;
            return PMDA_FETCH_STATIC;
        case 1:  /* used_cpu_user */
            atom->d = server_data->used_cpu_user;
            return PMDA_FETCH_STATIC;
        case 2:  /* used_cpu_sys_children */
            atom->d = server_data->used_cpu_sys_children;
            return PMDA_FETCH_STATIC;
        case 3:  /* used_cpu_user_children */
            atom->d = server_data->used_cpu_user_children;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    case 6:  /* Persistence metrics */
        switch (item) {
        case 0:  /* loading */
            atom->ul = server_data->loading;
            return PMDA_FETCH_STATIC;
        case 1:  /* rdb_changes_since_last_save */
            atom->ull = server_data->rdb_changes_since_last_save;
            return PMDA_FETCH_STATIC;
        case 2:  /* rdb_bgsave_in_progress */
            atom->ul = server_data->rdb_bgsave_in_progress;
            return PMDA_FETCH_STATIC;
        case 3:  /* rdb_last_save_time */
            atom->ul = server_data->rdb_last_save_time;
            return PMDA_FETCH_STATIC;
        case 4:  /* rdb_last_bgsave_status */
            if (server_data->rdb_last_bgsave_status == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->rdb_last_bgsave_status;
            return PMDA_FETCH_STATIC;
        case 5:  /* rdb_last_bgsave_time_sec */
            atom->l = server_data->rdb_last_bgsave_time_sec;
            return PMDA_FETCH_STATIC;
        case 6:  /* rdb_current_bgsave_time_sec */
            atom->l = server_data->rdb_current_bgsave_time_sec;
            return PMDA_FETCH_STATIC;
        case 7:  /* aof_enabled */
            if (server_data->aof_enabled == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->aof_enabled;
            return PMDA_FETCH_STATIC;
        case 8:  /* aof_rewrite_in_progress */
            atom->l = server_data->aof_rewrite_in_progress;
            return PMDA_FETCH_STATIC;
        case 9:  /* aof_rewrite_scheduled */
            atom->l = server_data->aof_rewrite_scheduled;
            return PMDA_FETCH_STATIC;
        case 10:  /* aof_last_rewrite_time_sec */
            atom->l = server_data->aof_last_rewrite_time_sec;
            return PMDA_FETCH_STATIC;
        case 11:  /* aof_current_rewrite_time_sec */
            atom->l = server_data->aof_current_rewrite_time_sec;
            return PMDA_FETCH_STATIC;
        case 12:  /* aof_last_bgrewrite_status */
            if (server_data->aof_last_bgrewrite_status == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->aof_last_bgrewrite_status;
            return PMDA_FETCH_STATIC;
        case 13:  /* aof_last_write_status */
            if (server_data->aof_last_write_status == NULL) return PM_ERR_VALUE;
            atom->cp = server_data->aof_last_write_status;
            return PMDA_FETCH_STATIC;
        default:
            return PM_ERR_PMID;
        }

    default:
        return PM_ERR_PMID;
    }
}

/*
 * Parse configuration file and setup instance domain
 */
static void
valkey_parse_config(void)
{
    int sep = pmPathSeparator();
    FILE *fp;
    char buf[MAX_LINE_LEN];
    int inst_id = 0;

    pmsprintf(mypath, sizeof(mypath), "%s%c" "valkey" "%c" "valkey.conf",
              pmGetConfig("PCP_SYSCONF_DIR"), sep, sep);

    if ((fp = fopen(mypath, "r")) == NULL) {
        pmNotifyErr(LOG_ERR, "fopen on %s failed: %s",
                    mypath, pmErrStr(-oserror()));
        return;
    }

    /* Clear existing instances */
    pmdaCacheOp(*server_indom, PMDA_CACHE_INACTIVE);

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        char *p = buf;
        char *hostname, *port_str;
        int port;
        server_data_t *data;
        char name[MAX_LINE_LEN];
        int sts;

        /* Skip whitespace */
        while (*p && isspace((int)*p)) p++;

        /* Skip comments and empty lines */
        if (*p == '#' || *p == '\0') continue;

        /* Look for host= lines */
        if (strncmp(p, "host=", 5) != 0) continue;

        p += 5;
        hostname = p;

        /* Find the colon separator */
        port_str = strchr(hostname, ':');
        if (port_str == NULL) {
            pmNotifyErr(LOG_WARNING, "Invalid host line in %s: %s", mypath, buf);
            continue;
        }

        *port_str = '\0';
        port_str++;

        /* Remove trailing whitespace from hostname */
        char *end = port_str - 2;
        while (end > hostname && isspace((int)*end)) {
            *end = '\0';
            end--;
        }

        /* Parse port number */
        port = atoi(port_str);
        if (port <= 0 || port > 65535) {
            pmNotifyErr(LOG_WARNING, "Invalid port in %s: %s", mypath, buf);
            continue;
        }

        /* Create server data */
        data = calloc(1, sizeof(server_data_t));
        if (data == NULL) {
            pmNotifyErr(LOG_ERR, "calloc failed for server_data_t");
            continue;
        }

        data->hostname = strdup(hostname);
        data->port = port;
        data->context = NULL;
        data->last_update = 0;

        /* Add to instance domain */
        pmsprintf(name, sizeof(name), "%s:%d", hostname, port);
        sts = pmdaCacheStore(*server_indom, PMDA_CACHE_ADD, name, data);
        if (sts < 0) {
            pmNotifyErr(LOG_ERR, "pmdaCacheStore failed: %s", pmErrStr(sts));
            free_server_data(data);
            continue;
        }

        inst_id++;
    }

    fclose(fp);

    if (pmdaCacheOp(*server_indom, PMDA_CACHE_SIZE_ACTIVE) < 1)
        pmNotifyErr(LOG_WARNING, "No Valkey servers configured in %s", mypath);
}

/*
 * Wrapper for pmdaFetch
 */
static int
valkey_fetch(int numpmid, pmID pmidlist[], pmdaResult **resp, pmdaExt *pmda)
{
    return pmdaFetch(numpmid, pmidlist, resp, pmda);
}

/*
 * Initialise the agent
 */
void
valkey_init(pmdaInterface *dp)
{
    if (isDSO) {
        int sep = pmPathSeparator();
        pmsprintf(mypath, sizeof(mypath), "%s%c" "valkey" "%c" "help",
                  pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
        pmdaDSO(dp, PMDA_INTERFACE_7, "valkey DSO", mypath);
    } else {
        pmSetProcessIdentity(username);
    }

    if (dp->status != 0)
        return;

    dp->version.any.fetch = valkey_fetch;
    pmdaSetFetchCallBack(dp, valkey_fetchCallBack);

    pmdaInit(dp, indomtab, sizeof(indomtab)/sizeof(indomtab[0]),
             metrictab, sizeof(metrictab)/sizeof(metrictab[0]));

    /* Load configuration and setup instances */
    valkey_parse_config();
}

static pmLongOptions longopts[] = {
    PMDA_OPTIONS_HEADER("Options"),
    PMOPT_DEBUG,
    PMDAOPT_DOMAIN,
    PMDAOPT_LOGFILE,
    PMDAOPT_USERNAME,
    PMOPT_HELP,
    PMDA_OPTIONS_TEXT("\nExactly one of the following options may appear:"),
    PMDAOPT_INET,
    PMDAOPT_PIPE,
    PMDAOPT_UNIX,
    PMDAOPT_IPV6,
    PMDA_OPTIONS_END
};

static pmdaOptions opts = {
    .short_options = "D:d:i:l:pu:U:6:?",
    .long_options = longopts,
};

/*
 * Set up the agent if running as a daemon
 */
int
main(int argc, char **argv)
{
    int sep = pmPathSeparator();
    pmdaInterface dispatch;

    isDSO = 0;
    pmSetProgname(argv[0]);
    pmGetUsername(&username);

    pmsprintf(mypath, sizeof(mypath), "%s%c" "valkey" "%c" "help",
              pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
    pmdaDaemon(&dispatch, PMDA_INTERFACE_7, pmGetProgname(), VALKEY,
               "valkey.log", mypath);

    pmdaGetOptions(argc, argv, &opts, &dispatch);
    if (opts.errors) {
        pmdaUsageMessage(&opts);
        exit(1);
    }
    if (opts.username)
        username = opts.username;

    pmdaOpenLog(&dispatch);
    pmdaConnect(&dispatch);
    valkey_init(&dispatch);
    pmdaMain(&dispatch);

    exit(0);
}
