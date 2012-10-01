/**
 * collectd - src/write_mysql.c
 * Copyright (C) 2011-2012  Cyril Feraudet
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
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
 *   Cyril Feraudet <cyril at feraudet.com>
 **/

#include "collectd.h"
#include "plugin.h"
#include "common.h"

#include "utils_cache.h"
#include "utils_parse_option.h"
#include "utils_avltree.c"

#ifdef HAVE_MYSQL_H
#include <mysql.h>
#elif defined(HAVE_MYSQL_MYSQL_H)
#include <mysql/mysql.h>
#endif

#include <pthread.h>

//static c_avl_tree_t *host_tree, *plugin_tree, *type_tree, *dataset_tree =
//  NULL;

typedef struct dataset_s dataset_t;
struct dataset_s
{
  char name[DATA_MAX_NAME_LEN];
  int id;
  int type_id;
};

typedef struct mysql_database_s mysql_database_t;
struct mysql_database_s
{
	char *instance;
	char *host;
	char *user;
	char *passwd;
	char *database;
	int   port;
	_Bool replace;
	c_avl_tree_t *host_tree, *plugin_tree, *type_tree, *dataset_tree; 
	MYSQL *conn;
	MYSQL_BIND data_bind[8], notif_bind[8];
	MYSQL_STMT *data_stmt, *notif_stmt;
	pthread_mutex_t mutexdb;
	pthread_mutex_t mutexhost_tree, mutexplugin_tree;
	pthread_mutex_t mutextype_tree, mutexdataset_tree;
	char data_query[1024];
}; 

static int write_mysql_write (const data_set_t * ds, const value_list_t * vl,
	user_data_t * ud);

static int write_mysql_init (user_data_t * ud);
static int notify_write_mysql (const notification_t * n, user_data_t * ud);
/*
static const char *config_keys[] = {
  "Host",
  "User",
  "Passwd",
  "Database",
  "Port",
  "Replace"
};
static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);

static char *host = "localhost";
static char *user = "root";
static char *passwd = "";
static char *database = "collectd";
static int   port = 0;
static _Bool replace = 1;
*/

#define HOST_ITEM   0
#define PLUGIN_ITEM 1
#define TYPE_ITEM   2

static const char *notif_query =
  "INSERT INTO notification  (date,host_id,plugin_id,"
  "plugin_instance,type_id,type_instance,severity,message) VALUES "
  "(?,?,?,?,?,?,?,?)";

static int
write_mysql_config_database (oconfig_item_t *ci)
{
	mysql_database_t *db;
	int status = 0;
	int i;

	if ((ci->values_num != 1)
	    || (ci->values[0].type != OCONFIG_TYPE_STRING))
	{
		WARNING ("write_mysql plugin: The `Database' block "
			 "needs exactly one string argument.");
		return (-1);
	}

	db = (mysql_database_t *) malloc (sizeof (*db));
	if (db == NULL)
	{
		ERROR ("mysql plugin: malloc failed.");
		return (-1);
	}
	memset (db, '\0', sizeof (*db));
	db->host		= NULL;
	db->user		= NULL;
	db->passwd		= NULL;
	db->database	= NULL;
	db->port		= NULL;
	db->replace		= NULL;

	status = cf_util_get_string (ci, &db->instance);
	if (status != 0)
	{
	    sfree (db);
		return (status);
	}
	assert (db->instance != NULL);
	for (i = 0; i < ci->children_num; i++)
	{
		oconfig_item_t *child = ci->children + i;
		if (strcasecmp ("Host", child->key) == 0)
			status = cf_util_get_string (child, &db->host);
		else if (strcasecmp ("User", child->key) == 0)
			status = cf_util_get_string (child, &db->user);
		else if (strcasecmp ("Passwd", child->key) == 0)
			status = cf_util_get_string (child, &db->passwd);
		else if (strcasecmp ("Port", child->key) == 0)
		{
			status = cf_util_get_port_number (child);
			if (status > 0)
			{
				db->port = status;
				status = 0;
			}
		}
		else if (strcasecmp ("Database", child->key) == 0)
			status = cf_util_get_string (child, &db->database);
		else if (strcasecmp ("Replace", child->key) == 0)
		    status = cf_util_get_boolean (child, &db->replace);
		else
        {
            WARNING ("write_mysql plugin: Option `%s' not allowed here.", child->key);
            status = -1;
        }

        if (status != 0)
            break;
	}

	if (status == 0)
	{
		user_data_t ud;
		char cb_name[DATA_MAX_NAME_LEN];
		DEBUG ("write_mysql plugin: Registering new write callback: %s",
			db->instance);
		ssnprintf (cb_name, sizeof (cb_name), "write_mysql/%s", db->instance);
		memset (&ud, 0, sizeof (ud));
		ud.data = (void *) db;
		//ud.free_func = write_mysql_database_free;
		write_mysql_init (&ud);
		plugin_register_write(cb_name, write_mysql_write, &ud);
		//plugin_register_notification (cb_name, notify_write_mysql, &ud);
	}
}

