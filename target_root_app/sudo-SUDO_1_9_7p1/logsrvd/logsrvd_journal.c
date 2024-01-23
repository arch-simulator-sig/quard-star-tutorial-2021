/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2021 Todd C. Miller <Todd.Miller@sudo.ws>
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
#include "sudo_iolog.h"
#include "sudo_util.h"

#include "log_server.pb-c.h"
#include "logsrvd.h"

/*
 * Helper function to set closure->journal and closure->journal_path.
 */
static bool
journal_fdopen(int fd, const char *journal_path,
    struct connection_closure *closure)
{
    debug_decl(journal_fdopen, SUDO_DEBUG_UTIL);

    closure->journal_path = strdup(journal_path);
    if (closure->journal_path == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to allocate memory");
	debug_return_bool(false);
    }

    /* Defer fdopen() until last--it cannot be undone. */
    if ((closure->journal = fdopen(fd, "r+")) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to fdopen journal file %s", journal_path);
	debug_return_bool(false);
    }

    debug_return_bool(true);
}

static int
journal_mkstemp(const char *parent_dir, char *pathbuf, int pathlen)
{
    int fd, len;
    debug_decl(journal_mkstemp, SUDO_DEBUG_UTIL);

    len = snprintf(pathbuf, pathlen, "%s/%s/%s",
	logsrvd_conf_relay_dir(), parent_dir, RELAY_TEMPLATE);
    if (len >= pathlen) {
	errno = ENAMETOOLONG;
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "%s/%s/%s", logsrvd_conf_relay_dir(), parent_dir, RELAY_TEMPLATE);
	debug_return_int(-1);
    }
    if (!sudo_mkdir_parents(pathbuf, ROOT_UID, ROOT_GID,
	    S_IRWXU|S_IXGRP|S_IXOTH, false)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to create parent dir for %s", pathbuf);
	debug_return_int(-1);
    }
    if ((fd = mkstemp(pathbuf)) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to create journal file %s", pathbuf);
	debug_return_int(-1);
    }

    debug_return_int(fd);
}

/*
 * Create a temporary file in the relay dir and store it in the closure.
 */
static bool
journal_create(struct connection_closure *closure)
{
    char journal_path[PATH_MAX];
    int fd;
    debug_decl(journal_create, SUDO_DEBUG_UTIL);

    fd = journal_mkstemp("incoming", journal_path, sizeof(journal_path));
    if (fd == -1) {
	closure->errstr = _("unable to create journal file");
	debug_return_bool(false);
    }
    if (!sudo_lock_file(fd, SUDO_TLOCK)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to lock journal file %s", journal_path);
	unlink(journal_path);
	close(fd);
	closure->errstr = _("unable to lock journal file");
	debug_return_bool(false);
    }
    if (!journal_fdopen(fd, journal_path, closure)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to fdopen journal file %s", journal_path);
	unlink(journal_path);
	close(fd);
	closure->errstr = _("unable to allocate memory");
	debug_return_bool(false);
    }

    debug_return_bool(true);
}

/*
 * Flush any buffered data, rewind journal to the beginning and
 * move to the outgoing directory.
 * The actual open file is closed in connection_closure_free().
 */
