/*
 * Copyright (C) 2003-2009 The Music Player Daemon Project
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

#include "../output/RaopOutputPlugin.hxx"
#include "OutputPlugin.hxx"
#include "MixerInternal.hxx"

struct RaopMixer final : public Mixer {
	/** the base mixer class */
	RaopOutput *self;

	RaopMixer(RaopOutput *_output)
		:Mixer(raop_mixer_plugin),
		self(_output) {}
};


static Mixer *
raop_mixer_init(void *ao, G_GNUC_UNUSED const struct config_param *param,
		 G_GNUC_UNUSED GError **error_r)
{
	return new RaopMixer(RoarOutput * ao);
}

static void
raop_mixer_finish(Mixer *data)
{
	RaopMixer *self = (RaopMixer *) data;

	delete self;
}

static int
roar_mixer_get_volume(Mixer *mixer, gcc_unused GError **error_r)
{
	RaopMixer *self = (RaopMixer *)mixer;
	return raop_output_get_volume(self->self);
}

static bool
raop_mixer_set_volume(Mixer *mixer, unsigned volume, GError **error_r)
{
	RaopMixer *self = (RaopMixer *)mixer;
	return raop_set_volume(self->self, volume, error_r);
}

const struct mixer_plugin raop_mixer_plugin = {
	.init = raop_mixer_init,
	.finish = raop_mixer_finish,
	.get_volume = raop_mixer_get_volume,
	.set_volume = raop_mixer_set_volume,
};
