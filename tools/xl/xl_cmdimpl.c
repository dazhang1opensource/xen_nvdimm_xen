/*
 * Copyright 2009-2017 Citrix Ltd and other contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/utsname.h> /* for utsname in xl info */
#include <xentoollog.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <xen/hvm/e820.h>

#include <libxl.h>
#include <libxl_utils.h>
#include <libxl_json.h>
#include <libxlutil.h>
#include "xl.h"
#include "xl_utils.h"
#include "xl_parse.h"

int logfile = 2;

/* every libxl action in xl uses this same libxl context */
libxl_ctx *ctx;

xlchild children[child_max];

const char *common_domname;
static int fd_lock = -1;

static const char savefileheader_magic[32]=
    "Xen saved domain, xl format\n \0 \r";

#ifndef LIBXL_HAVE_NO_SUSPEND_RESUME
static const char migrate_receiver_banner[]=
    "xl migration receiver ready, send binary domain data.\n";
static const char migrate_receiver_ready[]=
    "domain received, ready to unpause";
static const char migrate_permission_to_go[]=
    "domain is yours, you are cleared to unpause";
static const char migrate_report[]=
    "my copy unpause results are as follows";
#endif

  /* followed by one byte:
   *     0: everything went well, domain is running
   *            next thing is we all exit
   * non-0: things went badly
   *            next thing should be a migrate_permission_to_go
   *            from target to source
   */

#define XL_MANDATORY_FLAG_JSON (1U << 0) /* config data is in JSON format */
#define XL_MANDATORY_FLAG_STREAMv2 (1U << 1) /* stream is v2 */
#define XL_MANDATORY_FLAG_ALL  (XL_MANDATORY_FLAG_JSON |        \
                                XL_MANDATORY_FLAG_STREAMv2)

struct save_file_header {
    char magic[32]; /* savefileheader_magic */
    /* All uint32_ts are in domain's byte order. */
    uint32_t byteorder; /* SAVEFILE_BYTEORDER_VALUE */
    uint32_t mandatory_flags; /* unknown flags => reject restore */
    uint32_t optional_flags; /* unknown flags => reject restore */
    uint32_t optional_data_len; /* skip, or skip tail, if not understood */
};

/* Optional data, in order:
 *   4 bytes uint32_t  config file size
 *   n bytes           config file in Unix text file format
 */

#define SAVEFILE_BYTEORDER_VALUE ((uint32_t)0x01020304UL)

struct domain_create {
    int debug;
    int daemonize;
    int monitor; /* handle guest reboots etc */
    int paused;
    int dryrun;
    int quiet;
    int vnc;
    int vncautopass;
    int console_autoconnect;
    int checkpointed_stream;
    const char *config_file;
    char *extra_config; /* extra config string */
    const char *restore_file;
    char *colo_proxy_script;
    int migrate_fd; /* -1 means none */
    int send_back_fd; /* -1 means none */
    char **migration_domname_r; /* from malloc */
};

int child_report(xlchildnum child)
{
    int status;
    pid_t got = xl_waitpid(child, &status, 0);
    if (got < 0) {
        fprintf(stderr, "xl: warning, failed to waitpid for %s: %s\n",
                children[child].description, strerror(errno));
        return ERROR_FAIL;
    } else if (status) {
        xl_report_child_exitstatus(XTL_ERROR, child, got, status);
        return ERROR_FAIL;
    } else {
        return 0;
    }
}

static void console_child_report(xlchildnum child)
{
    if (xl_child_pid(child))
        child_report(child);
}

static int vncviewer(uint32_t domid, int autopass)
{
    libxl_vncviewer_exec(ctx, domid, autopass);
    fprintf(stderr, "Unable to execute vncviewer\n");
    return 1;
}

static void autoconnect_vncviewer(uint32_t domid, int autopass)
{
   console_child_report(child_vncviewer);

    pid_t pid = xl_fork(child_vncviewer, "vncviewer child");
    if (pid)
        return;

    postfork();

    sleep(1);
    vncviewer(domid, autopass);
    _exit(EXIT_FAILURE);
}

static int acquire_lock(void)
{
    int rc;
    struct flock fl;

    /* lock already acquired */
    if (fd_lock >= 0)
        return ERROR_INVAL;

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fd_lock = open(lockfile, O_WRONLY|O_CREAT, S_IWUSR);
    if (fd_lock < 0) {
        fprintf(stderr, "cannot open the lockfile %s errno=%d\n", lockfile, errno);
        return ERROR_FAIL;
    }
    if (fcntl(fd_lock, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd_lock);
        fprintf(stderr, "cannot set cloexec to lockfile %s errno=%d\n", lockfile, errno);
        return ERROR_FAIL;
    }
get_lock:
    rc = fcntl(fd_lock, F_SETLKW, &fl);
    if (rc < 0 && errno == EINTR)
        goto get_lock;
    if (rc < 0) {
        fprintf(stderr, "cannot acquire lock %s errno=%d\n", lockfile, errno);
        rc = ERROR_FAIL;
    } else
        rc = 0;
    return rc;
}

static int release_lock(void)
{
    int rc;
    struct flock fl;

    /* lock not acquired */
    if (fd_lock < 0)
        return ERROR_INVAL;

release_lock:
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    rc = fcntl(fd_lock, F_SETLKW, &fl);
    if (rc < 0 && errno == EINTR)
        goto release_lock;
    if (rc < 0) {
        fprintf(stderr, "cannot release lock %s, errno=%d\n", lockfile, errno);
        rc = ERROR_FAIL;
    } else
        rc = 0;
    close(fd_lock);
    fd_lock = -1;

    return rc;
}

static yajl_gen_status printf_info_one_json(yajl_gen hand, int domid,
                                            libxl_domain_config *d_config)
{
    yajl_gen_status s;

    s = yajl_gen_map_open(hand);
    if (s != yajl_gen_status_ok)
        goto out;

    s = yajl_gen_string(hand, (const unsigned char *)"domid",
                        sizeof("domid")-1);
    if (s != yajl_gen_status_ok)
        goto out;
    if (domid != -1)
        s = yajl_gen_integer(hand, domid);
    else
        s = yajl_gen_null(hand);
    if (s != yajl_gen_status_ok)
        goto out;

    s = yajl_gen_string(hand, (const unsigned char *)"config",
                        sizeof("config")-1);
    if (s != yajl_gen_status_ok)
        goto out;
    s = libxl_domain_config_gen_json(hand, d_config);
    if (s != yajl_gen_status_ok)
        goto out;

    s = yajl_gen_map_close(hand);
    if (s != yajl_gen_status_ok)
        goto out;

out:
    return s;
}

static void printf_info(enum output_format output_format,
                        int domid,
                        libxl_domain_config *d_config, FILE *fh)
{
    if (output_format == OUTPUT_FORMAT_SXP)
        return printf_info_sexp(domid, d_config, fh);

    const char *buf;
    libxl_yajl_length len = 0;
    yajl_gen_status s;
    yajl_gen hand;

    hand = libxl_yajl_gen_alloc(NULL);
    if (!hand) {
        fprintf(stderr, "unable to allocate JSON generator\n");
        return;
    }

    s = printf_info_one_json(hand, domid, d_config);
    if (s != yajl_gen_status_ok)
        goto out;

    s = yajl_gen_get_buf(hand, (const unsigned char **)&buf, &len);
    if (s != yajl_gen_status_ok)
        goto out;

    fputs(buf, fh);

out:
    yajl_gen_free(hand);

    if (s != yajl_gen_status_ok)
        fprintf(stderr,
                "unable to format domain config as JSON (YAJL:%d)\n", s);

    flush_stream(fh);
}

static int do_daemonize(char *name, const char *pidfile)
{
    char *fullname;
    pid_t child1;
    int nullfd, ret = 0;

    child1 = xl_fork(child_waitdaemon, "domain monitoring daemonizing child");
    if (child1) {
        ret = child_report(child_waitdaemon);
        if (ret) goto out;
        ret = 1;
        goto out;
    }

    postfork();

    ret = libxl_create_logfile(ctx, name, &fullname);
    if (ret) {
        LOG("failed to open logfile %s: %s",fullname,strerror(errno));
        exit(-1);
    }

    CHK_SYSCALL(logfile = open(fullname, O_WRONLY|O_CREAT|O_APPEND, 0644));
    free(fullname);
    assert(logfile >= 3);

    CHK_SYSCALL(nullfd = open("/dev/null", O_RDONLY));
    assert(nullfd >= 3);

    dup2(nullfd, 0);
    dup2(logfile, 1);
    dup2(logfile, 2);

    close(nullfd);

    CHK_SYSCALL(daemon(0, 1));

    if (pidfile) {
        int fd = open(pidfile, O_RDWR | O_CREAT, S_IRUSR|S_IWUSR);
        char *pid = NULL;

        if (fd == -1) {
            perror("Unable to open pidfile");
            exit(1);
        }

        if (asprintf(&pid, "%ld\n", (long)getpid()) == -1) {
            perror("Formatting pid");
            exit(1);
        }

        if (write(fd, pid, strlen(pid)) < 0) {
            perror("Writing pid");
            exit(1);
        }

        if (close(fd) < 0) {
            perror("Closing pidfile");
            exit(1);
        }

        free(pid);
    }

out:
    return ret;
}

static void reload_domain_config(uint32_t domid,
                                 libxl_domain_config *d_config)
{
    int rc;
    uint8_t *t_data;
    int ret, t_len;
    libxl_domain_config d_config_new;

    /* In case user has used "config-update" to store a new config
     * file.
     */
    ret = libxl_userdata_retrieve(ctx, domid, "xl", &t_data, &t_len);
    if (ret && errno != ENOENT) {
        LOG("\"xl\" configuration found but failed to load\n");
    }
    if (t_len > 0) {
        LOG("\"xl\" configuration found, using it\n");
        libxl_domain_config_dispose(d_config);
        libxl_domain_config_init(d_config);
        parse_config_data("<updated>", (const char *)t_data,
                          t_len, d_config);
        free(t_data);
        libxl_userdata_unlink(ctx, domid, "xl");
        return;
    }

    libxl_domain_config_init(&d_config_new);
    rc = libxl_retrieve_domain_configuration(ctx, domid, &d_config_new);
    if (rc) {
        LOG("failed to retrieve guest configuration (rc=%d). "
            "reusing old configuration", rc);
        libxl_domain_config_dispose(&d_config_new);
    } else {
        libxl_domain_config_dispose(d_config);
        /* Steal allocations */
        memcpy(d_config, &d_config_new, sizeof(libxl_domain_config));
    }
}

/* Can update r_domid if domain is destroyed */
static domain_restart_type handle_domain_death(uint32_t *r_domid,
                                               libxl_event *event,
                                               libxl_domain_config *d_config)
{
    domain_restart_type restart = DOMAIN_RESTART_NONE;
    libxl_action_on_shutdown action;

    switch (event->u.domain_shutdown.shutdown_reason) {
    case LIBXL_SHUTDOWN_REASON_POWEROFF:
        action = d_config->on_poweroff;
        break;
    case LIBXL_SHUTDOWN_REASON_REBOOT:
        action = d_config->on_reboot;
        break;
    case LIBXL_SHUTDOWN_REASON_SUSPEND:
        LOG("Domain has suspended.");
        return 0;
    case LIBXL_SHUTDOWN_REASON_CRASH:
        action = d_config->on_crash;
        break;
    case LIBXL_SHUTDOWN_REASON_WATCHDOG:
        action = d_config->on_watchdog;
        break;
    case LIBXL_SHUTDOWN_REASON_SOFT_RESET:
        action = d_config->on_soft_reset;
        break;
    default:
        LOG("Unknown shutdown reason code %d. Destroying domain.",
            event->u.domain_shutdown.shutdown_reason);
        action = LIBXL_ACTION_ON_SHUTDOWN_DESTROY;
    }

    LOG("Action for shutdown reason code %d is %s",
        event->u.domain_shutdown.shutdown_reason,
        get_action_on_shutdown_name(action));

    if (action == LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_DESTROY || action == LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_RESTART) {
        char *corefile;
        int rc;

        xasprintf(&corefile, XEN_DUMP_DIR "/%s", d_config->c_info.name);
        LOG("dumping core to %s", corefile);
        rc = libxl_domain_core_dump(ctx, *r_domid, corefile, NULL);
        if (rc) LOG("core dump failed (rc=%d).", rc);
        free(corefile);
        /* No point crying over spilled milk, continue on failure. */

        if (action == LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_DESTROY)
            action = LIBXL_ACTION_ON_SHUTDOWN_DESTROY;
        else
            action = LIBXL_ACTION_ON_SHUTDOWN_RESTART;
    }

    switch (action) {
    case LIBXL_ACTION_ON_SHUTDOWN_PRESERVE:
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_RESTART_RENAME:
        reload_domain_config(*r_domid, d_config);
        restart = DOMAIN_RESTART_RENAME;
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_RESTART:
        reload_domain_config(*r_domid, d_config);
        restart = DOMAIN_RESTART_NORMAL;
        /* fall-through */
    case LIBXL_ACTION_ON_SHUTDOWN_DESTROY:
        LOG("Domain %d needs to be cleaned up: destroying the domain",
            *r_domid);
        libxl_domain_destroy(ctx, *r_domid, 0);
        *r_domid = INVALID_DOMID;
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_SOFT_RESET:
        reload_domain_config(*r_domid, d_config);
        restart = DOMAIN_RESTART_SOFT_RESET;
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_DESTROY:
    case LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_RESTART:
        /* Already handled these above. */
        abort();
    }

    return restart;
}

/* Preserve a copy of a domain under a new name. Updates *r_domid */
static int preserve_domain(uint32_t *r_domid, libxl_event *event,
                           libxl_domain_config *d_config)
{
    time_t now;
    struct tm tm;
    char strtime[24];

    libxl_uuid new_uuid;

    int rc;

    now = time(NULL);
    if (now == ((time_t) -1)) {
        LOG("Failed to get current time for domain rename");
        return 0;
    }

    tzset();
    if (gmtime_r(&now, &tm) == NULL) {
        LOG("Failed to convert time to UTC");
        return 0;
    }

    if (!strftime(&strtime[0], sizeof(strtime), "-%Y%m%dT%H%MZ", &tm)) {
        LOG("Failed to format time as a string");
        return 0;
    }

    libxl_uuid_generate(&new_uuid);

    LOG("Preserving domain %u %s with suffix%s",
        *r_domid, d_config->c_info.name, strtime);
    rc = libxl_domain_preserve(ctx, *r_domid, &d_config->c_info,
                               strtime, new_uuid);

    /*
     * Although the domain still exists it is no longer the one we are
     * concerned with.
     */
    *r_domid = INVALID_DOMID;

    return rc == 0 ? 1 : 0;
}

/*
 * Returns false if memory can't be freed, but also if we encounter errors.
 * Returns true in case there is already, or we manage to free it, enough
 * memory, but also if autoballoon is false.
 */
static bool freemem(uint32_t domid, libxl_domain_build_info *b_info)
{
    int rc, retries = 3;
    uint64_t need_memkb, free_memkb;

    if (!autoballoon)
        return true;

    rc = libxl_domain_need_memory(ctx, b_info, &need_memkb);
    if (rc < 0)
        return false;

    do {
        rc = libxl_get_free_memory(ctx, &free_memkb);
        if (rc < 0)
            return false;

        if (free_memkb >= need_memkb)
            return true;

        rc = libxl_set_memory_target(ctx, 0, free_memkb - need_memkb, 1, 0);
        if (rc < 0)
            return false;

        /* wait until dom0 reaches its target, as long as we are making
         * progress */
        rc = libxl_wait_for_memory_target(ctx, 0, 10);
        if (rc < 0)
            return false;

        retries--;
    } while (retries > 0);

    return false;
}

