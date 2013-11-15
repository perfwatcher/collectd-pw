/**
 * collectd - src/jsonrpc_cb_perfwatcher.c
 * Copyright (C) 2012 Yves Mettier, Cyril Feraudet
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Yves Mettier <ymettier at free dot fr>
 *   Cyril Feraudet <cyril at feraudet dot com>
 **/

#include <sys/types.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>

#include "utils_avltree.h"
#include "utils_cache.h"
#include "common.h"
#include "plugin.h"
#include "jsonrpc.h"
#include "base64.h"
#include <json/json.h>
#include <rrd.h>
#include <rrd_client.h>
#define OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "JSONRPC plugin (perfwatcher) : "

extern char jsonrpc_datadir[];
extern char jsonrpc_rrdcached_daemon_address[];
extern char jsonrpc_rrdtool_path[];

/* #define RETURN_IF_WRONG_PARAMS_TYPE(params, type) {{{ */
#define RETURN_IF_WRONG_PARAMS_TYPE(params, type) do { \
        if(!json_object_is_type ((params), (type))) { \
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); \
        } \
} while(0)
/* }}} */

/* #define JSONRPC_FREE_AND_RETURN(gototarget) {{{ */
#define JSONRPC_FREE_AND_RETURN_0(rc, gototarget) do { \
        (rc) = 0; \
        goto gototarget; \
} while(0)
/* }}} */

/* #define JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT {{{ */
#define JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(gototarget) do { \
        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json object"); \
        DEBUG( OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__); \
        goto gototarget; \
} while(0)
/* }}} */

#ifndef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
static int jsonrpc_spawn_process(const char *path, char * const argv[], unsigned char **pngdata, size_t *pngsize) { /* {{{ */
#define JSONRPC_SPAWN_PROCESS_BUFFER_SIZE ((SSIZE_MAX) > 4096 ? 4096:(SSIZE_MAX))
        int rc = 0;       
        unsigned char buffer[JSONRPC_SPAWN_PROCESS_BUFFER_SIZE];
        int buffer_len = 0;
        int pos = 0;
        int pid = 0;
        int stdpipe[2];

        *pngdata = NULL;
        *pngsize = 0;

        if(-1 == pipe(stdpipe)) {
                return(JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        switch(pid = fork()) {
                case -1: /* failed to fork */
                        close(stdpipe[1]); /* Close it here because it will not be closed anywhere else */
                        goto jsonrpc_spawn_process__internal_error;
                case 0: /* child : execute the command */
                /* WARNING : using ERROR(), WARNING(), INFO(), DEBUG()... here
                 * may fail because of a lock. Do not use them here.
                 */
                        close(1);
                        dup(stdpipe[1]);
                        close(stdpipe[0]);
                        execv(path, argv);
                        /* This should not be executed */
                        perror("Could not execute");
                        exit(EXIT_FAILURE);
        }
        /* if we are here, fork() succeeded and we are the parent */
        close(stdpipe[1]);
        *pngsize = 1;
        pos = 0;
        errno = 0;
        while(0 < (buffer_len = read(stdpipe[0], buffer, JSONRPC_SPAWN_PROCESS_BUFFER_SIZE))) {
                *pngsize += buffer_len;
                if(NULL == (*pngdata = realloc(*pngdata, *pngsize))) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Memory allocation failed (%s:%d)", __FILE__, __LINE__);
                        *pngsize = 0;
                        goto jsonrpc_spawn_process__internal_error;
                }
                memcpy(*pngdata + pos, buffer, buffer_len);
                pos += buffer_len;
                if(buffer_len != JSONRPC_SPAWN_PROCESS_BUFFER_SIZE) break;
        }
        if(buffer_len == -1) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Read failed through the pipe; errno=%d (%s:%d)", errno, __FILE__, __LINE__);
                goto jsonrpc_spawn_process__internal_error;
        }
        if(*pngdata) (*pngdata)[*pngsize - 1] = '\0';

        JSONRPC_FREE_AND_RETURN_0(rc, jsonrpc_spawn_process__free_and_return);

        /* Error handling */
jsonrpc_spawn_process__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
/* jsonrpc_spawn_process__any_error: */
        if(*pngdata) free(*pngdata);
        *pngdata = NULL;
        *pngsize = 0;
jsonrpc_spawn_process__free_and_return:
        close(stdpipe[0]);
        if(0 != pid) {
                int status;
                kill(pid, SIGQUIT);
                waitpid(pid, &status, 0);
                if(0 != WEXITSTATUS(status)) {
                        if(*pngdata) free(*pngdata);
                        *pngdata = NULL;
                        *pngsize = 0;
                        if(0 == rc) rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
                }
        }
        return(rc);
} /* }}} jsonrpc_spawn_process */
#endif

static char *readlink_new(const char *path) { /* {{{ */
        char *linkstr = NULL;
        int linksize = 0;
        int linklen;
        ssize_t rc = -1;
        errno = ENAMETOOLONG;
        char *realpathstr = NULL;
        int realpathlen = 0;
        int pathlen;
        char *resultstr;

        while((-1 == rc) && (ENAMETOOLONG == errno)) {
                linksize += 1024;
                if(NULL == (linkstr = realloc(linkstr, linksize))) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory (%s:%d)", __FILE__, __LINE__);
                        return(NULL);
                }

                errno = 0;
                rc = readlink(path, linkstr, linksize);
        }
        if(-1 == rc) {
                if(linkstr) free(linkstr);
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "readlink(%s) failed, errno=%d (%s:%d)", path, errno, __FILE__, __LINE__);
                return(NULL);
        }
        /* Check that we have enough space to add a nul character at the end. */
        if(rc >= linksize) {
                linksize += 1;
                if(NULL == (linkstr = realloc(linkstr, linksize))) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory (%s:%d)", __FILE__, __LINE__);
                        return(NULL);
                }
        }
        /* Append a nul character at the end as readlink does not do it (too bad) */
        linkstr[rc] = '\0';

        /* Check if we have an absolute path or not */
        if(linkstr[0] == '/') return(linkstr);

        pathlen = strlen(path);
        assert(NULL != linkstr);
        linklen = strlen(linkstr);

        while((pathlen>0) && (path[pathlen] != '/')) pathlen--;
        if(0 == pathlen) return(linkstr); /* path was not absolute !!! */
        pathlen++;
        realpathlen = linklen + pathlen + 1;

        if(NULL == (realpathstr = malloc(realpathlen))) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory (%s:%d)", __FILE__, __LINE__);
                if(linkstr) free(linkstr);
                return(NULL);
        }

        memcpy(realpathstr, path, pathlen);
        memcpy(realpathstr + pathlen, linkstr, linklen + 1);
        free(linkstr);
        resultstr = realpath(realpathstr, NULL);
        free(realpathstr);

        return(resultstr);
} /* }}} readlink_new */

