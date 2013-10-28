/**
 * collectd - src/netstat.c
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
 *   Yves Mettier <ymettier at free.fr>
 *   Code borrowed from processes.c
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"


#if HAVE_LINUX_CONFIG_H
#  include <linux/config.h>
#endif
#ifndef CONFIG_HZ
#  define CONFIG_HZ 100
#endif

#if HAVE_ARPA_INET_H
#  include <arpa/inet.h>
#endif

#include <regex.h>

#ifndef ARG_MAX
#  define ARG_MAX 4096
#endif

enum {
 TCP_STATE__ESTABLISHED = 1,
 TCP_STATE__SYN_SENT,
 TCP_STATE__SYN_RECV,
 TCP_STATE__FIN_WAIT1,
 TCP_STATE__FIN_WAIT2,
 TCP_STATE__TIME_WAIT,
 TCP_STATE__CLOSE,
 TCP_STATE__CLOSE_WAIT,
 TCP_STATE__LAST_ACK,
 TCP_STATE__LISTEN,
 TCP_STATE__CLOSING /* now a valid state */
};

#define ARRAY_SIZE_FIRST 4
#define ARRAY_SIZE_MAX_INCREMENT 16

struct inode_list_s;
struct procstat_s;
struct procstat_entry_s;

#define PROCNETSTAT_IP_LEN (sizeof("XXXX:XXXX:XXXX:XXXX:XXXX:XXXX:XXXX:XXXX"))
#define PROCNETSTAT_NAME_LEN 256
struct inode_list_s {
    unsigned long inode;
    unsigned long age;

    /* statistics */
    unsigned long rxq;
    unsigned long txq;

    /* IP config */
    int port;
    int ip_type;
    union {
        struct in_addr in;
        struct in6_addr in6;
    } ip_u;
    char ip_escaped[PROCNETSTAT_IP_LEN];

    struct inode_list_s *next;
};

struct procstat_entry_s {
    pid_t id;
    char name[PATH_MAX];
    unsigned long age;

    struct inode_list_s **inode_array;
    int inode_array_size;
    int inode_array_nb;

    struct procstat_s *match;
    struct procstat_entry_s *next;
};

struct procstat_s {
    char name[PROCNETSTAT_NAME_LEN];
    char ip[PROCNETSTAT_IP_LEN];
    int port;
    int ip_type;
    union {
        struct in_addr in;
        struct in6_addr in6;
    } ip_u;
    regex_t *re;

    struct procstat_s *next;
};

typedef struct inode_list_s inode_list_t;
typedef struct procstat_entry_s procstat_entry_t;
typedef struct procstat_s procstat_t;

#define pse_array(pse) ((inode_list_t*)((pse)->pse_array.ptr))

static procstat_t *list_proc_defs = NULL;
static procstat_entry_t *list_proc_entry = NULL;
static inode_list_t *list_inodes = NULL;

char *ps_read_process_name (int pid, char *name, int namelen) { /* {{{ */
    char  filename[64];
    char  buffer[1024];

    int   buffer_len;

    size_t name_start_pos;
    size_t name_end_pos;
    size_t name_len;

    ssnprintf (filename, sizeof (filename), "/proc/%i/stat", pid);

    buffer_len = read_file_contents (filename,
            buffer, sizeof(buffer) - 1);
    if (buffer_len <= 0)
        return (NULL);
    buffer[buffer_len] = 0;

    /* The name of the process is enclosed in parens. Since the name can
     * contain parens itself, spaces, numbers and pretty much everything
     * else, use these to determine the process name. We don't use
     * strchr(3) and strrchr(3) to avoid pointer arithmetic which would
     * otherwise be required to determine name_len. */
    name_start_pos = 0;
    while ((buffer[name_start_pos] != '(')
            && (name_start_pos < buffer_len))
        name_start_pos++;

    name_end_pos = buffer_len;
    while ((buffer[name_end_pos] != ')')
            && (name_end_pos > 0))
        name_end_pos--;

    /* Either '(' or ')' is not found or they are in the wrong order.
     * Anyway, something weird that shouldn't happen ever. */
    if (name_start_pos >= name_end_pos)
    {
        ERROR ("processes plugin: name_start_pos = %zu >= name_end_pos = %zu",
                name_start_pos, name_end_pos);
        return (NULL);
    }

    name_len = (name_end_pos - name_start_pos) - 1;

    sstrncpy (name, &buffer[name_start_pos + 1], ((name_len + 1) <= namelen)?name_len + 1:namelen);

    /* success */
    return (name);
} /* }}} char *ps_read_process_name (...) */

