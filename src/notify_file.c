/**
 * collectd - src/notify_file.c 
 * Copyright (C) 2012       Cyril Feraudet
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
 * Lots code portion are grabed from src/csv.c 
 *
 * Authors:
 *   Cyril Feraudet <cyril@feraudet.com>
 **/

#include "collectd.h"
#include "plugin.h"
#include "common.h"
#include "utils_cache.h"
#include "utils_parse_option.h"
#include <zlib.h>


static const char *config_keys[] =
{
	"DataDir"
};
static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);
static char *datadir   = NULL;


static int value_list_to_filename (char *buffer, int buffer_len,
		const notification_t * n)
{
	int offset = 0;
	int status;

	if (datadir != NULL)
	{
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s/", datadir);
		if ((status < 1) || (status >= buffer_len - offset))
			return (-1);
		offset += status;
	}

	status = ssnprintf (buffer + offset, buffer_len - offset,
			"%s/", n->host);
	if ((status < 1) || (status >= buffer_len - offset))
		return (-1);
	offset += status;

	if (strlen (n->plugin_instance) > 0)
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s-%s/", n->plugin, n->plugin_instance);
	else
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s/", n->plugin);
	if ((status < 1) || (status >= buffer_len - offset))
		return (-1);
	offset += status;

	if (strlen (n->type_instance) > 0)
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s-%s", n->type, n->type_instance);
	else
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s", n->type);
	if ((status < 1) || (status >= buffer_len - offset))
		return (-1);
	offset += status;

	time_t now;
	struct tm stm;

	/* TODO: Find a way to minimize the calls to `localtime_r',
	 * since they are pretty expensive.. */
	now = time (NULL);
	if (localtime_r (&now, &stm) == NULL)
	{
		ERROR ("notify_file plugin: localtime_r failed");
		return (1);
	}

	strftime (buffer + offset, buffer_len - offset,
			"-%s.gz", &stm);

	return (0);
} /* int value_list_to_filename */

static int notify_file_config (const char *key, const char *value)
{
	if (strcasecmp ("DataDir", key) == 0)
	{
		if (datadir != NULL)
			free (datadir);
		datadir = strdup (value);
		if (datadir != NULL)
		{
			int len = strlen (datadir);
			while ((len > 0) && (datadir[len - 1] == '/'))
			{
				len--;
				datadir[len] = '\0';
			}
			if (len <= 0)
			{
				free (datadir);
				datadir = NULL;
			}
		}
	}
	else
	{
		return (-1);
	}
	return (0);
} /* int notify_file_config */

static int notify_file_notify (const notification_t * n,
		user_data_t __attribute__((unused)) *user_data)
{
	char         filename[512];
	FILE        *notify_file;
	int          notify_file_fd;

	if (value_list_to_filename (filename, sizeof (filename), n) != 0)
		return (-1);

	DEBUG ("notify_file plugin: notify_file_write: filename = %s;", filename);

	if (check_create_dir (filename))
		return (-1);

	notify_file = gzopen (filename, "w+");
	if (notify_file == NULL)
	{
		char errbuf[1024];
		ERROR ("notify_file plugin: gzopen (%s) failed: %s", filename,
				sstrerror (errno, errbuf, sizeof (errbuf)));
		return (-1);
	}
	notify_file_fd = fileno (notify_file);

	gzwrite(notify_file, n->message, strlen(n->message));
	//gzprintf (notify_file, "%s\n", n->message);

	/* The lock is implicitely released. I we don't release it explicitely
	 * because the `FILE *' may need to flush a cache first */
	gzclose (notify_file);

	return (0);
} /* int notify_file_notify */

void module_register (void)
{
	plugin_register_config ("notify_file", notify_file_config,
			config_keys, config_keys_num);
	plugin_register_notification ("notify_file", notify_file_notify, /* user_data = */ NULL);
} /* void module_register */

