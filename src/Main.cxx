/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
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
#include "Main.hxx"
#include "Instance.hxx"
#include "CommandLine.hxx"
#include "PlaylistFile.hxx"
#include "PlaylistGlobal.hxx"
#include "UpdateGlue.hxx"
#include "MusicChunk.hxx"
#include "StateFile.hxx"
#include "PlayerThread.hxx"
#include "Mapper.hxx"
#include "DatabaseGlue.hxx"
#include "DatabaseSimple.hxx"
#include "Permission.hxx"
#include "Listen.hxx"
#include "Client.hxx"
#include "ClientList.hxx"
#include "AllCommands.hxx"
#include "Partition.hxx"
#include "Volume.hxx"
#include "OutputAll.hxx"
#include "tag.h"
#include "conf.h"
#include "replay_gain_config.h"
#include "Idle.hxx"
#include "SignalHandlers.hxx"
#include "Log.hxx"
#include "GlobalEvents.hxx"
#include "InputInit.hxx"
#include "event/Loop.hxx"
#include "IOThread.hxx"
#include "fs/Path.hxx"
#include "PlaylistRegistry.hxx"
#include "ZeroconfGlue.hxx"
#include "DecoderList.hxx"
#include "AudioConfig.hxx"

extern "C" {
#include "daemon.h"
#include "stats.h"
#include "pcm/pcm_resample.h"
}

#include "mpd_error.h"

#ifdef ENABLE_INOTIFY
#include "InotifyUpdate.hxx"
#endif

#ifdef ENABLE_SQLITE
#include "StickerDatabase.hxx"
#endif

#ifdef ENABLE_ARCHIVE
#include "ArchiveList.hxx"
#endif

#include <glib.h>

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

enum {
	DEFAULT_BUFFER_SIZE = 2048,
	DEFAULT_BUFFER_BEFORE_PLAY = 10,
};

GThread *main_task;
EventLoop *main_loop;

Instance *instance;

static StateFile *state_file;

static inline GQuark main_quark()
{
  return g_quark_from_static_string ("main");
}

static bool
glue_daemonize_init(const struct options *options, GError **error_r)
{
	GError *error = NULL;

	char *pid_file = config_dup_path(CONF_PID_FILE, &error);
	if (pid_file == NULL && error != NULL) {
		g_propagate_error(error_r, error);
		return false;
	}

	daemonize_init(config_get_string(CONF_USER, NULL),
		       config_get_string(CONF_GROUP, NULL),
		       pid_file);
	g_free(pid_file);

	if (options->kill)
		daemonize_kill();

	return true;
}

static bool
glue_mapper_init(GError **error_r)
{
	GError *error = NULL;
	char *music_dir = config_dup_path(CONF_MUSIC_DIR, &error);
	if (music_dir == NULL && error != NULL) {
		g_propagate_error(error_r, error);
		return false;
	}

	char *playlist_dir = config_dup_path(CONF_PLAYLIST_DIR, &error);
	if (playlist_dir == NULL && error != NULL) {
		g_propagate_error(error_r, error);
		return false;
	}

	if (music_dir == NULL)
		music_dir = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_MUSIC));

	if (!mapper_init(music_dir, playlist_dir, &error)) {
		g_propagate_error(error_r, error);
		return false;
	}

	g_free(music_dir);
	g_free(playlist_dir);
	return true;
}

/**
 * Returns the database.  If this function returns false, this has not
 * succeeded, and the caller should create the database after the
 * process has been daemonized.
 */
static bool
glue_db_init_and_load(void)
{
	const struct config_param *param = config_get_param(CONF_DATABASE);
	const struct config_param *path = config_get_param(CONF_DB_FILE);

	if (param != NULL && path != NULL)
		g_message("Found both 'database' and 'db_file' setting - ignoring the latter");

	GError *error = NULL;
	bool ret;

	if (!mapper_has_music_directory()) {
		if (param != NULL)
			g_message("Found database setting without "
				  "music_directory - disabling database");
		if (path != NULL)
			g_message("Found db_file setting without "
				  "music_directory - disabling database");
		return true;
	}

	struct config_param *allocated = NULL;

	if (param == NULL && path != NULL) {
		allocated = new config_param("database", path->line);
		allocated->AddBlockParam("path", path->value, path->line);
		param = allocated;
	}

	if (!DatabaseGlobalInit(param, &error))
		MPD_ERROR("%s", error->message);

	delete allocated;

	ret = DatabaseGlobalOpen(&error);
	if (!ret)
		MPD_ERROR("%s", error->message);

	/* run database update after daemonization? */
	return !db_is_simple() || db_exists();
}