static int jsonrpc_datadir_append_string(const char *string, char **path, int is_hostname) { /* {{{ */
        size_t l, l1, l2;

        *path = NULL;
        /* Parse the string */
        if(is_hostname) {
                if(NULL != strchr(string, '/')) {
                        ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Found a '/' in parameter");
                        return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
                }
        }
        if((0 == strcmp(string, ".")) || (0 == strcmp(string, ".."))) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "'%s' should not be '.' or '..'", string);
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Create the path variable */
        l1 = strlen(jsonrpc_datadir[0]?jsonrpc_datadir:".");
        l2 = strlen(string);
        l = l1 + l2 + 2;
        if(NULL == (*path = malloc(l * sizeof(*path)))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        memcpy(*path, jsonrpc_datadir[0]?jsonrpc_datadir:".", l1);
        (*path)[l1] = '/';
        memcpy(*path+l1+1, string, l2+1);

        return(0);
} /* }}} jsonrpc_datadir_append_string */

static int jsonrpc_datadir_append_string_to_buffer(const char *string, char **buffer, int *buffer_size, int *datadir_len) { /* {{{ */
/* First run :
 * buffer = NULL;
 * buffer_size = 0;
 * datadir_len = 0;
 * rc = jsonrpc_datadir_append_string_to_buffer(rrd_file, &buffer, &buffer_size, &datadir_len);
 * ** this will allocate memory for the buffer. **
 *
 * Next runs (do not modify buffer, buffer_size and datadir_len) :
 * rc = jsonrpc_datadir_append_string_to_buffer(rrd_file, &buffer, &buffer_size, &datadir_len);
 */
        size_t l, string_len;
        int buffer_is_empty = 0;

        /* Parse the string */
        if((0 == strcmp(string, ".")) || (0 == strcmp(string, ".."))) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "'%s' should not be '.' or '..'", string);
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Create the path variable */
        if(0 == *datadir_len) {
                *datadir_len = strlen(jsonrpc_datadir[0]?jsonrpc_datadir:".");
                buffer_is_empty = 1;
        }
        string_len = strlen(string);
        l = *datadir_len + string_len + 2;
        if(l > *buffer_size) {
                *buffer_size = l + 10;
                if(NULL == (*buffer = realloc(*buffer, *buffer_size))) {
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                }
        }
        if(buffer_is_empty) {
                memcpy(*buffer, jsonrpc_datadir[0]?jsonrpc_datadir:".", *datadir_len);
                (*buffer)[*datadir_len] = '/';
                *datadir_len += 1;
        }
        memcpy(*buffer+*datadir_len, string, string_len+1);

        return(0);
} /* }}} jsonrpc_datadir_append_string_to_buffer */

static const char *jsonrpc_cb_get_param_string(struct json_object *params, char *key) { /* {{{ */
        const char *str = NULL;
        struct json_object *obj = NULL;

        /* Params : get the value for the given key */
        if(NULL == (obj = json_object_object_get(params, key))) return(NULL);
        if(!json_object_is_type (obj, json_type_string)) return(NULL);
        if(NULL == (str = json_object_get_string(obj))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return(NULL);
        }
        return(str);
} /* }}} jsonrpc_cb_get_param_string */

