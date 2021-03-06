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
#include "CueParser.hxx"
#include "util/StringUtil.hxx"
#include "song.h"
#include "tag.h"

#include <glib.h>

#include <assert.h>
#include <stdlib.h>

CueParser::CueParser()
	:state(HEADER), tag(tag_new()),
	 filename(nullptr),
	 current(nullptr),
	 previous(nullptr),
	 finished(nullptr),
	 end(false) {}

CueParser::~CueParser()
{
	tag_free(tag);
	g_free(filename);

	if (current != nullptr)
		song_free(current);

	if (previous != nullptr)
		song_free(previous);

	if (finished != nullptr)
		song_free(finished);
}

static const char *
cue_next_word(char *p, char **pp)
{
	assert(p >= *pp);
	assert(!g_ascii_isspace(*p));

	const char *word = p;
	while (*p != 0 && !g_ascii_isspace(*p))
		++p;

	*p = 0;
	*pp = p + 1;
	return word;
}

static const char *
cue_next_quoted(char *p, char **pp)
{
	assert(p >= *pp);
	assert(p[-1] == '"');

	char *end = strchr(p, '"');
	if (end == nullptr) {
		/* syntax error - ignore it silently */
		*pp = p + strlen(p);
		return p;
	}

	*end = 0;
	*pp = end + 1;

	return p;
}

static const char *
cue_next_token(char **pp)
{
	char *p = strchug_fast(*pp);
	if (*p == 0)
		return nullptr;

	return cue_next_word(p, pp);
}

static const char *
cue_next_value(char **pp)
{
	char *p = strchug_fast(*pp);
	if (*p == 0)
		return nullptr;

	if (*p == '"')
		return cue_next_quoted(p + 1, pp);
	else
		return cue_next_word(p, pp);
}

static void
cue_add_tag(struct tag *tag, enum tag_type type, char *p)
{
	const char *value = cue_next_value(&p);
	if (value != nullptr)
		tag_add_item(tag, type, value);

}

static void
cue_parse_rem(char *p, struct tag *tag)
{
	const char *type = cue_next_token(&p);
	if (type == nullptr)
		return;

	enum tag_type type2 = tag_name_parse_i(type);
	if (type2 != TAG_NUM_OF_ITEM_TYPES)
		cue_add_tag(tag, type2, p);
}

struct tag *
CueParser::GetCurrentTag()
{
	if (state == HEADER)
		return tag;
	else if (state == TRACK)
		return current->tag;
	else
		return nullptr;
}

static int
cue_parse_position(const char *p)
{
	char *endptr;
	unsigned long minutes = strtoul(p, &endptr, 10);
	if (endptr == p || *endptr != ':')
		return -1;

	p = endptr + 1;
	unsigned long seconds = strtoul(p, &endptr, 10);
	if (endptr == p || *endptr != ':')
		return -1;

	p = endptr + 1;
	unsigned long frames = strtoul(p, &endptr, 10);
	if (endptr == p || *endptr != 0)
		return -1;

	return minutes * 60000 + seconds * 1000 + frames * 1000 / 75;
}

void
CueParser::Commit()
{
	/* the caller of this library must call cue_parser_get() often
	   enough */
	assert(finished == nullptr);
	assert(!end);

	if (current == nullptr)
		return;

	finished = previous;
	previous = current;
	current = nullptr;
}

void
CueParser::Feed2(char *p)
{
	assert(!end);
	assert(p != nullptr);

	const char *command = cue_next_token(&p);
	if (command == nullptr)
		return;

	if (strcmp(command, "REM") == 0) {
		struct tag *current_tag = GetCurrentTag();
		if (current_tag != nullptr)
			cue_parse_rem(p, current_tag);
	} else if (strcmp(command, "PERFORMER") == 0) {
		/* MPD knows a "performer" tag, but it is not a good
		   match for this CUE tag; from the Hydrogenaudio
		   Knowledgebase: "At top-level this will specify the
		   CD artist, while at track-level it specifies the
		   track artist." */

		enum tag_type type = state == TRACK
			? TAG_ARTIST
			: TAG_ALBUM_ARTIST;

		struct tag *current_tag = GetCurrentTag();
		if (current_tag != nullptr)
			cue_add_tag(current_tag, type, p);
	} else if (strcmp(command, "TITLE") == 0) {
		if (state == HEADER)
			cue_add_tag(tag, TAG_ALBUM, p);
		else if (state == TRACK)
			cue_add_tag(current->tag, TAG_TITLE, p);
	} else if (strcmp(command, "FILE") == 0) {
		Commit();

		const char *new_filename = cue_next_value(&p);
		if (new_filename == nullptr)
			return;

		const char *type = cue_next_token(&p);
		if (type == nullptr)
			return;

		if (strcmp(type, "WAVE") != 0 &&
		    strcmp(type, "MP3") != 0 &&
		    strcmp(type, "AIFF") != 0) {
			state = IGNORE_FILE;
			return;
		}

		state = WAVE;
		g_free(filename);
		filename = g_strdup(new_filename);
	} else if (state == IGNORE_FILE) {
		return;
	} else if (strcmp(command, "TRACK") == 0) {
		Commit();

		const char *nr = cue_next_token(&p);
		if (nr == nullptr)
			return;

		const char *type = cue_next_token(&p);
		if (type == nullptr)
			return;

		if (strcmp(type, "AUDIO") != 0) {
			state = IGNORE_TRACK;
			return;
		}

		state = TRACK;
		current = song_remote_new(filename);
		assert(current->tag == nullptr);
		current->tag = tag_dup(tag);
		tag_add_item(current->tag, TAG_TRACK, nr);
		last_updated = false;
	} else if (state == IGNORE_TRACK) {
		return;
	} else if (state == TRACK && strcmp(command, "INDEX") == 0) {
		const char *nr = cue_next_token(&p);
		if (nr == nullptr)
			return;

		const char *position = cue_next_token(&p);
		if (position == nullptr)
			return;

		int position_ms = cue_parse_position(position);
		if (position_ms < 0)
			return;

		if (!last_updated && previous != nullptr &&
		    previous->start_ms < (unsigned)position_ms) {
			last_updated = true;
			previous->end_ms = position_ms;
			previous->tag->time =
				(previous->end_ms - previous->start_ms + 500) / 1000;
		}

		current->start_ms = position_ms;
	}
}

void
CueParser::Feed(const char *line)
{
	assert(!end);
	assert(line != nullptr);

	char *allocated = g_strdup(line);
	Feed2(allocated);
	g_free(allocated);
}

void
CueParser::Finish()
{
	if (end)
		/* has already been called, ignore */
		return;

	Commit();
	end = true;
}

struct song *
CueParser::Get()
{
	if (finished == nullptr && end) {
		/* cue_parser_finish() has been called already:
		   deliver all remaining (partial) results */
		assert(current == nullptr);

		finished = previous;
		previous = nullptr;
	}

	struct song *song = finished;
	finished = nullptr;
	return song;
}