static void autoconnect_console(libxl_ctx *ctx_ignored,
                                libxl_event *ev, void *priv)
{
    uint32_t bldomid = ev->domid;
    int notify_fd = *(int*)priv; /* write end of the notification pipe */

    libxl_event_free(ctx, ev);

    console_child_report(child_console);

    pid_t pid = xl_fork(child_console, "console child");
    if (pid)
        return;

    postfork();

    sleep(1);
    libxl_primary_console_exec(ctx, bldomid, notify_fd);
    /* Do not return. xl continued in child process */
    perror("xl: unable to exec console client");
    _exit(1);
}

static int domain_wait_event(uint32_t domid, libxl_event **event_r)
{
    int ret;
    for (;;) {
        ret = libxl_event_wait(ctx, event_r, LIBXL_EVENTMASK_ALL, 0,0);
        if (ret) {
            LOG("Domain %u, failed to get event, quitting (rc=%d)", domid, ret);
            return ret;
        }
        if ((*event_r)->domid != domid) {
            char *evstr = libxl_event_to_json(ctx, *event_r);
            LOG("INTERNAL PROBLEM - ignoring unexpected event for"
                " domain %d (expected %d): event=%s",
                (*event_r)->domid, domid, evstr);
            free(evstr);
            libxl_event_free(ctx, *event_r);
            continue;
        }
        return ret;
    }
}

static void evdisable_disk_ejects(libxl_evgen_disk_eject **diskws,
                                 int num_disks)
{
    int i;

    for (i = 0; i < num_disks; i++) {
        if (diskws[i])
            libxl_evdisable_disk_eject(ctx, diskws[i]);
        diskws[i] = NULL;
    }
}

static int create_domain(struct domain_create *dom_info)
{
    uint32_t domid = INVALID_DOMID;

    libxl_domain_config d_config;

    int debug = dom_info->debug;
    int daemonize = dom_info->daemonize;
    int monitor = dom_info->monitor;
    int paused = dom_info->paused;
    int vncautopass = dom_info->vncautopass;
    const char *config_file = dom_info->config_file;
    const char *extra_config = dom_info->extra_config;
    const char *restore_file = dom_info->restore_file;
    const char *config_source = NULL;
    const char *restore_source = NULL;
    int migrate_fd = dom_info->migrate_fd;
    bool config_in_json;

    int i;
    int need_daemon = daemonize;
    int ret, rc;
    libxl_evgen_domain_death *deathw = NULL;
    libxl_evgen_disk_eject **diskws = NULL; /* one per disk */
    unsigned int num_diskws = 0;
    void *config_data = 0;
    int config_len = 0;
    int restore_fd = -1;
    int restore_fd_to_close = -1;
    int send_back_fd = -1;
    const libxl_asyncprogress_how *autoconnect_console_how;
    int notify_pipe[2] = { -1, -1 };
    struct save_file_header hdr;
    uint32_t domid_soft_reset = INVALID_DOMID;

    int restoring = (restore_file || (migrate_fd >= 0));

    libxl_domain_config_init(&d_config);

    if (restoring) {
        uint8_t *optdata_begin = 0;
        const uint8_t *optdata_here = 0;
        union { uint32_t u32; char b[4]; } u32buf;
        uint32_t badflags;

        if (migrate_fd >= 0) {
            restore_source = "<incoming migration stream>";
            restore_fd = migrate_fd;
            send_back_fd = dom_info->send_back_fd;
        } else {
            restore_source = restore_file;
            restore_fd = open(restore_file, O_RDONLY);
            if (restore_fd == -1) {
                fprintf(stderr, "Can't open restore file: %s\n", strerror(errno));
                return ERROR_INVAL;
            }
            restore_fd_to_close = restore_fd;
            rc = libxl_fd_set_cloexec(ctx, restore_fd, 1);
            if (rc) return rc;
        }

        CHK_ERRNOVAL(libxl_read_exactly(
                         ctx, restore_fd, &hdr, sizeof(hdr),
                         restore_source, "header"));
        if (memcmp(hdr.magic, savefileheader_magic, sizeof(hdr.magic))) {
            fprintf(stderr, "File has wrong magic number -"
                    " corrupt or for a different tool?\n");
            return ERROR_INVAL;
        }
        if (hdr.byteorder != SAVEFILE_BYTEORDER_VALUE) {
            fprintf(stderr, "File has wrong byte order\n");
            return ERROR_INVAL;
        }
        fprintf(stderr, "Loading new save file %s"
                " (new xl fmt info"
                " 0x%"PRIx32"/0x%"PRIx32"/%"PRIu32")\n",
                restore_source, hdr.mandatory_flags, hdr.optional_flags,
                hdr.optional_data_len);

        badflags = hdr.mandatory_flags & ~XL_MANDATORY_FLAG_ALL;
        if (badflags) {
            fprintf(stderr, "Savefile has mandatory flag(s) 0x%"PRIx32" "
                    "which are not supported; need newer xl\n",
                    badflags);
            return ERROR_INVAL;
        }
        if (hdr.optional_data_len) {
            optdata_begin = xmalloc(hdr.optional_data_len);
            CHK_ERRNOVAL(libxl_read_exactly(
                             ctx, restore_fd, optdata_begin,
                             hdr.optional_data_len, restore_source,
                             "optdata"));
        }

#define OPTDATA_LEFT  (hdr.optional_data_len - (optdata_here - optdata_begin))
#define WITH_OPTDATA(amt, body)                                 \
            if (OPTDATA_LEFT < (amt)) {                         \
                fprintf(stderr, "Savefile truncated.\n");       \
                return ERROR_INVAL;                             \
            } else {                                            \
                body;                                           \
                optdata_here += (amt);                          \
            }

        optdata_here = optdata_begin;

        if (OPTDATA_LEFT) {
            fprintf(stderr, " Savefile contains xl domain config%s\n",
                    !!(hdr.mandatory_flags & XL_MANDATORY_FLAG_JSON)
                    ? " in JSON format" : "");
            WITH_OPTDATA(4, {
                memcpy(u32buf.b, optdata_here, 4);
                config_len = u32buf.u32;
            });
            WITH_OPTDATA(config_len, {
                config_data = xmalloc(config_len);
                memcpy(config_data, optdata_here, config_len);
            });
        }

    }

    if (config_file) {
        free(config_data);  config_data = 0;
        /* /dev/null represents special case (read config. from command line) */
        if (!strcmp(config_file, "/dev/null")) {
            config_len = 0;
        } else {
            ret = libxl_read_file_contents(ctx, config_file,
                                           &config_data, &config_len);
            if (ret) { fprintf(stderr, "Failed to read config file: %s: %s\n",
                               config_file, strerror(errno)); return ERROR_FAIL; }
        }
        if (!restoring && extra_config && strlen(extra_config)) {
            if (config_len > INT_MAX - (strlen(extra_config) + 2 + 1)) {
                fprintf(stderr, "Failed to attach extra configuration\n");
                return ERROR_FAIL;
            }
            /* allocate space for the extra config plus two EOLs plus \0 */
            config_data = xrealloc(config_data, config_len
                + strlen(extra_config) + 2 + 1);
            config_len += sprintf(config_data + config_len, "\n%s\n",
                extra_config);
        }
        config_source=config_file;
        config_in_json = false;
    } else {
        if (!config_data) {
            fprintf(stderr, "Config file not specified and"
                    " none in save file\n");
            return ERROR_INVAL;
        }
        config_source = "<saved>";
        config_in_json = !!(hdr.mandatory_flags & XL_MANDATORY_FLAG_JSON);
    }

    if (!dom_info->quiet)
        fprintf(stderr, "Parsing config from %s\n", config_source);

    if (config_in_json) {
        libxl_domain_config_from_json(ctx, &d_config,
                                      (const char *)config_data);
    } else {
        parse_config_data(config_source, config_data, config_len, &d_config);
    }

    if (migrate_fd >= 0) {
        if (d_config.c_info.name) {
            /* when we receive a domain we get its name from the config
             * file; and we receive it to a temporary name */
            assert(!common_domname);

            common_domname = d_config.c_info.name;
            d_config.c_info.name = 0; /* steals allocation from config */

            xasprintf(&d_config.c_info.name, "%s--incoming", common_domname);
            *dom_info->migration_domname_r = strdup(d_config.c_info.name);
        }
    }

    if (debug || dom_info->dryrun) {
        FILE *cfg_print_fh = (debug && !dom_info->dryrun) ? stderr : stdout;
        if (default_output_format == OUTPUT_FORMAT_SXP) {
            printf_info_sexp(-1, &d_config, cfg_print_fh);
        } else {
            char *json = libxl_domain_config_to_json(ctx, &d_config);
            if (!json) {
                fprintf(stderr,
                        "Failed to convert domain configuration to JSON\n");
                exit(1);
            }
            fputs(json, cfg_print_fh);
            free(json);
            flush_stream(cfg_print_fh);
        }
    }


    ret = 0;
    if (dom_info->dryrun)
        goto out;

start:
    assert(domid == INVALID_DOMID);

    rc = acquire_lock();
    if (rc < 0)
        goto error_out;

    if (domid_soft_reset == INVALID_DOMID) {
        if (!freemem(domid, &d_config.b_info)) {
            fprintf(stderr, "failed to free memory for the domain\n");
            ret = ERROR_FAIL;
            goto error_out;
        }
    }

    libxl_asyncprogress_how autoconnect_console_how_buf;
    if ( dom_info->console_autoconnect ) {
        if (libxl_pipe(ctx, notify_pipe)) {
            ret = ERROR_FAIL;
            goto error_out;
        }
        autoconnect_console_how_buf.callback = autoconnect_console;
        autoconnect_console_how_buf.for_callback = &notify_pipe[1];
        autoconnect_console_how = &autoconnect_console_how_buf;
    }else{
        autoconnect_console_how = 0;
    }

    if ( restoring ) {
        libxl_domain_restore_params params;

        libxl_domain_restore_params_init(&params);

        params.checkpointed_stream = dom_info->checkpointed_stream;
        params.stream_version =
            (hdr.mandatory_flags & XL_MANDATORY_FLAG_STREAMv2) ? 2 : 1;
        params.colo_proxy_script = dom_info->colo_proxy_script;

        ret = libxl_domain_create_restore(ctx, &d_config,
                                          &domid, restore_fd,
                                          send_back_fd, &params,
                                          0, autoconnect_console_how);

        libxl_domain_restore_params_dispose(&params);

        /*
         * On subsequent reboot etc we should create the domain, not
         * restore/migrate-receive it again.
         */
        restoring = 0;
    } else if (domid_soft_reset != INVALID_DOMID) {
        /* Do soft reset. */
        ret = libxl_domain_soft_reset(ctx, &d_config, domid_soft_reset,
                                      0, autoconnect_console_how);
        domid = domid_soft_reset;
        domid_soft_reset = INVALID_DOMID;
    } else {
        ret = libxl_domain_create_new(ctx, &d_config, &domid,
                                      0, autoconnect_console_how);
    }
    if ( ret )
        goto error_out;

    release_lock();

    if (restore_fd_to_close >= 0) {
        if (close(restore_fd_to_close))
            fprintf(stderr, "Failed to close restoring file, fd %d, errno %d\n",
                    restore_fd_to_close, errno);
        restore_fd_to_close = -1;
    }

    if (autoconnect_console_how) {
        char buf[1];
        int r;

        /* Try to get notification from xenconsole. Just move on if
         * error occurs -- it's only minor annoyance if console
         * doesn't show up.
         */
        do {
            r = read(notify_pipe[0], buf, 1);
        } while (r == -1 && errno == EINTR);

        if (r == -1)
            fprintf(stderr,
                    "Failed to get notification from xenconsole: %s\n",
                    strerror(errno));
        else if (r == 0)
            fprintf(stderr, "Got EOF from xenconsole notification fd\n");
        else if (r == 1 && buf[0] != 0x00)
            fprintf(stderr, "Got unexpected response from xenconsole: %#x\n",
                    buf[0]);

        close(notify_pipe[0]);
        close(notify_pipe[1]);
        notify_pipe[0] = notify_pipe[1] = -1;
    }

    if (!paused)
        libxl_domain_unpause(ctx, domid);

    ret = domid; /* caller gets success in parent */
    if (!daemonize && !monitor)
        goto out;

    if (dom_info->vnc)
        autoconnect_vncviewer(domid, vncautopass);

    if (need_daemon) {
        char *name;

        xasprintf(&name, "xl-%s", d_config.c_info.name);
        ret = do_daemonize(name, NULL);
        free(name);
        if (ret) {
            ret = (ret == 1) ? domid : ret;
            goto out;
        }
        need_daemon = 0;
    }
    LOG("Waiting for domain %s (domid %u) to die [pid %ld]",
        d_config.c_info.name, domid, (long)getpid());

    ret = libxl_evenable_domain_death(ctx, domid, 0, &deathw);
    if (ret) goto out;

    if (!diskws) {
        diskws = xmalloc(sizeof(*diskws) * d_config.num_disks);
        for (i = 0; i < d_config.num_disks; i++)
            diskws[i] = NULL;
        num_diskws = d_config.num_disks;
    }
    for (i = 0; i < num_diskws; i++) {
        if (d_config.disks[i].removable) {
            ret = libxl_evenable_disk_eject(ctx, domid, d_config.disks[i].vdev,
                                            0, &diskws[i]);
            if (ret) goto out;
        }
    }
    while (1) {
        libxl_event *event;
        ret = domain_wait_event(domid, &event);
        if (ret) goto out;

        switch (event->type) {

        case LIBXL_EVENT_TYPE_DOMAIN_SHUTDOWN:
            LOG("Domain %u has shut down, reason code %d 0x%x", domid,
                event->u.domain_shutdown.shutdown_reason,
                event->u.domain_shutdown.shutdown_reason);
            switch (handle_domain_death(&domid, event, &d_config)) {
            case DOMAIN_RESTART_SOFT_RESET:
                domid_soft_reset = domid;
                domid = INVALID_DOMID;
                /* fall through */
            case DOMAIN_RESTART_RENAME:
                if (domid_soft_reset == INVALID_DOMID &&
                    !preserve_domain(&domid, event, &d_config)) {
                    libxl_event_free(ctx, event);
                    /* If we fail then exit leaving the old domain in place. */
                    ret = -1;
                    goto out;
                }

                /* Otherwise fall through and restart. */
            case DOMAIN_RESTART_NORMAL:
                libxl_event_free(ctx, event);
                libxl_evdisable_domain_death(ctx, deathw);
                deathw = NULL;
                evdisable_disk_ejects(diskws, num_diskws);
                free(diskws);
                diskws = NULL;
                num_diskws = 0;
                /* discard any other events which may have been generated */
                while (!(ret = libxl_event_check(ctx, &event,
                                                 LIBXL_EVENTMASK_ALL, 0,0))) {
                    libxl_event_free(ctx, event);
                }
                if (ret != ERROR_NOT_READY) {
                    LOG("warning, libxl_event_check (cleanup) failed (rc=%d)",
                        ret);
                }

                /*
                 * Do not attempt to reconnect if we come round again due to a
                 * guest reboot -- the stdin/out will be disconnected by then.
                 */
                dom_info->console_autoconnect = 0;

                /* Some settings only make sense on first boot. */
                paused = 0;
                if (common_domname
                    && strcmp(d_config.c_info.name, common_domname)) {
                    d_config.c_info.name = strdup(common_domname);
                }

                /*
                 * XXX FIXME: If this sleep is not there then domain
                 * re-creation fails sometimes.
                 */
                LOG("Done. Rebooting now");
                sleep(2);
                goto start;

            case DOMAIN_RESTART_NONE:
                LOG("Done. Exiting now");
                libxl_event_free(ctx, event);
                ret = 0;
                goto out;

            default:
                abort();
            }

        case LIBXL_EVENT_TYPE_DOMAIN_DEATH:
            LOG("Domain %u has been destroyed.", domid);
            libxl_event_free(ctx, event);
            ret = 0;
            goto out;

        case LIBXL_EVENT_TYPE_DISK_EJECT:
            /* XXX what is this for? */
            libxl_cdrom_insert(ctx, domid, &event->u.disk_eject.disk, NULL);
            break;

        default:;
            char *evstr = libxl_event_to_json(ctx, event);
            LOG("warning, got unexpected event type %d, event=%s",
                event->type, evstr);
            free(evstr);
        }

        libxl_event_free(ctx, event);
    }

error_out:
    release_lock();
    if (libxl_domid_valid_guest(domid)) {
        libxl_domain_destroy(ctx, domid, 0);
        domid = INVALID_DOMID;
    }

out:
    if (restore_fd_to_close >= 0) {
        if (close(restore_fd_to_close))
            fprintf(stderr, "Failed to close restoring file, fd %d, errno %d\n",
                    restore_fd_to_close, errno);
        restore_fd_to_close = -1;
    }

    if (logfile != 2)
        close(logfile);

    libxl_domain_config_dispose(&d_config);

    free(config_data);

    console_child_report(child_console);

    if (deathw)
        libxl_evdisable_domain_death(ctx, deathw);
    if (diskws) {
        evdisable_disk_ejects(diskws, d_config.num_disks);
        free(diskws);
    }

    /*
     * If we have daemonized then do not return to the caller -- this has
     * already happened in the parent.
     */
    if ( daemonize && !need_daemon )
        exit(ret);

    return ret;
}