/* JSONRPC EXAMPLE SYNTAX for "pw_get_status" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_get_status",
       "params": { 
                    "timeout" : 240,
                    "server"  : [ "<list>", "<of>", "<hostnames>" ]
                 },
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_get_status (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        struct json_object *obj;
        struct json_object *result_servers_object;
        c_avl_tree_t *servers;
        cdtime_t *servers_status;
        cdtime_t now_before_timeout;
        cdtime_t *status_ptr;
        int timeout;
        struct array_list *al;
        struct json_object *server_array;
        int array_len;
        size_t i;
        char **names = NULL;
        cdtime_t *times = NULL;
        size_t number = 0;
        int cache_id;
        c_avl_iterator_t *avl_iter;
        char *key;
        char *buffer=NULL;
        int buffer_len = 0;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_object);

        /* Params : get the "timeout" */
        if(NULL == (obj = json_object_object_get(params, "timeout"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        timeout = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "server" array
         * and fill the server tree and the servers_status array
         */
        if(NULL == (server_array = json_object_object_get(params, "server"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (server_array, json_type_array)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        if(NULL == (servers = c_avl_create((void *) strcmp))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        al = json_object_get_array(server_array);
        assert(NULL != al);
        array_len = json_object_array_length (server_array);
        if(NULL == (servers_status = malloc(array_len * sizeof(*servers_status)))) {
                c_avl_destroy(servers);
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        for(i=0; i<array_len; i++) {
                struct json_object *element;
                const char *str;
                servers_status[i] = 0;
                element = json_object_array_get_idx(server_array, i);
                assert(NULL != element);
                if(!json_object_is_type (element, json_type_string)) {
                        c_avl_destroy(servers);
                        free(servers_status);
                        return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
                }
                if(NULL == (str = json_object_get_string(element))) {
                        c_avl_destroy(servers);
                        free(servers_status);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);

                }
                c_avl_insert(servers, (void*)str, &(servers_status[i]));
        }
        /* Get the names */
        cache_id = jsonrpc_cache_last_entry_find_and_ref (&names, &times, &number);
        if (cache_id == -1)
        {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "uc_get_names failed with status %i", cache_id);
                c_avl_destroy(servers);
                free(servers_status);
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        /* Parse the cache and update the servers_status array*/
        for (i = 0; i < number; i++) {
                size_t j;

                for(j=0; names[i][j] && names[i][j] != '/'; j++);
                if(j>= buffer_len) {
                        if(NULL == (buffer = realloc(buffer, j+1024))) {
                                c_avl_destroy(servers);
                                free(servers_status);
                                jsonrpc_cache_entry_unref(cache_id);
                                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                        }
                        buffer_len = j+1024;
                }
                memcpy(buffer, names[i],j);
                buffer[j] = '\0';

                if(0 == c_avl_get(servers, buffer, (void *) &status_ptr)) {
                        if(times[i] > *status_ptr) *status_ptr = times[i];
                }
        }
        jsonrpc_cache_entry_unref(cache_id);
        if(buffer) free(buffer);

        /* What time is it ? */
        now_before_timeout = cdtime();
        now_before_timeout -= (TIME_T_TO_CDTIME_T(timeout));


        /* Check the servers and build the result array */
        if(NULL == (result_servers_object = json_object_new_object())) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json array");
                c_avl_destroy(servers);
                free(servers_status);
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        /* Append the values to the array */
        avl_iter = c_avl_get_iterator(servers);
        while (c_avl_iterator_next (avl_iter, (void *) &key, (void *) &status_ptr) == 0) {
                if(*status_ptr == 0) {
                        obj =  json_object_new_string("unknown");
                } else if(*status_ptr > now_before_timeout) {
                        obj =  json_object_new_string("up");
                } else {
                        obj =  json_object_new_string("down");
                }


                if(NULL == obj) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json string");
                        c_avl_iterator_destroy(avl_iter);
                        json_object_put(result_servers_object);
                        c_avl_destroy(servers);
                        free(servers_status);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                }
                json_object_object_add(result_servers_object, key, obj);
        }
        c_avl_iterator_destroy(avl_iter);

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", result_servers_object);
        c_avl_destroy(servers);
        free(servers_status);

        return(0);
} /* }}} jsonrpc_cb_pw_get_status */

/* #define free_avl_tree_keys(tree) {{{ */
#define free_avl_tree_keys(tree) do {                                             \
			c_avl_iterator_t *it;                                                 \
			it = c_avl_get_iterator(tree);                                        \
			while (c_avl_iterator_next (it, (void *) &key, (void *) &useless_var) == 0) { \
					free(key);                                                    \
			}                                                                     \
			c_avl_iterator_destroy(it);                                           \
	} while(0)
/* }}} */

/* JSONRPC EXAMPLE SYNTAX for "pw_get_metric" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_get_metric",
       "params": [ "<list>", "<of>", "<hostnames>" ],
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_get_metric (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        struct json_object *result_metrics_array;
        c_avl_tree_t *servers;
        c_avl_tree_t *metrics;

        struct array_list *al;
        int array_len;
        size_t i;
        char **names = NULL;
        cdtime_t *times = NULL;
        size_t number = 0;
        int cache_id;
        c_avl_iterator_t *avl_iter;
        char *key;
        void *useless_var;
        char *buffer=NULL;
        int buffer_len = 0;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_array);

        if(NULL == (servers = c_avl_create((void *) strcmp))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        if(NULL == (metrics = c_avl_create((void *) strcmp))) {
                c_avl_destroy(servers);
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        al = json_object_get_array(params);
        assert(NULL != al);
        array_len = json_object_array_length (params);
        for(i=0; i<array_len; i++) {
                struct json_object *element;
                const char *str;
                element = json_object_array_get_idx(params, i);
                assert(NULL != element);
                if(!json_object_is_type (element, json_type_string)) {
                        c_avl_destroy(servers);
                        c_avl_destroy(metrics);
                        return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
                }
                if(NULL == (str = json_object_get_string(element))) {
                        c_avl_destroy(servers);
                        c_avl_destroy(metrics);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);

                }
                c_avl_insert(servers, (void*)str, (void*)NULL);
        }
        /* Get the names */
        cache_id = jsonrpc_cache_last_entry_find_and_ref (&names, &times, &number);
        if (cache_id == -1)
        {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "uc_get_names failed with status %i", cache_id);
                c_avl_destroy(servers);
                c_avl_destroy(metrics);
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        /* Parse the cache and update the metrics list */
        for (i = 0; i < number; i++) {
                size_t j;

                for(j=0; names[i][j] && names[i][j] != '/'; j++);
                assert(names[i][j] != '\0');
                if(j>= buffer_len) {
                        if(NULL == (buffer = realloc(buffer, j+1024))) {
                                c_avl_destroy(servers);
                                free_avl_tree_keys(metrics);
                                c_avl_destroy(metrics);
                                jsonrpc_cache_entry_unref(cache_id);
                                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                        }
                        buffer_len = j+1024;
                }
                memcpy(buffer, names[i],j);
                buffer[j] = '\0';

                if(
                                (0 == c_avl_get(servers, buffer, NULL)) /* if the name is in the list */
                                && (0 != c_avl_get(metrics, names[i]+j+1, NULL)) /* and if the metric is NOT already known */
                  ) {
                        char *m;
                        if(NULL == (m = strdup(names[i]+j+1))) {
                                c_avl_destroy(servers);
                                free_avl_tree_keys(metrics);
                                c_avl_destroy(metrics);
                                jsonrpc_cache_entry_unref(cache_id);
                                if(buffer) free(buffer);
                                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);

                        }
                        c_avl_insert(metrics, (void*)m, (void*)NULL);
                }
        }
        jsonrpc_cache_entry_unref(cache_id);
        if(buffer) free(buffer);

        /* Check the servers and build the result array */
        if(NULL == (result_metrics_array = json_object_new_array())) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json array");
                c_avl_destroy(servers);
                free_avl_tree_keys(metrics);
                c_avl_destroy(metrics);
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        /* Append the values to the array */
        avl_iter = c_avl_get_iterator(metrics);
        while (c_avl_iterator_next (avl_iter, (void *) &key, (void *) &useless_var) == 0) {
                struct json_object *obj;

                if(NULL == (obj =  json_object_new_string(key))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json string");
                        c_avl_iterator_destroy(avl_iter);
                        json_object_put(result_metrics_array);
                        c_avl_destroy(servers);
                        free_avl_tree_keys(metrics);
                        c_avl_destroy(metrics);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                }
                json_object_array_add(result_metrics_array,obj);
        }
        c_avl_iterator_destroy(avl_iter);

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", result_metrics_array);
        c_avl_destroy(servers);
        free_avl_tree_keys(metrics);
        c_avl_destroy(metrics);

        return(0);
} /* }}} jsonrpc_cb_pw_get_metric */

static int get_dir_files_into_resultobject(const char *path, struct json_object *resultobject) { /* {{{ */
        DIR *dh = NULL;
        struct dirent *f = NULL;
        struct dirent *fr = NULL;
        int r;
        int rc;
        size_t len;
        size_t nb;
        struct json_object *array = NULL;
        struct json_object *obj = NULL;

        /* Open the datadir directory */
        if(NULL == (dh = opendir(path))) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not open datadir '%s' (this is not an error)", path);
                return(0);
        }

        /* Allocate the dirent structure */
        len = offsetof(struct dirent, d_name) + pathconf(path, _PC_NAME_MAX) + 1;
        if(NULL == (f = malloc(len))) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory");
                goto get_dir_files_into_resultobject__internal_error;
        }

        /* Create the array of values */
        if(NULL == (array = json_object_new_array())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(get_dir_files_into_resultobject__internal_error);
        }

        /* Append the contents of the datadir directory to the array */
        nb = 0;
        while((0 == (r = readdir_r(dh, f ,&fr))) && (NULL != fr)) {
                if(0 == strcmp(f->d_name, ".")) continue;
                if(0 == strcmp(f->d_name, "..")) continue;
                if(NULL == (obj = json_object_new_string(f->d_name))) {
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(get_dir_files_into_resultobject__internal_error);
                }
                json_object_array_add(array,obj);
                nb += 1;
        }
        closedir(dh); dh = NULL;
        free(f); f = NULL;
        /* Check if something went wrong */
        if(0 != r) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not read a directory entry in datadir");
                goto get_dir_files_into_resultobject__internal_error;
        }

        json_object_object_add(resultobject, "values", array);
        array = NULL; /* do not free */

        /* Insert the nb of values in the result object */
        if(NULL == (obj = json_object_new_int((int)nb))) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(get_dir_files_into_resultobject__internal_error);
        }
        json_object_object_add(resultobject, "nb", obj);

        /* Insert the datadir in the result object */
        if(NULL == (obj = json_object_new_string(jsonrpc_datadir[0]?jsonrpc_datadir:"."))) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(get_dir_files_into_resultobject__internal_error);
        }
        json_object_object_add(resultobject, "datadir", obj);

        return(0);