static char *ps_get_cmdline (pid_t pid, char *psname, char *buf, size_t buf_len) { /* {{{ */
	char  *buf_ptr;
	size_t len;

	char file[PATH_MAX];
	int  fd;

	size_t n;

	if ((pid < 1) || (NULL == buf) || (buf_len < 2))
		return NULL;

	ssnprintf (file, sizeof (file), "/proc/%u/cmdline",
		       	(unsigned int) pid);

	errno = 0;
	fd = open (file, O_RDONLY);
	if (fd < 0) {
		char errbuf[4096];
		/* ENOENT means the process exited while we were handling it.
		 * Don't complain about this, it only fills the logs. */
		if (errno != ENOENT)
			WARNING ("processes plugin: Failed to open `%s': %s.", file,
					sstrerror (errno, errbuf, sizeof (errbuf)));
		return NULL;
	}

	buf_ptr = buf;
	len     = buf_len;

	n = 0;

	while (42) {
		ssize_t status;

		status = read (fd, (void *)buf_ptr, len);

		if (status < 0) {
			char errbuf[1024];

			if ((EAGAIN == errno) || (EINTR == errno))
				continue;

			WARNING ("processes plugin: Failed to read from `%s': %s.", file,
					sstrerror (errno, errbuf, sizeof (errbuf)));
			close (fd);
			return NULL;
		}

		n += status;

		if (status == 0)
			break;

		buf_ptr += status;
		len     -= status;

		if (len <= 0)
			break;
	}

	close (fd);

	if (0 == n) {
		/* cmdline not available; e.g. kernel thread, zombie */
		ssnprintf (buf, buf_len, "[%s]", psname);
		return buf;
	}

	assert (n <= buf_len);

	if (n == buf_len)
		--n;
	buf[n] = '\0';

	--n;
	/* remove trailing whitespace */
	while ((n > 0) && (isspace (buf[n]) || ('\0' == buf[n]))) {
		buf[n] = '\0';
		--n;
	}

	/* arguments are separated by '\0' in /proc/<pid>/cmdline */
	while (n > 0) {
		if ('\0' == buf[n])
			buf[n] = ' ';
		--n;
	}
	return buf;
} /* }}} char *ps_get_cmdline (...) */

/* put name of process from config to list_proc_defs tree
 * list_proc_defs is a list of 'procstat_t' structs with
 * processes names we want to watch
 */
static void ps_list_register (const char *name, const char *regexp, const char *ip, int port) { /* {{{ */
    procstat_t *new;
    procstat_t *ptr;
    int status;

    new = (procstat_t *) malloc (sizeof (procstat_t));
    if (new == NULL)
    {
        ERROR ("processes plugin: ps_list_register: malloc failed.");
        return;
    }
    memset (new, 0, sizeof (procstat_t));
    sstrncpy (new->name, name, sizeof (new->name));
    sstrncpy (new->ip, ip, sizeof (new->ip));
    new->port = port;

    /* Prepare IP stuff */
    if(ip[0]) {
        if(1 == inet_pton(AF_INET6, ip, &((new->ip_u).in6))) {
            new->ip_type = AF_INET6;
        } else if(0 != inet_aton(ip, &((new->ip_u).in))) {
            new->ip_type = AF_INET;
        } else {
            WARNING ("processes plugin: Invalid IP format. "
                    "This definition (%s) will be ignored. "
                    "Accepted formats : '' (all) or 'x.x.x.x' (IPv4) or 'x:x:x:x:x:x:x:x' (IPv6). ",
                    name);
            sfree (new);
            return;
        }
    }

	if ((regexp != NULL) && (regexp[0])) {
		DEBUG ("ProcessMatch: adding \"%s\" as criteria to process %s.", regexp, name);
		new->re = (regex_t *) malloc (sizeof (regex_t));
		if (new->re == NULL)
		{
			ERROR ("processes plugin: ps_list_register: malloc failed.");
			sfree (new);
			return;
		}

		status = regcomp (new->re, regexp, REG_EXTENDED | REG_NOSUB);
		if (status != 0)
		{
			DEBUG ("ProcessMatch: compiling the regular expression \"%s\" failed.", regexp);
			sfree(new->re);
			return;
		}
	}

	for (ptr = list_proc_defs; ptr != NULL; ptr = ptr->next) {
		if (strcmp (ptr->name, name) == 0)
		{
			WARNING ("processes plugin: You have configured more "
					"than one `Process' or "
					"`ProcessMatch' with the same name. "
					"All but the first setting will be "
					"ignored.");
			if(new->re) sfree (new->re);
			sfree (new);
			return;
		}

		if (ptr->next == NULL)
			break;
	}

	if (ptr == NULL)
		list_proc_defs = new;
	else
		ptr->next = new;
} /* }}} void ps_list_register */