void help(const char *command)
{
    int i;
    struct cmd_spec *cmd;

    if (!command || !strcmp(command, "help")) {
        printf("Usage xl [-vfN] <subcommand> [args]\n\n");
        printf("xl full list of subcommands:\n\n");
        for (i = 0; i < cmdtable_len; i++) {
            printf(" %-19s ", cmd_table[i].cmd_name);
            if (strlen(cmd_table[i].cmd_name) > 19)
                printf("\n %-19s ", "");
            printf("%s\n", cmd_table[i].cmd_desc);
        }
    } else {
        cmd = cmdtable_lookup(command);
        if (cmd) {
            printf("Usage: xl [-v%s%s] %s %s\n\n%s.\n\n",
                   cmd->modifies ? "f" : "",
                   cmd->can_dryrun ? "N" : "",
                   cmd->cmd_name,
                   cmd->cmd_usage,
                   cmd->cmd_desc);
            if (cmd->cmd_option)
                printf("Options:\n\n%s\n", cmd->cmd_option);
        }
        else {
            printf("command \"%s\" not implemented\n", command);
        }
    }
}

int main_console(int argc, char **argv)
{
    uint32_t domid;
    int opt = 0, num = 0;
    libxl_console_type type = 0;

    SWITCH_FOREACH_OPT(opt, "n:t:", NULL, "console", 1) {
    case 't':
        if (!strcmp(optarg, "pv"))
            type = LIBXL_CONSOLE_TYPE_PV;
        else if (!strcmp(optarg, "serial"))
            type = LIBXL_CONSOLE_TYPE_SERIAL;
        else {
            fprintf(stderr, "console type supported are: pv, serial\n");
            return EXIT_FAILURE;
        }
        break;
    case 'n':
        num = atoi(optarg);
        break;
    }

    domid = find_domain(argv[optind]);
    if (!type)
        libxl_primary_console_exec(ctx, domid, -1);
    else
        libxl_console_exec(ctx, domid, num, type, -1);
    fprintf(stderr, "Unable to attach console\n");
    return EXIT_FAILURE;
}

int main_vncviewer(int argc, char **argv)
{
    static const struct option opts[] = {
        {"autopass", 0, 0, 'a'},
        {"vncviewer-autopass", 0, 0, 'a'},
        COMMON_LONG_OPTS
    };
    uint32_t domid;
    int opt, autopass = 0;

    SWITCH_FOREACH_OPT(opt, "a", opts, "vncviewer", 1) {
    case 'a':
        autopass = 1;
        break;
    }

    domid = find_domain(argv[optind]);

    if (vncviewer(domid, autopass))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void pause_domain(uint32_t domid)
{
    libxl_domain_pause(ctx, domid);
}

static void unpause_domain(uint32_t domid)
{
    libxl_domain_unpause(ctx, domid);
}

static void destroy_domain(uint32_t domid, int force)
{
    int rc;

    if (domid == 0 && !force) {
        fprintf(stderr, "Not destroying domain 0; use -f to force.\n"
                        "This can only be done when using a disaggregated "
                        "hardware domain and toolstack.\n\n");
        exit(EXIT_FAILURE);
    }
    rc = libxl_domain_destroy(ctx, domid, 0);
    if (rc) { fprintf(stderr,"destroy failed (rc=%d)\n",rc); exit(EXIT_FAILURE); }
}

static void wait_for_domain_deaths(libxl_evgen_domain_death **deathws, int nr)
{
    int rc, count = 0;
    LOG("Waiting for %d domains", nr);
    while(1 && count < nr) {
        libxl_event *event;
        rc = libxl_event_wait(ctx, &event, LIBXL_EVENTMASK_ALL, 0,0);
        if (rc) {
            LOG("Failed to get event, quitting (rc=%d)", rc);
            exit(EXIT_FAILURE);
        }

        switch (event->type) {
        case LIBXL_EVENT_TYPE_DOMAIN_DEATH:
            LOG("Domain %d has been destroyed", event->domid);
            libxl_evdisable_domain_death(ctx, deathws[event->for_user]);
            count++;
            break;
        case LIBXL_EVENT_TYPE_DOMAIN_SHUTDOWN:
            LOG("Domain %d has been shut down, reason code %d",
                event->domid, event->u.domain_shutdown.shutdown_reason);
            libxl_evdisable_domain_death(ctx, deathws[event->for_user]);
            count++;
            break;
        default:
            LOG("Unexpected event type %d", event->type);
            break;
        }
        libxl_event_free(ctx, event);
    }
}

static void shutdown_domain(uint32_t domid,
                            libxl_evgen_domain_death **deathw,
                            libxl_ev_user for_user,
                            int fallback_trigger)
{
    int rc;

    fprintf(stderr, "Shutting down domain %u\n", domid);
    rc=libxl_domain_shutdown(ctx, domid);
    if (rc == ERROR_NOPARAVIRT) {
        if (fallback_trigger) {
            fprintf(stderr, "PV control interface not available:"
                    " sending ACPI power button event.\n");
            rc = libxl_send_trigger(ctx, domid, LIBXL_TRIGGER_POWER, 0);
        } else {
            fprintf(stderr, "PV control interface not available:"
                    " external graceful shutdown not possible.\n");
            fprintf(stderr, "Use \"-F\" to fallback to ACPI power event.\n");
        }
    }

    if (rc) {
        fprintf(stderr,"shutdown failed (rc=%d)\n",rc);exit(EXIT_FAILURE);
    }

    if (deathw) {
        rc = libxl_evenable_domain_death(ctx, domid, for_user, deathw);
        if (rc) {
            fprintf(stderr,"wait for death failed (evgen, rc=%d)\n",rc);
            exit(EXIT_FAILURE);
        }
    }
}

static void reboot_domain(uint32_t domid, libxl_evgen_domain_death **deathw,
                          libxl_ev_user for_user, int fallback_trigger)
{
    int rc;

    fprintf(stderr, "Rebooting domain %u\n", domid);
    rc=libxl_domain_reboot(ctx, domid);
    if (rc == ERROR_NOPARAVIRT) {
        if (fallback_trigger) {
            fprintf(stderr, "PV control interface not available:"
                    " sending ACPI reset button event.\n");
            rc = libxl_send_trigger(ctx, domid, LIBXL_TRIGGER_RESET, 0);
        } else {
            fprintf(stderr, "PV control interface not available:"
                    " external graceful reboot not possible.\n");
            fprintf(stderr, "Use \"-F\" to fallback to ACPI reset event.\n");
        }
    }
    if (rc) {
        fprintf(stderr,"reboot failed (rc=%d)\n",rc);exit(EXIT_FAILURE);
    }

    if (deathw) {
        rc = libxl_evenable_domain_death(ctx, domid, for_user, deathw);
        if (rc) {
            fprintf(stderr,"wait for death failed (evgen, rc=%d)\n",rc);
            exit(EXIT_FAILURE);
        }
    }
}

static void list_domains_details(const libxl_dominfo *info, int nb_domain)
{
    libxl_domain_config d_config;

    int i, rc;

    yajl_gen hand = NULL;
    yajl_gen_status s;
    const char *buf;
    libxl_yajl_length yajl_len = 0;

    if (default_output_format == OUTPUT_FORMAT_JSON) {
        hand = libxl_yajl_gen_alloc(NULL);
        if (!hand) {
            fprintf(stderr, "unable to allocate JSON generator\n");
            return;
        }

        s = yajl_gen_array_open(hand);
        if (s != yajl_gen_status_ok)
            goto out;
    } else
        s = yajl_gen_status_ok;

    for (i = 0; i < nb_domain; i++) {
        libxl_domain_config_init(&d_config);
        rc = libxl_retrieve_domain_configuration(ctx, info[i].domid, &d_config);
        if (rc)
            continue;
        if (default_output_format == OUTPUT_FORMAT_JSON)
            s = printf_info_one_json(hand, info[i].domid, &d_config);
        else
            printf_info_sexp(info[i].domid, &d_config, stdout);
        libxl_domain_config_dispose(&d_config);
        if (s != yajl_gen_status_ok)
            goto out;
    }

    if (default_output_format == OUTPUT_FORMAT_JSON) {
        s = yajl_gen_array_close(hand);
        if (s != yajl_gen_status_ok)
            goto out;

        s = yajl_gen_get_buf(hand, (const unsigned char **)&buf, &yajl_len);
        if (s != yajl_gen_status_ok)
            goto out;

        puts(buf);
    }

out:
    if (default_output_format == OUTPUT_FORMAT_JSON) {
        yajl_gen_free(hand);
        if (s != yajl_gen_status_ok)
            fprintf(stderr,
                    "unable to format domain config as JSON (YAJL:%d)\n", s);
    }
}

static void list_domains(bool verbose, bool context, bool claim, bool numa,
                         bool cpupool, const libxl_dominfo *info, int nb_domain)
{
    int i;
    static const char shutdown_reason_letters[]= "-rscwS";
    libxl_bitmap nodemap;
    libxl_physinfo physinfo;

    libxl_bitmap_init(&nodemap);
    libxl_physinfo_init(&physinfo);

    printf("Name                                        ID   Mem VCPUs\tState\tTime(s)");
    if (verbose) printf("   UUID                            Reason-Code\tSecurity Label");
    if (context && !verbose) printf("   Security Label");
    if (claim) printf("  Claimed");
    if (cpupool) printf("         Cpupool");
    if (numa) {
        if (libxl_node_bitmap_alloc(ctx, &nodemap, 0)) {
            fprintf(stderr, "libxl_node_bitmap_alloc_failed.\n");
            exit(EXIT_FAILURE);
        }
        if (libxl_get_physinfo(ctx, &physinfo) != 0) {
            fprintf(stderr, "libxl_physinfo failed.\n");
            libxl_bitmap_dispose(&nodemap);
            exit(EXIT_FAILURE);
        }

        printf(" NODE Affinity");
    }
    printf("\n");
    for (i = 0; i < nb_domain; i++) {
        char *domname;
        libxl_shutdown_reason shutdown_reason;
        domname = libxl_domid_to_name(ctx, info[i].domid);
        shutdown_reason = info[i].shutdown ? info[i].shutdown_reason : 0;
        printf("%-40s %5d %5lu %5d     %c%c%c%c%c%c  %8.1f",
                domname,
                info[i].domid,
                (unsigned long) ((info[i].current_memkb +
                    info[i].outstanding_memkb)/ 1024),
                info[i].vcpu_online,
                info[i].running ? 'r' : '-',
                info[i].blocked ? 'b' : '-',
                info[i].paused ? 'p' : '-',
                info[i].shutdown ? 's' : '-',
                (shutdown_reason >= 0 &&
                 shutdown_reason < sizeof(shutdown_reason_letters)-1
                 ? shutdown_reason_letters[shutdown_reason] : '?'),
                info[i].dying ? 'd' : '-',
                ((float)info[i].cpu_time / 1e9));
        free(domname);
        if (verbose) {
            printf(" " LIBXL_UUID_FMT, LIBXL_UUID_BYTES(info[i].uuid));
            if (info[i].shutdown) printf(" %8x", shutdown_reason);
            else printf(" %8s", "-");
        }
        if (claim)
            printf(" %5lu", (unsigned long)info[i].outstanding_memkb / 1024);
        if (verbose || context)
            printf(" %16s", info[i].ssid_label ? : "-");
        if (cpupool) {
            char *poolname = libxl_cpupoolid_to_name(ctx, info[i].cpupool);
            printf("%16s", poolname);
            free(poolname);
        }
        if (numa) {
            libxl_domain_get_nodeaffinity(ctx, info[i].domid, &nodemap);

            putchar(' ');
            print_bitmap(nodemap.map, physinfo.nr_nodes, stdout);
        }
        putchar('\n');
    }

    libxl_bitmap_dispose(&nodemap);
    libxl_physinfo_dispose(&physinfo);
}

static void list_vm(void)
{
    libxl_vminfo *info;
    char *domname;
    int nb_vm, i;

    info = libxl_list_vm(ctx, &nb_vm);

    if (!info) {
        fprintf(stderr, "libxl_list_vm failed.\n");
        exit(EXIT_FAILURE);
    }
    printf("UUID                                  ID    name\n");
    for (i = 0; i < nb_vm; i++) {
        domname = libxl_domid_to_name(ctx, info[i].domid);
        printf(LIBXL_UUID_FMT "  %d    %-30s\n", LIBXL_UUID_BYTES(info[i].uuid),
            info[i].domid, domname);
        free(domname);
    }
    libxl_vminfo_list_free(info, nb_vm);
}

static void core_dump_domain(uint32_t domid, const char *filename)
{
    int rc;

    rc=libxl_domain_core_dump(ctx, domid, filename, NULL);
    if (rc) { fprintf(stderr,"core dump failed (rc=%d)\n",rc);exit(EXIT_FAILURE); }
}

#ifndef LIBXL_HAVE_NO_SUSPEND_RESUME
static void save_domain_core_begin(uint32_t domid,
                                   const char *override_config_file,
                                   uint8_t **config_data_r,
                                   int *config_len_r)
{
    int rc;
    libxl_domain_config d_config;
    char *config_c = 0;

    /* configuration file in optional data: */

    libxl_domain_config_init(&d_config);

    if (override_config_file) {
        void *config_v = 0;
        rc = libxl_read_file_contents(ctx, override_config_file,
                                      &config_v, config_len_r);
        if (rc) {
            fprintf(stderr, "unable to read overridden config file\n");
            exit(EXIT_FAILURE);
        }
        parse_config_data(override_config_file, config_v, *config_len_r,
                          &d_config);
        free(config_v);
    } else {
        rc = libxl_retrieve_domain_configuration(ctx, domid, &d_config);
        if (rc) {
            fprintf(stderr, "unable to retrieve domain configuration\n");
            exit(EXIT_FAILURE);
        }
    }

    config_c = libxl_domain_config_to_json(ctx, &d_config);
    if (!config_c) {
        fprintf(stderr, "unable to convert config file to JSON\n");
        exit(EXIT_FAILURE);
    }
    *config_data_r = (uint8_t *)config_c;
    *config_len_r = strlen(config_c) + 1; /* including trailing '\0' */

    libxl_domain_config_dispose(&d_config);
}

static void save_domain_core_writeconfig(int fd, const char *source,
                                  const uint8_t *config_data, int config_len)
{
    struct save_file_header hdr;
    uint8_t *optdata_begin;
    union { uint32_t u32; char b[4]; } u32buf;

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, savefileheader_magic, sizeof(hdr.magic));
    hdr.byteorder = SAVEFILE_BYTEORDER_VALUE;
    hdr.mandatory_flags = XL_MANDATORY_FLAG_STREAMv2;

    optdata_begin= 0;

#define ADD_OPTDATA(ptr, len) ({                                            \
    if ((len)) {                                                        \
        hdr.optional_data_len += (len);                                 \
        optdata_begin = xrealloc(optdata_begin, hdr.optional_data_len); \
        memcpy(optdata_begin + hdr.optional_data_len - (len),           \
               (ptr), (len));                                           \
    }                                                                   \
                          })

    u32buf.u32 = config_len;
    ADD_OPTDATA(u32buf.b,    4);
    ADD_OPTDATA(config_data, config_len);
    if (config_len)
        hdr.mandatory_flags |= XL_MANDATORY_FLAG_JSON;

    /* that's the optional data */

    CHK_ERRNOVAL(libxl_write_exactly(
                     ctx, fd, &hdr, sizeof(hdr), source, "header"));
    CHK_ERRNOVAL(libxl_write_exactly(
                     ctx, fd, optdata_begin, hdr.optional_data_len,
                     source, "header"));

    free(optdata_begin);

    fprintf(stderr, "Saving to %s new xl format (info"
            " 0x%"PRIx32"/0x%"PRIx32"/%"PRIu32")\n",
            source, hdr.mandatory_flags, hdr.optional_flags,
            hdr.optional_data_len);
}

