/**
 * collectd - src/jsonrpc_cb_perfwatcher.h
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

#ifndef JSONRPC_CB_PERFWATCHER_H
#define JSONRPC_CB_PERFWATCHER_H

#define JSONRPC_CB_TABLE_PERFWATCHER \
	{ "pw_get_status",       jsonrpc_cb_pw_get_status       }, \
	{ "pw_get_metric",       jsonrpc_cb_pw_get_metric       }, \
	{ "pw_get_dir_hosts",    jsonrpc_cb_pw_get_dir_hosts    }, \
	{ "pw_get_dir_plugins",  jsonrpc_cb_pw_get_dir_plugins  }, \
	{ "pw_get_dir_types",    jsonrpc_cb_pw_get_dir_types    }, \
	{ "pw_get_dir_all_rrds_for_host", jsonrpc_cb_pw_get_dir_all_rrds_for_host }, \
	{ "pw_rrd_info",         jsonrpc_cb_pw_rrd_info         }, \
	{ "pw_rrd_check_files",  jsonrpc_cb_pw_rrd_check_files  }, \
	{ "pw_rrd_flush",        jsonrpc_cb_pw_rrd_flush        }, \
	{ "pw_rrd_graphonly",    jsonrpc_cb_pw_rrd_graphonly    }, \
	{ "pw_rrd_get_points",    jsonrpc_cb_pw_rrd_get_points   },

int jsonrpc_cb_pw_get_status      (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_get_metric      (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_get_dir_hosts   (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_get_dir_plugins (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_get_dir_types   (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_get_dir_all_rrds_for_host   (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_rrd_info        (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_rrd_check_files (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_rrd_flush       (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_rrd_graphonly   (struct json_object *params, struct json_object *result, const char **errorstring);
int jsonrpc_cb_pw_rrd_get_points  (struct json_object *params, struct json_object *result, const char **errorstring);

#endif /* JSONRPC_CB_PERFWATCHER_H */