static bool
journal_finish(struct connection_closure *closure)
{
    char outgoing_path[PATH_MAX];
    size_t len;
    int fd;
    debug_decl(journal_finish, SUDO_DEBUG_UTIL);

    if (fflush(closure->journal) != 0) {
	closure->errstr = _("unable to write journal file");
	debug_return_bool(false);
    }
    rewind(closure->journal);

    /* Move journal to the outgoing directory. */
    fd = journal_mkstemp("outgoing", outgoing_path, sizeof(outgoing_path));
    if (fd == -1) {
	closure->errstr = _("unable to rename journal file");
	debug_return_bool(false);
    }
    close(fd);
    if (rename(closure->journal_path, outgoing_path) == -1) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to rename %s -> %s", closure->journal_path, outgoing_path);
	closure->errstr = _("unable to rename journal file");
	unlink(outgoing_path);
	debug_return_bool(false);
    }
    sudo_debug_printf(SUDO_DEBUG_INFO|SUDO_DEBUG_LINENO,
	"renamed %s -> %s", closure->journal_path, outgoing_path);
    len = strlen(outgoing_path);
    if (strlen(closure->journal_path) == len) {
	/* This should always be true. */
	memcpy(closure->journal_path, outgoing_path, len);
    } else {
	sudo_debug_printf(SUDO_DEBUG_WARN|SUDO_DEBUG_LINENO,
	    "length mismatch %zu != %zu", strlen(closure->journal_path), len);
	free(closure->journal_path);
	closure->journal_path = strdup(outgoing_path);
	if (closure->journal_path == NULL) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to strdup new journal path %s", outgoing_path);
	    closure->errstr = _("unable to allocate memory");
	    debug_return_bool(false);
	}
    }

    debug_return_bool(true);
}

/*
 * Seek ahead in the journal to the specified target time.
 * Returns true if we reached the target time exactly, else false.
 */
static bool
journal_seek(struct timespec *target, struct connection_closure *closure)
{
    ClientMessage *msg = NULL;
    size_t nread, bufsize = 0;
    uint8_t *buf = NULL;
    uint32_t msg_len;
    bool ret = false;
    debug_decl(journal_seek, SUDO_DEBUG_UTIL);

    for (;;) {
	TimeSpec *delay = NULL;

	/* Read message size (uint32_t in network byte order). */
	nread = fread(&msg_len, sizeof(msg_len), 1, closure->journal);
	if (nread != 1) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to read message length from %s", closure->journal_path);
	    if (feof(closure->journal))
		closure->errstr = _("unexpected EOF reading journal file");
	    else
		closure->errstr = _("error reading journal file");
	    break;
	}
	msg_len = ntohl(msg_len);
	if (msg_len > MESSAGE_SIZE_MAX) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"%s: client message too large %u > %u",
		closure->journal_path, msg_len, MESSAGE_SIZE_MAX);
	    closure->errstr = _("client message too large");
	    break;
	}

	/* Read actual message now that we know the size. */
	if (msg_len != 0) {
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"%s: reading message %u bytes", closure->journal_path, msg_len);

	    if (msg_len > bufsize) {
		bufsize = sudo_pow2_roundup(msg_len);
		free(buf);
		if ((buf = malloc(bufsize)) == NULL) {
		    closure->errstr = _("unable to allocate memory");
		    break;
		}
	    }

	    nread = fread(buf, msg_len, 1, closure->journal);
	    if (nread != 1) {
		sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		    "unable to read message from %s", closure->journal_path);
		if (feof(closure->journal))
		    closure->errstr = _("unexpected EOF reading journal file");
		else
		    closure->errstr = _("error reading journal file");
		break;
	    }
	}

	client_message__free_unpacked(msg, NULL);
	msg = client_message__unpack(NULL, msg_len, buf);
	if (msg == NULL) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to unpack ClientMessage size %u", msg_len);
	    closure->errstr = _("invalid journal file, unable to restart");
	    break;
	}

	switch (msg->type_case) {
	case CLIENT_MESSAGE__TYPE_HELLO_MSG:
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"seeking past ClientHello (%d)", msg->type_case);
	    break;
	case CLIENT_MESSAGE__TYPE_ACCEPT_MSG:
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"seeking past AcceptMessage (%d)", msg->type_case);
	    break;
	case CLIENT_MESSAGE__TYPE_REJECT_MSG:
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"seeking past RejectMessage (%d)", msg->type_case);
	    break;
	case CLIENT_MESSAGE__TYPE_EXIT_MSG:
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"seeking past ExitMessage (%d)", msg->type_case);
	    break;
	case CLIENT_MESSAGE__TYPE_RESTART_MSG:
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"seeking past RestartMessage (%d)", msg->type_case);
	    break;
	case CLIENT_MESSAGE__TYPE_ALERT_MSG:
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"seeking past AlertMessage (%d)", msg->type_case);
	    break;
	case CLIENT_MESSAGE__TYPE_TTYIN_BUF:
	    delay = msg->u.ttyin_buf->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read IoBuffer (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	case CLIENT_MESSAGE__TYPE_TTYOUT_BUF:
	    delay = msg->u.ttyout_buf->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read IoBuffer (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	case CLIENT_MESSAGE__TYPE_STDIN_BUF:
	    delay = msg->u.stdin_buf->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read IoBuffer (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	case CLIENT_MESSAGE__TYPE_STDOUT_BUF:
	    delay = msg->u.stdout_buf->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read stdout_buf (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	case CLIENT_MESSAGE__TYPE_STDERR_BUF:
	    delay = msg->u.stderr_buf->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read stderr_buf (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	case CLIENT_MESSAGE__TYPE_WINSIZE_EVENT:
	    delay = msg->u.winsize_event->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read ChangeWindowSize (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	case CLIENT_MESSAGE__TYPE_SUSPEND_EVENT:
	    delay = msg->u.suspend_event->delay;
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"read CommandSuspend (%d), delay [%lld, %ld]", msg->type_case,
		(long long)delay->tv_sec, (long)delay->tv_nsec);
	    break;
	default:
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unexpected type_case value %d", msg->type_case);
	    break;
	}
	if (delay != NULL) {
	    update_elapsed_time(delay, &closure->elapsed_time);
	    sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
		"%s: elapsed time now [%lld, %ld]", closure->journal_path,
		(long long)closure->elapsed_time.tv_sec,
		closure->elapsed_time.tv_nsec);
	}

	if (sudo_timespeccmp(&closure->elapsed_time, target, >=)) {
	    if (sudo_timespeccmp(&closure->elapsed_time, target, ==)) {
		ret = true;
		break;
	    }

	    /* Mismatch between resume point and stored log. */
	    closure->errstr = _("invalid journal file, unable to restart");
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to find resume point [%lld, %ld] in %s",
		(long long)target->tv_sec, target->tv_nsec,
		closure->journal_path);
	    break;
	}
    }

    client_message__free_unpacked(msg, NULL);
    free(buf);

    debug_return_bool(ret);
}