/**
 * Configure and initialize the sticker subsystem.
 */
static void
glue_sticker_init(void)
{
#ifdef ENABLE_SQLITE
	GError *error = NULL;
	char *sticker_file = config_dup_path(CONF_STICKER_FILE, &error);
	if (sticker_file == NULL && error != NULL)
		MPD_ERROR("%s", error->message);

	if (!sticker_global_init(sticker_file, &error))
		MPD_ERROR("%s", error->message);

	g_free(sticker_file);
#endif
}

static bool
glue_state_file_init(GError **error_r)
{
	GError *error = NULL;

	char *path = config_dup_path(CONF_STATE_FILE, &error);
	if (path == nullptr) {
		if (error != nullptr) {
			g_propagate_error(error_r, error);
			return false;
		}

		return true;
	}

	Path path_fs = Path::FromUTF8(path);

	if (path_fs.IsNull()) {
		g_free(path);
		g_set_error(error_r, main_quark(), 0,
			    "Failed to convert state file path to FS encoding");
		return false;
	}

	state_file = new StateFile(std::move(path_fs), path,
				   *instance->partition, *main_loop);
	g_free(path);

	state_file->Read();
	return true;
}

/**
 * Windows-only initialization of the Winsock2 library.
 */
static void winsock_init(void)
{
#ifdef WIN32
	WSADATA sockinfo;
	int retval;

	retval = WSAStartup(MAKEWORD(2, 2), &sockinfo);
	if(retval != 0)
	{
		MPD_ERROR("Attempt to open Winsock2 failed; error code %d\n",
			retval);
	}

	if (LOBYTE(sockinfo.wVersion) != 2)
	{
		MPD_ERROR("We use Winsock2 but your version is either too new "
			  "or old; please install Winsock 2.x\n");
	}
#endif
}

/**
 * Initialize the decoder and player core, including the music pipe.
 */
static void
initialize_decoder_and_player(void)
{
	const struct config_param *param;
	char *test;
	size_t buffer_size;
	float perc;
	unsigned buffered_chunks;
	unsigned buffered_before_play;

	param = config_get_param(CONF_AUDIO_BUFFER_SIZE);
	if (param != NULL) {
		long tmp = strtol(param->value, &test, 10);
		if (*test != '\0' || tmp <= 0 || tmp == LONG_MAX)
			MPD_ERROR("buffer size \"%s\" is not a positive integer, "
				  "line %i\n", param->value, param->line);
		buffer_size = tmp;
	} else
		buffer_size = DEFAULT_BUFFER_SIZE;

	buffer_size *= 1024;

	buffered_chunks = buffer_size / CHUNK_SIZE;

	if (buffered_chunks >= 1 << 15)
		MPD_ERROR("buffer size \"%li\" is too big\n", (long)buffer_size);

	param = config_get_param(CONF_BUFFER_BEFORE_PLAY);
	if (param != NULL) {
		perc = strtod(param->value, &test);
		if (*test != '%' || perc < 0 || perc > 100) {
			MPD_ERROR("buffered before play \"%s\" is not a positive "
				  "percentage and less than 100 percent, line %i",
				  param->value, param->line);
		}
	} else
		perc = DEFAULT_BUFFER_BEFORE_PLAY;

	buffered_before_play = (perc / 100) * buffered_chunks;
	if (buffered_before_play > buffered_chunks)
		buffered_before_play = buffered_chunks;

	const unsigned max_length =
		config_get_positive(CONF_MAX_PLAYLIST_LENGTH,
				    DEFAULT_PLAYLIST_MAX_LENGTH);

	instance->partition = new Partition(*instance,
					    max_length,
					    buffered_chunks,
					    buffered_before_play);
}

/**
 * Handler for GlobalEvents::IDLE.
 */
static void
idle_event_emitted(void)
{
	/* send "idle" notifications to all subscribed
	   clients */
	unsigned flags = idle_get();
	if (flags != 0)
		instance->client_list->IdleAdd(flags);

	if (flags & (IDLE_PLAYLIST|IDLE_PLAYER|IDLE_MIXER|IDLE_OUTPUT))
		state_file->CheckModified();
}

/**
 * Handler for GlobalEvents::SHUTDOWN.
 */
static void
shutdown_event_emitted(void)
{
	main_loop->Break();
}

int main(int argc, char *argv[])
{
#ifdef WIN32
	return win32_main(argc, argv);
#else
	return mpd_main(argc, argv);
#endif
}