static int save_domain(uint32_t domid, const char *filename, int checkpoint,
                            int leavepaused, const char *override_config_file)
{
    int fd;
    uint8_t *config_data;
    int config_len;

    save_domain_core_begin(domid, override_config_file,
                           &config_data, &config_len);

    if (!config_len) {
        fputs(" Savefile will not contain xl domain config\n", stderr);
    }

    fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "Failed to open temp file %s for writing\n", filename);
        exit(EXIT_FAILURE);
    }

    save_domain_core_writeconfig(fd, filename, config_data, config_len);

    int rc = libxl_domain_suspend(ctx, domid, fd, 0, NULL);
    close(fd);

    if (rc < 0) {
        fprintf(stderr, "Failed to save domain, resuming domain\n");
        libxl_domain_resume(ctx, domid, 1, 0);
    }
    else if (leavepaused || checkpoint) {
        if (leavepaused)
            libxl_domain_pause(ctx, domid);
        libxl_domain_resume(ctx, domid, 1, 0);
    }
    else
        libxl_domain_destroy(ctx, domid, 0);

    exit(rc < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
}

static pid_t create_migration_child(const char *rune, int *send_fd,
                                        int *recv_fd)
{
    int sendpipe[2], recvpipe[2];
    pid_t child;

    if (!rune || !send_fd || !recv_fd)
        return -1;

    MUST( libxl_pipe(ctx, sendpipe) );
    MUST( libxl_pipe(ctx, recvpipe) );

    child = xl_fork(child_migration, "migration transport process");

    if (!child) {
        dup2(sendpipe[0], 0);
        dup2(recvpipe[1], 1);
        close(sendpipe[0]); close(sendpipe[1]);
        close(recvpipe[0]); close(recvpipe[1]);
        execlp("sh","sh","-c",rune,(char*)0);
        perror("failed to exec sh");
        exit(EXIT_FAILURE);
    }

    close(sendpipe[0]);
    close(recvpipe[1]);
    *send_fd = sendpipe[1];
    *recv_fd = recvpipe[0];

    /* if receiver dies, we get an error and can clean up
       rather than just dying */
    signal(SIGPIPE, SIG_IGN);

    return child;
}

static int migrate_read_fixedmessage(int fd, const void *msg, int msgsz,
                                     const char *what, const char *rune) {
    char buf[msgsz];
    const char *stream;
    int rc;

    stream = rune ? "migration receiver stream" : "migration stream";
    rc = libxl_read_exactly(ctx, fd, buf, msgsz, stream, what);
    if (rc) return 1;

    if (memcmp(buf, msg, msgsz)) {
        fprintf(stderr, "%s contained unexpected data instead of %s\n",
                stream, what);
        if (rune)
            fprintf(stderr, "(command run was: %s )\n", rune);
        return 1;
    }
    return 0;
}

static void migration_child_report(int recv_fd) {
    pid_t child;
    int status, sr;
    struct timeval now, waituntil, timeout;
    static const struct timeval pollinterval = { 0, 1000 }; /* 1ms */

    if (!xl_child_pid(child_migration)) return;

    CHK_SYSCALL(gettimeofday(&waituntil, 0));
    waituntil.tv_sec += 2;

    for (;;) {
        pid_t migration_child = xl_child_pid(child_migration);
        child = xl_waitpid(child_migration, &status, WNOHANG);

        if (child == migration_child) {
            if (status)
                xl_report_child_exitstatus(XTL_INFO, child_migration,
                                           migration_child, status);
            break;
        }
        if (child == -1) {
            fprintf(stderr, "wait for migration child [%ld] failed: %s\n",
                    (long)migration_child, strerror(errno));
            break;
        }
        assert(child == 0);

        CHK_SYSCALL(gettimeofday(&now, 0));
        if (timercmp(&now, &waituntil, >)) {
            fprintf(stderr, "migration child [%ld] not exiting, no longer"
                    " waiting (exit status will be unreported)\n",
                    (long)migration_child);
            break;
        }
        timersub(&waituntil, &now, &timeout);

        if (recv_fd >= 0) {
            fd_set readfds, exceptfds;
            FD_ZERO(&readfds);
            FD_ZERO(&exceptfds);
            FD_SET(recv_fd, &readfds);
            FD_SET(recv_fd, &exceptfds);
            sr = select(recv_fd+1, &readfds,0,&exceptfds, &timeout);
        } else {
            if (timercmp(&timeout, &pollinterval, >))
                timeout = pollinterval;
            sr = select(0,0,0,0, &timeout);
        }
        if (sr > 0) {
            recv_fd = -1;
        } else if (sr == 0) {
        } else if (sr == -1) {
            if (errno != EINTR) {
                fprintf(stderr, "migration child [%ld] exit wait select"
                        " failed unexpectedly: %s\n",
                        (long)migration_child, strerror(errno));
                break;
            }
        }
    }
}

static void migrate_do_preamble(int send_fd, int recv_fd, pid_t child,
                                uint8_t *config_data, int config_len,
                                const char *rune)
{
    int rc = 0;

    if (send_fd < 0 || recv_fd < 0) {
        fprintf(stderr, "migrate_do_preamble: invalid file descriptors\n");
        exit(EXIT_FAILURE);
    }

    rc = migrate_read_fixedmessage(recv_fd, migrate_receiver_banner,
                                   sizeof(migrate_receiver_banner)-1,
                                   "banner", rune);
    if (rc) {
        close(send_fd);
        migration_child_report(recv_fd);
        exit(EXIT_FAILURE);
    }

    save_domain_core_writeconfig(send_fd, "migration stream",
                                 config_data, config_len);

}

static void migrate_domain(uint32_t domid, const char *rune, int debug,
                           const char *override_config_file)
{
    pid_t child = -1;
    int rc;
    int send_fd = -1, recv_fd = -1;
    char *away_domname;
    char rc_buf;
    uint8_t *config_data;
    int config_len, flags = LIBXL_SUSPEND_LIVE;

    save_domain_core_begin(domid, override_config_file,
                           &config_data, &config_len);

    if (!config_len) {
        fprintf(stderr, "No config file stored for running domain and "
                "none supplied - cannot migrate.\n");
        exit(EXIT_FAILURE);
    }

    child = create_migration_child(rune, &send_fd, &recv_fd);

    migrate_do_preamble(send_fd, recv_fd, child, config_data, config_len,
                        rune);

    xtl_stdiostream_adjust_flags(logger, XTL_STDIOSTREAM_HIDE_PROGRESS, 0);

    if (debug)
        flags |= LIBXL_SUSPEND_DEBUG;
    rc = libxl_domain_suspend(ctx, domid, send_fd, flags, NULL);
    if (rc) {
        fprintf(stderr, "migration sender: libxl_domain_suspend failed"
                " (rc=%d)\n", rc);
        if (rc == ERROR_GUEST_TIMEDOUT)
            goto failed_suspend;
        else
            goto failed_resume;
    }

    //fprintf(stderr, "migration sender: Transfer complete.\n");
    // Should only be printed when debugging as it's a bit messy with
    // progress indication.

    rc = migrate_read_fixedmessage(recv_fd, migrate_receiver_ready,
                                   sizeof(migrate_receiver_ready),
                                   "ready message", rune);
    if (rc) goto failed_resume;

    xtl_stdiostream_adjust_flags(logger, 0, XTL_STDIOSTREAM_HIDE_PROGRESS);

    /* right, at this point we are about give the destination
     * permission to rename and resume, so we must first rename the
     * domain away ourselves */

    fprintf(stderr, "migration sender: Target has acknowledged transfer.\n");

    if (common_domname) {
        xasprintf(&away_domname, "%s--migratedaway", common_domname);
        rc = libxl_domain_rename(ctx, domid, common_domname, away_domname);
        if (rc) goto failed_resume;
    }

    /* point of no return - as soon as we have tried to say
     * "go" to the receiver, it's not safe to carry on.  We leave
     * the domain renamed to %s--migratedaway in case that's helpful.
     */

    fprintf(stderr, "migration sender: Giving target permission to start.\n");

    rc = libxl_write_exactly(ctx, send_fd,
                             migrate_permission_to_go,
                             sizeof(migrate_permission_to_go),
                             "migration stream", "GO message");
    if (rc) goto failed_badly;

    rc = migrate_read_fixedmessage(recv_fd, migrate_report,
                                   sizeof(migrate_report),
                                   "success/failure report message", rune);
    if (rc) goto failed_badly;

    rc = libxl_read_exactly(ctx, recv_fd,
                            &rc_buf, 1,
                            "migration ack stream", "success/failure status");
    if (rc) goto failed_badly;

    if (rc_buf) {
        fprintf(stderr, "migration sender: Target reports startup failure"
                " (status code %d).\n", rc_buf);

        rc = migrate_read_fixedmessage(recv_fd, migrate_permission_to_go,
                                       sizeof(migrate_permission_to_go),
                                       "permission for sender to resume",
                                       rune);
        if (rc) goto failed_badly;

        fprintf(stderr, "migration sender: Trying to resume at our end.\n");

        if (common_domname) {
            libxl_domain_rename(ctx, domid, away_domname, common_domname);
        }
        rc = libxl_domain_resume(ctx, domid, 1, 0);
        if (!rc) fprintf(stderr, "migration sender: Resumed OK.\n");

        fprintf(stderr, "Migration failed due to problems at target.\n");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "migration sender: Target reports successful startup.\n");
    libxl_domain_destroy(ctx, domid, 0); /* bang! */
    fprintf(stderr, "Migration successful.\n");
    exit(EXIT_SUCCESS);

 failed_suspend:
    close(send_fd);
    migration_child_report(recv_fd);
    fprintf(stderr, "Migration failed, failed to suspend at sender.\n");
    exit(EXIT_FAILURE);

 failed_resume:
    close(send_fd);
    migration_child_report(recv_fd);
    fprintf(stderr, "Migration failed, resuming at sender.\n");
    libxl_domain_resume(ctx, domid, 1, 0);
    exit(EXIT_FAILURE);

 failed_badly:
    fprintf(stderr,
 "** Migration failed during final handshake **\n"
 "Domain state is now undefined !\n"
 "Please CHECK AT BOTH ENDS for running instances, before renaming and\n"
 " resuming at most one instance.  Two simultaneous instances of the domain\n"
 " would probably result in SEVERE DATA LOSS and it is now your\n"
 " responsibility to avoid that.  Sorry.\n");

    close(send_fd);
    migration_child_report(recv_fd);
    exit(EXIT_FAILURE);
}