/*
 * Restart an existing journal.
 * Seeks to the resume_point in RestartMessage before continuing.
 * Returns true if we reached the target time exactly, else false.
 */
static bool
journal_restart(RestartMessage *msg, uint8_t *buf, size_t buflen,
    struct connection_closure *closure)
{
    struct timespec target;
    int fd, len;
    char *cp, journal_path[PATH_MAX];
    debug_decl(journal_restart, SUDO_DEBUG_UTIL);

    /* Strip off leading hostname from log_id. */
    if ((cp = strchr(msg->log_id, '/')) != NULL) {
        if (cp != msg->log_id)
            cp++;
    } else {
    	cp = msg->log_id;
    }
    len = snprintf(journal_path, sizeof(journal_path), "%s/incoming/%s",
	logsrvd_conf_relay_dir(), cp);
    if (len >= ssizeof(journal_path)) {
	errno = ENAMETOOLONG;
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "%s/incoming/%s", logsrvd_conf_relay_dir(), cp);
	closure->errstr = _("unable to create journal file");
	debug_return_bool(false);
    }
    if ((fd = open(journal_path, O_RDWR)) == -1) {
        sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
            "unable to open journal file %s", journal_path);
	closure->errstr = _("unable to create journal file");
        debug_return_bool(false);
    }
    if (!journal_fdopen(fd, journal_path, closure)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to fdopen journal file %s", journal_path);
	close(fd);
	closure->errstr = _("unable to allocate memory");
	debug_return_bool(false);
    }

    /* Seek forward to resume point. */
    target.tv_sec = msg->resume_point->tv_sec;
    target.tv_nsec = msg->resume_point->tv_nsec;
    if (!journal_seek(&target, closure)) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO|SUDO_DEBUG_ERRNO,
	    "unable to seek to [%lld, %ld] in journal file %s",
	    (long long)target.tv_sec, target.tv_nsec, journal_path);
	debug_return_bool(false);
    }

    debug_return_bool(true);
}