/* Add an inode to a procstat_entry_t */
static void inode_list_add(procstat_entry_t *pse, unsigned long inode) { /* {{{ */
    inode_list_t *il;
    int i;
    /* Check if the inode already exists */
    for(il = list_inodes; il; il = il->next) {
        if(inode == il->inode) break;
    }
    /* Check if we need to create a new one */
    if(NULL == il) {
        if(NULL == (il = malloc(sizeof(*il)))) {
            return;
        }
        memset(il, '\0', sizeof(*il));
        il->inode = inode;
        il->next = list_inodes;
        list_inodes = il;
    }

    /* inode found (found or newly created). Let's fill it */
    il->age = 0;
    for(i=0; i < pse->inode_array_nb; i++) {
        if(pse->inode_array[i] == il) return;
    }

    /* Append the inode to the pse->inode_array */
    if(pse->inode_array_nb == pse->inode_array_size) {
        /* Not enough space : let's resize the array */
        int s;
        if(pse->inode_array_size == 0) s = ARRAY_SIZE_FIRST;
        else if(pse->inode_array_size > ARRAY_SIZE_MAX_INCREMENT) s = ARRAY_SIZE_MAX_INCREMENT;
        else s = pse->inode_array_size;
        s += pse->inode_array_size;

        if(NULL == (pse->inode_array = realloc(pse->inode_array, s * sizeof(**pse->inode_array)))) {
            pse->inode_array_size = 0;
            pse->inode_array_nb = 0;
            return;
        }
        pse->inode_array_size = s;
    }
    pse->inode_array[pse->inode_array_nb] = il;
    pse->inode_array_nb++;

    return;
} /* }}} void inode_list_add */

/* Update sockets list for a procstat_entry_t */
static int pse_list_update_inodes(procstat_entry_t *pse) { /* {{{ */
    DIR *dh;
    char proc_pid_fd[64]; /* Should be enough for /proc/<pid>/fd/<num> */

    snprintf(proc_pid_fd, sizeof(proc_pid_fd), "/proc/%d/fd", pse->id);
    if(NULL == (dh = opendir(proc_pid_fd))) {
        return(1);
    }
    int proc_pid_fd_len = strlen(proc_pid_fd);
    char *p = proc_pid_fd + proc_pid_fd_len;
    struct dirent *ent;
    p[0] = '/';
    p++;

    while(NULL != (ent = readdir(dh))) {
        struct stat st;
        if(!strcmp(ent->d_name, ".")) continue;
        if(!strcmp(ent->d_name, "..")) continue;
        strncpy(p, ent->d_name, proc_pid_fd_len);

        if((0 == stat(proc_pid_fd, &st)) && (S_ISSOCK(st.st_mode))) {
            inode_list_add(pse, st.st_ino);
        }
    }
    closedir(dh);

    return(0);
} /* }}} int pse_list_update_inodes */

/* try to match name against entry, returns 1 if success */
static int ps_list_match (const char *cmdline, procstat_t *ps) { /* {{{ */
    int status;
    if (NULL == ps->re) return(0); /* match them all */

    assert (cmdline != NULL);

    status = regexec (ps->re, cmdline,
            /* nmatch = */ 0,
            /* pmatch = */ NULL,
            /* eflags = */ 0);
    if (status == 0) return (1);

    return (0);
} /* }}} int ps_list_match */

/* Find procstat_t that match name/cmdline */
static procstat_t *ps_list_search(const char *cmdline) { /* {{{ */
    procstat_t *ps;
    for (ps = list_proc_defs; ps; ps = ps->next) {
        if(1 == (ps_list_match (cmdline, ps))) return(ps);
    }
    return(NULL);
} /* }}} procstat_t *ps_list_search */

/* Find procstat_entry_t that match a given PID */
static procstat_entry_t *pse_list_search(pid_t pid) { /* {{{ */
    procstat_entry_t *pse;
    for(pse = list_proc_entry; pse; pse = pse->next) {
        if(pse->id == pid) return(pse);
    }
    return(NULL);
} /* }}} procstat_entry_t *pse_list_search_entry */

