/**
 * collectd - src/jsonrpc_cb_topps.c
 * Copyright (C) 2012 Yves Mettier
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
 **/

#include "common.h"
#include "utils_avltree.h"
#include "plugin.h"
#include "jsonrpc.h"
#include <json/json.h>
#include <zlib.h>
#define OUTPUT_PREFIX_JSONRPC_CB_TOPPS "JSONRPC plugin (topps) : "

extern char toppsdatadir[];

#define TIMELINE_TIMEOUT_DEFAULT 60
#define TIMELINE_TIMEOUT_HIGH_VALUE 86400

#define TIMELINE_KEY_MAXLEN 256
#define TIMELINE_UNAME_MAXLEN 257 /* getconf LOGIN_NAME_MAX returns 256 */
#define TIMELINE_GNAME_MAXLEN 257
#define TIMELINE_CMD_MAXLEN 2048
typedef struct {
        char key[TIMELINE_KEY_MAXLEN];
        time_t tm_min;
        time_t tm_max;
        pid_t pid;
        pid_t ppid;
        uid_t uid;
        gid_t gid;
        char uname[TIMELINE_UNAME_MAXLEN];
        char gname[TIMELINE_GNAME_MAXLEN];
        char cmd[TIMELINE_CMD_MAXLEN];
} timeline_ps_item_t;

typedef enum {
        TIMELINE_READ_FILE_STATUS_OK,
        TIMELINE_READ_FILE_STATUS_FILE_NOT_FOUND,
        TIMELINE_READ_FILE_STATUS_AFTER_TIMESTAMP,
        TIMELINE_READ_FILE_STATUS_SYNTAX_ERROR,
        TIMELINE_READ_FILE_STATUS_UNKNOWN_VERSION,
        TIMELINE_READ_FILE_STATUS_ERROR_IN_FILE,
        TIMELINE_READ_FILE_STATUS_ERROR_CRITICAL
} timeline_read_file_status_e;

static int mkpath_by_tm_and_num(char *buffer, size_t bufferlen, time_t tm, int n) /* {{{ */
{
        struct tm stm;
        int status;

        char timebuffer[25]; /* 2^64 is a 20-digits decimal number. So 25 should be enough */
        if (localtime_r (&tm, &stm) == NULL)
        {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "localtime_r failed");
                return (-1);
        }
        strftime(timebuffer, sizeof(timebuffer), "%s", &stm);
        status = ssnprintf (buffer, bufferlen,
                        "%1$.2s/%1$.4s/ps-%1$.6s0000-%2$d.gz", timebuffer,n);
        if ((status < 1) || (status >= bufferlen)) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Filename buffer too small (%s:%d)", __FILE__, __LINE__);
                return (-1);
        }
        return(0);

} /* }}} mkpath_by_tm_and_num */

static int check_if_file_contains_tm(gzFile gzfh, const char *filename, time_t tm_start, int *err) { /* {{{ */
        /* Return 0 if tm_start is inside the file,
         *          or if an error occured (*err is not nul if an error occured)
         * Return -n if we should look before 
         * Return n if we should look after
         * n is min(|tm_start-begin|, |tm_end-begin|)
         */
        char line[4096];
        size_t l;
        time_t tm_last;
        time_t tm_first;

        *err = 0;

        /* Read version */
        if(NULL == gzgets(gzfh, line, sizeof(line))) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                goto check_if_file_contains_tm_read_failed;
        }
        for( l = strlen(line) - 1; l>0; l--) {
                if(line[l] == '\n') line[l] = '\0';
                else if(line[l] == '\r') line[l] = '\0';
                else break;
        }
        if(!strcmp(line, "Version 1.0")) {
                time_t tm1, tm2;
                /* Read 2nd line : last tm */
                if(NULL == gzgets(gzfh, line, sizeof(line))) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                        goto check_if_file_contains_tm_read_failed;
                }

                errno=0;
                tm_last = strtol(line, NULL, 10);
                if(0 != errno) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                        goto check_if_file_contains_tm_read_failed;
                }

                /* Read 3rd line : first tm (and start of the records) */
                if(NULL == gzgets(gzfh, line, sizeof(line))) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                        goto check_if_file_contains_tm_read_failed;
                }

                errno=0;
                tm_first = strtol(line, NULL, 10);
                if(0 != errno) {
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                        goto check_if_file_contains_tm_read_failed;
                }

                if((tm_start >= tm_first) && (tm_start <= tm_last)) return(0); /* tm_start is inside the file */
                tm1 = abs(tm_start - tm_first);
                tm2 = abs(tm_start - tm_last);
                tm1 = (tm1 < tm2)?tm1:tm2;
                tm1 = ((tm_start - tm_first) > 0) ? tm1 : -tm1;
                return(tm1);

        } else {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : wrong version nomber (found '%s')", filename, line);
                goto check_if_file_contains_tm_read_failed;
        }
        *err = 1;
        return(0);

check_if_file_contains_tm_read_failed:
        *err = 2;
        return(0);

} /* }}} check_if_file_contains_tm */

