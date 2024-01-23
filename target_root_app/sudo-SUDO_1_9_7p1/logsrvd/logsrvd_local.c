/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2019-2021 Todd C. Miller <Todd.Miller@sudo.ws>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif /* HAVE_STDBOOL_H */
#if defined(HAVE_STDINT_H)
# include <stdint.h>
#elif defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sudo_compat.h"
#include "sudo_conf.h"
#include "sudo_debug.h"
#include "sudo_event.h"
#include "sudo_eventlog.h"
#include "sudo_gettext.h"
#include "sudo_json.h"
#include "sudo_iolog.h"
#include "sudo_rand.h"
#include "sudo_util.h"

#include "log_server.pb-c.h"
#include "logsrvd.h"

struct logsrvd_info_closure {
    InfoMessage **info_msgs;
    size_t infolen;
};

static double random_drop;

bool
set_random_drop(const char *dropstr)
{
    char *ep;
    debug_decl(set_random_drop, SUDO_DEBUG_UTIL);

    errno = 0;
    random_drop = strtod(dropstr, &ep);
    if (*ep != '\0' || errno != 0)
	debug_return_bool(false);
    random_drop /= 100.0;	/* convert from percentage */

    debug_return_bool(true);
}

static bool
logsrvd_json_log_cb(struct json_container *json, void *v)
{
    struct logsrvd_info_closure *closure = v;
    struct json_value json_value;
    size_t idx;
    debug_decl(logsrvd_json_log_cb, SUDO_DEBUG_UTIL);

    for (idx = 0; idx < closure->infolen; idx++) {
	InfoMessage *info = closure->info_msgs[idx];

	switch (info->value_case) {
	case INFO_MESSAGE__VALUE_NUMVAL:
	    json_value.type = JSON_NUMBER;
	    json_value.u.number = info->u.numval;
	    if (!sudo_json_add_value(json, info->key, &json_value))
		goto bad;
	    break;
	case INFO_MESSAGE__VALUE_STRVAL:
	    json_value.type = JSON_STRING;
	    json_value.u.string = info->u.strval;
	    if (!sudo_json_add_value(json, info->key, &json_value))
		goto bad;
	    break;
	case INFO_MESSAGE__VALUE_STRLISTVAL: {
	    InfoMessage__StringList *strlist = info->u.strlistval;
	    size_t n;

	    if (!sudo_json_open_array(json, info->key))
		goto bad;
	    for (n = 0; n < strlist->n_strings; n++) {
		json_value.type = JSON_STRING;
		json_value.u.string = strlist->strings[n];
		if (!sudo_json_add_value(json, NULL, &json_value))
		    goto bad;
	    }
	    if (!sudo_json_close_array(json))
		goto bad;
	    break;
	}
	default:
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unexpected value case %d", info->value_case);
	    goto bad;
	}
    }
    debug_return_bool(true);
bad:
    debug_return_bool(false);
}

/*
 * Parse and store an AcceptMessage locally.
 */
bool
store_accept_local(AcceptMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    char *log_id = NULL;
    struct logsrvd_info_closure info = { msg->info_msgs, msg->n_info_msgs };
    debug_decl(store_accept_local, SUDO_DEBUG_UTIL);

    /* Store sudo-style event and I/O logs. */
    closure->evlog = evlog_new(msg->submit_time, msg->info_msgs,
	msg->n_info_msgs, closure);
    if (closure->evlog == NULL) {
	closure->errstr = _("error parsing AcceptMessage");
	debug_return_bool(false);
    }

    /* Create I/O log info file and parent directories. */
    if (msg->expect_iobufs) {
	if (!iolog_init(msg, closure)) {
	    closure->errstr = _("error creating I/O log");
	    debug_return_bool(false);
	}
	closure->log_io = true;
	log_id = closure->evlog->iolog_path;
    }

    if (!eventlog_accept(closure->evlog, 0, logsrvd_json_log_cb, &info)) {
	closure->errstr = _("error logging accept event");
	debug_return_bool(false);
    }

    if (log_id != NULL) {
	/* Send log ID to client for restarting connections. */
	if (!fmt_log_id_message(log_id, closure))
	    debug_return_bool(false);
	if (sudo_ev_add(closure->evbase, closure->write_ev,
		logsrvd_conf_server_timeout(), false) == -1) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to add server write event");
	    debug_return_bool(false);
	}
    }

    debug_return_bool(true);
}

/*
 * Parse and store a RejectMessage locally.
 */
bool
store_reject_local(RejectMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    struct logsrvd_info_closure info = { msg->info_msgs, msg->n_info_msgs };
    debug_decl(store_reject_local, SUDO_DEBUG_UTIL);

    closure->evlog = evlog_new(msg->submit_time, msg->info_msgs,
	msg->n_info_msgs, closure);
    if (closure->evlog == NULL) {
	closure->errstr = _("error parsing RejectMessage");
	debug_return_bool(false);
    }

    if (!eventlog_reject(closure->evlog, 0, msg->reason,
	    logsrvd_json_log_cb, &info)) {
	closure->errstr = _("error logging reject event");
	debug_return_bool(false);
    }

    debug_return_bool(true);
}

