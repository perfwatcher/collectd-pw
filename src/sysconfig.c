#include "collectd.h"
#include "plugin.h"
#include "common.h"
#include <time.h>

static time_t sysconfig_last_run = 0;

static void get_dmidecode (char *message) {
    char buf[80]; 
    int status;
    char *fstatus;
    FILE *fp;
    status = system("/usr/sbin/dmidecode --dump-bin /tmp/dmidecode.bin >/dev/null "
       "; base64 /tmp/dmidecode.bin > /tmp/dmidecode.b64 ; rm /tmp/dmidecode.bin");
    if ( status && (fp = fopen("/tmp/dmidecode.b64","r"))) {
        while((fstatus = fgets(buf, 80, fp))) {
            strncat(message, buf, sizeof(buf));
        }
        fclose(fp);
    }
}

static void get_collectd_version (char *message, size_t maxlen) {
	snprintf(message, maxlen, 
		"Package=%s\nVersion=%s\nComplation date=%s %s\n",
		PACKAGE,     VERSION,    __DATE__, __TIME__
		);
}

static void get_collectd_package_version (char *message) {
    char buf[80]; 
    char *fstatus;
    FILE *fp;
        if ((fp = fopen("/etc/collectd.release","r"))) {
            while((fstatus = fgets(buf, 80, fp))) {
                strncat(message, buf, sizeof(buf));
            }
            fclose(fp);
        }
}

static void get_distrib (char *message) {
    int n;
    char *fstatus;
    FILE *fp;
    char buf[255]; 
    char file[39][25] = {"/etc/annvix-release","/etc/arch-release","/etc/arklinux-release",
    "/etc/aurox-release","/etc/blackcat-release","/etc/cobalt-release","/etc/conectiva-release",
    "/etc/debian_version","/etc/debian_release","/etc/fedora-release","/etc/gentoo-release",
    "/etc/immunix-release","/etc/knoppix_version","/etc/lfs-release","/etc/linuxppc-release",
    "/etc/mandrake-release","/etc/mandriva-release","/etc/mandrake-release",
    "/etc/mandakelinux-release","/etc/mklinux-release","/etc/nld-release","/etc/pld-release",
    "/etc/redhat-release","/etc/redhat_version","/etc/slackware-version","/etc/slackware-release",
    "/etc/e-smith-release","/etc/release","/etc/sun-release","/etc/SuSE-release",
    "/etc/novell-release","/etc/sles-release","/etc/tinysofa-release","/etc/turbolinux-release",
    "/etc/lsb-release","/etc/ultrapenguin-release","/etc/UnitedLinux-release","/etc/va-release",
    "/etc/yellowdog-release"};
    for (n = 0; n < 39; n++) {
        if ((fp = fopen(file[n],"r"))) {
            while((fstatus = fgets(buf, 255, fp))) {
                strncat(message, buf, sizeof(buf));
            }
            fclose(fp);
        }
    }
}

static void sysconfig_notify(notification_t *notif, const char *type, const char *message) {
        memset (notif, '\0', sizeof (*notif));
        notif->severity = NOTIF_OKAY;
        notif->time = cdtime ();
        sstrncpy(notif->host, hostname_g, sizeof(notif->host));
        sstrncpy(notif->plugin, "sysconfig", sizeof(notif->plugin));
        sstrncpy(notif->type, type, sizeof(notif->type));
        sstrncpy(notif->message, message, sizeof(notif->message));
        plugin_dispatch_notification(notif);
}
    
static void sysconfig_collectd_data_and_send(void) {
    char   message[NOTIF_MAX_MSG_LEN];
    notification_t notif;

    message[0] = '\0';
    get_dmidecode (message);
    if (strlen(message) > 0) { sysconfig_notify(&notif, "dmidecode", message); }

    message[0] = '\0';
    get_collectd_version (message, sizeof(message));
    if (strlen(message) > 0) { sysconfig_notify(&notif, "collectd_version_info", message); }

    message[0] = '\0';
    get_collectd_package_version (message);
    if (strlen(message) > 0) { sysconfig_notify(&notif, "collectd_package_version", message); }

    message[0] = '\0';
    get_distrib (message);
    if (strlen(message) > 0) { sysconfig_notify(&notif, "distrib", message); }
}

static int sysconfig_read (void) {
	time_t now;

	now = time(NULL);
	/* Run every 24 hours */
	if(now-sysconfig_last_run > 86400) {
			sysconfig_collectd_data_and_send();
			sysconfig_last_run = now;
	}
    return 0;
}

void module_register (void)
{
	plugin_register_read ("sysconfig", sysconfig_read);
}

