/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2007 Linpro AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 * Author: Dag-Erling Smørgrav <des@linpro.no>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 *
 * Log tailer for Varnish
 */

#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifdef HAVE_SYS_QUEUE_H
#include <sys/queue.h>
#else
#include "queue.h"
#endif

#include "vsb.h"

#include "libvarnish.h"
#include "shmlog.h"
#include "varnishapi.h"

struct top {
	unsigned char		rec[4 + 255];
	unsigned		clen;
	unsigned		hash;
	TAILQ_ENTRY(top)	list;
	double			count;
};

static TAILQ_HEAD(tophead, top) top_head = TAILQ_HEAD_INITIALIZER(top_head);

static unsigned ntop;

/*--------------------------------------------------------------------*/

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static int f_flag = 0;

static void
accumulate(const unsigned char *p)
{
	struct top *tp, *tp2;
	const unsigned char *q;
	unsigned int u;
	int i;

	// fprintf(stderr, "%*.*s\n", p[1], p[1], p + 4);

	u = 0;
	q = p + 4;
	for (i = 0; i < p[1]; i++, q++) {
		if (f_flag && (*q == ':' || isspace(*q)))
			break;
		u += *q;
	}

	TAILQ_FOREACH(tp, &top_head, list) {
		if (tp->hash != u)
			continue;
		if (tp->rec[0] != p[0])
			continue;
		if (tp->clen != q - p)
			continue;
		if (memcmp(p + 4, tp->rec + 4, q - (p + 4)))
			continue;
		tp->count += 1.0;
		break;
	}
	if (tp == NULL) {
		ntop++;
		tp = calloc(sizeof *tp, 1);
		assert(tp != NULL);
		tp->hash = u;
		tp->count = 1.0;
		tp->clen = q - p;
		TAILQ_INSERT_TAIL(&top_head, tp, list);
	}
	memcpy(tp->rec, p, 4 + p[1]);
	while (1) {
		tp2 = TAILQ_PREV(tp, tophead, list);
		if (tp2 == NULL || tp2->count >= tp->count)
			break;
		TAILQ_REMOVE(&top_head, tp2, list);
		TAILQ_INSERT_AFTER(&top_head, tp, tp2, list);
	}
	while (1) {
		tp2 = TAILQ_NEXT(tp, list);
		if (tp2 == NULL || tp2->count <= tp->count)
			break;
		TAILQ_REMOVE(&top_head, tp2, list);
		TAILQ_INSERT_BEFORE(tp, tp2, list);
	}
}

static void
update(void)
{
	struct top *tp, *tp2;
	int l;
	double t = 0;
	static time_t last;
	time_t now;

	now = time(NULL);
	if (now == last)
		return;
	last = now;

	erase();
	l = 1;
	mvprintw(0, 0, "%*s", COLS - 1, VSL_Name());
	mvprintw(0, 0, "list length %u", ntop);
	TAILQ_FOREACH_SAFE(tp, &top_head, list, tp2) {
		if (++l < LINES) {
			int len = tp->rec[1];
			if (len > COLS - 20)
				len = COLS - 20;
			mvprintw(l, 0, "%9.2f %-9.9s %*.*s\n",
			    tp->count, VSL_tags[tp->rec[0]],
			    len, len, tp->rec + 4);
			t = tp->count;
		}
		tp->count *= .999;
		if (tp->count * 10 < t || l > LINES * 10) {
			TAILQ_REMOVE(&top_head, tp, list);
			free(tp);
			ntop--;
		}
	}
	refresh();
}

static void *
accumulate_thread(void *arg)
{
	struct VSL_data *vd = arg;

	for (;;) {
		unsigned char *p;
		int i;

		i = VSL_NextLog(vd, &p);
		if (i < 0)
			break;
		if (i == 0) {
			usleep(50000);
			continue;
		}

		pthread_mutex_lock(&mtx);
		accumulate(p);
		pthread_mutex_unlock(&mtx);
	}
	return (arg);
}

static void
do_curses(struct VSL_data *vd)
{
	pthread_t thr;
	int ch;

	if (pthread_create(&thr, NULL, accumulate_thread, vd) != 0) {
		fprintf(stderr, "pthread_create(): %s\n", strerror(errno));
		exit(1);
	}

	initscr();
	raw();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	curs_set(0);
	erase();
	for (;;) {
		pthread_mutex_lock(&mtx);
		update();
		pthread_mutex_unlock(&mtx);

		timeout(1000);
		switch ((ch = getch())) {
		case ERR:
			break;
		case KEY_RESIZE:
			erase();
			break;
		case '\014': /* Ctrl-L */
		case '\024': /* Ctrl-T */
			redrawwin(stdscr);
			refresh();
			break;
		case '\003': /* Ctrl-C */
			raise(SIGINT);
			break;
		case '\032': /* Ctrl-Z */
			endwin();
			raise(SIGTSTP);
			break;
		case '\021': /* Ctrl-Q */
		case 'Q':
		case 'q':
			endwin();
			return;
		default:
			beep();
			break;
		}
	}
}

static void
dump(void)
{
	struct top *tp, *tp2;
	int len;

	TAILQ_FOREACH_SAFE(tp, &top_head, list, tp2) {
		if (tp->count <= 1.0)
			break;
		len = tp->rec[1];
		printf("%9.2f %*.*s\n", tp->count, len, len, tp->rec + 4);
	}
}

static void
do_once(struct VSL_data *vd)
{
	unsigned char *p;

	while (VSL_NextLog(vd, &p) > 0)
		accumulate(p);
	dump();
}

static void
usage(void)
{
	fprintf(stderr, "usage: varnishtop %s [-1fV] [-n varnish_name]\n", VSL_USAGE);
	exit(1);
}

int
main(int argc, char **argv)
{
	struct VSL_data *vd;
	const char *n_arg = NULL;
	int o, once = 0;

	vd = VSL_New();

	while ((o = getopt(argc, argv, VSL_ARGS "1fn:V")) != -1) {
		switch (o) {
		case '1':
			VSL_Arg(vd, 'd', NULL);
			once = 1;
			break;
		case 'n':
			n_arg = optarg;
			break;
		case 'f':
			f_flag = 1;
			break;
		case 'V':
			varnish_version("varnishtop");
			exit(0);
		default:
			if (VSL_Arg(vd, o, optarg) > 0)
				break;
			usage();
		}
	}

	if (VSL_OpenLog(vd, n_arg))
		exit (1);

	if (once) {
		VSL_NonBlocking(vd, 1);
		do_once(vd);
	} else {
		do_curses(vd);
	}
	exit(0);
}