int mpd_main(int argc, char *argv[])
{
	struct options options;
	clock_t start;
	bool create_db;
	GError *error = NULL;
	bool success;

	daemonize_close_stdin();

#ifdef HAVE_LOCALE_H
	/* initialize locale */
	setlocale(LC_CTYPE,"");
#endif

	g_set_application_name("Music Player Daemon");

#if !GLIB_CHECK_VERSION(2,32,0)
	/* enable GLib's thread safety code */
	g_thread_init(NULL);
#endif

	io_thread_init();
	winsock_init();
	config_global_init();

	success = parse_cmdline(argc, argv, &options, &error);
	if (!success) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	if (!glue_daemonize_init(&options, &error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	stats_global_init();
	tag_lib_init();

	if (!log_init(options.verbose, options.log_stderr, &error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	main_task = g_thread_self();
	main_loop = new EventLoop(EventLoop::Default());

	instance = new Instance();

	const unsigned max_clients = config_get_positive(CONF_MAX_CONN, 10);
	instance->client_list = new ClientList(max_clients);

	success = listen_global_init(&error);
	if (!success) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	daemonize_set_user();

	GlobalEvents::Initialize();
	GlobalEvents::Register(GlobalEvents::IDLE, idle_event_emitted);
	GlobalEvents::Register(GlobalEvents::SHUTDOWN, shutdown_event_emitted);

	Path::GlobalInit();

	if (!glue_mapper_init(&error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	initPermissions();
	playlist_global_init();
	spl_global_init();
#ifdef ENABLE_ARCHIVE
	archive_plugin_init_all();
#endif

	if (!pcm_resample_global_init(&error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	decoder_plugin_init_all();
	update_global_init();

	create_db = !glue_db_init_and_load();

	glue_sticker_init();

	command_init();
	initialize_decoder_and_player();
	volume_init();
	initAudioConfig();
	audio_output_all_init(&instance->partition->pc);
	client_manager_init();
	replay_gain_global_init();

	if (!input_stream_global_init(&error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	playlist_list_global_init();

	daemonize(options.daemon);

	setup_log_output(options.log_stderr);

	initSigHandlers();

	if (!io_thread_start(&error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	ZeroconfInit(*main_loop);

	player_create(&instance->partition->pc);

	if (create_db) {
		/* the database failed to load: recreate the
		   database */
		unsigned job = update_enqueue(NULL, true);
		if (job == 0)
			MPD_ERROR("directory update failed");
	}

	if (!glue_state_file_init(&error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	audio_output_all_set_replay_gain_mode(replay_gain_get_real_mode(instance->partition->playlist.queue.random));

	success = config_get_bool(CONF_AUTO_UPDATE, false);
#ifdef ENABLE_INOTIFY
	if (success && mapper_has_music_directory())
		mpd_inotify_init(config_get_unsigned(CONF_AUTO_UPDATE_DEPTH,
						     G_MAXUINT));
#else
	if (success)
		g_warning("inotify: auto_update was disabled. enable during compilation phase");
#endif

	config_global_check();

	/* enable all audio outputs (if not already done by
	   playlist_state_restore() */
	instance->partition->pc.UpdateAudio();

#ifdef WIN32
	win32_app_started();
#endif

	/* run the main loop */
	main_loop->Run();

#ifdef WIN32
	win32_app_stopping();
#endif

	/* cleanup */

#ifdef ENABLE_INOTIFY
	mpd_inotify_finish();
#endif

	if (state_file != nullptr) {
		state_file->Write();
		delete state_file;
	}

	instance->partition->pc.Kill();
	ZeroconfDeinit();
	listen_global_finish();
	delete instance->client_list;

	start = clock();
	DatabaseGlobalDeinit();
	g_debug("db_finish took %f seconds",
		((float)(clock()-start))/CLOCKS_PER_SEC);

#ifdef ENABLE_SQLITE
	sticker_global_finish();
#endif

	GlobalEvents::Deinitialize();

	playlist_list_global_finish();
	input_stream_global_finish();
	audio_output_all_finish();
	volume_finish();
	mapper_finish();
	delete instance->partition;
	command_finish();
	update_global_finish();
	decoder_plugin_deinit_all();
#ifdef ENABLE_ARCHIVE
	archive_plugin_deinit_all();
#endif
	config_global_finish();
	stats_global_finish();
	io_thread_deinit();
	delete instance;
	delete main_loop;
	daemonize_finish();
#ifdef WIN32
	WSACleanup();
#endif

	log_deinit();
	return EXIT_SUCCESS;
}