static int check_path(const char *hostname, int tm_start, int tm_end, char *buffer, size_t bufferlen) /* {{{ */
{
        /* Path syntax where timestamp = AABBCCDDDD :
         * ${toppsdatadir}/${hostname}/AA/AABB/AABBCC0000-X.gz
         * Checking path means testing that the ${toppsdatadir}/${hostname}/AA/AABB directory exists.
         * If not, check with tm_margin.
         *
         * Start at tm_start. If tm_end < tm_start, search backward.
         */
        gzFile gzfh=NULL;
        int offset = 0;
        int status;
        short file_found;
        time_t tm;
        int n=0;
        int best_distance;
        int max_distance;
        int best_n;
        int best_tm;
        int last_seen_n_low,last_seen_n_high;
        int last_seen_tm_low,last_seen_tm_high;
        int last_before_flush_n = -1;
        int last_before_flush_tm = -1;
        int flush_needed = 0;
        int flush_already_done = 0;
        short watchdog;
        int search_direction;

        if (toppsdatadir != NULL)
        {
                status = ssnprintf (buffer, bufferlen, "%s/", toppsdatadir);
                if ((status < 1) || (status >= bufferlen )) {
                        ERROR(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Filename buffer too small (%s:%d)", __FILE__, __LINE__);
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                }
                offset += status;
        }
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG toppsdatadir='%s' (%s:%d)", toppsdatadir, __FILE__, __LINE__);
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG offset = %d (%s:%d)", offset, __FILE__, __LINE__);

        status = ssnprintf (buffer + offset, bufferlen - offset,
                        "%s/", hostname);
        if ((status < 1) || (status >= bufferlen - offset)) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Filename buffer too small (%s:%d)", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        offset += status;
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG offset = %d (%s:%d)", offset, __FILE__, __LINE__);

        /* Start search */
        max_distance = abs(tm_start - tm_end); /* distance should be < max_distance */
        file_found = 0;
        best_distance = max_distance + 1;
        best_n = 0;
        best_tm = 0;
        last_seen_tm_high = 0;
        last_seen_tm_low = 0;
        last_seen_n_high = 0;
        last_seen_n_low = 0;
        search_direction = 1; /* positive value to go forward at first time */

        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG WE ARE SEARCHING FOR tm_start = '%d' max_distance = %d (%s:%d)", tm_start, max_distance, __FILE__, __LINE__);

        n = 0;
        tm = 10000 * (int)(tm_start / 10000);
        if(tm_start <= tm_end) tm -= 10000; /* if searching forward, search starts before tm_start. */
        //        else                   tm += 10000; /* if searching backward, search starts after tm_start */

#define WATCHDOGMAX 100 /* max number of cycles in this loop. Prevent from infinite loop if something is missing in this complex algo */
        for(watchdog = 0; watchdog < WATCHDOGMAX; watchdog++) { /* There are many cases to get out of this loop. See the many 'break' instructions */
                int local_err;

                if((0 == flush_already_done) && (2 == flush_needed)) {
                        /* Back to flush position */
                        assert(last_before_flush_tm != -1); /* ensure that it was initialized */
                        assert(last_before_flush_n != -1);  /* ensure that it was initialized */
                        tm = last_before_flush_tm;
                        n = last_before_flush_n;
                }

                if(mkpath_by_tm_and_num(buffer + offset, bufferlen - offset,tm, n)) {
                        return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                }

                /* Try to open the file.
                 * Flush if necessary
                 */
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG tm = %ld filename = '%s' (%s:%d)", tm, buffer, __FILE__, __LINE__);
                if(NULL == (gzfh = gzopen(buffer, "r"))) {
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG COULD NOT OPEN = '%s' (%s:%d)", buffer, __FILE__, __LINE__);
                        if((0 == flush_already_done) && (2 == flush_needed)) {
                                /* Open failed. Maybe we should flush ? */
                                int status;
                                time_t flush_tm;
                                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG Calling plugin_flush('write_top',10,%s) (%s:%d)", hostname, __FILE__, __LINE__);
                                status = plugin_flush ("write_top", 0, hostname);
                                if (status == 0) {
                                        /* Flush done. Try again with older values */
                                }
                                flush_already_done = 1;

                                flush_tm = time(NULL);
                                while((time(NULL) - flush_tm) < 10) { /* wait no more than 10 seconds for flush */
                                        sleep(1);
                                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG Trying to open '%s' again... (%s:%d)", buffer, __FILE__, __LINE__);
                                        if(NULL != (gzfh = gzopen(buffer, "r")))  break;
                                }
                        }
                }

                /* File is supposed to be opened, with or without a flush.
                 * Check that the file was really opened
                 */
                if(NULL == gzfh) { /* File could NOT be opened */
                        if((0 == flush_already_done) && (search_direction > 0)) {
                                if(0 == flush_needed) {
                                        last_before_flush_tm = tm; /* save this position */
                                        last_before_flush_n = n;
                                }
                                flush_needed++;
                        }
                } else { /* File could be opened */
                        int distance;

                        flush_needed = 0;
                        distance = check_if_file_contains_tm(gzfh, buffer, tm_start,&local_err);
                        gzclose(gzfh);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG distance = '%d' (%s:%d)", distance, __FILE__, __LINE__);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG best_distance was = '%d' (%s:%d)", best_distance, __FILE__, __LINE__);
                        if(0 == local_err) { /* ignore this file if something wrong happened */
                                int adistance = abs(distance);
                                /* Check if file found */
                                if(0 == distance) {
                                        best_distance = distance;
                                        best_n = n;
                                        best_tm = tm;
                                        file_found = 1;
                                        break;
                                }
                                /* Check if we found a better file */
                                if(adistance <= best_distance) {
                                        if(
                                                        ((distance < 0) && (tm_start <= tm_end))
                                                        ||
                                                        ((distance > 0) && (tm_start >= tm_end))
                                          ) {
                                                best_distance = adistance;
                                                best_n = n;
                                                best_tm = tm;
                                                if(adistance < max_distance) file_found = 1;

                                        }
                                }
                                search_direction = distance;
                        } /* 0 == local_err */
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG best_distance is now = '%d' (file found : %d)(%s:%d)", best_distance, file_found, __FILE__, __LINE__);
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG best_tm/n = '%d/%d' (%s:%d)", best_tm, best_n, __FILE__, __LINE__);
                } /* NULL != gzfh */

                /* Move to next file and check if we should
                 * leave.
                 */
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG search_direction = '%d' (%s:%d)", search_direction, __FILE__, __LINE__);
                if(search_direction > 0) {
                        if(NULL == gzfh) {
                                n = 0;
                                tm += 10000;
                        } else {
                                last_seen_tm_low = tm;
                                last_seen_n_low = n;
                                n += 1;
                        }
                        if(last_seen_tm_high) {
                                if((tm >= last_seen_tm_high) && (n >= last_seen_n_high)) {
                                        break; /* already been there or after */
                                }
                        }
                } else { /* search_direction < 0 */
                        if(NULL != gzfh) {
                                last_seen_tm_high = tm;
                                last_seen_n_high = n;
                        }
                        n = 0;
                        tm -= 10000;
                        if(last_seen_tm_low) {
                                if((tm <= last_seen_tm_low) && (n <= last_seen_n_low)) {
                                        break; /* already been there or before */
                                }
                        }
                }
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG fenetre tm/n = '%d/%d','%d/%d' (%s:%d)", last_seen_tm_low,last_seen_n_low,last_seen_tm_high,last_seen_n_high, __FILE__, __LINE__);
                if(tm_start <= tm_end) { /* When searching forward */
                        if((tm > tm_end) && (
                                                !((0 == flush_already_done) && (2 == flush_needed)) /* Do not break if flush needed */
                                            )) break;
                        /* There should be no reason to search and limit in the past */
                } else { /* When searching backward */
                        if((tm > (tm_start + 10000)) && (
                                                !((0 == flush_already_done) && (2 == flush_needed)) /* Do not break if flush needed */

                                                )) break; /* Going too far in the future (or recent past) */
                        if(tm < (tm_end - 86400)) break; /* Going too far in the past */
                        /* Note : a big old file could contain the data we are
                         * looking for. However, the user should not keep more
                         * than 1 day of data in memory for each hosts. This
                         * is not optimal and is dangerous for the data.
                         */
                }
        }
        if(watchdog >= WATCHDOGMAX) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Infinite loop in %s:%d. hostname='%s', tm=%d, tm_end=%d", __FILE__, __LINE__, hostname, tm_start, tm_end);
                return(JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG file_found = '%d' (%s:%d)", file_found, __FILE__, __LINE__);
        if(file_found) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG filename = '%s' (%s:%d)", buffer, __FILE__, __LINE__);
                if(mkpath_by_tm_and_num(buffer + offset, bufferlen - offset,best_tm, best_n)) {
                        return(JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
                }
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG filename = '%s' (%s:%d)", buffer, __FILE__, __LINE__);
        } else {
                buffer[0] = '\0';
        }

        return(0);
} /* }}} check_path */

static struct json_object *read_top_ps_file(const char *filename, int tm, short take_next, time_t *data_tm, int *err) /* {{{ */
{
        /* 
         * Return values :
         *   returned value : json array with the result if success. NULL otherwise.
         *   data_tm        : exact tm to search
         *   err            : not nul if an error occured.
         *
         * If returned value is not nul, it is the json array with the result. data_tm
         * contains the tm of the data found.
         * If the returned value is nul, check if err is nul or not.
         *   If err is nul, data_tm is set to the tm to search. Call again with this
         *   value.
         *   If err is not nul, an error occured.
         */
        gzFile gzfh=NULL;
        int errnum;
        char line[4096];
        size_t l;
        struct json_object *top_ps_array = NULL;

        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG Trying to open '%s' (%s:%d)", filename, __FILE__, __LINE__);
        *data_tm = 0;
        *err = 0;
        if(NULL == (gzfh = gzopen(filename, "r"))) {
                *err = 1;
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not gzopen for reading (%s:%d)", filename, __FILE__, __LINE__);
                return(NULL);
        }
        /* Read version */
        if(NULL == gzgets(gzfh, line, sizeof(line))) {
                gzclose(gzfh);
                *err = 1;
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                return(NULL);
        }
        for( l = strlen(line) -1 ; l>0; l--) {
                if(line[l] == '\n') line[l] = '\0';
                else if(line[l] == '\r') line[l] = '\0';
                else break;
        }
        if(!strcmp(line, "Version 1.0")) {
                time_t tm_current, tm_prev, tm_last;
                enum { top_ps_state_tm, top_ps_state_nb_lines, top_ps_state_line } state;
                long n;
                long nb_lines;
                short record_lines = 0;
                short record_last = 0;
                /* Read 2nd line : last tm */
                if(NULL == gzgets(gzfh, line, sizeof(line))) {
                        gzclose(gzfh);
                        *err = 1;
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                        return(NULL);
                }
                /* Check if the last one is the one we want.
                 * If yes, optimize and remember that when we reach it, we
                 * record it.
                 */
                tm_last = strtol(line, NULL, 10);
                if(0 != errno) {
                        gzclose(gzfh);
                        *err = 1;
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                        return(NULL);
                }
                if((0 == take_next) && (tm > tm_last)) {
                        record_last = 1;
                }


                state = top_ps_state_tm;
                tm_current = 0;
                tm_prev = 0;
                nb_lines = 0;
                n = 0;
                while(
                                ((record_lines != 0) || (NULL == top_ps_array)) && 
                                (NULL != gzgets(gzfh, line, sizeof(line)))
                     ) {
                        json_object *json_string;

                        switch(state) {
                                case top_ps_state_tm :
                                        errno=0;
                                        tm_prev = tm_current;
                                        tm_current = strtol(line, NULL, 10);
                                        if(0 != errno) {
                                                gzclose(gzfh);
                                                *err = 1;
                                                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                                                return(NULL);
                                        }
                                        if(tm_current == tm) {
                                                /* We fould the one we are looking for.
                                                 * Start recording. */
                                                *data_tm = tm_current;
                                                record_lines = 1;
                                        } else if((tm_current == tm_last) && (record_last)) {
                                                *data_tm = tm_current;
                                                record_lines = 1;
                                        } else if(take_next && (tm > tm_prev) && (tm < tm_current)) {
                                                /* The one we are looking for does not exist. The one
                                                 * starting now is the best we can find.
                                                 * Start recording. */
                                                *data_tm = tm_current;
                                                record_lines = 1;
                                        } else if((0 == take_next) && (tm_current > tm)) {
                                                /* We wanted the previous one and we just missed it */
                                                gzclose(gzfh);
                                                if(tm_prev) {
                                                        *data_tm = tm_prev;
                                                        *err = 0; /* no error : try again with exact tm */
                                                        return(NULL);
                                                } else {
                                                        /* this one is not the one we want. And there is no
                                                         * previous one. Error. */
                                                        *err = 1;
                                                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not find '%d' before '%ld' (%s:%d)", filename, tm, tm_current, __FILE__, __LINE__);
                                                        return(NULL);
                                                }
                                        }
                                        state = top_ps_state_nb_lines;
                                        break;
                                case top_ps_state_nb_lines :
                                        errno=0;
                                        nb_lines = strtol(line, NULL, 10);
                                        if(0 != errno) {
                                                gzclose(gzfh);
                                                *err = 1;
                                                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                                                return(NULL);
                                        }
                                        n = 0;
                                        state = top_ps_state_line;
                                        break;
                                case top_ps_state_line :
                                        if(record_lines) {
                                                /* record the line */
                                                if(NULL == top_ps_array) {
                                                        if(NULL == (top_ps_array = json_object_new_array())) {
                                                                gzclose(gzfh);
                                                                *err = 1;
                                                                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not create a new JSON array (%s:%d)", filename, __FILE__, __LINE__);
                                                                return(NULL);
                                                        }
                                                }
                                                /* Remove CR and LF at the end of the line */
                                                l = strlen(line) - 1;
                                                while(l > 0 && ((line[l] == '\r' ) || (line[l] == '\r' ))) {
                                                        line[l] = '\0';
                                                        l -= 1;
                                                }
                                                if(NULL == (json_string = json_object_new_string(line))) {
                                                        json_object_put(top_ps_array);
                                                        gzclose(gzfh);
                                                        *err = 1;
                                                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not create a new JSON string (%s:%d)", filename, __FILE__, __LINE__);
                                                        return(NULL);
                                                }
                                                json_object_array_add(top_ps_array,json_string);
                                        }
                                        n++;
                                        if(n >= nb_lines) {
                                                state = top_ps_state_tm;
                                                record_lines = 0; /* End recoding */
                                        }
                                        break;
                        }
                }
                gzerror(gzfh, &errnum);
                gzclose(gzfh);
                if(errnum < 0) {
                        *err = 1;
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                        return(NULL);
                }
        }
        if(NULL == top_ps_array) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not find '%d' before the end of the file (%s:%d)", filename, tm, __FILE__, __LINE__);
                return(NULL);
        }

        return(top_ps_array);
} /* }}} read_top_ps_file */