/* Create a new procstat_entry_t and add it to the list it */
static procstat_entry_t *pse_list_register(pid_t pid) { /* {{{ */
    procstat_entry_t *pse;
	char cmdline[ARG_MAX];
    char psname[PATH_MAX];
    char *rs;

    if(NULL == ps_read_process_name(pid, psname, sizeof(psname))) return(NULL); /* if there is no process name, we can do nothing */

    if(NULL == (rs = ps_get_cmdline(pid, psname, cmdline, sizeof(cmdline)))) return(NULL); /* if there is no command line, we can do nothing */

    if(NULL == (pse = malloc (sizeof (*pse)))) return(NULL);

    memset (pse, 0, sizeof (*pse));
    strncpy(pse->name, psname, sizeof(pse->name));
    pse->id = pid;
    pse->match = ps_list_search(rs);
    pse->next = list_proc_entry;
    list_proc_entry = pse;

    return (pse);
} /* }}} procstat_entry_t *pse_list_register */

/* Free a procstat_entry_t */
static void pse_list_free(procstat_entry_t *pse) { /* {{{ */
    if(pse->inode_array) free(pse->inode_array);
    free(pse);
} /* }}} void pse_list_free */

/* Check if a procstat_entry_t exists. If yes, update. If not, create a new one and register it */
static void pse_list_update(pid_t pid) { /* {{{ */
    procstat_entry_t *pse;

    if(NULL == (pse = pse_list_search(pid))) {
        pse = pse_list_register(pid);
    }
    if(NULL == pse) return;
    if(pse->match) {
        if(0 !=  pse_list_update_inodes(pse)) {
            return; /* do not refresh it : it will die when age is too big. */
        }
    }
    pse->age = 0;
} /* }}} void pse_list_update */

/* remove old entries from instances of processes in list_proc_defs */
static int update_process_list (void) { /* {{{ */
    struct dirent *ent;
    DIR           *proc;
    inode_list_t *il, *ilprev;
    procstat_entry_t *pse, *pseprev;

/* Mark all inode sockets for deletion. Mark all pse for deletion too.
 * Then some will be refreshed.
 * All remaining marked for deletion will be deleted at last.
 */
    for(il = list_inodes; il; il = il->next) {
        il->age++;
    }
    for (pse = list_proc_entry; pse; pse = pse->next) {
        pse->age++;
    }
/* Update the processes list */
    if ((proc = opendir ("/proc")) == NULL) {
        char errbuf[1024];
        ERROR ("Cannot open `/proc': %s",
                sstrerror (errno, errbuf, sizeof (errbuf)));
        return (-1);
    }

    while ((ent = readdir (proc)) != NULL) {
        int pid;

        if (!isdigit (ent->d_name[0])) continue;
        if ((pid = atoi (ent->d_name)) < 1) continue;

        pse_list_update(pid);
    } /* while(readdir) */
    closedir (proc);

/* Remove all remaining socket inodes still marked for deletion */
    ilprev = NULL;
    for(il = list_inodes; il; il = il->next) {
        if(il->age > 0) {
            if(NULL == ilprev) {
                list_inodes = il->next;
                free(il);
                il = list_inodes;
            } else {
                ilprev->next = il->next;
                free(il);
                il = ilprev;
            }
        }
    }
/* Remove all remaining procstat_entry_t still marked for deletion */
    pseprev = NULL;
    for(pse = list_proc_entry; pse; pse = pse->next) {
        if(pse->age > 0) {
            if(NULL == pseprev) {
                list_proc_entry = pse->next;
                pse_list_free(pse);
                pse = list_proc_entry;
            } else {
                pseprev->next = pse->next;
                pse_list_free(pse);
                pse = pseprev;
            }
        } else {
            pseprev = pse;
        }
    }

    return (0);
} /* }}} int update_process_list */

/* submit global state (e.g.: txq or rxq) */
static void submit_value (char *plugin_instance, const char *state, double value) { /* {{{ */
	value_t values[1];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].gauge = value;

	vl.values = values;
	vl.values_len = 1;
	sstrncpy (vl.host, hostname_g, sizeof (vl.host));
	sstrncpy (vl.plugin, "procnetstat", sizeof (vl.plugin));
	sstrncpy (vl.plugin_instance, plugin_instance, sizeof (vl.plugin_instance));
	sstrncpy (vl.type, "netstatqueue", sizeof (vl.type));
	sstrncpy (vl.type_instance, state, sizeof (vl.type_instance));

	plugin_dispatch_values (&vl);
} /* }}} void submit_value */

