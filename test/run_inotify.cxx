/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "InotifySource.hxx"
#include "event/Loop.hxx"

#include <glib.h>

#include <sys/inotify.h>
#include <signal.h>

static EventLoop *event_loop;

static void
exit_signal_handler(G_GNUC_UNUSED int signum)
{
	event_loop->Break();
}

enum {
	IN_MASK = IN_ATTRIB|IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_DELETE_SELF
	|IN_MOVE|IN_MOVE_SELF
#ifdef IN_ONLYDIR
	|IN_ONLYDIR
#endif
};

static void
my_inotify_callback(G_GNUC_UNUSED int wd, unsigned mask,
		    const char *name, G_GNUC_UNUSED void *ctx)
{
	g_print("mask=0x%x name='%s'\n", mask, name);
}

int main(int argc, char **argv)
{
	GError *error = NULL;
	const char *path;

	if (argc != 2) {
		g_printerr("Usage: run_inotify PATH\n");
		return 1;
	}

	path = argv[1];

	event_loop = new EventLoop(EventLoop::Default());

	InotifySource *source = InotifySource::Create(*event_loop,
						      my_inotify_callback,
						      nullptr, &error);
	if (source == NULL) {
		g_warning("%s", error->message);
		g_error_free(error);
		return 2;
	}

	int descriptor = source->Add(path, IN_MASK, &error);
	if (descriptor < 0) {
		delete source;
		g_warning("%s", error->message);
		g_error_free(error);
		return 2;
	}

	struct sigaction sa;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = exit_signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	event_loop->Run();

	delete source;
	delete event_loop;
}