bool
store_exit_local(ExitMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    mode_t mode;
    debug_decl(store_exit_local, SUDO_DEBUG_UTIL);

    /* Sudo I/O logs don't store this info. */
    if (msg->signal != NULL && msg->signal[0] != '\0') {
	sudo_debug_printf(SUDO_DEBUG_INFO|SUDO_DEBUG_LINENO,
	    "command was killed by SIG%s%s", msg->signal,
	    msg->dumped_core ? " (core dumped)" : "");
    } else {
	sudo_debug_printf(SUDO_DEBUG_INFO|SUDO_DEBUG_LINENO,
	    "command exited with %d", msg->exit_value);
    }

    if (closure->log_io) {
	/* Clear write bits from I/O timing file to indicate completion. */
	mode = logsrvd_conf_iolog_mode();
	CLR(mode, S_IWUSR|S_IWGRP|S_IWOTH);
	if (fchmodat(closure->iolog_dir_fd, "timing", mode, 0) == -1) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
		"unable to fchmodat timing file");
	}
    }

    debug_return_bool(true);
}

bool
store_restart_local(RestartMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    struct timespec target;
    struct stat sb;
    int iofd;
    debug_decl(store_restart_local, SUDO_DEBUG_UTIL);

    target.tv_sec = msg->resume_point->tv_sec;
    target.tv_nsec = msg->resume_point->tv_nsec;

    /* We must allocate closure->evlog for iolog_path. */
    closure->evlog = calloc(1, sizeof(*closure->evlog));
    if (closure->evlog == NULL) {
        sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
            "calloc(1, %zu)", sizeof(*closure->evlog));
	closure->errstr = _("unable to allocate memory");
        goto bad;
    }
    closure->evlog->iolog_path = strdup(msg->log_id);
    if (closure->evlog->iolog_path == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "strdup");
	closure->errstr = _("unable to allocate memory");
	goto bad;
    }

    /* We use iolog_dir_fd in calls to openat(2) */
    closure->iolog_dir_fd =
	iolog_openat(AT_FDCWD, closure->evlog->iolog_path, O_RDONLY);
    if (closure->iolog_dir_fd == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "%s", closure->evlog->iolog_path);
	goto bad;
    }

    /* If the timing file write bit is clear, log is already complete. */
    if (fstatat(closure->iolog_dir_fd, "timing", &sb, 0) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to stat %s/timing", closure->evlog->iolog_path);
	goto bad;
    }
    if (!ISSET(sb.st_mode, S_IWUSR)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "%s already complete", closure->evlog->iolog_path);
	closure->errstr = _("log is already complete, cannot be restarted");
	goto bad;
    }

    /* Open existing I/O log files. */
    if (!iolog_open_all(closure->iolog_dir_fd, closure->evlog->iolog_path,
	    closure->iolog_files, "r+"))
	goto bad;

    /* Compressed logs don't support random access, so rewrite them. */
    for (iofd = 0; iofd < IOFD_MAX; iofd++) {
	if (closure->iolog_files[iofd].compressed)
	    debug_return_bool(iolog_rewrite(&target, closure));
    }

    /* Parse timing file until we reach the target point. */
    if (!iolog_seekto(closure->iolog_dir_fd, closure->evlog->iolog_path,
	    closure->iolog_files, &closure->elapsed_time, &target))
	goto bad;

    /* Must seek or flush before switching from read -> write. */
    if (iolog_seek(&closure->iolog_files[IOFD_TIMING], 0, SEEK_CUR) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "lseek(IOFD_TIMING, 0, SEEK_CUR)");
	goto bad;
    }

    /* Ready to log I/O buffers. */
    debug_return_bool(true);
bad:
    if (closure->errstr == NULL)
	closure->errstr = _("unable to restart log");
    debug_return_bool(false);
}

bool
store_alert_local(AlertMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    struct timespec alert_time;
    debug_decl(store_alert_local, SUDO_DEBUG_UTIL);

    if (msg->info_msgs != NULL && msg->n_info_msgs != 0) {
	closure->evlog = evlog_new(NULL, msg->info_msgs,
	    msg->n_info_msgs, closure);
	if (closure->evlog == NULL) {
	    closure->errstr = _("error parsing AlertMessage");
	    debug_return_bool(false);
	}
    }

    alert_time.tv_sec = msg->alert_time->tv_sec;
    alert_time.tv_nsec = msg->alert_time->tv_nsec;
    if (!eventlog_alert(closure->evlog, 0, &alert_time, msg->reason, NULL)) {
	closure->errstr = _("error logging alert event");
	debug_return_bool(false);
    }

    debug_return_bool(true);
}