/* Update statistics in an inode */
static void update_inode_list_with_inode(unsigned long inode, char *addr, int port, unsigned long txq, unsigned long rxq) { /* {{{ */
    inode_list_t *il;
    for(il = list_inodes; il; il = il->next) {
        if(il->inode == inode) break;
    }
    if(NULL == il) return; /* not found, not interesting */

    il->rxq= rxq;
    il->txq= txq;
    if(0 == il->port) {
        int i;
        il->port = port;

        if (strlen(addr) > 8) {
#if HAVE_NETINET_IP6_H
            sscanf(addr, "%08X%08X%08X%08X", &((il->ip_u).in6).s6_addr32[0], &((il->ip_u).in6).s6_addr32[1],
                    &((il->ip_u).in6).s6_addr32[2], &((il->ip_u).in6).s6_addr32[3]);
            il->ip_type = AF_INET6;
            if(NULL == inet_ntop(AF_INET6, &((il->ip_u).in6), il->ip_escaped, sizeof(il->ip_escaped))) {
                il->ip_escaped[0] = '\0';
            }
#endif
        } else {
            sscanf(addr, "%X",
                    &((struct sockaddr_in *) &((il->ip_u).in))->sin_addr.s_addr);
            il->ip_type = AF_INET;
            if(NULL == inet_ntop(AF_INET, &((il->ip_u).in), il->ip_escaped, sizeof(il->ip_escaped))) {
                il->ip_escaped[0] = '\0';
            }
        }
        for(i=0; il->ip_escaped[i]; i++) {
            if(NULL == strchr("0123456789ABCDEFabcdef:.", il->ip_escaped[i])) il->ip_escaped[i] = '_';
        }
    }
} /* }}} void update_inode_list_with_inode */

/* Read tcp data from /proc/net/tcp */
static int read_tcp() { /* {{{ */
    FILE *fh;
    char buffer[1024];
    int n;

    if (NULL == (fh = fopen ("/proc/net/tcp", "r")))
        return (-1);

    n = 0;
    while (fgets (buffer, sizeof(buffer), fh) != NULL) {
        unsigned long rxq, txq, time_len, retr, inode;
        int num, local_port, rem_port, d, state, uid, timer_run, timeout;
        char rem_addr[128], local_addr[128];
#if HAVE_AFINET6
        struct sockaddr_in6 localaddr;
        char addr6[INET6_ADDRSTRLEN];
        struct in6_addr in6;
#endif

        n++;
        if (n == 1) continue;

        num = sscanf(buffer,
                "%d: %64[0-9A-Fa-f]:%X %64[0-9A-Fa-f]:%X %X %lX:%lX %X:%lX %lX %d %d %lu %*s\n",
                &d, local_addr, &local_port, rem_addr, &rem_port, &state,
                &txq, &rxq, &timer_run, &time_len, &retr, &uid, &timeout, &inode);

        if (num < 11) {
            DEBUG ("procnetstat: warning, got bogus tcp line in /dev/proc/tcp");
            continue;
        }

        update_inode_list_with_inode(inode, local_addr, local_port, txq, rxq);

    } /* while (fgets) */

    if (fclose (fh)) {
        char errbuf[1024];
        WARNING ("procnetstat: fclose: %s", sstrerror (errno, errbuf, sizeof (errbuf)));
    }

    return (0);
} /* }}} int read_tcp() */

static int compare_ip_and_port(procstat_t *ps, inode_list_t *il) { /* {{{ */
    /* Check if port differ */
    if((ps->port) && (ps->port != il->port)) return(1);

    if(ps->ip[0]) {
        /* Check if ip type differ */
        if((il->ip_type != ps->ip_type)) return(1);

        if(ps->ip_type == AF_INET) {
            if(
                    ((struct sockaddr_in *)&((il->ip_u).in))->sin_addr.s_addr
                    !=
                    ((struct sockaddr_in *)&((ps->ip_u).in))->sin_addr.s_addr
              ) return(1);

        } else if(ps->ip_type == AF_INET6) {
            int i;
            for (i=0; i<4; i++) {
                if(((il->ip_u).in6).s6_addr32[i] != ((ps->ip_u).in6).s6_addr32[i]) return(1);
            }
        } else {
            return(1); /* what is that ? */
        }
    }

    return(0);
} /* }}} int compare_ip_and_port */