static int
write_mysql_config (oconfig_item_t *ci)
{
	int i;
	if (ci == NULL)
		return (EINVAL);
	for (i = 0; i < ci->children_num; i++)
	{
		oconfig_item_t *child = ci->children + i;

		if (strcasecmp ("Database", child->key) == 0) {
			write_mysql_config_database (child);
		} else
			WARNING ("mysql plugin: Option \"%s\" not allowed here.",
					child->key);
	}
	return (0);
}

/*
static int
write_mysql_config (const char *key, const char *value)
{
  if (strcasecmp ("Host", key) == 0)
    {
      host = strdup (value);
    }
  else if (strcasecmp ("User", key) == 0)
    {
      user = strdup (value);
    }
  else if (strcasecmp ("Passwd", key) == 0)
    {
      passwd = strdup (value);
    }
  else if (strcasecmp ("Database", key) == 0)
    {
      database = strdup (value);
    }
  else if (strcasecmp ("Port", key) == 0)
    {
      port = service_name_to_port_number (value);
    }
  else if (strcasecmp ("Replace", key) == 0)
    {
      replace = IS_TRUE (value);
    }
}
*/

static int
write_mysql_init (user_data_t * ud)
{
  mysql_database_t *db;
  my_bool my_true = 1;
  if ((ud == NULL) || (ud->data == NULL))
  { 
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	return (-1);
  }     
  db = (mysql_database_t *) ud->data;
  db->conn = mysql_init (NULL);
  if (!mysql_thread_safe ())
    {
      ERROR ("write_mysql plugin: mysqlclient Thread Safe OFF");
      return (-1);
    }
  else
    {
      DEBUG ("write_mysql plugin: mysqlclient Thread Safe ON");
    }
  if (mysql_real_connect (db->conn, db->host, db->user, db->passwd, db->database, db->port, NULL, 0)
      == NULL)
    {
      ERROR ("write_mysql plugin: Failed to connect to database %s "
	     " at server %s with user %s : %s", db->database, db->host, db->user,
	     mysql_error (db->conn));
    }
  char tmpquery[1024] = "%s INTO data "
    "(date,host_id,plugin_id,plugin_instance,type_id,type_instance,dataset_id,value)"
    " VALUES (?,?,?,?,?,?,?,?)";
  ssnprintf (db->data_query, sizeof (tmpquery), tmpquery, db->replace ? "REPLACE" : "INSERT");
  mysql_options (db->conn, MYSQL_OPT_RECONNECT, &my_true);
  db->data_stmt = mysql_stmt_init (db->conn);
  db->notif_stmt = mysql_stmt_init (db->conn);
  mysql_stmt_prepare (db->data_stmt, db->data_query, strlen (db->data_query));
  mysql_stmt_prepare (db->notif_stmt, notif_query, strlen (notif_query));
  db->host_tree = c_avl_create ((void *) strcmp);
  db->plugin_tree = c_avl_create ((void *) strcmp);
  db->type_tree = c_avl_create ((void *) strcmp);
  db->dataset_tree = c_avl_create ((void *) strcmp);
  return (0);
}