static void migrate_receive(int debug, int daemonize, int monitor,
                            int pause_after_migration,
                            int send_fd, int recv_fd,
                            libxl_checkpointed_stream checkpointed,
                            char *colo_proxy_script)
{
    uint32_t domid;
    int rc, rc2;
    char rc_buf;
    char *migration_domname;
    struct domain_create dom_info;

    signal(SIGPIPE, SIG_IGN);
    /* if we get SIGPIPE we'd rather just have it as an error */

    fprintf(stderr, "migration target: Ready to receive domain.\n");

    CHK_ERRNOVAL(libxl_write_exactly(
                     ctx, send_fd, migrate_receiver_banner,
                     sizeof(migrate_receiver_banner)-1,
                     "migration ack stream", "banner") );

    memset(&dom_info, 0, sizeof(dom_info));
    dom_info.debug = debug;
    dom_info.daemonize = daemonize;
    dom_info.monitor = monitor;
    dom_info.paused = 1;
    dom_info.migrate_fd = recv_fd;
    dom_info.send_back_fd = send_fd;
    dom_info.migration_domname_r = &migration_domname;
    dom_info.checkpointed_stream = checkpointed;
    dom_info.colo_proxy_script = colo_proxy_script;

    rc = create_domain(&dom_info);
    if (rc < 0) {
        fprintf(stderr, "migration target: Domain creation failed"
                " (code %d).\n", rc);
        exit(EXIT_FAILURE);
    }

    domid = rc;

    switch (checkpointed) {
    case LIBXL_CHECKPOINTED_STREAM_REMUS:
    case LIBXL_CHECKPOINTED_STREAM_COLO:
    {
        const char *ha = checkpointed == LIBXL_CHECKPOINTED_STREAM_COLO ?
                         "COLO" : "Remus";
        /* If we are here, it means that the sender (primary) has crashed.
         * TODO: Split-Brain Check.
         */
        fprintf(stderr, "migration target: %s Failover for domain %u\n",
                ha, domid);

        /*
         * If domain renaming fails, lets just continue (as we need the domain
         * to be up & dom names may not matter much, as long as its reachable
         * over network).
         *
         * If domain unpausing fails, destroy domain ? Or is it better to have
         * a consistent copy of the domain (memory, cpu state, disk)
         * on atleast one physical host ? Right now, lets just leave the domain
         * as is and let the Administrator decide (or troubleshoot).
         */
        if (migration_domname) {
            rc = libxl_domain_rename(ctx, domid, migration_domname,
                                     common_domname);
            if (rc)
                fprintf(stderr, "migration target (%s): "
                        "Failed to rename domain from %s to %s:%d\n",
                        ha, migration_domname, common_domname, rc);
        }

        if (checkpointed == LIBXL_CHECKPOINTED_STREAM_COLO)
            /* The guest is running after failover in COLO mode */
            exit(rc ? -ERROR_FAIL: 0);

        rc = libxl_domain_unpause(ctx, domid);
        if (rc)
            fprintf(stderr, "migration target (%s): "
                    "Failed to unpause domain %s (id: %u):%d\n",
                    ha, common_domname, domid, rc);

        exit(rc ? EXIT_FAILURE : EXIT_SUCCESS);
    }
    default:
        /* do nothing */
        break;
    }

    fprintf(stderr, "migration target: Transfer complete,"
            " requesting permission to start domain.\n");

    rc = libxl_write_exactly(ctx, send_fd,
                             migrate_receiver_ready,
                             sizeof(migrate_receiver_ready),
                             "migration ack stream", "ready message");
    if (rc) exit(EXIT_FAILURE);

    rc = migrate_read_fixedmessage(recv_fd, migrate_permission_to_go,
                                   sizeof(migrate_permission_to_go),
                                   "GO message", 0);
    if (rc) goto perhaps_destroy_notify_rc;

    fprintf(stderr, "migration target: Got permission, starting domain.\n");

    if (migration_domname) {
        rc = libxl_domain_rename(ctx, domid, migration_domname, common_domname);
        if (rc) goto perhaps_destroy_notify_rc;
    }

    if (!pause_after_migration) {
        rc = libxl_domain_unpause(ctx, domid);
        if (rc) goto perhaps_destroy_notify_rc;
    }

    fprintf(stderr, "migration target: Domain started successsfully.\n");
    rc = 0;

 perhaps_destroy_notify_rc:
    rc2 = libxl_write_exactly(ctx, send_fd,
                              migrate_report, sizeof(migrate_report),
                              "migration ack stream",
                              "success/failure report");
    if (rc2) exit(EXIT_FAILURE);

    rc_buf = -rc;
    assert(!!rc_buf == !!rc);
    rc2 = libxl_write_exactly(ctx, send_fd, &rc_buf, 1,
                              "migration ack stream",
                              "success/failure code");
    if (rc2) exit(EXIT_FAILURE);

    if (rc) {
        fprintf(stderr, "migration target: Failure, destroying our copy.\n");

        rc2 = libxl_domain_destroy(ctx, domid, 0);
        if (rc2) {
            fprintf(stderr, "migration target: Failed to destroy our copy"
                    " (code %d).\n", rc2);
            exit(EXIT_FAILURE);
        }

        fprintf(stderr, "migration target: Cleanup OK, granting sender"
                " permission to resume.\n");

        rc2 = libxl_write_exactly(ctx, send_fd,
                                  migrate_permission_to_go,
                                  sizeof(migrate_permission_to_go),
                                  "migration ack stream",
                                  "permission to sender to have domain back");
        if (rc2) exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}

int main_restore(int argc, char **argv)
{
    const char *checkpoint_file = NULL;
    const char *config_file = NULL;
    struct domain_create dom_info;
    int paused = 0, debug = 0, daemonize = 1, monitor = 1,
        console_autoconnect = 0, vnc = 0, vncautopass = 0;
    int opt, rc;
    static struct option opts[] = {
        {"vncviewer", 0, 0, 'V'},
        {"vncviewer-autopass", 0, 0, 'A'},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "FcpdeVA", opts, "restore", 1) {
    case 'c':
        console_autoconnect = 1;
        break;
    case 'p':
        paused = 1;
        break;
    case 'd':
        debug = 1;
        break;
    case 'F':
        daemonize = 0;
        break;
    case 'e':
        daemonize = 0;
        monitor = 0;
        break;
    case 'V':
        vnc = 1;
        break;
    case 'A':
        vnc = vncautopass = 1;
        break;
    }

    if (argc-optind == 1) {
        checkpoint_file = argv[optind];
    } else if (argc-optind == 2) {
        config_file = argv[optind];
        checkpoint_file = argv[optind + 1];
    } else {
        help("restore");
        return EXIT_FAILURE;
    }

    memset(&dom_info, 0, sizeof(dom_info));
    dom_info.debug = debug;
    dom_info.daemonize = daemonize;
    dom_info.monitor = monitor;
    dom_info.paused = paused;
    dom_info.config_file = config_file;
    dom_info.restore_file = checkpoint_file;
    dom_info.migrate_fd = -1;
    dom_info.send_back_fd = -1;
    dom_info.vnc = vnc;
    dom_info.vncautopass = vncautopass;
    dom_info.console_autoconnect = console_autoconnect;

    rc = create_domain(&dom_info);
    if (rc < 0)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

int main_migrate_receive(int argc, char **argv)
{
    int debug = 0, daemonize = 1, monitor = 1, pause_after_migration = 0;
    libxl_checkpointed_stream checkpointed = LIBXL_CHECKPOINTED_STREAM_NONE;
    int opt;
    char *script = NULL;
    static struct option opts[] = {
        {"colo", 0, 0, 0x100},
        /* It is a shame that the management code for disk is not here. */
        {"coloft-script", 1, 0, 0x200},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "Fedrp", opts, "migrate-receive", 0) {
    case 'F':
        daemonize = 0;
        break;
    case 'e':
        daemonize = 0;
        monitor = 0;
        break;
    case 'd':
        debug = 1;
        break;
    case 'r':
        checkpointed = LIBXL_CHECKPOINTED_STREAM_REMUS;
        break;
    case 0x100:
        checkpointed = LIBXL_CHECKPOINTED_STREAM_COLO;
        break;
    case 0x200:
        script = optarg;
        break;
    case 'p':
        pause_after_migration = 1;
        break;
    }

    if (argc-optind != 0) {
        help("migrate-receive");
        return EXIT_FAILURE;
    }
    migrate_receive(debug, daemonize, monitor, pause_after_migration,
                    STDOUT_FILENO, STDIN_FILENO,
                    checkpointed, script);

    return EXIT_SUCCESS;
}

int main_save(int argc, char **argv)
{
    uint32_t domid;
    const char *filename;
    const char *config_filename = NULL;
    int checkpoint = 0;
    int leavepaused = 0;
    int opt;

    SWITCH_FOREACH_OPT(opt, "cp", NULL, "save", 2) {
    case 'c':
        checkpoint = 1;
        break;
    case 'p':
        leavepaused = 1;
        break;
    }

    if (argc-optind > 3) {
        help("save");
        return EXIT_FAILURE;
    }

    domid = find_domain(argv[optind]);
    filename = argv[optind + 1];
    if ( argc - optind >= 3 )
        config_filename = argv[optind + 2];

    save_domain(domid, filename, checkpoint, leavepaused, config_filename);
    return EXIT_SUCCESS;
}

int main_migrate(int argc, char **argv)
{
    uint32_t domid;
    const char *config_filename = NULL;
    const char *ssh_command = "ssh";
    char *rune = NULL;
    char *host;
    int opt, daemonize = 1, monitor = 1, debug = 0, pause_after_migration = 0;
    static struct option opts[] = {
        {"debug", 0, 0, 0x100},
        {"live", 0, 0, 0x200},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "FC:s:ep", opts, "migrate", 2) {
    case 'C':
        config_filename = optarg;
        break;
    case 's':
        ssh_command = optarg;
        break;
    case 'F':
        daemonize = 0;
        break;
    case 'e':
        daemonize = 0;
        monitor = 0;
        break;
    case 'p':
        pause_after_migration = 1;
        break;
    case 0x100: /* --debug */
        debug = 1;
        break;
    case 0x200: /* --live */
        /* ignored for compatibility with xm */
        break;
    }

    domid = find_domain(argv[optind]);
    host = argv[optind + 1];

    bool pass_tty_arg = progress_use_cr || (isatty(2) > 0);

    if (!ssh_command[0]) {
        rune= host;
    } else {
        char verbose_buf[minmsglevel_default+3];
        int verbose_len;
        verbose_buf[0] = ' ';
        verbose_buf[1] = '-';
        memset(verbose_buf+2, 'v', minmsglevel_default);
        verbose_buf[sizeof(verbose_buf)-1] = 0;
        if (minmsglevel == minmsglevel_default) {
            verbose_len = 0;
        } else {
            verbose_len = (minmsglevel_default - minmsglevel) + 2;
        }
        xasprintf(&rune, "exec %s %s xl%s%.*s migrate-receive%s%s%s",
                  ssh_command, host,
                  pass_tty_arg ? " -t" : "",
                  verbose_len, verbose_buf,
                  daemonize ? "" : " -e",
                  debug ? " -d" : "",
                  pause_after_migration ? " -p" : "");
    }

    migrate_domain(domid, rune, debug, config_filename);
    return EXIT_SUCCESS;
}
#endif

int main_dump_core(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "dump-core", 2) {
        /* No options */
    }

    core_dump_domain(find_domain(argv[optind]), argv[optind + 1]);
    return EXIT_SUCCESS;
}

int main_pause(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "pause", 1) {
        /* No options */
    }

    pause_domain(find_domain(argv[optind]));

    return EXIT_SUCCESS;
}

int main_unpause(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "unpause", 1) {
        /* No options */
    }

    unpause_domain(find_domain(argv[optind]));

    return EXIT_SUCCESS;
}

int main_destroy(int argc, char **argv)
{
    int opt;
    int force = 0;

    SWITCH_FOREACH_OPT(opt, "f", NULL, "destroy", 1) {
    case 'f':
        force = 1;
        break;
    }

    destroy_domain(find_domain(argv[optind]), force);
    return EXIT_SUCCESS;
}

static int main_shutdown_or_reboot(int do_reboot, int argc, char **argv)
{
    const char *what = do_reboot ? "reboot" : "shutdown";
    void (*fn)(uint32_t domid,
               libxl_evgen_domain_death **, libxl_ev_user, int) =
        do_reboot ? &reboot_domain : &shutdown_domain;
    int opt, i, nb_domain;
    int wait_for_it = 0, all = 0, nrdeathws = 0;
    int fallback_trigger = 0;
    static struct option opts[] = {
        {"all", 0, 0, 'a'},
        {"wait", 0, 0, 'w'},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "awF", opts, what, 0) {
    case 'a':
        all = 1;
        break;
    case 'w':
        wait_for_it = 1;
        break;
    case 'F':
        fallback_trigger = 1;
        break;
    }

    if (!argv[optind] && !all) {
        fprintf(stderr, "You must specify -a or a domain id.\n\n");
        return EXIT_FAILURE;
    }

    if (all) {
        libxl_dominfo *dominfo;
        libxl_evgen_domain_death **deathws = NULL;
        if (!(dominfo = libxl_list_domain(ctx, &nb_domain))) {
            fprintf(stderr, "libxl_list_domain failed.\n");
            return EXIT_FAILURE;
        }

        if (wait_for_it)
            deathws = calloc(nb_domain, sizeof(*deathws));

        for (i = 0; i<nb_domain; i++) {
            if (dominfo[i].domid == 0 || dominfo[i].never_stop)
                continue;
            fn(dominfo[i].domid, deathws ? &deathws[i] : NULL, i,
               fallback_trigger);
            nrdeathws++;
        }

        if (deathws) {
            wait_for_domain_deaths(deathws, nrdeathws);
            free(deathws);
        }

        libxl_dominfo_list_free(dominfo, nb_domain);
    } else {
        libxl_evgen_domain_death *deathw = NULL;
        uint32_t domid = find_domain(argv[optind]);

        fn(domid, wait_for_it ? &deathw : NULL, 0, fallback_trigger);

        if (wait_for_it)
            wait_for_domain_deaths(&deathw, 1);
    }


    return EXIT_SUCCESS;
}

int main_shutdown(int argc, char **argv)
{
    return main_shutdown_or_reboot(0, argc, argv);
}

int main_reboot(int argc, char **argv)
{
    return main_shutdown_or_reboot(1, argc, argv);
}

int main_list(int argc, char **argv)
{
    int opt;
    bool verbose = false;
    bool context = false;
    bool details = false;
    bool cpupool = false;
    bool numa = false;
    static struct option opts[] = {
        {"long", 0, 0, 'l'},
        {"verbose", 0, 0, 'v'},
        {"context", 0, 0, 'Z'},
        {"cpupool", 0, 0, 'c'},
        {"numa", 0, 0, 'n'},
        COMMON_LONG_OPTS
    };

    libxl_dominfo info_buf;
    libxl_dominfo *info, *info_free=0;
    int nb_domain, rc;

    SWITCH_FOREACH_OPT(opt, "lvhZcn", opts, "list", 0) {
    case 'l':
        details = true;
        break;
    case 'v':
        verbose = true;
        break;
    case 'Z':
        context = true;
        break;
    case 'c':
        cpupool = true;
        break;
    case 'n':
        numa = true;
        break;
    }

    libxl_dominfo_init(&info_buf);

    if (optind >= argc) {
        info = libxl_list_domain(ctx, &nb_domain);
        if (!info) {
            fprintf(stderr, "libxl_list_domain failed.\n");
            return EXIT_FAILURE;
        }
        info_free = info;
    } else if (optind == argc-1) {
        uint32_t domid = find_domain(argv[optind]);
        rc = libxl_domain_info(ctx, &info_buf, domid);
        if (rc == ERROR_DOMAIN_NOTFOUND) {
            fprintf(stderr, "Error: Domain \'%s\' does not exist.\n",
                argv[optind]);
            return EXIT_FAILURE;
        }
        if (rc) {
            fprintf(stderr, "libxl_domain_info failed (code %d).\n", rc);
            return EXIT_FAILURE;
        }
        info = &info_buf;
        nb_domain = 1;
    } else {
        help("list");
        return EXIT_FAILURE;
    }

    if (details)
        list_domains_details(info, nb_domain);
    else
        list_domains(verbose, context, false /* claim */, numa, cpupool,
                     info, nb_domain);

    if (info_free)
        libxl_dominfo_list_free(info, nb_domain);

    libxl_dominfo_dispose(&info_buf);

    return EXIT_SUCCESS;
}

int main_vm_list(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "vm-list", 0) {
        /* No options */
    }

    list_vm();
    return EXIT_SUCCESS;
}

int main_create(int argc, char **argv)
{
    const char *filename = NULL;
    struct domain_create dom_info;
    int paused = 0, debug = 0, daemonize = 1, console_autoconnect = 0,
        quiet = 0, monitor = 1, vnc = 0, vncautopass = 0;
    int opt, rc;
    static struct option opts[] = {
        {"dryrun", 0, 0, 'n'},
        {"quiet", 0, 0, 'q'},
        {"defconfig", 1, 0, 'f'},
        {"vncviewer", 0, 0, 'V'},
        {"vncviewer-autopass", 0, 0, 'A'},
        COMMON_LONG_OPTS
    };

    dom_info.extra_config = NULL;

    if (argv[1] && argv[1][0] != '-' && !strchr(argv[1], '=')) {
        filename = argv[1];
        argc--; argv++;
    }

    SWITCH_FOREACH_OPT(opt, "Fnqf:pcdeVA", opts, "create", 0) {
    case 'f':
        filename = optarg;
        break;
    case 'p':
        paused = 1;
        break;
    case 'c':
        console_autoconnect = 1;
        break;
    case 'd':
        debug = 1;
        break;
    case 'F':
        daemonize = 0;
        break;
    case 'e':
        daemonize = 0;
        monitor = 0;
        break;
    case 'n':
        dryrun_only = 1;
        break;
    case 'q':
        quiet = 1;
        break;
    case 'V':
        vnc = 1;
        break;
    case 'A':
        vnc = vncautopass = 1;
        break;
    }

    memset(&dom_info, 0, sizeof(dom_info));

    for (; optind < argc; optind++) {
        if (strchr(argv[optind], '=') != NULL) {
            string_realloc_append(&dom_info.extra_config, argv[optind]);
            string_realloc_append(&dom_info.extra_config, "\n");
        } else if (!filename) {
            filename = argv[optind];
        } else {
            help("create");
            free(dom_info.extra_config);
            return 2;
        }
    }

    dom_info.debug = debug;
    dom_info.daemonize = daemonize;
    dom_info.monitor = monitor;
    dom_info.paused = paused;
    dom_info.dryrun = dryrun_only;
    dom_info.quiet = quiet;
    dom_info.config_file = filename;
    dom_info.migrate_fd = -1;
    dom_info.send_back_fd = -1;
    dom_info.vnc = vnc;
    dom_info.vncautopass = vncautopass;
    dom_info.console_autoconnect = console_autoconnect;

    rc = create_domain(&dom_info);
    if (rc < 0) {
        free(dom_info.extra_config);
        return -rc;
    }

    free(dom_info.extra_config);
    return 0;
}

