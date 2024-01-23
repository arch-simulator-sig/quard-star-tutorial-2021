/*
 * SPDX-License-Identifier: ISC
 *
 * Copyright (c) 2009-2021 Todd C. Miller <Todd.Miller@sudo.ws>
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

/*
 * This is an open source non-commercial project. Dear PVS-Studio, please check it.
 * PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif
#include <time.h>
#include <errno.h>

#include "sudo_compat.h"
#include "sudo_debug.h"
#include "sudo_fatal.h"
#include "sudo_gettext.h"
#include "sudo_util.h"
#include "sudo_iolog.h"

/*
 * Create temporary directory and any parent directories as needed.
 */
bool
iolog_mkdtemp(char *path)
{
    const mode_t iolog_dirmode = iolog_get_dir_mode();
    const uid_t iolog_uid = iolog_get_uid();
    const gid_t iolog_gid = iolog_get_gid();
    bool ok, uid_changed = false;
    debug_decl(iolog_mkdtemp, SUDO_DEBUG_UTIL);

    ok = sudo_mkdir_parents(path, iolog_uid, iolog_gid, iolog_dirmode, true);
    if (!ok && errno == EACCES) {
	/* Try again as the I/O log owner (for NFS). */
	uid_changed = iolog_swapids(false);
	if (uid_changed)
	    ok = sudo_mkdir_parents(path, -1, -1, iolog_dirmode, false);
    }
    if (ok) {
	/* Create final path component. */
	sudo_debug_printf(SUDO_DEBUG_DEBUG|SUDO_DEBUG_LINENO,
	    "mkdtemp %s", path);
	/* We cannot retry mkdtemp() so always open as iolog user */
	if (!uid_changed)
	    uid_changed = iolog_swapids(false);
	if (mkdtemp(path) == NULL) {
	    sudo_warn(U_("unable to mkdir %s"), path);
	    ok = false;
	} else {
	    if (chmod(path, iolog_dirmode) != 0) {
		sudo_warn(U_("unable to change mode of %s to 0%o"),
		    path, (unsigned int)iolog_dirmode);
	    }
	}
    }

    if (uid_changed) {
	if (!iolog_swapids(true))
	    ok = false;
    }
    debug_return_bool(ok);
}
