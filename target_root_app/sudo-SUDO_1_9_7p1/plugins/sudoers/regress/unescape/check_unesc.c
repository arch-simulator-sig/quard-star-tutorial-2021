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

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUDO_ERROR_WRAP 0

#include "sudoers.h"

struct test_data {
    char *input;
    char *result;
    size_t result_len;
    size_t bufsize;
} test_data[] = {
    { "\\\0ABC", "\\", 1, 2 },			/* 1 */
    { "\\ \\;", "\\ ;", 3, 4 },			/* 2 */
    { "\\\t\\;", "\\\t;", 3, 4 },		/* 3 */
    { "\\foo", "foo", 3, 4 },			/* 4 */
    { "foo\\ bar", "foo\\ bar", 8, 9 },		/* 5 */
    { "foo bar", "f", 7, 2 },			/* 6 */
    { "foo bar", "", 7, 1 },			/* 7 */
    { "foo bar", NULL, 7, 0 },			/* 8 */
    { NULL }
};

sudo_dso_public int main(int argc, char *argv[]);

static void
test_strlcpy_unescape(int *ntests_out, int *errors_out)
{
    int ntests = *ntests_out;
    int errors = *errors_out;
    struct test_data *td;
    char buf[1024];
    size_t len;

    for (td = test_data; td->input != NULL; td++) {
	ntests++;
	memset(buf, 'A', sizeof(buf));
	len = strlcpy_unescape(buf, td->input, td->bufsize);
	if (len != td->result_len) {
	    sudo_warnx("%d: \"%s\": bad return %zu, expected %zu",
		ntests, td->input, len, td->result_len);
	    errors++;
	}
	len = td->result ? strlen(td->result) : 0;
	if ((len != 0 || td->bufsize != 0) && len >= td->bufsize) {
	    sudo_warnx("%d: \"%s\": bad length %zu >= %zu",
		ntests, td->input, len, td->bufsize);
	    errors++;
	}
	if (td->result != NULL && strcmp(td->result, buf) != 0) {
	    sudo_warnx("%d: \"%s\": got \"%s\", expected \"%s\"",
		ntests, td->input, buf, td->result);
	    errors++;
	}
	if (buf[td->bufsize] != 'A') {
	    sudo_warnx("%d: \"%s\": wrote past end of buffer at %zu (0x%x)",
		ntests, td->input, td->bufsize, buf[td->bufsize]);
	    errors++;
	}
    }

    *ntests_out = ntests;
    *errors_out = errors;
}

static void
test_strvec_join(char sep, int *ntests_out, int *errors_out)
{
    int ntests = *ntests_out;
    int errors = *errors_out;
    char buf[64*1024 + 1], expected[64*1024 + 3];
    char *argv[3], *result;

    /* Test joining an argument vector while unescaping. */
    /* Simulate: sudoedit -s '\' `perl -e 'print "A" x 65536'` */
    memset(buf, 'A', sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    argv[0] = "\\";
    argv[1] = buf;
    argv[2] = NULL;

    memset(expected, 'A', sizeof(expected));
    expected[0] = '\\';
    expected[1] = sep;
    expected[sizeof(expected) - 1] = '\0';

    ntests++;
    result = strvec_join(argv, sep, strlcpy_unescape);
    if (result == NULL) {
	sudo_warnx("%d: failed to join argument vector", ntests);
	errors++;
    } else if (strcmp(result, expected) != 0) {
	sudo_warnx("%d: got \"%s\", expected \"%s\"", ntests,
	    result, expected);
	errors++;
    }
    free(result);

    *ntests_out = ntests;
    *errors_out = errors;
}

int
main(int argc, char *argv[])
{
    int ntests = 0, errors = 0;

    initprogname(argc > 0 ? argv[0] : "check_unesc");

    /* strlcpy_unescape tests */
    test_strlcpy_unescape(&ntests, &errors);

    /* strvec_join test */
    test_strvec_join(' ', &ntests, &errors);
    test_strvec_join('\n', &ntests, &errors);

    if (ntests != 0) {
	printf("%s: %d tests run, %d errors, %d%% success rate\n",
	    getprogname(), ntests, errors, (ntests - errors) * 100 / ntests);
    }

    exit(errors);
}