int main_config_update(int argc, char **argv)
{
    uint32_t domid;
    const char *filename = NULL;
    char *extra_config = NULL;
    void *config_data = 0;
    int config_len = 0;
    libxl_domain_config d_config;
    int opt, rc;
    int debug = 0;
    static struct option opts[] = {
        {"defconfig", 1, 0, 'f'},
        COMMON_LONG_OPTS
    };

    if (argc < 2) {
        fprintf(stderr, "xl config-update requires a domain argument\n");
        help("config-update");
        exit(1);
    }

    fprintf(stderr, "WARNING: xl now has better capability to manage domain configuration, "
            "avoid using this command when possible\n");

    domid = find_domain(argv[1]);
    argc--; argv++;

    if (argv[1] && argv[1][0] != '-' && !strchr(argv[1], '=')) {
        filename = argv[1];
        argc--; argv++;
    }

    SWITCH_FOREACH_OPT(opt, "dqf:", opts, "config_update", 0) {
    case 'd':
        debug = 1;
        break;
    case 'f':
        filename = optarg;
        break;
    }

    for (; optind < argc; optind++) {
        if (strchr(argv[optind], '=') != NULL) {
            string_realloc_append(&extra_config, argv[optind]);
            string_realloc_append(&extra_config, "\n");
        } else if (!filename) {
            filename = argv[optind];
        } else {
            help("create");
            free(extra_config);
            return 2;
        }
    }
    if (filename) {
        free(config_data);  config_data = 0;
        rc = libxl_read_file_contents(ctx, filename,
                                      &config_data, &config_len);
        if (rc) { fprintf(stderr, "Failed to read config file: %s: %s\n",
                           filename, strerror(errno));
                  free(extra_config); return ERROR_FAIL; }
        if (extra_config && strlen(extra_config)) {
            if (config_len > INT_MAX - (strlen(extra_config) + 2 + 1)) {
                fprintf(stderr, "Failed to attach extra configuration\n");
                exit(1);
            }
            /* allocate space for the extra config plus two EOLs plus \0 */
            config_data = realloc(config_data, config_len
                + strlen(extra_config) + 2 + 1);
            if (!config_data) {
                fprintf(stderr, "Failed to realloc config_data\n");
                exit(1);
            }
            config_len += sprintf(config_data + config_len, "\n%s\n",
                extra_config);
        }
    } else {
        fprintf(stderr, "Config file not specified\n");
        exit(1);
    }

    libxl_domain_config_init(&d_config);

    parse_config_data(filename, config_data, config_len, &d_config);

    if (debug || dryrun_only)
        printf_info(default_output_format, -1, &d_config, stdout);

    if (!dryrun_only) {
        fprintf(stderr, "setting dom%u configuration\n", domid);
        rc = libxl_userdata_store(ctx, domid, "xl",
                                   config_data, config_len);
        if (rc) {
            fprintf(stderr, "failed to update configuration\n");
            exit(1);
        }
    }

    libxl_domain_config_dispose(&d_config);

    free(config_data);
    free(extra_config);
    return 0;
}

static void button_press(uint32_t domid, const char *b)
{
    libxl_trigger trigger;

    if (!strcmp(b, "power")) {
        trigger = LIBXL_TRIGGER_POWER;
    } else if (!strcmp(b, "sleep")) {
        trigger = LIBXL_TRIGGER_SLEEP;
    } else {
        fprintf(stderr, "%s is an invalid button identifier\n", b);
        exit(EXIT_FAILURE);
    }

    libxl_send_trigger(ctx, domid, trigger, 0);
}

int main_button_press(int argc, char **argv)
{
    int opt;

    fprintf(stderr, "WARNING: \"button-press\" is deprecated. "
            "Please use \"trigger\"\n");


    SWITCH_FOREACH_OPT(opt, "", NULL, "button-press", 2) {
        /* No options */
    }

    button_press(find_domain(argv[optind]), argv[optind + 1]);

    return 0;
}

/* Possibly select a specific piece of `xl info` to print. */
static const char *info_name;
static int maybe_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
static int maybe_printf(const char *fmt, ...)
{
    va_list ap;
    char *str;
    int count = 0;

    va_start(ap, fmt);
    if (vasprintf(&str, fmt, ap) != -1) {
        if (info_name) {
            char *s;

            if (!strncmp(str, info_name, strlen(info_name)) &&
                (s = strchr(str, ':')) && s[1] == ' ')
                count = fputs(&s[2], stdout);
        } else
            count = fputs(str, stdout);

        free(str);
    }
    va_end(ap);

    return count;
}

static void output_xeninfo(void)
{
    const libxl_version_info *info;
    libxl_scheduler sched;
    int rc;

    if (!(info = libxl_get_version_info(ctx))) {
        fprintf(stderr, "libxl_get_version_info failed.\n");
        return;
    }

    rc = libxl_get_scheduler(ctx);
    if (rc < 0) {
        fprintf(stderr, "get_scheduler sysctl failed.\n");
        return;
    }
    sched = rc;

    maybe_printf("xen_major              : %d\n", info->xen_version_major);
    maybe_printf("xen_minor              : %d\n", info->xen_version_minor);
    maybe_printf("xen_extra              : %s\n", info->xen_version_extra);
    maybe_printf("xen_version            : %d.%d%s\n", info->xen_version_major,
           info->xen_version_minor, info->xen_version_extra);
    maybe_printf("xen_caps               : %s\n", info->capabilities);
    maybe_printf("xen_scheduler          : %s\n", libxl_scheduler_to_string(sched));
    maybe_printf("xen_pagesize           : %u\n", info->pagesize);
    maybe_printf("platform_params        : virt_start=0x%"PRIx64"\n", info->virt_start);
    maybe_printf("xen_changeset          : %s\n", info->changeset);
    maybe_printf("xen_commandline        : %s\n", info->commandline);
    maybe_printf("cc_compiler            : %s\n", info->compiler);
    maybe_printf("cc_compile_by          : %s\n", info->compile_by);
    maybe_printf("cc_compile_domain      : %s\n", info->compile_domain);
    maybe_printf("cc_compile_date        : %s\n", info->compile_date);
    maybe_printf("build_id               : %s\n", info->build_id);

    return;
}

static void output_nodeinfo(void)
{
    struct utsname utsbuf;

    if (uname(&utsbuf) < 0)
        return;

    maybe_printf("host                   : %s\n", utsbuf.nodename);
    maybe_printf("release                : %s\n", utsbuf.release);
    maybe_printf("version                : %s\n", utsbuf.version);
    maybe_printf("machine                : %s\n", utsbuf.machine);
}

static void output_physinfo(void)
{
    libxl_physinfo info;
    const libxl_version_info *vinfo;
    unsigned int i;
    libxl_bitmap cpumap;
    int n = 0;

    if (libxl_get_physinfo(ctx, &info) != 0) {
        fprintf(stderr, "libxl_physinfo failed.\n");
        return;
    }
    maybe_printf("nr_cpus                : %d\n", info.nr_cpus);
    maybe_printf("max_cpu_id             : %d\n", info.max_cpu_id);
    maybe_printf("nr_nodes               : %d\n", info.nr_nodes);
    maybe_printf("cores_per_socket       : %d\n", info.cores_per_socket);
    maybe_printf("threads_per_core       : %d\n", info.threads_per_core);
    maybe_printf("cpu_mhz                : %d\n", info.cpu_khz / 1000);

    maybe_printf("hw_caps                : %08x:%08x:%08x:%08x:%08x:%08x:%08x:%08x\n",
         info.hw_cap[0], info.hw_cap[1], info.hw_cap[2], info.hw_cap[3],
         info.hw_cap[4], info.hw_cap[5], info.hw_cap[6], info.hw_cap[7]
        );

    maybe_printf("virt_caps              :%s%s\n",
         info.cap_hvm ? " hvm" : "",
         info.cap_hvm_directio ? " hvm_directio" : ""
        );

    vinfo = libxl_get_version_info(ctx);
    if (vinfo) {
        i = (1 << 20) / vinfo->pagesize;
        maybe_printf("total_memory           : %"PRIu64"\n", info.total_pages / i);
        maybe_printf("free_memory            : %"PRIu64"\n", (info.free_pages - info.outstanding_pages) / i);
        maybe_printf("sharing_freed_memory   : %"PRIu64"\n", info.sharing_freed_pages / i);
        maybe_printf("sharing_used_memory    : %"PRIu64"\n", info.sharing_used_frames / i);
        maybe_printf("outstanding_claims     : %"PRIu64"\n", info.outstanding_pages / i);
    }
    if (!libxl_get_freecpus(ctx, &cpumap)) {
        libxl_for_each_bit(i, cpumap)
            if (libxl_bitmap_test(&cpumap, i))
                n++;
        maybe_printf("free_cpus              : %d\n", n);
        free(cpumap.map);
    }
    libxl_physinfo_dispose(&info);
    return;
}

static void output_numainfo(void)
{
    libxl_numainfo *info;
    int i, j, nr;

    info = libxl_get_numainfo(ctx, &nr);
    if (info == NULL) {
        fprintf(stderr, "libxl_get_numainfo failed.\n");
        return;
    }

    printf("numa_info              :\n");
    printf("node:    memsize    memfree    distances\n");

    for (i = 0; i < nr; i++) {
        if (info[i].size != LIBXL_NUMAINFO_INVALID_ENTRY) {
            printf("%4d:    %6"PRIu64"     %6"PRIu64"      %d", i,
                   info[i].size >> 20, info[i].free >> 20,
                   info[i].dists[0]);
            for (j = 1; j < info[i].num_dists; j++)
                printf(",%d", info[i].dists[j]);
            printf("\n");
        }
    }

    libxl_numainfo_list_free(info, nr);

    return;
}

static void output_topologyinfo(void)
{
    libxl_cputopology *cpuinfo;
    int i, nr;
    libxl_pcitopology *pciinfo;
    int valid_devs = 0;


    cpuinfo = libxl_get_cpu_topology(ctx, &nr);
    if (cpuinfo == NULL) {
        fprintf(stderr, "libxl_get_cpu_topology failed.\n");
        return;
    }

    printf("cpu_topology           :\n");
    printf("cpu:    core    socket     node\n");

    for (i = 0; i < nr; i++) {
        if (cpuinfo[i].core != LIBXL_CPUTOPOLOGY_INVALID_ENTRY)
            printf("%3d:    %4d     %4d     %4d\n", i,
                   cpuinfo[i].core, cpuinfo[i].socket, cpuinfo[i].node);
    }

    libxl_cputopology_list_free(cpuinfo, nr);

    pciinfo = libxl_get_pci_topology(ctx, &nr);
    if (pciinfo == NULL) {
        fprintf(stderr, "libxl_get_pci_topology failed.\n");
        return;
    }

    printf("device topology        :\n");
    printf("device           node\n");
    for (i = 0; i < nr; i++) {
        if (pciinfo[i].node != LIBXL_PCITOPOLOGY_INVALID_ENTRY) {
            printf("%04x:%02x:%02x.%01x      %d\n", pciinfo[i].seg,
                   pciinfo[i].bus,
                   ((pciinfo[i].devfn >> 3) & 0x1f), (pciinfo[i].devfn & 7),
                   pciinfo[i].node);
            valid_devs++;
        }
    }

    if (valid_devs == 0)
        printf("No device topology data available\n");

    libxl_pcitopology_list_free(pciinfo, nr);

    return;
}

static void print_info(int numa)
{
    output_nodeinfo();

    output_physinfo();

    if (numa) {
        output_topologyinfo();
        output_numainfo();
    }
    output_xeninfo();

    maybe_printf("xend_config_format     : 4\n");

    return;
}

int main_info(int argc, char **argv)
{
    int opt;
    static struct option opts[] = {
        {"numa", 0, 0, 'n'},
        COMMON_LONG_OPTS
    };
    int numa = 0;

    SWITCH_FOREACH_OPT(opt, "n", opts, "info", 0) {
    case 'n':
        numa = 1;
        break;
    }

    /*
     * If an extra argument is provided, filter out a specific piece of
     * information.
     */
    if (numa == 0 && argc > optind)
        info_name = argv[optind];

    print_info(numa);
    return 0;
}

int main_domid(int argc, char **argv)
{
    uint32_t domid;
    int opt;
    const char *domname = NULL;

    SWITCH_FOREACH_OPT(opt, "", NULL, "domid", 1) {
        /* No options */
    }

    domname = argv[optind];

    if (libxl_name_to_domid(ctx, domname, &domid)) {
        fprintf(stderr, "Can't get domid of domain name '%s', maybe this domain does not exist.\n", domname);
        return EXIT_FAILURE;
    }

    printf("%u\n", domid);

    return EXIT_SUCCESS;
}

int main_domname(int argc, char **argv)
{
    uint32_t domid;
    int opt;
    char *domname = NULL;
    char *endptr = NULL;

    SWITCH_FOREACH_OPT(opt, "", NULL, "domname", 1) {
        /* No options */
    }

    domid = strtol(argv[optind], &endptr, 10);
    if (domid == 0 && !strcmp(endptr, argv[optind])) {
        /*no digits at all*/
        fprintf(stderr, "Invalid domain id.\n\n");
        return EXIT_FAILURE;
    }

    domname = libxl_domid_to_name(ctx, domid);
    if (!domname) {
        fprintf(stderr, "Can't get domain name of domain id '%u', maybe this domain does not exist.\n", domid);
        return EXIT_FAILURE;
    }

    printf("%s\n", domname);
    free(domname);

    return EXIT_SUCCESS;
}

int main_rename(int argc, char **argv)
{
    uint32_t domid;
    int opt;
    const char *dom, *new_name;

    SWITCH_FOREACH_OPT(opt, "", NULL, "rename", 2) {
        /* No options */
    }

    dom = argv[optind++];
    new_name = argv[optind];

    domid = find_domain(dom);
    if (libxl_domain_rename(ctx, domid, common_domname, new_name)) {
        fprintf(stderr, "Can't rename domain '%s'.\n", dom);
        return 1;
    }

    return 0;
}

int main_trigger(int argc, char **argv)
{
    uint32_t domid;
    int opt;
    char *endptr = NULL;
    int vcpuid = 0;
    const char *trigger_name = NULL;
    libxl_trigger trigger;

    SWITCH_FOREACH_OPT(opt, "", NULL, "trigger", 2) {
        /* No options */
    }

    domid = find_domain(argv[optind++]);

    trigger_name = argv[optind++];
    if (libxl_trigger_from_string(trigger_name, &trigger)) {
        fprintf(stderr, "Invalid trigger \"%s\"\n", trigger_name);
        return EXIT_FAILURE;
    }

    if (argv[optind]) {
        vcpuid = strtol(argv[optind], &endptr, 10);
        if (vcpuid == 0 && !strcmp(endptr, argv[optind])) {
            fprintf(stderr, "Invalid vcpuid, using default vcpuid=0.\n\n");
        }
    }

    libxl_send_trigger(ctx, domid, trigger, vcpuid);

    return EXIT_SUCCESS;
}


int main_sysrq(int argc, char **argv)
{
    uint32_t domid;
    int opt;
    const char *sysrq = NULL;

    SWITCH_FOREACH_OPT(opt, "", NULL, "sysrq", 2) {
        /* No options */
    }

    domid = find_domain(argv[optind++]);

    sysrq = argv[optind];

    if (sysrq[1] != '\0') {
        fprintf(stderr, "Invalid sysrq.\n\n");
        help("sysrq");
        return EXIT_FAILURE;
    }

    libxl_send_sysrq(ctx, domid, sysrq[0]);

    return EXIT_SUCCESS;
}

