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
#include "plugin.h"
#include "jsonrpc.h"
#include <json/json.h>
#include <zlib.h>
#define OUTPUT_PREFIX_JSONRPC_CB_TOPPS "JSONRPC plugin (topps) : "

extern char toppsdatadir[];

static char * check_path(const char *hostname, int tm_start, int tm_end, short find_first) /* {{{ */
{
/* Path syntax where timestamp = AABBCCDDDD :
 * ${toppsdatadir}/${hostname}/AA/AABB/AABBCC0000-X.gz
 * Checking path means testing that the ${toppsdatadir}/${hostname}/AA/AABB directory exists.
 * If not, check with tm_margin.
 */
	return(NULL);
} /* }}} check_path */

int jsonrpc_cb_topps_get_top (struct json_object *params, struct json_object *result, const char **errorstring) /* {{{ */
{
		struct json_object *obj;
		struct json_object *result_topps_object;
		int param_timestamp_start=0;
		int param_timestamp_end=0;
		const char *param_hostname = NULL;
		const char *param_first_or_last;
		short find_first=0;

		char *topps_filename_dir = NULL;

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

		/* Params : get the "first_or_last" */
		if(NULL == (obj = json_object_object_get(params, "first_or_last"))) {
				return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
		}
		if(!json_object_is_type (obj, json_type_string)) {
				return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
		}
		errno = 0;
		if(NULL == (param_first_or_last = json_object_get_string(obj))) {
				return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
		}
		if(errno != 0) {
				return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS);
		}
		if(!strcmp(param_first_or_last, "first")) {
				find_first = 1;
		} else if(!strcmp(param_first_or_last, "last")) {
				find_first = -1;
		}

		/* Check args */
		if(0 == param_timestamp_start) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
		if(0 == param_timestamp_end) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
		if(0 == find_first) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }
		if(NULL == param_hostname) { return (JSONRPC_ERROR_CODE_32602_INVALID_PARAMS); }

		/* Check the servers and build the result array */
		if(NULL == (result_topps_object = json_object_new_object())) {
				DEBUG (OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Could not create a json array");
				DEBUG(OUTPUT_PREFIX_JSONRPC_CB_TOPPS "Internal error %s:%d", __FILE__, __LINE__);
				return (JSONRPC_ERROR_CODE_32603_INTERNAL_ERROR);
		}

		if(NULL == (topps_filename_dir = check_path(param_hostname, param_timestamp_start, param_timestamp_end, find_first))) {
				obj =  json_object_new_string("path not found");
				json_object_object_add(result_topps_object, "status", obj);
				json_object_object_add(result, "result", result_topps_object);
				return(0);
		}

		/* TODO */

		/* Last : add the "result" to the result object */
		json_object_object_add(result, "result", result_topps_object);

		return(0);
} /* }}} jsonrpc_cb_topps_get_top */

/* vim: set fdm=marker sw=8 ts=8 tw=78 et : */