get_dir_files_into_resultobject__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
/* get_dir_files_into_resultobject__any_error: */
        if(dh) closedir(dh);
        if(f) free(f);
        if(array) json_object_put(array);
        return(rc);
} /* }}} get_dir_files_into_resultobject */

/* #define GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR {{{ */
#define GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR do { \
        DEBUG( OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__); \
        goto get_dir_rrd_contents_into_resultobject__internal_error; \
} while(0)
/* }}} */
static int get_dir_rrd_contents_into_resultobject(const char *path, struct json_object *resultobject, int level, size_t *nb) { /* {{{ */
        DIR *dh = NULL;
        struct dirent *f = NULL;
        struct dirent *fr;
        int r;
        size_t len;
        char *current_path = NULL;
        int current_path_len = 0;
        char *current_path_filename;
        int path_len;
        char *current_filename = NULL;
        int current_filename_len = 0;
        int rc;

        /* Open the datadir directory */
        if(NULL == (dh = opendir(path))) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not open datadir '%s' (this is not an error)", path);
                return(0);
        }

        /* Allocate the dirent structure */
        len = offsetof(struct dirent, d_name) + pathconf(path, _PC_NAME_MAX) + 1;
        if(NULL == (f = malloc(len))) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory");
                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
        }

        /* Allocate buffer for filename (plugin/instance or type/instance) */
        current_filename_len = 128;
        if(NULL == (current_filename = malloc(current_filename_len))) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory");
                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
        }

        /* Allocate buffer for path+filename */
        path_len = strlen(path);
        current_path_len = path_len + current_filename_len + 1;
        if(NULL == (current_path = malloc(current_path_len))) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory");
                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
        }
        memcpy(current_path, path, path_len);
        current_path_filename = current_path + path_len;
        current_path_filename[0] = '/';
        current_path_filename++;

        /* Append the contents of the datadir directory to the resultobject */
        while((0 == (r = readdir_r(dh, f ,&fr))) && (NULL != fr)) {
                struct stat statbuf;
                int l;
                char *instance;
                struct json_object *obj_p, *obj_i;
                if(0 == strcmp(f->d_name, ".")) continue;
                if(0 == strcmp(f->d_name, "..")) continue;

                /* Check the size of the 2 buffers */
                l = strlen(f->d_name);
                if(current_filename_len < (1 + l)) {
                        current_filename_len = l + 128;
                        current_path_len = path_len + current_filename_len + 1;
                        if(NULL == (current_filename = realloc(current_filename, current_filename_len))) {
                                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory");
                                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
                        }
                        if(NULL == (current_path = realloc(current_path, current_path_len))) {
                                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory");
                                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
                        }
                }
                /* Copy the filename to the buffer, then parse for first '-'. */
                memcpy(current_path_filename, f->d_name, l+1);
                if(0 != stat(current_path, &statbuf)) continue;
                memcpy(current_filename, f->d_name, l+1);
                if(level > 1) {
                        if(!strcmp(current_filename+l-4, ".rrd")) {
                                /* Check for .rrd extension and remove it. */
                                l -= 4;
                                current_filename[l] = '\0';
                        } else {
                                continue; /* keep only files with rrd extension */
                        }
                }
                instance = strchr(current_filename, '-');
                if(instance) {
                        instance[0] = '\0';
                        instance++;
                        if('\0' == instance[0]) instance = NULL;
                }
                /* Find the object with current plugin or type. If not found,
                 * create it.
                 * Result is obj_p.
                 */
                if(NULL == (obj_p = json_object_object_get(resultobject, current_filename))) {
                        if(NULL == (obj_p = json_object_new_object())) {
                                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json object");
                                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
                        }
                        json_object_object_add(resultobject, current_filename, obj_p);
                }

                if( (S_ISDIR(statbuf.st_mode) && (level == 1)) || (level > 1)) {
                        /* Add the dir/file into the object */
                        if(NULL == (obj_i = json_object_new_object())) {
                                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json object");
                                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
                        }
                        json_object_object_add(obj_p, instance?instance:"", obj_i);

                        /* If this is a dir, fill it recursively */
                        if(S_ISDIR(statbuf.st_mode)) {
                                if(0 != (rc = get_dir_rrd_contents_into_resultobject(current_path, obj_i, level + 1, nb))) {
                                        goto get_dir_rrd_contents_into_resultobject__any_error;
                                }
                        } else {
                                *nb += 1;
                        }
                }
        }
        closedir(dh); dh = NULL;
        free(f); f = NULL;
        free(current_path); current_path = NULL;
        free(current_filename); current_filename = NULL;

        /* Check if something went wrong */
        if(0 != r) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not read a directory entry in datadir");
                GET_DIR_RRD_CONTENTS_INTO_RESULTOBJECT__INTERNAL_ERROR;
        }

        return(0);