int jsonrpc_cb_topps_get_top (struct json_object *params, struct json_object *result, const char **errorstring) /* {{{ */
{
        /*
         * { params : { "hostname" : "<a host name>",
         *              "tm"       : <a timestamp to search>,
         *              "end_tm"   : <a timestamp on which search will end>
         *            }
         * }
         *
         * Return :
         * { result : { "status" : "OK" or "some string message if not found",
         *              "tm" : <the timestamp of the data>,
         *              "topps" : [ "string 1", "string 2", ... ]
         *            }
         * }
         *
         * Note : tm can be bigger or lower than end_tm.
         * If tm == end_tm, search exactly tm.
         * If tm < end_tm, search forward.
         * If tm > end_tm, search backward.
         *
         */
        struct json_object *obj;
        struct json_object *result_topps_object;
        int param_timestamp_start=0;
        int param_timestamp_end=0;
        const char *param_hostname = NULL;

        char topps_filename_dir[2048];
        int err;
        time_t result_tm;

        /* Parse the params */
        if(!json_object_is_type (params, json_type_object)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "start_tm" timestamp */
        if(NULL == (obj = json_object_object_get(params, "tm"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_timestamp_start = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "end_tm" timestamp */
        if(NULL == (obj = json_object_object_get(params, "end_tm"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_timestamp_end = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "hostname" */
        if(NULL == (obj = json_object_object_get(params, "hostname"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_string)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        if(NULL == (param_hostname = json_object_get_string(obj))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Check args */
        if(0 == param_timestamp_start) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
        if(0 == param_timestamp_end) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
        if(NULL == param_hostname) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }

        /* Check the servers and build the result array */
        if(NULL == (result_topps_object = json_object_new_object())) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json array");
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        if(0 != (err = check_path(param_hostname, param_timestamp_start, param_timestamp_end, topps_filename_dir, sizeof(topps_filename_dir)))) {
                json_object_put(result_topps_object);
                return(err);
        }
        if('\0' == topps_filename_dir[0]) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG topps_filename_dir[0] == '\\0' bummer ! (%s:%d)", __FILE__, __LINE__);
                obj =  json_object_new_string("path not found or no file for this tm");
                json_object_object_add(result_topps_object, "status", obj);
                json_object_object_add(result, "result", result_topps_object);
                return(0);
        }
        /* Read the file, 1st time */
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG read_top_ps_file('%s', '%d',...) (%s:%d)", topps_filename_dir,param_timestamp_start, __FILE__, __LINE__);
        obj = read_top_ps_file(
                        /* filename  = */ topps_filename_dir,
                        /* tm        = */ param_timestamp_start,
                        /* take_next = */ (param_timestamp_end>=param_timestamp_start)?1:0,
                        /* data_tm   = */ &result_tm, 
                        /* *err      = */ &err);
        if(NULL == obj) {
                /* If obj could not be created, check if it is an error.
                 * Otherwise, try again with returned result_tm.
                 */
                if(err) {
                        json_object_put(result_topps_object);
                        return(err);
                }
        }
        /* Check if result_tm is inside [start .. end] */
        if (
                        ( (param_timestamp_end >= param_timestamp_start) && (result_tm <= param_timestamp_end) ) || 
                        ( (param_timestamp_end <  param_timestamp_start) && (result_tm >= param_timestamp_end) )
           ) {
                /* OK, result_tm is correct. Go on... */
                if(NULL == obj) {
                        /* Here, we found a correct result_tm, but did
                         * not record. Try again with the exact tm.
                         */ 
                        time_t tm2 = result_tm;
                        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG read_top_ps_file('%s', '%ld',...) 2nd time (%s:%d)", topps_filename_dir,tm2, __FILE__, __LINE__);
                        obj = read_top_ps_file(
                                        /* filename  = */ topps_filename_dir,
                                        /* tm        = */ tm2,
                                        /* take_next = */ (param_timestamp_end>=param_timestamp_start)?1:0,
                                        /* data_tm   = */ &result_tm, 
                                        /* *err      = */ &err);
                        if(NULL == obj) {
                                json_object_put(result_topps_object);
                                return(err?err:1);
                        }
                }
        } else {
                /* result_tm is too far from what we want.
                 * If an object obj was defined, purge it. */
                if(NULL != obj) {
                        json_object_put(obj);
                }

                obj =  json_object_new_string("path not found or no file for this tm");
                json_object_object_add(result_topps_object, "status", obj);
                json_object_object_add(result, "result", result_topps_object);
                return(0);
        }

        json_object_object_add(result_topps_object, "topps", obj);
        obj =  json_object_new_int(result_tm);
        json_object_object_add(result_topps_object, "tm", obj);
        obj =  json_object_new_string("OK");
        json_object_object_add(result_topps_object, "status", obj);

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", result_topps_object);

        return(0);
} /* }}} jsonrpc_cb_topps_get_top */

static int timeline_update_ps(timeline_ps_item_t *new_ps_item, time_t tm, c_avl_tree_t *processes) /* {{{ */
{
        /* Returns :
         * 0 OK and new_ps_item was insered in the tree.
         * 1 OK and the new_ps_item can be freed or reused.
         * -1 ERROR
         */
        int status = -1;
        timeline_ps_item_t *ps_item;

        if(0 == c_avl_get(processes, new_ps_item->key, (void*)&ps_item)) {
                if(tm < ps_item->tm_min) ps_item->tm_min = tm;
                if(tm > ps_item->tm_max) ps_item->tm_max = tm;
                status = 1;
        } else {
                new_ps_item->tm_min = tm;
                new_ps_item->tm_max = tm;
                if(0 == c_avl_insert(processes, new_ps_item->key, new_ps_item)) {
                        status = 0;
                } else {
                        status = -1; /* error */
                }
        }
        return(status);
} /* }}} timeline_update_ps */

static int parse_line_to_ps_item(char *line, timeline_ps_item_t *ps_item) { /* {{{ */
        long long int lli;
        char *ptr1,*ptr2;
        size_t l;

        ptr1 = line;
        errno=0;
#define CONVERT_PTR1_TO_LLI_OR_RETURN do { \
        lli = strtoll(ptr1, &ptr2, 10); \
        if(0 != errno) return(1); \
        if(ptr1 == ptr2) return(1); \
        if(NULL == ptr2) return(1); \
        ptr1 = ptr2; \
        while(ptr1[0] == ' ') ptr1++; \
} while(0)

        CONVERT_PTR1_TO_LLI_OR_RETURN; ps_item->pid = lli;
        CONVERT_PTR1_TO_LLI_OR_RETURN; ps_item->ppid = lli;
        CONVERT_PTR1_TO_LLI_OR_RETURN; ps_item->uid = lli;

        l = strcspn(ptr1, " \t");
        if(l >= TIMELINE_UNAME_MAXLEN) return(1);
        memcpy(ps_item->uname, ptr1, l);
        ps_item->uname[l] = '\0';
        ptr1 += l;
        while((ptr1[0] == ' ') || (ptr1[0] == '\t')) ptr1++; 

        CONVERT_PTR1_TO_LLI_OR_RETURN; ps_item->gid = lli;

        l = strcspn(ptr1, " \t");
        if(l >= TIMELINE_GNAME_MAXLEN) return(1);
        memcpy(ps_item->gname, ptr1, l);
        ps_item->gname[l] = '\0';
        ptr1 += l;
        while((ptr1[0] == ' ') || (ptr1[0] == '\t')) ptr1++; 

        CONVERT_PTR1_TO_LLI_OR_RETURN; 
        CONVERT_PTR1_TO_LLI_OR_RETURN; 
        CONVERT_PTR1_TO_LLI_OR_RETURN; 

        l = strcspn(ptr1, "\r\n");
        if(l >= TIMELINE_CMD_MAXLEN) l = TIMELINE_CMD_MAXLEN - 1;
        memcpy(ps_item->cmd, ptr1, l);
        ps_item->cmd[l] = '\0';

        snprintf(ps_item->key, TIMELINE_KEY_MAXLEN, "%ud %s %s %s", ps_item->pid, ps_item->uname,ps_item->gname,ps_item->cmd);

        return(0);
} /* }}} parse_line_to_ps_item */

static timeline_read_file_status_e timeline_read_file(const char *filename, time_t timestamp_start, time_t timestamp_end, c_avl_tree_t *processes, time_t *first_tm_in_file, time_t *last_tm_in_file) /* {{{ */
{
        gzFile gzfh=NULL;
        int errnum;
        char line[4096];
        timeline_ps_item_t *ps_item = NULL;
        size_t l;

        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG Trying to open '%s' (%s:%d)", filename, __FILE__, __LINE__);
        if(NULL == (gzfh = gzopen(filename, "r"))) {
                return(TIMELINE_READ_FILE_STATUS_FILE_NOT_FOUND);
        }
        *first_tm_in_file = 0;
        *last_tm_in_file = 0;
        /* Read version */
        if(NULL == gzgets(gzfh, line, sizeof(line))) {
                gzclose(gzfh);
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                return(TIMELINE_READ_FILE_STATUS_ERROR_IN_FILE);
        }
        for( l = strlen(line) -1 ; l>0; l--) {
                if(line[l] == '\n') line[l] = '\0';
                else if(line[l] == '\r') line[l] = '\0';
                else break;
        }
        if(!strcmp(line, "Version 1.0")) {
                time_t tm;
                enum { top_ps_state_tm, top_ps_state_nb_lines, top_ps_state_line } state;
                long n;
                long nb_lines;
                short record_lines = 0;
                /* Read 2nd line : last tm */
                if(NULL == gzgets(gzfh, line, sizeof(line))) {
                        gzclose(gzfh);
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                        return(TIMELINE_READ_FILE_STATUS_ERROR_IN_FILE);
                }
                /* Check if the last one is the one we want.
                 * If yes, optimize and remember that when we reach it, we
                 * record it.
                 */
                tm= strtol(line, NULL, 10);
                if(0 != errno) {
                        gzclose(gzfh);
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                        return(TIMELINE_READ_FILE_STATUS_ERROR_CRITICAL);
                }
                if(tm < timestamp_start) {
                        gzclose(gzfh);
                        return(TIMELINE_READ_FILE_STATUS_OK); /* No process to record here */
                }

                state = top_ps_state_tm;
                tm = 0;
                nb_lines = 0;
                n = 0;
                while(NULL != gzgets(gzfh, line, sizeof(line))) {
                        switch(state) {
                                case top_ps_state_tm :
                                        errno=0;
                                        tm = strtol(line, NULL, 10);
                                        if(0 != errno) {
                                                gzclose(gzfh);
                                                if(NULL != ps_item) free(ps_item);
                                                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                                                return(TIMELINE_READ_FILE_STATUS_ERROR_CRITICAL);
                                        }
                                        if((tm >= timestamp_start) && (tm <= timestamp_end)) {
                                                record_lines = 1; /* Start recording. */
                                                if((tm < *first_tm_in_file) || (*first_tm_in_file == 0)) *first_tm_in_file = tm;
                                                if(tm > *last_tm_in_file) *last_tm_in_file = tm;
                                        } else {
                                                record_lines = 0; /* Not in range : do not record */
                                        }
                                        state = top_ps_state_nb_lines;
                                        break;
                                case top_ps_state_nb_lines :
                                        errno=0;
                                        nb_lines = strtol(line, NULL, 10);
                                        if(0 != errno) {
                                                gzclose(gzfh);
                                                if(NULL != ps_item) free(ps_item);
                                                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                                                return(TIMELINE_READ_FILE_STATUS_ERROR_CRITICAL);
                                        }
                                        n = 0;
                                        state = top_ps_state_line;
                                        break;
                                case top_ps_state_line :
                                        if(record_lines) { 
                                                /* Remove CR and LF at the end of the line */
                                                if(NULL == ps_item) {
                                                        if(NULL == (ps_item = malloc(sizeof(*ps_item)))) {
                                                                gzclose(gzfh);
                                                                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Not enough memory while reading '%s' (%s:%d)", filename, line, __FILE__, __LINE__);
                                                                return(TIMELINE_READ_FILE_STATUS_ERROR_CRITICAL);

                                                        }
                                                }
                                                if(0 == parse_line_to_ps_item(line, ps_item)) {
                                                        int r;
                                                        r = timeline_update_ps(ps_item, tm, processes);
                                                        switch(r) {
                                                                case 0:  /* ps_item was used : we need a new one next time */
                                                                        ps_item = NULL;
                                                                        break;
                                                                case 1: /* nothing to do */
                                                                        break; 
                                                                default : /* Something went wrong */
                                                                        gzclose(gzfh);
                                                                        if(NULL != ps_item) free(ps_item);
                                                                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not convert '%s' to integer (%s:%d)", filename, line, __FILE__, __LINE__);
                                                                        return(TIMELINE_READ_FILE_STATUS_ERROR_CRITICAL);
                                                        }
                                                }
                                        }
                                        n++;
                                        if(n >= nb_lines) {
                                                state = top_ps_state_tm;
                                        }
                                        break;
                        }
                }
                gzerror(gzfh, &errnum);
                gzclose(gzfh);
                if(errnum < 0) {
                        if(NULL != ps_item) free(ps_item);
                        ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "'%s' : Could not read a line (%s:%d)", filename, __FILE__, __LINE__);
                        return(TIMELINE_READ_FILE_STATUS_ERROR_IN_FILE);
                }
        } else {
                return(TIMELINE_READ_FILE_STATUS_UNKNOWN_VERSION);
        }

        if(NULL != ps_item) free(ps_item);
        return(TIMELINE_READ_FILE_STATUS_OK);
} /* }}} timeline_read_file */

#define free_avl_tree(tree) do {                                      /* {{{ */                   \
			c_avl_iterator_t *it;                                                     \
			it = c_avl_get_iterator(tree);                                            \
			while (c_avl_iterator_next (it, (void *) &key, (void *) &ps_item) == 0) { \
					free(ps_item);                                            \
			}                                                                         \
			c_avl_iterator_destroy(it);                                               \
	} while(0) /* }}} free_avl_tree */

static struct json_object *timeline_build( /* {{{ */
                const char *hostname,
                time_t timestamp_start,
                time_t timestamp_end,
                time_t interval,
                time_t ignore_short_lived,
                int ignore_resident,
                time_t request_tm1,
                time_t timeout
                )
{
        struct json_object *timeline_array = NULL;
        char topps_filename_dir[2048];
        int offset = 0;
        int status;
        time_t tm;
        time_t tm_offset;
        int n;
        c_avl_tree_t *processes;
        c_avl_iterator_t *avl_iter;
        char *key;
        timeline_ps_item_t *ps_item;
        time_t request_tm2;
        time_t tm_found_first=0;
        time_t tm_found_last=0;

        /* Build toppsdatadir/hostname directory */
        if (toppsdatadir != NULL)
        {
                status = ssnprintf (topps_filename_dir, sizeof(topps_filename_dir), "%s/", toppsdatadir);
                if ((status < 1) || (status >= sizeof(topps_filename_dir) )) {
                        ERROR(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Filename buffer too small (%s:%d)", __FILE__, __LINE__);
                        return (NULL);
                }
                offset += status;
        }
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG toppsdatadir='%s' (%s:%d)", toppsdatadir, __FILE__, __LINE__);
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG offset = %d (%s:%d)", offset, __FILE__, __LINE__);

        status = ssnprintf (topps_filename_dir + offset, sizeof(topps_filename_dir) - offset,
                        "%s/", hostname);
        if ((status < 1) || (status >= sizeof(topps_filename_dir) - offset)) {
                ERROR(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Filename buffer too small (%s:%d)", __FILE__, __LINE__);
                return (NULL);
        }
        offset += status;
        DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "DEBUG offset = %d (%s:%d)", offset, __FILE__, __LINE__);

        /* Create a tree to store the processes */
        if(NULL == (processes = c_avl_create((void *) strcmp))) {
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Internal error %s:%d", __FILE__, __LINE__);
                return (NULL);
        }

        /* Parse ps-*.gz files */
        tm = 10000* ((int)(timestamp_start/10000));
        tm_offset = 10000* (1+(int)(interval/10000));
        n = 0;
        request_tm2 = time(NULL);
        while((tm <= timestamp_end) && ((request_tm2-request_tm1) < timeout)) {
                time_t first_tm_in_file=0;
                time_t last_tm_in_file=0;
                timeline_read_file_status_e read_file_status;
                if(mkpath_by_tm_and_num(topps_filename_dir + offset, sizeof(topps_filename_dir) - offset,tm, n)) {
                        free_avl_tree(processes);
                        c_avl_destroy(processes);
                        return (NULL);
                }
                read_file_status = timeline_read_file(topps_filename_dir, timestamp_start, timestamp_end, processes, &first_tm_in_file, &last_tm_in_file);
                switch (read_file_status) {
                        case TIMELINE_READ_FILE_STATUS_OK: /* OK */
                        case TIMELINE_READ_FILE_STATUS_UNKNOWN_VERSION: /* unknown version (same as OK but we ignore what could not be read of that specific file) */
                        case TIMELINE_READ_FILE_STATUS_ERROR_IN_FILE: /* unknown error in the file (same as OK but we ignore what could not be read of that specific file) */
                        case TIMELINE_READ_FILE_STATUS_SYNTAX_ERROR: /* syntax error (same as OK but we ignore what could not be read of that specific file) */
                                if((first_tm_in_file > 0) && (((tm_found_first == 0) || (first_tm_in_file < tm_found_first)))) tm_found_first = first_tm_in_file;
                                if(last_tm_in_file > tm_found_last) tm_found_last = last_tm_in_file; /* No need to test if last_tm_in_file == 0 of course ! */
                                if(interval >= 10000) {
                                        tm += tm_offset;
                                } else {
                                        n += 1;
                                }
                                break;
                        case TIMELINE_READ_FILE_STATUS_FILE_NOT_FOUND: /* file not found */
                                n=0;
                                tm += tm_offset;
                                break;
                        case TIMELINE_READ_FILE_STATUS_AFTER_TIMESTAMP: /* after timestamp : should stop here */
                                tm = timestamp_end + 1; /* leave the loop */
                                break;
                        default: /* error */
                                free_avl_tree(processes);
                                c_avl_destroy(processes);
                                return (NULL);
                }
                request_tm2 = time(NULL);
        }

        /* Create a new array for the returned result */
        if(NULL == (timeline_array = json_object_new_array())) {
                ERROR (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a new JSON array (%s:%d)",  __FILE__, __LINE__);
                free_avl_tree(processes);
                c_avl_destroy(processes);
                return(NULL);
        }
        if((request_tm2-request_tm1) >= timeout) {
                free_avl_tree(processes);
                c_avl_destroy(processes);
                return(NULL);
        }

        /* Analyze the "processes" avl tree and fill the result array */
        avl_iter = c_avl_get_iterator(processes);
        while (c_avl_iterator_next (avl_iter, (void *) &key, (void *) &ps_item) == 0) {
                struct json_object *hash_obj;
                struct json_object *obj;

                /* Ignore short lived processes */
                if((ps_item->tm_max - ps_item->tm_min) < ignore_short_lived) continue;

                /* Ignore resident processes (processes that lived all the time we are checking */
                if(ignore_resident && (ps_item->tm_min <= tm_found_first) && (ps_item->tm_max >= tm_found_last)) continue;

                if(NULL == (hash_obj = json_object_new_object())) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json object (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }

                if(NULL == (obj = json_object_new_int(ps_item->tm_min))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json int (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "start", obj);

                if(NULL == (obj = json_object_new_int(ps_item->tm_max))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json int (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "end", obj);

                if(NULL == (obj = json_object_new_int(ps_item->pid))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json int (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "pid", obj);

                if(NULL == (obj = json_object_new_int(ps_item->ppid))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json int (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "ppid", obj);

                if(NULL == (obj = json_object_new_int(ps_item->uid))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json int (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "uid", obj);

                if(NULL == (obj = json_object_new_string(ps_item->uname))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json string (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "uname", obj);

                if(NULL == (obj = json_object_new_int(ps_item->gid))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json int (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "gid", obj);

                if(NULL == (obj = json_object_new_string(ps_item->gname))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json string (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "gname", obj);

                if(NULL == (obj = json_object_new_string(ps_item->cmd))) {
                        DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json string (%s:%d)", __FILE__, __LINE__);
                        goto timeline_build_array_failure_g;
                }
                json_object_object_add(hash_obj, "cmd", obj);

                json_object_array_add(timeline_array,hash_obj);
        }
        c_avl_iterator_destroy(avl_iter);
        free_avl_tree(processes);
        c_avl_destroy(processes);

        return(timeline_array);

timeline_build_array_failure_g:
        c_avl_iterator_destroy(avl_iter);
        free_avl_tree(processes);
        c_avl_destroy(processes);
        json_object_put(timeline_array);
        return (NULL);
} /* }}} timeline_build */

int jsonrpc_cb_topps_get_timeline (struct json_object *params, struct json_object *result, const char **errorstring) /* {{{ */
{
        /*
         * { params : { "hostname" : "<a host name>",
         *              "start_tm"  : <a timestamp where to start the timeline>,
         *              "end_tm"    : <a timestamp where to start the timeline>,
         *              "interval"  : <approximative nb of seconds between 2 top ps files>
         *              "ignore_short_lived" : <nb of seconds. Ignore process if they lived to short>
         *              "ignore_resident" : <bool : ignore processes that were running before tm_start and after tm_end>
         *              "timeout"   : <approximative max nb of seconds to spend on this request>
         *            }
         * }
         *
         * Return :
         * { result : { "status" : "OK" or "TIMEOUT" or "some string message if not found",
         *              "timeline" : [ {
         *                             'start': tm,
         *                             'end': tm,
         *                             'pid': pid,
         *                             'ppid': ppid,
         *                             'uid': uid,
         *                             'uname': 'user name',
         *                             'gid': gid,
         *                             'gname': 'group name',
         *                             'cmd': 'command',
         *                             },
         *                             { ... }, { ... }, ...
         *                           ]
         *            }
         * }
         *
         * Note : tm_start should be lower than tm_end.
         * Note : interval is approximative and will rounded to lower 10000
         * - if interval == 0, check all ps-*-*.gz files
         * - if interval > 10000, check only ps-*-0.gz files
         *
         */
        struct json_object *obj;
        struct json_object *result_topps_object;
        int param_timestamp_start=0;
        int param_timestamp_end=0;
        int param_interval=0;
        int param_ignore_short_lived=0;
        json_bool param_ignore_resident=0;
        const char *param_hostname = NULL;
        time_t param_timeout = TIMELINE_TIMEOUT_DEFAULT;
        time_t request_tm1;
        time_t request_tm2;

        request_tm1 = time(NULL);
        /* Parse the params */
        if(!json_object_is_type (params, json_type_object)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "start_tm" timestamp */
        if(NULL == (obj = json_object_object_get(params, "start_tm"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_timestamp_start = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "end_tm" timestamp */
        if(NULL == (obj = json_object_object_get(params, "end_tm"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_timestamp_end = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "hostname" */
        if(NULL == (obj = json_object_object_get(params, "hostname"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_string)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        if(NULL == (param_hostname = json_object_get_string(obj))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        /* Params : get the "interval" timestamp */
        if(NULL == (obj = json_object_object_get(params, "interval"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_interval = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Params : get the "ignore_short_lived" timestamp */
        if(NULL == (obj = json_object_object_get(params, "ignore_short_lived"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_ignore_short_lived = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Params : get the "ignore_resident" flag */
        if(NULL == (obj = json_object_object_get(params, "ignore_resident"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_boolean)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_ignore_resident = json_object_get_boolean(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Params : get the "timeout" nb of seconds */
        if(NULL == (obj = json_object_object_get(params, "timeout"))) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        if(!json_object_is_type (obj, json_type_int)) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }
        errno = 0;
        param_timeout = json_object_get_int(obj);
        if(errno != 0) {
                return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
        }

        /* Check args */
        if(0 == param_timestamp_start) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
        if(0 == param_timestamp_end) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
        if(NULL == param_hostname) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
        if(param_timestamp_start >= param_timestamp_end) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
        if(param_timeout == 0) param_timeout = TIMELINE_TIMEOUT_HIGH_VALUE; /* Some way to say no timeout */

        /* Check the servers and build the result array */
        if(NULL == (result_topps_object = json_object_new_object())) {
                DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json array");
                DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Internal error %s:%d", __FILE__, __LINE__);
                return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
        }

        if(NULL == (obj = timeline_build(
                        /* hostname           = */ param_hostname,
                        /* start_tm           = */ param_timestamp_start,
                        /* end_tm             = */ param_timestamp_end,
                        /* interval           = */ param_interval, 
                        /* ignore_short_lived = */ param_ignore_short_lived, 
                        /* ignore_resident    = */ param_ignore_resident,
                        /* request_tm1        = */ request_tm1,
                        /* timeout            = */ param_timeout
                        ))) {
                obj =  json_object_new_string("Something went wrong");
                json_object_object_add(result_topps_object, "status", obj);
                json_object_object_add(result, "result", result_topps_object);
                return(0);
        }
        json_object_object_add(result_topps_object, "timeline", obj);
        request_tm2 = time(NULL);
        obj =  json_object_new_string(((request_tm2-request_tm1) > param_timeout)?"TIMEOUT":"OK");
        json_object_object_add(result_topps_object, "status", obj);

        /* Last : add the "result" to the result object */
        json_object_object_add(result, "result", result_topps_object);

        return(0);
} /* }}} jsonrpc_cb_topps_get_timeline */

/* vim: set fdm=marker sw=8 ts=8 tw=78 et : */
