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
#include "utils_ignorelist.h"
#include <unistd.h>
#include <zlib.h>


static const char *config_keys[] =
{
	"DataDir",
	"Plugin",
	"InvertPluginList",
	"LinkLast"
};
static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);
static char *datadir   = NULL;
static ignorelist_t *linklast_list = NULL;
static ignorelist_t *plugintype_list = NULL;

static int value_list_to_filename (char *buffer, int buffer_len, const notification_t * n, short is_link_last) /* {{{ */
{
        int offset = 0;
        int status;
        char timebuffer[25]; /* 2^64 is a 20-digits decimal number. So 25 should be enough */
        time_t now;
        struct tm stm;


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


        if(is_link_last) {
                memcpy(timebuffer, "last", sizeof("last"));

                status = ssnprintf (buffer + offset, buffer_len - offset,
                                "%s/", timebuffer);
                if ((status < 1) || (status >= buffer_len - offset))
                        return (-1);
                offset += status;
        } else {
                /* TODO: Find a way to minimize the calls to `localtime_r',
                 * since they are pretty expensive.. */
                now = time (NULL);
                if (localtime_r (&now, &stm) == NULL)
                {
                        ERROR ("notify_file plugin: localtime_r failed");
                        return (1);
                }
                strftime(timebuffer, sizeof(timebuffer), "%s", &stm);
                status = ssnprintf (buffer + offset, buffer_len - offset,
                                "%1$.2s/%1$.4s/%1$.6s/", timebuffer);
                if ((status < 1) || (status >= buffer_len - offset))
                        return (-1);
                offset += status;
        }

        if (strlen (n->type_instance) > 0)
                status = ssnprintf (buffer + offset, buffer_len - offset,
                                "%s-%s-%s.gz", n->type, n->type_instance, timebuffer);
        else
                status = ssnprintf (buffer + offset, buffer_len - offset,
                                "%s-%s.gz", n->type, timebuffer);
        if ((status < 1) || (status >= buffer_len - offset))
                return (-1);

        return (0);
} /* }}} int value_list_to_filename */

static int notify_file_config (const char *key, const char *value) /* {{{ */
{
		if (plugintype_list == NULL) plugintype_list = ignorelist_create (/* invert = */ 1);
		if (plugintype_list == NULL) return (1);
		if (linklast_list == NULL) linklast_list = ignorelist_create (/* invert = */ 1);
		if (linklast_list == NULL) return (1);

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
		} else if (strcasecmp ("Plugin", key) == 0) {
				ignorelist_add (plugintype_list, value);
		} else if (strcasecmp ("InvertPluginList", key) == 0) {
				int invert = 1;
				if (IS_TRUE (value))
						invert = 0;
				ignorelist_set_invert (plugintype_list, invert);
		} else if (strcasecmp ("LinkLast", key) == 0) {
				ignorelist_add (linklast_list, value);
		} else {
				return (-1);
		}
		return (0);
} /* }}} int notify_file_config */

static int notify_file_notify (const notification_t * n, user_data_t __attribute__((unused)) *user_data) /* {{{ */
{
        char filename[512];
        gzFile notify_file;

        if (ignorelist_match (plugintype_list, n->plugin) != 0)
                return 0;

        if (value_list_to_filename (filename, sizeof (filename), n, 0) != 0)
                return (-1);

        DEBUG ("notify_file plugin: notify_file_write: filename = %s;", filename);

        if (check_create_dir (filename))
                return (-1);

        notify_file = gzopen (filename, "w");
        if (notify_file == NULL)
        {
                char errbuf[1024];
                ERROR ("notify_file plugin: gzopen (%s) failed: %s", filename,
                                sstrerror (errno, errbuf, sizeof (errbuf)));
                return (-1);
        }

        gzwrite(notify_file, n->message, strlen(n->message));
        //gzprintf (notify_file, "%s\n", n->message);

        /* The lock is implicitely released. I we don't release it explicitely
         * because the `FILE *' may need to flush a cache first */
        gzclose (notify_file);

        if (ignorelist_match (linklast_list, n->plugin) == 0) {
                char link_name[512];

                if (value_list_to_filename (link_name, sizeof (link_name), n, 1) != 0) return (-1);
                if (check_create_dir (link_name)) return (-1);

                unlink(link_name); /* No need to check for failure : link will do it */
                if(0 != symlink(filename, link_name)) {
                        char errbuf[1024];
                        ERROR ("notify_file plugin: failed to symlink %s -> %s (%s)", link_name, filename,
                                        sstrerror (errno, errbuf, sizeof (errbuf)));
                }
        }


        return (0);
} /* }}} int notify_file_notify */

void module_register (void)
{
	plugin_register_config ("notify_file", notify_file_config,
			config_keys, config_keys_num);
	plugin_register_notification ("notify_file", notify_file_notify, /* user_data = */ NULL);
} /* void module_register */

/* vim: set fdm=marker sw=8 ts=8 tw=78 et : */