get_dir_rrd_contents_into_resultobject__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
get_dir_rrd_contents_into_resultobject__any_error:
        if(dh) closedir(dh);
        if(f) free(f);
        if(current_path) free(current_path);
        if(current_filename) free(current_filename);
        return (rc);
} /* }}} get_dir_rrd_contents_into_resultobject */

/* JSONRPC EXAMPLE SYNTAX for "pw_get_dir_hosts" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_get_dir_hosts",
       "params": "<some hostname>",
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_get_dir_hosts (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        int r;
        struct json_object *resultobject;

        *errorstring = NULL;

        /* Create the result object */
        if(NULL == (resultobject = json_object_new_object())) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not create a json object");
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        /* Read the datadir directory */
        r = get_dir_files_into_resultobject(jsonrpc_datadir[0]?jsonrpc_datadir:".", resultobject);
        if(0 != r) {
                json_object_put(resultobject);
                return(r);
        }

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);

        return(0);
} /* }}} jsonrpc_cb_pw_get_dir_hosts */

/* JSONRPC EXAMPLE SYNTAX for "pw_get_dir_plugins" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_get_dir_plugins",
       "params": { "hostname" : "<some hostname>" },
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_get_dir_plugins (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        char *path = NULL;
        const char *str = NULL;
        int r, rc;
        struct json_object *resultobject = NULL;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_object);

        /* Params : get the "hostname" */
        if(NULL == (str = jsonrpc_cb_get_param_string(params, "hostname"))) {
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_plugins__any_error;
        }

        if(0 != (r = jsonrpc_datadir_append_string(str, &path, 1))) {
                rc = r;
                goto jsonrpc_cb_pw_get_dir_plugins__any_error;
                return(r);
        }

        /* Create the result object */
        if(NULL == (resultobject = json_object_new_object())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_get_dir_plugins__internal_error);
        }

        /* Read the 'path' directory */
        r = get_dir_files_into_resultobject(path, resultobject);
        free(path);
        path = NULL;
        if(0 != r) {
                rc = r;
                goto jsonrpc_cb_pw_get_dir_plugins__any_error;
        }

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);

        return(0);
jsonrpc_cb_pw_get_dir_plugins__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_get_dir_plugins__any_error:
        if(resultobject) json_object_put(resultobject);
        if(path) free(path);
        return(rc);
} /* }}} jsonrpc_cb_pw_get_dir_plugins */

/* JSONRPC EXAMPLE SYNTAX for "pw_get_dir_types" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_get_dir_types",
       "params": { "hostname" : "<some hostname>", "plugin" : "<a plugin or a plugin-instance>" },
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_get_dir_types (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        char *path = NULL;
        const char *str_hostname = NULL;
        const char *str_plugins = NULL;
        size_t l, l1, l2, l3;
        int r, rc;
        struct json_object *resultobject = NULL;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_object);

        /* Params : get the "hostname" */
        if(NULL == (str_hostname = jsonrpc_cb_get_param_string(params, "hostname"))) {
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }

        /* Params : get the "plugins" */
        if(NULL == (str_plugins = jsonrpc_cb_get_param_string(params, "plugin"))) {
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }

        /* Parse the hostname */
        if(NULL != strchr(str_hostname, '/')) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Found a '/' in parameter");
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }
        if((0 == strcmp(str_hostname, ".")) || (0 == strcmp(str_hostname, ".."))) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "'%s' is not a hostname", str_hostname);
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }

        /* Parse the plugins */
        if(NULL != strchr(str_plugins, '/')) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Found a '/' in parameter");
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }
        if((0 == strcmp(str_plugins, ".")) || (0 == strcmp(str_plugins, ".."))) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "'%s' is not a plugin(-instance)", str_plugins);
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }

        /* Create the path variable */
        l1 = strlen(jsonrpc_datadir[0]?jsonrpc_datadir:".");
        l2 = strlen(str_hostname);
        l3 = strlen(str_plugins);
        l = l1 + l2 + l3 + 3;
        if(NULL == (path = malloc(l * sizeof(*path)))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                goto jsonrpc_cb_pw_get_dir_types__internal_error;
        }
        memcpy(path, jsonrpc_datadir[0]?jsonrpc_datadir:".", l1);
        path[l1] = '/';
        memcpy(path+l1+1, str_hostname, l2);
        path[l1+l2+1] = '/';
        memcpy(path+l1+l2+2, str_plugins, l3+1);

        /* Create the result object */
        if(NULL == (resultobject = json_object_new_object())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_get_dir_types__internal_error);
        }

        /* Read the 'path' directory */
        r = get_dir_files_into_resultobject(path, resultobject);
        free(path);
        path = NULL;
        if(0 != r) {
                rc = r;
                goto jsonrpc_cb_pw_get_dir_types__any_error;
        }

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);

        return(0);
jsonrpc_cb_pw_get_dir_types__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_get_dir_types__any_error:
        if(resultobject) json_object_put(resultobject);
        if(path) free(path);
        return(rc);
} /* }}} jsonrpc_cb_pw_get_dir_types */

/* JSONRPC EXAMPLE SYNTAX for "pw_get_dir_all_rrds_for_host" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_get_dir_all_rrds_for_host",
       "params": { "hostname" : "<some hostname>" },
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_get_dir_all_rrds_for_host (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        int rc;
        int r;
        struct json_object *resultobject = NULL;
        struct json_object *obj_rrds = NULL;
        struct json_object *obj_nb = NULL;
        char *path = NULL;
        const char *str;
        size_t nb;

        *errorstring = NULL;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_object);

        /* Params : get the "hostname" */
        if(NULL == (str = jsonrpc_cb_get_param_string(params, "hostname"))) {
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_get_dir_all_rrds_for_host__any_error;
        }

        if(0 != (r = jsonrpc_datadir_append_string(str, &path, 1))) {
                rc = r;
                goto jsonrpc_cb_pw_get_dir_all_rrds_for_host__any_error;
        }

        /* Create the result object */
        if(NULL == (resultobject = json_object_new_object())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_get_dir_all_rrds_for_host__internal_error);
        }

        /* Create the rrds object */
        if(NULL == (obj_rrds = json_object_new_object())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_get_dir_all_rrds_for_host__internal_error);
        }

        nb = 0;
        if(0 != (rc = get_dir_rrd_contents_into_resultobject(path, obj_rrds, 1, &nb))) {
                goto jsonrpc_cb_pw_get_dir_all_rrds_for_host__any_error;
        }

        json_object_object_add(resultobject, "values", obj_rrds);
        free(path);

        /* Insert the nb of values in the result object */
        if(NULL == (obj_nb = json_object_new_int((int)nb))) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_get_dir_all_rrds_for_host__internal_error);
        }
        json_object_object_add(resultobject, "nb", obj_nb);

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);

        return(0);