static bool
journal_write(uint8_t *buf, size_t len, struct connection_closure *closure)
{
    uint32_t msg_len;
    debug_decl(journal_write, SUDO_DEBUG_UTIL);

    /* 32-bit message length in network byte order. */
    msg_len = htonl((uint32_t)len);
    if (fwrite(&msg_len, 1, sizeof(msg_len), closure->journal) != sizeof(msg_len)) {
	closure->errstr = _("unable to write journal file");
	debug_return_bool(false);
    }
    /* message payload */
    if (fwrite(buf, 1, len, closure->journal) != len) {
	closure->errstr = _("unable to write journal file");
	debug_return_bool(false);
    }
    debug_return_bool(true);
}

/*
 * Store an AcceptMessage from the client in the journal.
 */
static bool
journal_accept(AcceptMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_accept, SUDO_DEBUG_UTIL);

    /* Store message in a journal for later relaying. */
    if (!journal_create(closure))
	debug_return_bool(false);
    if (!journal_write(buf, len, closure))
	debug_return_bool(false);

    if (msg->expect_iobufs) {
	/* Send log ID to client for restarting connections. */
	if (!fmt_log_id_message(closure->journal_path, closure))
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
 * Store a RejectMessage from the client in the journal.
 */
static bool
journal_reject(RejectMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_reject, SUDO_DEBUG_UTIL);

    /* Store message in a journal for later relaying. */
    if (!journal_create(closure))
	debug_return_bool(false);
    if (!journal_write(buf, len, closure))
	debug_return_bool(false);

    debug_return_bool(true);
}

/*
 * Store an ExitMessage from the client in the journal.
 */
static bool
journal_exit(ExitMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_exit, SUDO_DEBUG_UTIL);

    /* Store exit message in journal. */
    if (!journal_write(buf, len, closure))
	debug_return_bool(false);
    if (!journal_finish(closure))
	debug_return_bool(false);

    debug_return_bool(true);
}

/*
 * Store an AlertMessage from the client in the journal.
 */
static bool
journal_alert(AlertMessage *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_alert, SUDO_DEBUG_UTIL);

    debug_return_bool(journal_write(buf, len, closure));
}

/*
 * Store an IoBuffer from the client in the journal.
 */
static bool
journal_iobuf(int iofd, IoBuffer *iobuf, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_iobuf, SUDO_DEBUG_UTIL);

    if (!journal_write(buf, len, closure))
	debug_return_bool(false);
    update_elapsed_time(iobuf->delay, &closure->elapsed_time);

    debug_return_bool(true);
}

/*
 * Store a CommandSuspend message from the client in the journal.
 */
static bool
journal_suspend(CommandSuspend *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_suspend, SUDO_DEBUG_UTIL);

    debug_return_bool(journal_write(buf, len, closure));
}

/*
 * Store a ChangeWindowSize message from the client in the journal.
 */
static bool
journal_winsize(ChangeWindowSize *msg, uint8_t *buf, size_t len,
    struct connection_closure *closure)
{
    debug_decl(journal_winsize, SUDO_DEBUG_UTIL);

    debug_return_bool(journal_write(buf, len, closure));
}

struct client_message_switch cms_journal = {
    journal_accept,
    journal_reject,
    journal_exit,
    journal_restart,
    journal_alert,
    journal_iobuf,
    journal_suspend,
    journal_winsize
};