static int
add_item_id (const char *name, const int item, user_data_t * ud)
{
  int *id = malloc (sizeof (int));
  char query[1024];
  pthread_mutex_t *mutex;
  c_avl_tree_t *tree;
  MYSQL_BIND param_bind[1], result_bind[1];
  MYSQL_STMT *stmt;
  mysql_database_t *db;
  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	  return (-1);
  }
  db = (mysql_database_t *) ud->data;
  ssnprintf (query, sizeof (query), "SELECT id FROM %s WHERE name = ?",
	     item == HOST_ITEM ? "host" : item ==
	     PLUGIN_ITEM ? "plugin" : "type");
  DEBUG ("write_mysql plugin: %s", query);
  memset (param_bind, '\0', sizeof (MYSQL_BIND));
  memset (result_bind, '\0', sizeof (MYSQL_BIND));
  param_bind[0].buffer_type = MYSQL_TYPE_STRING;
  param_bind[0].buffer = (char *) name;
  param_bind[0].buffer_length = strlen (name);
  result_bind[0].buffer_type = MYSQL_TYPE_LONG;
  result_bind[0].buffer = (void *) id;
  pthread_mutex_lock (&db->mutexdb);
  if (mysql_ping (db->conn) != 0)
    {
      ERROR
	("write_mysql plugin: add_item_id - Failed to re-connect to database : %s",
	 mysql_error (db->conn));
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  stmt = mysql_stmt_init (db->conn);
  if (mysql_stmt_prepare (stmt, query, strlen (query)) != 0)
    {
      ERROR
	("write_mysql plugin: add_item_id - Failed to prepare statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_bind_param (stmt, param_bind) != 0)
    {
      ERROR
	("write_mysql plugin: add_item_id - Failed to bind param to statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_bind_result (stmt, result_bind) != 0)
    {
      ERROR
	("write_mysql plugin: add_item_id - Failed to bind result to statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_execute (stmt) != 0)
    {
      ERROR
	("write_mysql plugin: add_item_id - Failed to execute re-prepared statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_store_result (stmt) != 0)
    {
      ERROR
	("write_mysql plugin: add_item_id - Failed to store result : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_fetch (stmt) == 0)
    {
      mysql_stmt_free_result (stmt);
      mysql_stmt_close (stmt);
      pthread_mutex_unlock (&db->mutexdb);
      DEBUG ("get %s_id from DB : %d (%s)",
	     item == HOST_ITEM ? "host" : item ==
	     PLUGIN_ITEM ? "plugin" : "type", *id, name);
    }
  else
    {
      mysql_stmt_free_result (stmt);
      mysql_stmt_close (stmt);
      stmt = mysql_stmt_init (db->conn);
      ssnprintf (query, sizeof (query), "INSERT INTO %s (name) VALUES (?)",
		 item == HOST_ITEM ? "host" : item ==
		 PLUGIN_ITEM ? "plugin" : "type");
      if (mysql_stmt_prepare (stmt, query, strlen (query)) != 0)
	{
	  ERROR
	    ("write_mysql plugin: add_item_id - Failed to prepare statement : %s / %s",
	     mysql_stmt_error (stmt), query);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
      if (mysql_stmt_bind_param (stmt, param_bind) != 0)
	{
	  ERROR
	    ("write_mysql plugin: add_item_id - Failed to bind param to statement : %s / %s",
	     mysql_stmt_error (stmt), query);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
      if (mysql_stmt_execute (stmt) != 0)
	{
	  ERROR
	    ("write_mysql plugin: add_item_id - Failed to execute re-prepared statement : %s / %s",
	     mysql_stmt_error (stmt), query);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
      *id = mysql_stmt_insert_id (stmt);
      mysql_stmt_close (stmt);
      pthread_mutex_unlock (&db->mutexdb);
      DEBUG ("insert %s_id in DB : %d (%s)",
	     item == HOST_ITEM ? "host" : item ==
	     PLUGIN_ITEM ? "plugin" : "type", *id, name);
    }
  switch (item)
    {
    case HOST_ITEM:
      mutex = &db->mutexhost_tree;
      tree = db->host_tree;
      break;
    case PLUGIN_ITEM:
      mutex = &db->mutexplugin_tree;
      tree = db->plugin_tree;
      break;
    case TYPE_ITEM:
      mutex = &db->mutextype_tree;
      tree = db->type_tree;
      break;
    }
  pthread_mutex_lock (mutex);
  c_avl_insert (tree, strdup (name), (void *) id);
  pthread_mutex_unlock (mutex);
  return *id;
}

static int
add_dataset_id (data_source_t * ds, int type_id, user_data_t * ud)
{
  int *id = malloc (sizeof (int));
  char tree_key[DATA_MAX_NAME_LEN * 2];
  dataset_t *newdataset;
  char *type;
  mysql_database_t *db;
  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	  return (-1);
  }
  db = (mysql_database_t *) ud->data;
  switch (ds->type)
    {
    case DS_TYPE_COUNTER:
      type = "COUNTER";
      break;
    case DS_TYPE_DERIVE:
      type = "DERIVE";
      break;
    case DS_TYPE_ABSOLUTE:
      type = "ABSOLUTE";
      break;
    default:
      type = "GAUGE";
      break;
    }
  char *query = "SELECT id FROM dataset WHERE name = ? AND type_id = ?";
  MYSQL_BIND param_bind[2], result_bind[1], param_bind2[5];
  MYSQL_STMT *stmt;
  memset (param_bind, '\0', sizeof (MYSQL_BIND) * 2);
  memset (param_bind2, '\0', sizeof (MYSQL_BIND) * 5);
  memset (result_bind, '\0', sizeof (MYSQL_BIND));
  param_bind[0].buffer_type = MYSQL_TYPE_STRING;
  param_bind[0].buffer = (char *) ds->name;
  param_bind[0].buffer_length = strlen (ds->name);
  param_bind[1].buffer_type = MYSQL_TYPE_LONG;
  param_bind[1].buffer = (void *) &type_id;
  param_bind2[0].buffer_type = MYSQL_TYPE_STRING;
  param_bind2[0].buffer = (char *) ds->name;
  param_bind2[0].buffer_length = strlen (ds->name);
  param_bind2[1].buffer_type = MYSQL_TYPE_LONG;
  param_bind2[1].buffer = (void *) &type_id;
  param_bind2[2].buffer_type = MYSQL_TYPE_STRING;
  param_bind2[2].buffer = (char *) &type;
  param_bind2[2].buffer_length = strlen (type);
  param_bind2[3].buffer_type = MYSQL_TYPE_DOUBLE;
  param_bind2[3].buffer = (void *) &ds->min;
  param_bind2[4].buffer_type = MYSQL_TYPE_DOUBLE;
  param_bind2[4].buffer = (void *) &ds->max;
  result_bind[0].buffer_type = MYSQL_TYPE_LONG;
  result_bind[0].buffer = (void *) id;
  pthread_mutex_lock (&db->mutexdb);
  if (mysql_ping (db->conn) != 0)
    {
      ERROR
	("write_mysql plugin: add_dataset_id - Failed to re-connect to database : %s",
	 mysql_error (db->conn));
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  stmt = mysql_stmt_init (db->conn);
  if (mysql_stmt_prepare (stmt, query, strlen (query)) != 0)
    {
      ERROR
	("write_mysql plugin: add_dataset_id - Failed to prepare statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_bind_param (stmt, param_bind) != 0)
    {
      ERROR
	("write_mysql plugin: add_dataset_id - Failed to bind param to statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_bind_result (stmt, result_bind) != 0)
    {
      ERROR
	("write_mysql plugin: add_dataset_id - Failed to bind result to statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_execute (stmt) != 0)
    {
      ERROR
	("write_mysql plugin: add_dataset_id - Failed to execute re-prepared statement : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_store_result (stmt) != 0)
    {
      ERROR
	("write_mysql plugin: add_dataset_id - Failed to store result : %s / %s",
	 mysql_stmt_error (stmt), query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_fetch (stmt) == 0)
    {
      mysql_stmt_free_result (stmt);
      mysql_stmt_close (stmt);
      pthread_mutex_unlock (&db->mutexdb);
      DEBUG ("get dataset_id from DB : %d (%s) (%d)", *id, ds->name, type_id);
    }
  else
    {
      mysql_stmt_free_result (stmt);
      mysql_stmt_close (stmt);
      stmt = mysql_stmt_init (db->conn);
      char *queryins =
	"INSERT INTO dataset (name,type_id,type,min,max) VALUES (?,?,?,?,?)";
      if (mysql_stmt_prepare (stmt, queryins, strlen (queryins)) != 0)
	{
	  ERROR
	    ("write_mysql plugin: add_dataset_id - Failed to prepare statement : %s / %s",
	     mysql_stmt_error (stmt), queryins);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
      if (mysql_stmt_bind_param (stmt, param_bind2) != 0)
	{
	  ERROR
	    ("write_mysql plugin: add_dataset_id - Failed to bind param to statement : %s / %s",
	     mysql_stmt_error (stmt), queryins);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
      if (mysql_stmt_execute (stmt) != 0)
	{
	  ERROR
	    ("write_mysql plugin: add_dataset_id - Failed to execute re-prepared statement : %s / %s",
	     mysql_stmt_error (stmt), queryins);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
      *id = mysql_stmt_insert_id (stmt);
      mysql_stmt_close (stmt);
      // pthread_mutex_unlock (&db->mutexdb);
      DEBUG ("insert dataset_id in DB : %d (%s) (%d)", *id, ds->name,
	     type_id);
    }
  pthread_mutex_unlock (&db->mutexdb);
  ssnprintf (tree_key, sizeof (tree_key), "%s_%d", ds->name, type_id);
  newdataset = malloc (sizeof (dataset_t));
  sstrncpy (newdataset->name, ds->name, sizeof (newdataset->name));
  newdataset->id = *id;
  newdataset->type_id = type_id;
  pthread_mutex_lock (&db->mutexdataset_tree);
  c_avl_insert (db->dataset_tree, strdup (tree_key), newdataset);
  pthread_mutex_unlock (&db->mutexdataset_tree);
  sfree (id);
  return newdataset->id;

}

static int
get_item_id (const char *name, const int item, user_data_t * ud)
{
  int *id;
  pthread_mutex_t *mutex;
  c_avl_tree_t *tree;
  mysql_database_t *db;

  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	return (-1);
  }
  
  db = (mysql_database_t *) ud->data;

  switch (item)
    {
    case HOST_ITEM:
      mutex = &db->mutexhost_tree;
      tree = db->host_tree;
      break;
    case PLUGIN_ITEM:
      mutex = &db->mutexplugin_tree;
      tree = db->plugin_tree;
      break;
    case TYPE_ITEM:
      mutex = &db->mutextype_tree;
      tree = db->type_tree;
      break;
    }
  if (strlen (name) == 0)
    {
      return -1;
    }
  pthread_mutex_lock (mutex);
  if (c_avl_get (tree, name, (void *) &id) == 0)
    {
      pthread_mutex_unlock (mutex);
      DEBUG ("get_item_id : get %s_id for %s from cache",
	     item == HOST_ITEM ? "host" : item ==
	     PLUGIN_ITEM ? "plugin" : "type", name);
      return *id;
    }
  else
    {
      pthread_mutex_unlock (mutex);
      DEBUG ("get_item_id : insert %s_id for %s into cache",
	     item == HOST_ITEM ? "host" : item ==
	     PLUGIN_ITEM ? "plugin" : "type", name);
      return add_item_id (name, item, ud);
    }
}

static int
get_dataset_id (data_source_t * ds, int type_id, user_data_t * ud)
{
  char tree_key[DATA_MAX_NAME_LEN * 2];
  dataset_t *newdataset;
  mysql_database_t *db;
  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	  return (-1);
  }
  db = (mysql_database_t *) ud->data;
  ssnprintf (tree_key, sizeof (tree_key), "%s_%d", ds->name, type_id);
  pthread_mutex_lock (&db->mutexdataset_tree);
  if (c_avl_get (db->dataset_tree, tree_key, (void *) &newdataset) == 0)
    {
      pthread_mutex_unlock (&db->mutexdataset_tree);
      DEBUG ("dataset_id from cache : %d | %s", newdataset->id, tree_key);
      return newdataset->id;
    }
  else
    {
      pthread_mutex_unlock (&db->mutexdataset_tree);
      return add_dataset_id (ds, type_id, ud);
    }
}

static int wm_cdtime_t_to_mysql_time (cdtime_t in, MYSQL_TIME *out) /* {{{ */
{
  time_t t;
  struct tm stm;

  memset (out, 0, sizeof (*out));
  memset (&stm, 0, sizeof (stm));

  t = CDTIME_T_TO_TIME_T (in);
  if (localtime_r (&t, &stm) == NULL)
  {
    ERROR ("write_mysql plugin: localtime_r(%.3f) failed.",
        CDTIME_T_TO_DOUBLE (in));
    return (-1);
  }

  out->year   = stm.tm_year + 1900;
  out->month  = stm.tm_mon + 1;
  out->day    = stm.tm_mday;
  out->hour   = stm.tm_hour;
  out->minute = stm.tm_min;
  out->second = stm.tm_sec;

  return (0);
} /* }}} int wm_cdtime_t_to_mysql_time */

static int
write_mysql_write (const data_set_t * ds, const value_list_t * vl,
		   user_data_t * ud)
{
  int i;
  int host_id, plugin_id, type_id, aa;
  gauge_t *rates = NULL;

  host_id = get_item_id ((char *) vl->host, HOST_ITEM, ud);
  plugin_id = get_item_id ((char *) vl->plugin, PLUGIN_ITEM, ud);
  type_id = get_item_id ((char *) vl->type, TYPE_ITEM, ud);

  mysql_database_t *db;
  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	  return (-1);
  }
  db = (mysql_database_t *) ud->data;

  if (host_id == -1 || plugin_id == -1 || type_id == -1)
    {
      return -1;
    }

  for (i = 0; i < ds->ds_num; i++)
    {
      int len;
      data_source_t *dsrc = ds->ds + i;
      int dataset_id = get_dataset_id (dsrc, type_id, ud);
      MYSQL_BIND binding[8];
      MYSQL_TIME mysql_date;

      if (dataset_id == -1)
        return -1;

      wm_cdtime_t_to_mysql_time (vl->time, &mysql_date);

      memset (binding, 0, sizeof (binding));
      binding[0].buffer_type = MYSQL_TYPE_DATETIME;
      binding[0].buffer = (char *) &mysql_date;
      binding[1].buffer_type = MYSQL_TYPE_LONG;
      binding[1].buffer = (void *) &host_id;
      binding[2].buffer_type = MYSQL_TYPE_LONG;
      binding[2].buffer = (void *) &plugin_id;
      binding[3].buffer_type = MYSQL_TYPE_STRING;
      binding[3].buffer = (void *) vl->plugin_instance;
      binding[3].buffer_length = strlen (vl->plugin_instance);
      binding[4].buffer_type = MYSQL_TYPE_LONG;
      binding[4].buffer = (void *) &type_id;
      binding[5].buffer_type = MYSQL_TYPE_STRING;
      binding[5].buffer = (void *) vl->type_instance;
      binding[5].buffer_length = strlen (vl->type_instance);
      binding[6].buffer_type = MYSQL_TYPE_LONG;
      binding[6].buffer = (void *) &dataset_id;
      if (dsrc->type == DS_TYPE_GAUGE)
	{
	  binding[7].buffer_type = MYSQL_TYPE_DOUBLE;
	  binding[7].buffer = (void *) &(vl->values[i].gauge);

	}
      else
	{
	  if (rates == NULL)
	    {
	      rates = uc_get_rate (ds, vl);
	    }
	  if (rates == NULL)
	    {
	      sfree (rates);
	      continue;
	    }
	  binding[7].buffer_type = MYSQL_TYPE_DOUBLE;
	  binding[7].buffer = (void *) &(rates[i]);
	}

      pthread_mutex_lock (&db->mutexdb);

      if (mysql_ping (db->conn) != 0)
	{
	  ERROR
	    ("write_mysql plugin: write_mysql_write - Failed to re-connect to database : %s",
	     mysql_error (db->conn));
	  pthread_mutex_unlock (&db->mutexdb);
	  sfree (rates);
	  return -1;
	}
      if (mysql_stmt_bind_param (db->data_stmt, binding) != 0)
	{
	  ERROR
	    ("write_mysql plugin: write_mysql_write - Failed to bind param to statement : %s / %s",
	     mysql_stmt_error (db->data_stmt), db->data_query);
	  pthread_mutex_unlock (&db->mutexdb);
	  sfree (rates);
	  return -1;
	}
      if (mysql_stmt_execute (db->data_stmt) != 0)
	{
	  // Try to re-prepare statement
	  db->data_stmt = mysql_stmt_init (db->conn);
	  mysql_stmt_prepare (db->data_stmt, db->data_query, strlen (db->data_query));
	  mysql_stmt_bind_param (db->data_stmt, binding);
	  if (mysql_stmt_execute (db->data_stmt) != 0)
	    {
	      ERROR
		("write_mysql plugin: Failed to execute re-prepared statement : %s / %s",
		 mysql_stmt_error (db->data_stmt), db->data_query);
	      pthread_mutex_unlock (&db->mutexdb);
	      sfree (rates);
	      return -1;
	    }
	}
      sfree (rates);
      pthread_mutex_unlock (&db->mutexdb);
    }
  return (0);
}

static int
notify_write_mysql (const notification_t * n, user_data_t * ud)
{

  int host_id, plugin_id, type_id, len;
  char severity[32];
  MYSQL_TIME mysql_date;

  mysql_database_t *db;
  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("write_mysql plugin: get_item_id: Invalid user data.");
	  return (-1);
  }
  db = (mysql_database_t *) ud->data;

  host_id = get_item_id (n->host, HOST_ITEM, ud);
  plugin_id = get_item_id (n->plugin, PLUGIN_ITEM, ud);
  type_id = get_item_id (n->type, TYPE_ITEM, ud);

  wm_cdtime_t_to_mysql_time (n->time, &mysql_date);

  memset (db->notif_bind, 0, sizeof (db->notif_bind));

  pthread_mutex_lock (&db->mutexdb);

  db->notif_bind[0].buffer_type = MYSQL_TYPE_DATETIME;
  db->notif_bind[0].buffer = (char *) &mysql_date;
  db->notif_bind[0].is_null = 0;
  db->notif_bind[0].length = 0;
  db->notif_bind[1].buffer_type = MYSQL_TYPE_LONG;
  db->notif_bind[1].buffer = (void *) &host_id;
  db->notif_bind[2].buffer_type = MYSQL_TYPE_LONG;
  db->notif_bind[2].buffer = (void *) &plugin_id;
  db->notif_bind[3].buffer_type = MYSQL_TYPE_STRING;
  db->notif_bind[3].buffer = (void *) n->plugin_instance;
  db->notif_bind[3].buffer_length = strlen (n->plugin_instance);
  db->notif_bind[4].buffer_type = MYSQL_TYPE_LONG;
  db->notif_bind[4].buffer = (void *) &type_id;
  db->notif_bind[5].buffer_type = MYSQL_TYPE_STRING;
  db->notif_bind[5].buffer = (void *) n->type_instance;
  db->notif_bind[5].buffer_length = strlen (n->type_instance);
  db->notif_bind[6].buffer_type = MYSQL_TYPE_STRING;
  ssnprintf (severity, sizeof (severity), "%s",
	     (n->severity == NOTIF_FAILURE) ? "FAILURE"
	     : ((n->severity == NOTIF_WARNING) ? "WARNING"
		: ((n->severity == NOTIF_OKAY) ? "OKAY" : "UNKNOWN")));
  db->notif_bind[6].buffer = (void *) severity;
  db->notif_bind[6].buffer_length = strlen (severity);
  db->notif_bind[7].buffer_type = MYSQL_TYPE_VAR_STRING;
  db->notif_bind[7].buffer = (void *) n->message;
  db->notif_bind[7].buffer_length = strlen (n->message);
  if (mysql_ping (db->conn) != 0)
    {
      ERROR
	("write_mysql plugin: write_mysql_write - Failed to re-connect to database : %s",
	 mysql_error (db->conn));
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }
  if (mysql_stmt_bind_param (db->notif_stmt, db->notif_bind) != 0)
    {
      ERROR
	("write_mysql plugin: notify_write_mysql - Failed to bind param to statement : %s / %s",
	 mysql_stmt_error (db->notif_stmt), notif_query);
      pthread_mutex_unlock (&db->mutexdb);
      return -1;
    }

  if (mysql_stmt_execute (db->notif_stmt) != 0)
    {
      // Try to re-prepare statement
      db->notif_stmt = mysql_stmt_init (db->conn);
      mysql_stmt_prepare (db->notif_stmt, notif_query, strlen (notif_query));
      mysql_stmt_bind_param (db->notif_stmt, db->notif_bind);
      if (mysql_stmt_execute (db->notif_stmt) != 0)
	{
	  ERROR
	    ("write_mysql plugin: Failed to execute re-prepared statement : %s / %s",
	     mysql_stmt_error (db->notif_stmt), notif_query);
	  pthread_mutex_unlock (&db->mutexdb);
	  return -1;
	}
    }
  pthread_mutex_unlock (&db->mutexdb);
  return 0;
}
/*
static void
free_tree (c_avl_tree_t * tree)
{
  void *key = NULL;
  void *value = NULL;

  if (tree == NULL)
    {
      return;
    }
  while (c_avl_pick (tree, &key, &value) == 0)
    {
      sfree (key);
      sfree (value);
      key = NULL;
      value = NULL;
    }
  c_avl_destroy (tree);
  tree = NULL;
}

static int
write_mysql_shutdown (void)
{
  free_tree (host_tree);
  free_tree (plugin_tree);
  free_tree (type_tree);
  free_tree (dataset_tree);
  mysql_close (conn);
  return 0;
}
*/
void
module_register (void)
{
	plugin_register_complex_config ("write_mysql", write_mysql_config);
//  plugin_register_init ("write_mysql", write_mysql_init);
//  plugin_register_config ("write_mysql", write_mysql_config,
//			  config_keys, config_keys_num);
//  plugin_register_write ("write_mysql", write_mysql_write, /* user_data = */
//			 NULL);
//  plugin_register_shutdown ("write_mysql", write_mysql_shutdown);
//  plugin_register_notification ("write_mysql", notify_write_mysql,
//				/* user_data = */ NULL);
}