bool
store_iobuf_local(int iofd, IoBuffer *iobuf, uint8_t *buf, size_t buflen,
    struct connection_closure *closure)
{
    const struct eventlog *evlog = closure->evlog;
    const char *errstr;
    char tbuf[1024];
    int len;
    debug_decl(store_iobuf_local, SUDO_DEBUG_UTIL);

    /* Open log file as needed. */
    if (!closure->iolog_files[iofd].enabled) {
	if (!iolog_create(iofd, closure))
	    goto bad;
    }

    /* Format timing data. */
    /* FIXME - assumes IOFD_* matches IO_EVENT_* */
    len = snprintf(tbuf, sizeof(tbuf), "%d %lld.%09d %zu\n",
	iofd, (long long)iobuf->delay->tv_sec, (int)iobuf->delay->tv_nsec,
	iobuf->data.len);
    if (len < 0 || len >= ssizeof(tbuf)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to format timing buffer, len %d", len);
	goto bad;
    }

    /* Write to specified I/O log file. */
    if (!iolog_write(&closure->iolog_files[iofd], iobuf->data.data,
	    iobuf->data.len, &errstr)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to write to %s/%s: %s", evlog->iolog_path,
	    iolog_fd_to_name(iofd), errstr);
	goto bad;
    }

    /* Write timing data. */
    if (!iolog_write(&closure->iolog_files[IOFD_TIMING], tbuf,
	    len, &errstr)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to write to %s/%s: %s", evlog->iolog_path,
	    iolog_fd_to_name(IOFD_TIMING), errstr);
	goto bad;
    }

    update_elapsed_time(iobuf->delay, &closure->elapsed_time);

    /* Random drop is a debugging tool to test client restart. */
    if (random_drop > 0.0) {
	double randval = arc4random() / (double)UINT32_MAX;
	if (randval < random_drop) {
	    sudo_debug_printf(SUDO_DEBUG_WARN|SUDO_DEBUG_LINENO,
		"randomly dropping connection (%f < %f)", randval, random_drop);
	    debug_return_bool(false);
	}
    }

    debug_return_bool(true);
bad:
    if (closure->errstr == NULL)
	closure->errstr = _("error writing IoBuffer");
    debug_return_bool(false);
}

bool
store_winsize_local(ChangeWindowSize *msg, uint8_t *buf, size_t buflen,
    struct connection_closure *closure)
{
    const char *errstr;
    char tbuf[1024];
    int len;
    debug_decl(store_winsize_local, SUDO_DEBUG_UTIL);

    /* Format timing data including new window size. */
    len = snprintf(tbuf, sizeof(tbuf), "%d %lld.%09d %d %d\n", IO_EVENT_WINSIZE,
	(long long)msg->delay->tv_sec, (int)msg->delay->tv_nsec,
	msg->rows, msg->cols);
    if (len < 0 || len >= ssizeof(tbuf)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to format timing buffer, len %d", len);
	goto bad;
    }

    /* Write timing data. */
    if (!iolog_write(&closure->iolog_files[IOFD_TIMING], tbuf,
	    len, &errstr)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to write to %s/%s: %s", closure->evlog->iolog_path,
	    iolog_fd_to_name(IOFD_TIMING), errstr);
	goto bad;
    }

    update_elapsed_time(msg->delay, &closure->elapsed_time);

    debug_return_bool(true);
bad:
    if (closure->errstr == NULL)
	closure->errstr = _("error writing ChangeWindowSize");
    debug_return_bool(false);
}

bool
store_suspend_local(CommandSuspend *msg, uint8_t *buf, size_t buflen,
    struct connection_closure *closure)
{
    const char *errstr;
    char tbuf[1024];
    int len;
    debug_decl(store_suspend_local, SUDO_DEBUG_UTIL);

    /* Format timing data including suspend signal. */
    len = snprintf(tbuf, sizeof(tbuf), "%d %lld.%09d %s\n", IO_EVENT_SUSPEND,
	(long long)msg->delay->tv_sec, (int)msg->delay->tv_nsec,
	msg->signal);
    if (len < 0 || len >= ssizeof(tbuf)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to format timing buffer, len %d, signal %s",
	    len, msg->signal);
	goto bad;
    }

    /* Write timing data. */
    if (!iolog_write(&closure->iolog_files[IOFD_TIMING], tbuf,
	    len, &errstr)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to write to %s/%s: %s", closure->evlog->iolog_path,
	    iolog_fd_to_name(IOFD_TIMING), errstr);
	goto bad;
    }

    update_elapsed_time(msg->delay, &closure->elapsed_time);

    debug_return_bool(true);
bad:
    if (closure->errstr == NULL)
	closure->errstr = _("error writing CommandSuspend");
    debug_return_bool(false);
}

struct client_message_switch cms_local = {
    store_accept_local,
    store_reject_local,
    store_exit_local,
    store_restart_local,
    store_alert_local,
    store_iobuf_local,
    store_suspend_local,
    store_winsize_local
};