jsonrpc_cb_pw_get_dir_all_rrds_for_host__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_get_dir_all_rrds_for_host__any_error:
        if(resultobject) json_object_put(resultobject);
        if(obj_rrds) json_object_put(obj_rrds);
        if(path) free(path);
        return(rc);
} /* }}} jsonrpc_cb_pw_get_dir_all_rrds_for_host */

/* JSONRPC EXAMPLE SYNTAX for "pw_rrd_info" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_rrd_info",
       "params": { "rrdfile" : "<some host/plugin-instance/type-instance.rrd filename>" },
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_rrd_info (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        int rc;
        int r;
        rrd_info_t *rrdinfo_data = NULL;
        rrd_info_t *p = NULL;
        struct json_object *resultobject = NULL;
        struct json_object *obj_ds = NULL;
        struct json_object *obj_rra = NULL;
        struct json_object *obj_rrd = NULL;
        char *path = NULL;
        const char *str;
        char *keybuffer = NULL;
        int keybuffer_size = 0;;

        *errorstring = NULL;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_object);

        /* Params : get the "hostname" */
        if(NULL == (str = jsonrpc_cb_get_param_string(params, "rrdfile"))) {
                rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                goto jsonrpc_cb_pw_rrd_info__any_error;
        }

        if(0 != (r = jsonrpc_datadir_append_string(str, &path, 0))) {
                rc = r;
                goto jsonrpc_cb_pw_rrd_info__any_error;
        }

        if(NULL == (rrdinfo_data = rrd_info_r(path))) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not open and parse '%s'", path);
                goto jsonrpc_cb_pw_rrd_info__internal_error;
        }

        /* Create the result object */
        if(NULL == (resultobject = json_object_new_object())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
        }

        for(p=rrdinfo_data; p; p = p->next) {
                char *key_instance;
                int l;
                int offset = 0;
                struct json_object *obj = NULL;
                struct json_object *obj_infoval = NULL;
                struct json_object *obj_instance = NULL;
                if(!strncmp(p->key, "ds[", 3)) {
                        offset = 3;
                        if((NULL == obj_ds) && (NULL == (obj_ds = json_object_new_object()))) {
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                        }
                        obj = obj_ds;
                } else if(!strncmp(p->key, "rra[", 4)) {
                        offset = 4;
                        if((NULL == obj_rra) && (NULL == (obj_rra = json_object_new_object()))) {
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                        }
                        obj = obj_rra;
                } else {
                        offset = 0;
                        if((NULL == obj_rrd) && (NULL == (obj_rrd = json_object_new_object()))) {
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                        }
                        obj = obj_rrd;
                }

                if(offset > 0) {
                        if(NULL == (key_instance = strchr(p->key, ']'))) continue;
                        l = key_instance - p->key - offset;
                        if(l >= keybuffer_size) {
                                keybuffer_size = l+64;
                                keybuffer = realloc(keybuffer, keybuffer_size);
                        }
                        memcpy(keybuffer, p->key + offset, l);
                        keybuffer[l] = '\0';
                        if(NULL == (key_instance = strchr(key_instance, '.'))) continue;
                        key_instance++;

                } else {
                        key_instance = p->key;
                }

                switch(p->type) {
                        case RD_I_VAL:
                                if(NULL == (obj_infoval = json_object_new_int((double)(p->value).u_val))) {
                                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                                }
                                break;
                        case RD_I_CNT:
                                if(NULL == (obj_infoval = json_object_new_int((int)(p->value).u_cnt))) {
                                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                                }
                                break;
                        case RD_I_INT:
                                if(NULL == (obj_infoval = json_object_new_int((int)(p->value).u_int))) {
                                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                                }
                                break;
                        case RD_I_STR:
                                if(NULL == (obj_infoval = json_object_new_string((p->value).u_str))) {
                                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                                }
                                break;
                        default : /* not supported*/
                                continue;
                }
                if(offset > 0) {
                        if(NULL == (obj_instance = json_object_object_get(obj, keybuffer))) {
                                if(NULL == (obj_instance = json_object_new_object())) {
                                        json_object_put(obj_infoval);
                                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_info__internal_error);
                                }
                                json_object_object_add(obj, keybuffer, obj_instance);
                        }
                        obj = obj_instance;
                }
                json_object_object_add(obj, key_instance, obj_infoval);
        }
        rrd_info_free(rrdinfo_data);

        if(obj_rrd) json_object_object_add(resultobject, "rrd", obj_rrd);
        if(obj_ds) json_object_object_add(resultobject, "DS", obj_ds);
        if(obj_rra) json_object_object_add(resultobject, "RRA", obj_rra);


        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);
        if(path) free(path);
        if(keybuffer) free(keybuffer);

        return(0);

jsonrpc_cb_pw_rrd_info__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_rrd_info__any_error:
        if(resultobject) json_object_put(resultobject);
        if(obj_rrd) json_object_put(obj_rrd);
        if(obj_rra) json_object_put(obj_rra);
        if(obj_ds) json_object_put(obj_ds);
        if(path) free(path);
        if(keybuffer) free(keybuffer);
        if(rrdinfo_data) rrd_info_free(rrdinfo_data);
        return(rc);
} /* }}} jsonrpc_cb_pw_rrd_info */