int main_debug_keys(int argc, char **argv)
{
    int opt;
    char *keys;

    SWITCH_FOREACH_OPT(opt, "", NULL, "debug-keys", 1) {
        /* No options */
    }

    keys = argv[optind];

    if (libxl_send_debug_keys(ctx, keys)) {
        fprintf(stderr, "cannot send debug keys: %s\n", keys);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int main_dmesg(int argc, char **argv)
{
    unsigned int clear = 0;
    libxl_xen_console_reader *cr;
    char *line;
    int opt, ret = 1;

    SWITCH_FOREACH_OPT(opt, "c", NULL, "dmesg", 0) {
    case 'c':
        clear = 1;
        break;
    }

    cr = libxl_xen_console_read_start(ctx, clear);
    if (!cr)
        goto finish;

    while ((ret = libxl_xen_console_read_line(ctx, cr, &line)) > 0)
        printf("%s", line);

finish:
    if (cr)
        libxl_xen_console_read_finish(ctx, cr);
    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main_top(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "top", 0) {
        /* No options */
    }

    return system("xentop");
}

int main_channellist(int argc, char **argv)
{
    int opt;
    libxl_device_channel *channels;
    libxl_channelinfo channelinfo;
    int nb, i;

    SWITCH_FOREACH_OPT(opt, "", NULL, "channel-list", 1) {
        /* No options */
    }

    /*      Idx BE state evt-ch ring-ref connection params*/
    printf("%-3s %-2s %-5s %-6s %8s %-10s %-30s\n",
           "Idx", "BE", "state", "evt-ch", "ring-ref", "connection", "");
    for (argv += optind, argc -= optind; argc > 0; --argc, ++argv) {
        uint32_t domid = find_domain(*argv);
        channels = libxl_device_channel_list(ctx, domid, &nb);
        if (!channels)
            continue;
        for (i = 0; i < nb; ++i) {
            if (!libxl_device_channel_getinfo(ctx, domid, &channels[i],
                &channelinfo)) {
                printf("%-3d %-2d ", channels[i].devid, channelinfo.backend_id);
                printf("%-5d ", channelinfo.state);
                printf("%-6d %-8d ", channelinfo.evtch, channelinfo.rref);
                printf("%-10s ", libxl_channel_connection_to_string(
                       channels[i].connection));
                switch (channels[i].connection) {
                    case LIBXL_CHANNEL_CONNECTION_PTY:
                        printf("%-30s ", channelinfo.u.pty.path);
                        break;
                    default:
                        break;
                }
                printf("\n");
                libxl_channelinfo_dispose(&channelinfo);
            }
            libxl_device_channel_dispose(&channels[i]);
        }
        free(channels);
    }
    return 0;
}

static char *uptime_to_string(unsigned long uptime, int short_mode)
{
    int sec, min, hour, day;
    char *time_string;

    day = (int)(uptime / 86400);
    uptime -= (day * 86400);
    hour = (int)(uptime / 3600);
    uptime -= (hour * 3600);
    min = (int)(uptime / 60);
    uptime -= (min * 60);
    sec = uptime;

    if (short_mode)
        if (day > 1)
            xasprintf(&time_string, "%d days, %2d:%02d", day, hour, min);
        else if (day == 1)
            xasprintf(&time_string, "%d day, %2d:%02d", day, hour, min);
        else
            xasprintf(&time_string, "%2d:%02d", hour, min);
    else
        if (day > 1)
            xasprintf(&time_string, "%d days, %2d:%02d:%02d", day, hour, min, sec);
        else if (day == 1)
            xasprintf(&time_string, "%d day, %2d:%02d:%02d", day, hour, min, sec);
        else
            xasprintf(&time_string, "%2d:%02d:%02d", hour, min, sec);

    return time_string;
}

int main_claims(int argc, char **argv)
{
    libxl_dominfo *info;
    int opt;
    int nb_domain;

    SWITCH_FOREACH_OPT(opt, "", NULL, "claims", 0) {
        /* No options */
    }

    if (!claim_mode)
        fprintf(stderr, "claim_mode not enabled (see man xl.conf).\n");

    info = libxl_list_domain(ctx, &nb_domain);
    if (!info) {
        fprintf(stderr, "libxl_list_domain failed.\n");
        return 1;
    }

    list_domains(false /* verbose */, false /* context */, true /* claim */,
                 false /* numa */, false /* cpupool */, info, nb_domain);

    libxl_dominfo_list_free(info, nb_domain);
    return 0;
}

static char *current_time_to_string(time_t now)
{
    char now_str[100];
    struct tm *tmp;

    tmp = localtime(&now);
    if (tmp == NULL) {
        fprintf(stderr, "Get localtime error");
        exit(-1);
    }
    if (strftime(now_str, sizeof(now_str), "%H:%M:%S", tmp) == 0) {
        fprintf(stderr, "strftime returned 0");
        exit(-1);
    }
    return strdup(now_str);
}

static void print_dom0_uptime(int short_mode, time_t now)
{
    int fd;
    ssize_t nr;
    char buf[512];
    uint32_t uptime = 0;
    char *uptime_str = NULL;
    char *now_str = NULL;
    char *domname;

    fd = open("/proc/uptime", O_RDONLY);
    if (fd == -1)
        goto err;

    nr = read(fd, buf, sizeof(buf) - 1);
    if (nr == -1) {
        close(fd);
        goto err;
    }
    close(fd);

    buf[nr] = '\0';

    strtok(buf, " ");
    uptime = strtoul(buf, NULL, 10);

    domname = libxl_domid_to_name(ctx, 0);
    if (short_mode)
    {
        now_str = current_time_to_string(now);
        uptime_str = uptime_to_string(uptime, 1);
        printf(" %s up %s, %s (%d)\n", now_str, uptime_str,
               domname, 0);
    }
    else
    {
        now_str = NULL;
        uptime_str = uptime_to_string(uptime, 0);
        printf("%-33s %4d %s\n", domname,
               0, uptime_str);
    }

    free(now_str);
    free(uptime_str);
    free(domname);
    return;
err:
    fprintf(stderr, "Can not get Dom0 uptime.\n");
    exit(-1);
}

static void print_domU_uptime(uint32_t domuid, int short_mode, time_t now)
{
    uint32_t s_time = 0;
    uint32_t uptime = 0;
    char *uptime_str = NULL;
    char *now_str = NULL;
    char *domname;

    s_time = libxl_vm_get_start_time(ctx, domuid);
    if (s_time == -1)
        return;
    uptime = now - s_time;
    domname = libxl_domid_to_name(ctx, domuid);
    if (short_mode)
    {
        now_str = current_time_to_string(now);
        uptime_str = uptime_to_string(uptime, 1);
        printf(" %s up %s, %s (%d)\n", now_str, uptime_str,
               domname, domuid);
    }
    else
    {
        now_str = NULL;
        uptime_str = uptime_to_string(uptime, 0);
        printf("%-33s %4d %s\n", domname,
               domuid, uptime_str);
    }

    free(domname);
    free(now_str);
    free(uptime_str);
    return;
}

static void print_uptime(int short_mode, uint32_t doms[], int nb_doms)
{
    libxl_vminfo *info;
    time_t now;
    int nb_vm, i;

    now = time(NULL);

    if (!short_mode)
        printf("%-33s %4s %s\n", "Name", "ID", "Uptime");

    if (nb_doms == 0) {
        print_dom0_uptime(short_mode, now);
        info = libxl_list_vm(ctx, &nb_vm);
        if (info == NULL) {
            fprintf(stderr, "Could not list vms.\n");
            return;
        }
        for (i = 0; i < nb_vm; i++) {
            if (info[i].domid == 0) continue;
            print_domU_uptime(info[i].domid, short_mode, now);
        }
        libxl_vminfo_list_free(info, nb_vm);
    } else {
        for (i = 0; i < nb_doms; i++) {
            if (doms[i] == 0)
                print_dom0_uptime(short_mode, now);
            else
                print_domU_uptime(doms[i], short_mode, now);
        }
    }
}

int main_uptime(int argc, char **argv)
{
    const char *dom;
    int short_mode = 0;
    uint32_t domains[100];
    int nb_doms = 0;
    int opt;

    SWITCH_FOREACH_OPT(opt, "s", NULL, "uptime", 0) {
    case 's':
        short_mode = 1;
        break;
    }

    for (;(dom = argv[optind]) != NULL; nb_doms++,optind++)
        domains[nb_doms] = find_domain(dom);

    print_uptime(short_mode, domains, nb_doms);

    return 0;
}

#ifndef LIBXL_HAVE_NO_SUSPEND_RESUME
int main_remus(int argc, char **argv)
{
    uint32_t domid;
    int opt, rc, daemonize = 1;
    const char *ssh_command = "ssh";
    char *host = NULL, *rune = NULL;
    libxl_domain_remus_info r_info;
    int send_fd = -1, recv_fd = -1;
    pid_t child = -1;
    uint8_t *config_data;
    int config_len;

    memset(&r_info, 0, sizeof(libxl_domain_remus_info));

    SWITCH_FOREACH_OPT(opt, "Fbundi:s:N:ec", NULL, "remus", 2) {
    case 'i':
        r_info.interval = atoi(optarg);
        break;
    case 'F':
        libxl_defbool_set(&r_info.allow_unsafe, true);
        break;
    case 'b':
        libxl_defbool_set(&r_info.blackhole, true);
        break;
    case 'u':
        libxl_defbool_set(&r_info.compression, false);
        break;
    case 'n':
        libxl_defbool_set(&r_info.netbuf, false);
        break;
    case 'N':
        r_info.netbufscript = optarg;
        break;
    case 'd':
        libxl_defbool_set(&r_info.diskbuf, false);
        break;
    case 's':
        ssh_command = optarg;
        break;
    case 'e':
        daemonize = 0;
        break;
    case 'c':
        libxl_defbool_set(&r_info.colo, true);
    }

    domid = find_domain(argv[optind]);
    host = argv[optind + 1];

    /* Defaults */
    libxl_defbool_setdefault(&r_info.blackhole, false);
    libxl_defbool_setdefault(&r_info.colo, false);
    if (!libxl_defbool_val(r_info.colo) && !r_info.interval)
        r_info.interval = 200;

    if (libxl_defbool_val(r_info.colo)) {
        if (r_info.interval || libxl_defbool_val(r_info.blackhole) ||
            !libxl_defbool_is_default(r_info.netbuf) ||
            !libxl_defbool_is_default(r_info.diskbuf)) {
            perror("option -c is conflict with -i, -d, -n or -b");
            exit(-1);
        }

        if (libxl_defbool_is_default(r_info.compression)) {
            perror("COLO can't be used with memory compression. "
                   "Disable memory checkpoint compression now...");
            libxl_defbool_set(&r_info.compression, false);
        }
    }

    if (!r_info.netbufscript) {
        if (libxl_defbool_val(r_info.colo))
            r_info.netbufscript = default_colo_proxy_script;
        else
            r_info.netbufscript = default_remus_netbufscript;
    }

    if (libxl_defbool_val(r_info.blackhole)) {
        send_fd = open("/dev/null", O_RDWR, 0644);
        if (send_fd < 0) {
            perror("failed to open /dev/null");
            exit(EXIT_FAILURE);
        }
    } else {

        if (!ssh_command[0]) {
            rune = host;
        } else {
            if (!libxl_defbool_val(r_info.colo)) {
                xasprintf(&rune, "exec %s %s xl migrate-receive %s %s",
                          ssh_command, host,
                          "-r",
                          daemonize ? "" : " -e");
            } else {
                xasprintf(&rune, "exec %s %s xl migrate-receive %s %s %s %s",
                          ssh_command, host,
                          "--colo",
                          r_info.netbufscript ? "--coloft-script" : "",
                          r_info.netbufscript ? r_info.netbufscript : "",
                          daemonize ? "" : " -e");
            }
        }

        save_domain_core_begin(domid, NULL, &config_data, &config_len);

        if (!config_len) {
            fprintf(stderr, "No config file stored for running domain and "
                    "none supplied - cannot start remus.\n");
            exit(EXIT_FAILURE);
        }

        child = create_migration_child(rune, &send_fd, &recv_fd);

        migrate_do_preamble(send_fd, recv_fd, child, config_data, config_len,
                            rune);

        if (ssh_command[0])
            free(rune);
    }

    /* Point of no return */
    rc = libxl_domain_remus_start(ctx, &r_info, domid, send_fd, recv_fd, 0);

    /* check if the domain exists. User may have xl destroyed the
     * domain to force failover
     */
    if (libxl_domain_info(ctx, 0, domid)) {
        fprintf(stderr, "%s: Primary domain has been destroyed.\n",
                libxl_defbool_val(r_info.colo) ? "COLO" : "Remus");
        close(send_fd);
        return EXIT_SUCCESS;
    }

    /* If we are here, it means remus setup/domain suspend/backup has
     * failed. Try to resume the domain and exit gracefully.
     * TODO: Split-Brain check.
     */
    if (rc == ERROR_GUEST_TIMEDOUT)
        fprintf(stderr, "Failed to suspend domain at primary.\n");
    else {
        fprintf(stderr, "%s: Backup failed? resuming domain at primary.\n",
                libxl_defbool_val(r_info.colo) ? "COLO" : "Remus");
        libxl_domain_resume(ctx, domid, 1, 0);
    }

    close(send_fd);
    return EXIT_FAILURE;
}
#endif

int main_devd(int argc, char **argv)
{
    int ret = 0, opt = 0, daemonize = 1;
    const char *pidfile = NULL;
    static const struct option opts[] = {
        {"pidfile", 1, 0, 'p'},
        COMMON_LONG_OPTS,
        {0, 0, 0, 0}
    };

    SWITCH_FOREACH_OPT(opt, "Fp:", opts, "devd", 0) {
    case 'F':
        daemonize = 0;
        break;
    case 'p':
        pidfile = optarg;
        break;
    }

    if (daemonize) {
        ret = do_daemonize("xldevd", pidfile);
        if (ret) {
            ret = (ret == 1) ? 0 : ret;
            goto out;
        }
    }

    libxl_device_events_handler(ctx, 0);

out:
    return ret;
}

#ifdef LIBXL_HAVE_PSR_CMT
static int psr_cmt_hwinfo(void)
{
    int rc;
    int enabled;
    uint32_t total_rmid;

    printf("Cache Monitoring Technology (CMT):\n");

    enabled = libxl_psr_cmt_enabled(ctx);
    printf("%-16s: %s\n", "Enabled", enabled ? "1" : "0");
    if (!enabled)
        return 0;

    rc = libxl_psr_cmt_get_total_rmid(ctx, &total_rmid);
    if (rc) {
        fprintf(stderr, "Failed to get max RMID value\n");
        return rc;
    }
    printf("%-16s: %u\n", "Total RMID", total_rmid);

    printf("Supported monitor types:\n");
    if (libxl_psr_cmt_type_supported(ctx, LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY))
        printf("cache-occupancy\n");
    if (libxl_psr_cmt_type_supported(ctx, LIBXL_PSR_CMT_TYPE_TOTAL_MEM_COUNT))
        printf("total-mem-bandwidth\n");
    if (libxl_psr_cmt_type_supported(ctx, LIBXL_PSR_CMT_TYPE_LOCAL_MEM_COUNT))
        printf("local-mem-bandwidth\n");

    return rc;
}

#define MBM_SAMPLE_RETRY_MAX 4
static int psr_cmt_get_mem_bandwidth(uint32_t domid,
                                     libxl_psr_cmt_type type,
                                     uint32_t socketid,
                                     uint64_t *bandwidth_r)
{
    uint64_t sample1, sample2;
    uint64_t tsc1, tsc2;
    int retry_attempts = 0;
    int rc;

    while (1) {
        rc = libxl_psr_cmt_get_sample(ctx, domid, type, socketid,
                                      &sample1, &tsc1);
        if (rc < 0)
            return rc;

        usleep(10000);

        rc = libxl_psr_cmt_get_sample(ctx, domid, type, socketid,
                                      &sample2, &tsc2);
        if (rc < 0)
            return rc;

        if (tsc2 <= tsc1)
            return -1;

        /*
         * Hardware guarantees at most 1 overflow can happen if the duration
         * between two samples is less than 1 second. Note that tsc returned
         * from hypervisor is already-scaled time(ns).
         */
        if (tsc2 - tsc1 < 1000000000 && sample2 >= sample1)
            break;

        if (retry_attempts < MBM_SAMPLE_RETRY_MAX) {
            retry_attempts++;
        } else {
            fprintf(stderr, "event counter overflowed\n");
            return -1;
        }
    }

    *bandwidth_r = (sample2 - sample1) * 1000000000 / (tsc2 - tsc1) / 1024;
    return 0;
}

static void psr_cmt_print_domain_info(libxl_dominfo *dominfo,
                                      libxl_psr_cmt_type type,
                                      libxl_bitmap *socketmap)
{
    char *domain_name;
    uint32_t socketid;
    uint64_t monitor_data;

    if (!libxl_psr_cmt_domain_attached(ctx, dominfo->domid))
        return;

    domain_name = libxl_domid_to_name(ctx, dominfo->domid);
    printf("%-40s %5d", domain_name, dominfo->domid);
    free(domain_name);

    libxl_for_each_set_bit(socketid, *socketmap) {
        switch (type) {
        case LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY:
            if (!libxl_psr_cmt_get_sample(ctx, dominfo->domid, type, socketid,
                                          &monitor_data, NULL))
                printf("%13"PRIu64" KB", monitor_data / 1024);
            break;
        case LIBXL_PSR_CMT_TYPE_TOTAL_MEM_COUNT:
        case LIBXL_PSR_CMT_TYPE_LOCAL_MEM_COUNT:
            if (!psr_cmt_get_mem_bandwidth(dominfo->domid, type, socketid,
                                           &monitor_data))
                printf("%11"PRIu64" KB/s", monitor_data);
            break;
        default:
            return;
        }
    }

    printf("\n");
}

static int psr_cmt_show(libxl_psr_cmt_type type, uint32_t domid)
{
    uint32_t i, socketid, total_rmid;
    uint32_t l3_cache_size;
    libxl_bitmap socketmap;
    int rc, nr_domains;

    if (!libxl_psr_cmt_enabled(ctx)) {
        fprintf(stderr, "CMT is disabled in the system\n");
        return -1;
    }

    if (!libxl_psr_cmt_type_supported(ctx, type)) {
        fprintf(stderr, "Monitor type '%s' is not supported in the system\n",
                libxl_psr_cmt_type_to_string(type));
        return -1;
    }

    libxl_bitmap_init(&socketmap);
    libxl_socket_bitmap_alloc(ctx, &socketmap, 0);
    rc = libxl_get_online_socketmap(ctx, &socketmap);
    if (rc < 0) {
        fprintf(stderr, "Failed getting available sockets, rc: %d\n", rc);
        goto out;
    }

    rc = libxl_psr_cmt_get_total_rmid(ctx, &total_rmid);
    if (rc < 0) {
        fprintf(stderr, "Failed to get max RMID value\n");
        goto out;
    }

    printf("Total RMID: %d\n", total_rmid);

    /* Header */
    printf("%-40s %5s", "Name", "ID");
    libxl_for_each_set_bit(socketid, socketmap)
        printf("%14s %d", "Socket", socketid);
    printf("\n");

    if (type == LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY) {
            /* Total L3 cache size */
            printf("%-46s", "Total L3 Cache Size");
            libxl_for_each_set_bit(socketid, socketmap) {
                rc = libxl_psr_cmt_get_l3_cache_size(ctx, socketid,
                                                     &l3_cache_size);
                if (rc < 0) {
                    fprintf(stderr,
                            "Failed to get system l3 cache size for socket:%d\n",
                            socketid);
                    goto out;
                }
                printf("%13u KB", l3_cache_size);
            }
            printf("\n");
    }

    /* Each domain */
    if (domid != INVALID_DOMID) {
        libxl_dominfo dominfo;

        libxl_dominfo_init(&dominfo);
        if (libxl_domain_info(ctx, &dominfo, domid)) {
            fprintf(stderr, "Failed to get domain info for %d\n", domid);
            rc = -1;
            goto out;
        }
        psr_cmt_print_domain_info(&dominfo, type, &socketmap);
        libxl_dominfo_dispose(&dominfo);
    }
    else
    {
        libxl_dominfo *list;
        if (!(list = libxl_list_domain(ctx, &nr_domains))) {
            fprintf(stderr, "Failed to get domain info for domain list.\n");
            rc = -1;
            goto out;
        }
        for (i = 0; i < nr_domains; i++)
            psr_cmt_print_domain_info(list + i, type, &socketmap);
        libxl_dominfo_list_free(list, nr_domains);
    }

out:
    libxl_bitmap_dispose(&socketmap);
    return rc;
}

int main_psr_cmt_attach(int argc, char **argv)
{
    uint32_t domid;
    int opt, ret = 0;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cmt-attach", 1) {
        /* No options */
    }

    domid = find_domain(argv[optind]);
    ret = libxl_psr_cmt_attach(ctx, domid);

    return ret;
}

int main_psr_cmt_detach(int argc, char **argv)
{
    uint32_t domid;
    int opt, ret = 0;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cmt-detach", 1) {
        /* No options */
    }

    domid = find_domain(argv[optind]);
    ret = libxl_psr_cmt_detach(ctx, domid);

    return ret;
}