/* Collects statistics and send them */
static void read_statistics_from_cache() { /* {{{ */

    procstat_entry_t *pse;
    for(pse = list_proc_entry; pse; pse = pse->next) {
        procstat_t *ps;
        int i;
        if(NULL == (ps = pse->match)) continue;
        if(0 < pse->age) continue; /* old process */

        for(i=0; i<pse->inode_array_nb; i++) {
            inode_list_t *il = pse->inode_array[i];
            if(0 < il->age) continue; /* old inode */
            if(0 == compare_ip_and_port(ps,il)) {
                char plugin_instance[DATA_MAX_NAME_LEN];
                char *name;
                if(ps->re) {
                    name = ps->name;
                } else {
                    name = pse->name;
                }
                if(ps->ip[0]) {
                    if(ps->port) {
                        strncpy(plugin_instance, ps->name, sizeof(plugin_instance));
                    } else {
                        snprintf(plugin_instance, sizeof(plugin_instance), "%s_%d", ps->name, il->port);
                    }
                } else {
                    if(ps->port) {
                        snprintf(plugin_instance, sizeof(plugin_instance), "%s_%s", ps->name, il->ip_escaped);
                    } else {
                        snprintf(plugin_instance, sizeof(plugin_instance), "%s_%s_%d", ps->name, il->ip_escaped, il->port);
                    }
                }

                submit_value(plugin_instance, "rxq", il->rxq);
                submit_value(plugin_instance, "txq", il->txq);


            }
        }

    }

} /* }}} void read_statistics_from_cache */

/* do actual readings from kernel */
static int psn_read (void) { /* {{{ */

    update_process_list();
    read_tcp();
    read_statistics_from_cache();

    return (0);
} /* }}} int psn_read */

static int psn_init (void) { /* {{{ */
	return (0);
} /* }}} int psn_init */

/* put all pre-defined 'Process' names from config to list_proc_defs tree */
static int psn_config (oconfig_item_t *ci) { /* {{{ */
	int i;
    /* Syntax :
     * ProcessMatch <name:string> <regex:string> <ip:string> <port:number>
     * name : any name. Will be used as prefix for plugin instance.
     * regex : a regex to match a process.
     * ip : a IPv4 or IPv6 address, or an empty string to match all local address
     * port : a port number, or 0 to match any port.
     *
     * Plugin instance depends on if ip and port are set or not.
     * ip:y port:y -> name
     * ip:n port:y -> name_ip
     * ip:y port:n -> name_port
     * ip:n port:n -> name_ip_port
     *
     * If not set, ip or port come from /proc/net/tcp.
     * - ip is set if ip[0] is not nul.
     * - port is set if port is not nul.
     *
     * If there is no regex, name is the short name of the process. Otherwise, name is the name of the rule.
     */

	for (i = 0; i < ci->children_num; ++i) {
		oconfig_item_t *c = ci->children + i;

		if (strcasecmp (c->key, "ProcessMatch") == 0)
		{
			if ((c->values_num != 4)
					|| (OCONFIG_TYPE_STRING != c->values[0].type)
					|| (OCONFIG_TYPE_STRING != c->values[1].type)
					|| (OCONFIG_TYPE_STRING != c->values[2].type)
					|| (OCONFIG_TYPE_NUMBER != c->values[3].type)
                    )
			{
				ERROR ("procnetstat plugin: `ProcessMatch' needs exactly "
						"3 string and 1 number arguments (got %i).",
						c->values_num);
				continue;
			}

			if (c->children_num != 0) {
				WARNING ("procnetstat plugin: the `ProcessMatch' config option "
						"does not expect any child elements -- ignoring "
						"content (%i elements) of the <ProcessMatch '%s' '%s' '%s' '%d'> "
						"block.", c->children_num, 
                        c->values[0].value.string,
                        c->values[1].value.string,
                        c->values[2].value.string,
                        (int)(c->values[3].value.number)
                        );
			}

            ps_list_register (
                    c->values[0].value.string,
                    c->values[1].value.string,
                    c->values[2].value.string,
                    (int)(c->values[3].value.number)
                    );
		}
		else
		{
			ERROR ("processes plugin: The `%s' configuration option is not "
					"understood and will be ignored.", c->key);
			continue;
		}
	}

	return (0);
} /* }}} int psn_config */

void module_register (void) { /* {{{ */
	plugin_register_complex_config ("procnetstat", psn_config);
	plugin_register_init ("procnetstat", psn_init);
	plugin_register_read ("procnetstat", psn_read);
} /* }}} void module_register */

/* vim: set filetype=c fdm=marker sw=4 ts=4 et : */