/* JSONRPC EXAMPLE SYNTAX for "pw_rrd_check_files" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_rrd_check_files",
       "params": [ "<list>", "<of>", "<rrd files>" ],
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_rrd_check_files (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        int rc;
        struct array_list *al;
        struct json_object *resultobject = NULL;
        int array_len;
        int i;
        char *rrd_file_path = NULL;
        int rrd_file_path_size = 0;
        int datadir_len = 0;

        *errorstring = NULL;

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_array);

        if(NULL == (resultobject = json_object_new_array())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
        }

        al = json_object_get_array(params);
        assert(NULL != al);
        array_len = json_object_array_length (params);
        for(i=0; i<array_len; i++) {
                struct stat statbuf;
                struct json_object *element;
                struct json_object *obj;
                struct json_object *obj_element;
                const char *rrd_filename;
                const char *rrd_file_to_test;

                element = json_object_array_get_idx(params, i);
                assert(NULL != element);
                if(!json_object_is_type (element, json_type_string)) {
                        rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                        goto jsonrpc_cb_pw_rrd_check_files__any_error;
                }
                if(NULL == (rrd_filename = json_object_get_string(element))) {
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        goto jsonrpc_cb_pw_rrd_check_files__internal_error;

                }

                /* Add file name to the result element */
                if(NULL == (obj = json_object_new_object())) {
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                }
                if(NULL == (obj_element = json_object_new_string(rrd_filename))) {
                        json_object_put(obj);
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                }
                json_object_object_add(obj, "file", obj_element);


                if(rrd_filename[0] == '/') {
                        rrd_file_to_test = rrd_filename;
                } else {
                        if(0 != (rc = jsonrpc_datadir_append_string_to_buffer(rrd_filename, &rrd_file_path, &rrd_file_path_size, &datadir_len))) {
                                json_object_put(obj);
                                goto jsonrpc_cb_pw_rrd_check_files__any_error;
                        }
                        if(NULL == (obj_element = json_object_new_string(rrd_file_path))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        json_object_object_add(obj, "path", obj_element);

                        rrd_file_to_test = rrd_file_path;
                }


                /* Read info about the file  */
                if(0 != lstat(rrd_file_to_test, &statbuf)) {
                        if(NULL == (obj_element = json_object_new_string("ERR"))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        json_object_object_add(obj, "type", obj_element);
                        json_object_array_add(resultobject, obj);
                        continue;
                }
                /* Add file type to the result element */
                if(S_ISREG(statbuf.st_mode)) {
                        /* This is a regular file */
                        if(NULL == (obj_element = json_object_new_string("REG"))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        json_object_object_add(obj, "type", obj_element);
                } else if(S_ISLNK(statbuf.st_mode)) {
                        /* This is a link */
                        char *lnk;
                        if(NULL == (lnk = readlink_new(rrd_file_to_test))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        if(NULL == (obj_element = json_object_new_string(lnk))) {
                                free(lnk);
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        json_object_object_add(obj, "linked_to", obj_element);
                        free(lnk);

                        if(NULL == (obj_element = json_object_new_string("LNK"))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        json_object_object_add(obj, "type", obj_element);
                } else {
                        /* This is an unsupported type */
                        if(NULL == (obj_element = json_object_new_string("BAD"))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_check_files__internal_error);
                        }
                        json_object_object_add(obj, "type", obj_element);
                }

                json_object_array_add(resultobject, obj);
        }

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);

        if(rrd_file_path) free(rrd_file_path);

        return(0);

jsonrpc_cb_pw_rrd_check_files__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_rrd_check_files__any_error:
        if(resultobject) json_object_put(resultobject);
        if(rrd_file_path) free(rrd_file_path);
        return(rc);
} /* }}} jsonrpc_cb_pw_rrd_check_files */

/* JSONRPC EXAMPLE SYNTAX for "pw_rrd_flush" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_rrd_flush",
       "params": [ "<list>", "<of>", "<rrd files>" ],
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_rrd_flush (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        int rc;
        struct array_list *al;
        int array_len;
        struct json_object *resultobject = NULL;
        int i;
        char *rrd_file_path = NULL;
        int rrd_file_path_size = 0;
        int datadir_len = 0;
        int flush_result_code = 1;
        int status;

        *errorstring = NULL;
        /* Check first if we are able to flush */
        if('\0' == jsonrpc_rrdcached_daemon_address[0]) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "RRDCachedDaemonAddress is not defined in the configuration file. It is needed if you want to flush.");
                if(NULL == (resultobject = json_object_new_boolean(1))) {
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_flush__internal_error);
                }
                json_object_object_add(result, "result", resultobject);
                return(0);
        }

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_array);

        if(0 != (status = rrdc_connect (jsonrpc_rrdcached_daemon_address))) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "rrdc_connect (%s) failed with status %d.", jsonrpc_rrdcached_daemon_address, status);
                goto jsonrpc_cb_pw_rrd_flush__internal_error;
        }

        al = json_object_get_array(params);
        assert(NULL != al);
        array_len = json_object_array_length (params);
        for(i=0; i<array_len; i++) {
                struct json_object *element;
                struct json_object *obj;
                struct json_object *obj_element;
                const char *rrd_filename;
                const char *rrd_file_to_flush;

                element = json_object_array_get_idx(params, i);
                assert(NULL != element);
                if(!json_object_is_type (element, json_type_string)) {
                        rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                        goto jsonrpc_cb_pw_rrd_flush__any_error;
                }
                if(NULL == (rrd_filename = json_object_get_string(element))) {
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        goto jsonrpc_cb_pw_rrd_flush__internal_error;

                }

                /* Add file name to the result element */
                if(NULL == (obj = json_object_new_object())) {
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_flush__internal_error);
                }
                if(NULL == (obj_element = json_object_new_string(rrd_filename))) {
                        json_object_put(obj);
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_flush__internal_error);
                }
                json_object_object_add(obj, "file", obj_element);


                if(rrd_filename[0] == '/') {
                        rrd_file_to_flush = rrd_filename;
                } else {
                        if(0 != (rc = jsonrpc_datadir_append_string_to_buffer(rrd_filename, &rrd_file_path, &rrd_file_path_size, &datadir_len))) {
                                json_object_put(obj);
                                goto jsonrpc_cb_pw_rrd_flush__any_error;
                        }
                        if(NULL == (obj_element = json_object_new_string(rrd_file_path))) {
                                json_object_put(obj);
                                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_flush__internal_error);
                        }
                        json_object_object_add(obj, "path", obj_element);

                        rrd_file_to_flush = rrd_file_path;

                }
                if(0 != (status = rrdc_flush (rrd_file_to_flush))) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "rrdc_flush (%s) failed with status %d.", rrd_file_to_flush, status);
                        flush_result_code = 0;
                } else {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "rrdc_flush (%s): Success.", rrd_file_to_flush);
                }
        }

        rrdc_disconnect();

        /* Last : add the "result" to the result object */
        if(NULL == (resultobject = json_object_new_boolean(flush_result_code))) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_flush__internal_error);
        }
        json_object_object_add(result, "result", resultobject);

        if(rrd_file_path) free(rrd_file_path);

        return(0);

jsonrpc_cb_pw_rrd_flush__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_rrd_flush__any_error:
        if(resultobject) json_object_put(resultobject);
        if(rrd_file_path) free(rrd_file_path);
        return(rc);
} /* }}} jsonrpc_cb_pw_rrd_flush */

/* jsonrpc example syntax for "pw_rrd_graphonly" {{{
   {
       "jsonrpc": "2.0",
       "method" : "pw_rrd_graphonly",
       "params": [ "<list>", "<of>", "<rrd>", "<command>", "<lines>" ],
       "id": 3
   }
}}} */
int jsonrpc_cb_pw_rrd_graphonly (struct json_object *params, struct json_object *result, const char **errorstring) { /* {{{ */
        int rc;
        struct array_list *al;
        struct json_object *resultobject = NULL;
        int array_len;
        int i;
        int graph_argc;
        const char ** graph_argv = NULL;
        unsigned char *pngdata = NULL;
        size_t pngsize = 0;
        char *pngstr_b64 = NULL;
        size_t pnglen_b64;
        struct json_object *obj = NULL;
#ifdef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
#define JSONRPC_CB_PW_RRD_GRAPHONLY__ARG_OFFSET 2
        rrd_info_t *grinfo = NULL;
        rrd_info_t *walker;
#else
#define JSONRPC_CB_PW_RRD_GRAPHONLY__ARG_OFFSET 3
        int flush_before = 1;
#endif

        *errorstring = NULL;

#ifndef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
        /* Check first rrdtool path was defined */
        if('\0' == jsonrpc_rrdtool_path[0]) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "RRDToolPath is not defined in the configuration file. It is needed if you want to graph.");
                if(NULL == (resultobject = json_object_new_boolean(1))) {
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_graphonly__internal_error);
                }
                json_object_object_add(result, "result", resultobject);
                return(0);
        }


        /* Check first if we are able to flush */
        if('\0' == jsonrpc_rrdcached_daemon_address[0]) {
                WARNING (OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "RRDCachedDaemonAddress is not defined in the configuration file. It is needed if you want to flush.");
                flush_before = 0;
        }
#endif

        /* Parse the params */
        RETURN_IF_WRONG_PARAMS_TYPE(params, json_type_array);

        al = json_object_get_array(params);
        assert(NULL != al);
        array_len = json_object_array_length (params);

        /* Prepare the "rrdtool graph" command line */
        graph_argc = array_len + JSONRPC_CB_PW_RRD_GRAPHONLY__ARG_OFFSET;
#ifndef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
        graph_argc += flush_before?2:0;
#endif

        /* Allocate for graph_argc elements plus 1 NULL element for NULL
         * terminated array
         */
        if(NULL == (graph_argv = calloc(1 + graph_argc, sizeof(*graph_argv)))) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory %s:%d", __FILE__, __LINE__);
                goto jsonrpc_cb_pw_rrd_graphonly__internal_error;
        }
#ifndef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
        graph_argv[0] = jsonrpc_rrdtool_path;
#endif
        graph_argv[JSONRPC_CB_PW_RRD_GRAPHONLY__ARG_OFFSET-2] = "graph";
        graph_argv[JSONRPC_CB_PW_RRD_GRAPHONLY__ARG_OFFSET-1] = "-";

        for(i=0; i<array_len; i++) {
                struct json_object *element;

                element = json_object_array_get_idx(params, i);
                assert(NULL != element);
                if(!json_object_is_type (element, json_type_string)) {
                        rc = JSONRPC_ERROR_CODE_32602_INVALID_PARAMS;
                        goto jsonrpc_cb_pw_rrd_graphonly__any_error;
                }
                if(NULL == (graph_argv[JSONRPC_CB_PW_RRD_GRAPHONLY__ARG_OFFSET+i] = json_object_get_string(element))) {
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                        goto jsonrpc_cb_pw_rrd_graphonly__internal_error;
                }
        }

        /* Create the result object */
        if(NULL == (resultobject = json_object_new_object())) {
                JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_graphonly__internal_error);
        }

#ifdef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
        /* Generate the graph (with rrd_graph_v) */
        if(NULL == (grinfo = rrd_graph_v(graph_argc, (char **)graph_argv))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d", __FILE__, __LINE__);
                goto jsonrpc_cb_pw_rrd_graphonly__internal_error;
        }

        for(walker = grinfo; walker; walker = walker->next) {
                if(! strcmp(walker->key, "image")) {
                        pngdata = walker->value.u_blo.ptr;
                        pngsize = walker->value.u_blo.size;
                        break;

                }
        }
#else
        /* Generate the graph (with "rrdtool graph -") */
        if(flush_before) {
                graph_argv[graph_argc - 2] = "--daemon";
                graph_argv[graph_argc - 1] = jsonrpc_rrdcached_daemon_address;
                graph_argv[graph_argc - 0] = NULL; /* we allocated graph_argc + 1 elements so there is no bug here ! */
        }
        rc = jsonrpc_spawn_process(jsonrpc_rrdtool_path, (char * const *) graph_argv, &pngdata, &pngsize);
        if(0 != rc) {
                goto jsonrpc_cb_pw_rrd_graphonly__any_error;
        }
#endif

        /* Encode the image */
        if(pngdata) {
                pnglen_b64 = base64_encode_alloc ((const char *)pngdata, pngsize - 1, &pngstr_b64);

                if (NULL == pngstr_b64) {
                        if(0 == pnglen_b64) {
                                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Internal error %s:%d (buffer for base64 conversion is too small !?)", __FILE__, __LINE__);
                        } else {
                                ERROR(OUTPUT_PREFIX_JSONRPC_CB_PERFWATCHER "Could not allocate memory %s:%d", __FILE__, __LINE__);
                        }
                        goto jsonrpc_cb_pw_rrd_graphonly__internal_error;
                }
                /* Add the image to the result */
                if(NULL == (obj = json_object_new_string(pngstr_b64))) {
                        JSONRPC_CB_COULD_NOT_CREATE_A_JSON_OBJECT(jsonrpc_cb_pw_rrd_graphonly__internal_error);
                }
                json_object_object_add(resultobject, "image", obj);
        }

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", resultobject);

        if(graph_argv) free(graph_argv);
#ifdef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
        if(grinfo) rrd_info_free(grinfo);
#else
        if(pngdata) free(pngdata);
#endif

        return(0);

jsonrpc_cb_pw_rrd_graphonly__internal_error:
        rc = JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR;
jsonrpc_cb_pw_rrd_graphonly__any_error:
        if(resultobject) json_object_put(resultobject);
        if(graph_argv) free(graph_argv);
#ifdef JSONRPC_GRAPH_RRDS_WITH_LIBRRD
        if(grinfo) rrd_info_free(grinfo);
#else
        if(pngdata) free(pngdata);
#endif
        return(rc);
} /* }}} jsonrpc_cb_pw_rrd_graphonly */
/* vim: set fdm=marker sw=8 ts=8 tw=78 et : */