int main_psr_cmt_show(int argc, char **argv)
{
    int opt, ret = 0;
    uint32_t domid;
    libxl_psr_cmt_type type;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cmt-show", 1) {
        /* No options */
    }

    if (!strcmp(argv[optind], "cache-occupancy"))
        type = LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY;
    else if (!strcmp(argv[optind], "total-mem-bandwidth"))
        type = LIBXL_PSR_CMT_TYPE_TOTAL_MEM_COUNT;
    else if (!strcmp(argv[optind], "local-mem-bandwidth"))
        type = LIBXL_PSR_CMT_TYPE_LOCAL_MEM_COUNT;
    else {
        help("psr-cmt-show");
        return 2;
    }

    if (optind + 1 >= argc)
        domid = INVALID_DOMID;
    else if (optind + 1 == argc - 1)
        domid = find_domain(argv[optind + 1]);
    else {
        help("psr-cmt-show");
        return 2;
    }

    ret = psr_cmt_show(type, domid);

    return ret;
}
#endif

#ifdef LIBXL_HAVE_PSR_CAT
static int psr_cat_hwinfo(void)
{
    int rc;
    int i, nr;
    uint32_t l3_cache_size;
    libxl_psr_cat_info *info;

    printf("Cache Allocation Technology (CAT):\n");

    rc = libxl_psr_cat_get_l3_info(ctx, &info, &nr);
    if (rc) {
        fprintf(stderr, "Failed to get cat info\n");
        return rc;
    }

    for (i = 0; i < nr; i++) {
        rc = libxl_psr_cmt_get_l3_cache_size(ctx, info[i].id, &l3_cache_size);
        if (rc) {
            fprintf(stderr, "Failed to get l3 cache size for socket:%d\n",
                    info[i].id);
            goto out;
        }
        printf("%-16s: %u\n", "Socket ID", info[i].id);
        printf("%-16s: %uKB\n", "L3 Cache", l3_cache_size);
        printf("%-16s: %s\n", "CDP Status",
               info[i].cdp_enabled ? "Enabled" : "Disabled");
        printf("%-16s: %u\n", "Maximum COS", info[i].cos_max);
        printf("%-16s: %u\n", "CBM length", info[i].cbm_len);
        printf("%-16s: %#llx\n", "Default CBM",
               (1ull << info[i].cbm_len) - 1);
    }

out:
    libxl_psr_cat_info_list_free(info, nr);
    return rc;
}

static void psr_cat_print_one_domain_cbm_type(uint32_t domid, uint32_t socketid,
                                              libxl_psr_cbm_type type)
{
    uint64_t cbm;

    if (!libxl_psr_cat_get_cbm(ctx, domid, type, socketid, &cbm))
        printf("%#16"PRIx64, cbm);
    else
        printf("%16s", "error");
}

static void psr_cat_print_one_domain_cbm(uint32_t domid, uint32_t socketid,
                                         bool cdp_enabled)
{
    char *domain_name;

    domain_name = libxl_domid_to_name(ctx, domid);
    printf("%5d%25s", domid, domain_name);
    free(domain_name);

    if (!cdp_enabled) {
        psr_cat_print_one_domain_cbm_type(domid, socketid,
                                          LIBXL_PSR_CBM_TYPE_L3_CBM);
    } else {
        psr_cat_print_one_domain_cbm_type(domid, socketid,
                                          LIBXL_PSR_CBM_TYPE_L3_CBM_CODE);
        psr_cat_print_one_domain_cbm_type(domid, socketid,
                                          LIBXL_PSR_CBM_TYPE_L3_CBM_DATA);
    }

    printf("\n");
}

static int psr_cat_print_domain_cbm(uint32_t domid, uint32_t socketid,
                                    bool cdp_enabled)
{
    int i, nr_domains;
    libxl_dominfo *list;

    if (domid != INVALID_DOMID) {
        psr_cat_print_one_domain_cbm(domid, socketid, cdp_enabled);
        return 0;
    }

    if (!(list = libxl_list_domain(ctx, &nr_domains))) {
        fprintf(stderr, "Failed to get domain list for cbm display\n");
        return -1;
    }

    for (i = 0; i < nr_domains; i++)
        psr_cat_print_one_domain_cbm(list[i].domid, socketid, cdp_enabled);
    libxl_dominfo_list_free(list, nr_domains);

    return 0;
}

static int psr_cat_print_socket(uint32_t domid, libxl_psr_cat_info *info)
{
    int rc;
    uint32_t l3_cache_size;

    rc = libxl_psr_cmt_get_l3_cache_size(ctx, info->id, &l3_cache_size);
    if (rc) {
        fprintf(stderr, "Failed to get l3 cache size for socket:%d\n",
                info->id);
        return -1;
    }

    printf("%-16s: %u\n", "Socket ID", info->id);
    printf("%-16s: %uKB\n", "L3 Cache", l3_cache_size);
    printf("%-16s: %#llx\n", "Default CBM", (1ull << info->cbm_len) - 1);
    if (info->cdp_enabled)
        printf("%5s%25s%16s%16s\n", "ID", "NAME", "CBM (code)", "CBM (data)");
    else
        printf("%5s%25s%16s\n", "ID", "NAME", "CBM");

    return psr_cat_print_domain_cbm(domid, info->id, info->cdp_enabled);
}

static int psr_cat_show(uint32_t domid)
{
    int i, nr;
    int rc;
    libxl_psr_cat_info *info;

    rc = libxl_psr_cat_get_l3_info(ctx, &info, &nr);
    if (rc) {
        fprintf(stderr, "Failed to get cat info\n");
        return rc;
    }

    for (i = 0; i < nr; i++) {
        rc = psr_cat_print_socket(domid, info + i);
        if (rc)
            goto out;
    }

out:
    libxl_psr_cat_info_list_free(info, nr);
    return rc;
}

int main_psr_cat_cbm_set(int argc, char **argv)
{
    uint32_t domid;
    libxl_psr_cbm_type type;
    uint64_t cbm;
    int ret, opt = 0;
    int opt_data = 0, opt_code = 0;
    libxl_bitmap target_map;
    char *value;
    libxl_string_list socket_list;
    unsigned long start, end;
    int i, j, len;

    static struct option opts[] = {
        {"socket", 1, 0, 's'},
        {"data", 0, 0, 'd'},
        {"code", 0, 0, 'c'},
        COMMON_LONG_OPTS
    };

    libxl_socket_bitmap_alloc(ctx, &target_map, 0);
    libxl_bitmap_set_none(&target_map);

    SWITCH_FOREACH_OPT(opt, "s:cd", opts, "psr-cat-cbm-set", 2) {
    case 's':
        trim(isspace, optarg, &value);
        split_string_into_string_list(value, ",", &socket_list);
        len = libxl_string_list_length(&socket_list);
        for (i = 0; i < len; i++) {
            parse_range(socket_list[i], &start, &end);
            for (j = start; j <= end; j++)
                libxl_bitmap_set(&target_map, j);
        }

        libxl_string_list_dispose(&socket_list);
        free(value);
        break;
    case 'd':
        opt_data = 1;
        break;
    case 'c':
        opt_code = 1;
        break;
    }

    if (opt_data && opt_code) {
        fprintf(stderr, "Cannot handle -c and -d at the same time\n");
        return -1;
    } else if (opt_data) {
        type = LIBXL_PSR_CBM_TYPE_L3_CBM_DATA;
    } else if (opt_code) {
        type = LIBXL_PSR_CBM_TYPE_L3_CBM_CODE;
    } else {
        type = LIBXL_PSR_CBM_TYPE_L3_CBM;
    }

    if (libxl_bitmap_is_empty(&target_map))
        libxl_bitmap_set_any(&target_map);

    if (argc != optind + 2) {
        help("psr-cat-cbm-set");
        return 2;
    }

    domid = find_domain(argv[optind]);
    cbm = strtoll(argv[optind + 1], NULL , 0);

    ret = libxl_psr_cat_set_cbm(ctx, domid, type, &target_map, cbm);

    libxl_bitmap_dispose(&target_map);
    return ret;
}

int main_psr_cat_show(int argc, char **argv)
{
    int opt;
    uint32_t domid;

    SWITCH_FOREACH_OPT(opt, "", NULL, "psr-cat-show", 0) {
        /* No options */
    }

    if (optind >= argc)
        domid = INVALID_DOMID;
    else if (optind == argc - 1)
        domid = find_domain(argv[optind]);
    else {
        help("psr-cat-show");
        return 2;
    }

    return psr_cat_show(domid);
}

int main_psr_hwinfo(int argc, char **argv)
{
    int opt, ret = 0;
    bool all = true, cmt = false, cat = false;
    static struct option opts[] = {
        {"cmt", 0, 0, 'm'},
        {"cat", 0, 0, 'a'},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "ma", opts, "psr-hwinfo", 0) {
    case 'm':
        all = false; cmt = true;
        break;
    case 'a':
        all = false; cat = true;
        break;
    }

    if (!ret && (all || cmt))
        ret = psr_cmt_hwinfo();

    if (!ret && (all || cat))
        ret = psr_cat_hwinfo();

    return ret;
}

#endif

int main_qemu_monitor_command(int argc, char **argv)
{
    int opt;
    uint32_t domid;
    char *cmd;
    char *output;
    int ret;

    SWITCH_FOREACH_OPT(opt, "", NULL, "qemu-monitor-command", 2) {
        /* No options */
    }

    domid = find_domain(argv[optind]);
    cmd = argv[optind + 1];

    if (argc - optind > 2) {
        fprintf(stderr, "Invalid arguments.\n");
        return EXIT_FAILURE;
    }

    ret = libxl_qemu_monitor_command(ctx, domid, cmd, &output);
    if (!ret && output) {
        printf("%s\n", output);
        free(output);
    }

    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */