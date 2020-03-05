/* output.c - Output module frontend
 *
 * Copyright (C) 2007 Ivo Clarysse,  (C) 2012 Henner Zeller
 *
 * This file is part of GMediaRender.
 *
 * GMediaRender is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GMediaRender is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GMediaRender; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>

#include <glib.h>

#include "logging.h"
#include "output_module.h"
#ifdef HAVE_GST
#include "output_gstreamer.h"
#endif
#include "output.h"

static struct output_module *modules[] = {
#ifdef HAVE_GST
	&gstreamer_output,
#else
	// this will be a runtime error, but there is not much point
	// in waiting till then.
#error "No output configured. You need to ./configure --with-gstreamer"
#endif
};

/** Additions: ALSA volume control **/

#include <alsa/asoundlib.h>

snd_mixer_elem_t* 	mixer_elem;
long			mixer_max;
snd_mixer_t 		*mixer_handle;
snd_mixer_selem_id_t 	*mixer_sid;

int init_alsa(const char *mixer_name) {
	mixer_max = 0;
    	long min;
    	const char *card = "default";

	if (! mixer_name) {
		Log_info("alsa", "No mixer control defined, won't enable ALSA mixer control");
		return -1;
	}

    	snd_mixer_open(&mixer_handle, 0);

	if (! mixer_handle) {
		Log_error("alsa", "Can't open mixer");
                return -1;
	}

    	snd_mixer_attach(mixer_handle, card);
    	snd_mixer_selem_register(mixer_handle, NULL, NULL);
    	snd_mixer_load(mixer_handle);

    	snd_mixer_selem_id_alloca(&mixer_sid);

	if (! mixer_sid) {
                Log_error("alsa", "Can't allocate mixer_sid");
                return -1;
        }

    	snd_mixer_selem_id_set_index(mixer_sid, 0);
    	snd_mixer_selem_id_set_name(mixer_sid, mixer_name);
    	mixer_elem = snd_mixer_find_selem(mixer_handle, mixer_sid);

	if (! mixer_elem) {
                Log_error("alsa", "Mixer control %s does not exist", mixer_name);
                return -1;
        }

	Log_info("alsa", "Using alsa mixer control %s", mixer_name);

	snd_mixer_selem_get_playback_volume_range(mixer_elem, &min, &mixer_max);

	return 0;
}

void close_alsa(void) {
	snd_mixer_close(mixer_handle);
}

int set_alsa_volume(float value) {
	if (mixer_max <= 0)
		return 0;

	/* Convert Volume to a 0-100% setting based on 60dB volume range */
	float db = 20*log10(value);
	long volume = (db+60)*mixer_max/60;

	if (!(snd_mixer_selem_set_playback_volume_all(
		mixer_elem, volume)))
		return 0;
	else
		return 1;
}

float get_alsa_volume(void) {
	if (mixer_max <= 0)
                return 0;
	
	long value;
	snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_MONO, &value);

	/* Convert 0-max back to multiplier with a 60db range) */
	float db = value*60/mixer_max-60;

	Log_error("alsa", "value %ld, db %f", value, db);
	if (db <= -60) {
		return 0;
	}
	return pow(10,db/20);
}

/************************************/

static struct output_module *output_module = NULL;

void output_dump_modules(void)
{
	int count;

	count = sizeof(modules) / sizeof(struct output_module *);
	if (count == 0) {
		puts("  NONE!");
	} else {
		int i;
		for (i=0; i<count; i++) {
			printf("Available output: %s\t%s%s\n",
			       modules[i]->shortname,
			       modules[i]->description,
			       (i==0) ? " (default)" : "");
		}
	}
}

int output_init(const char *shortname, const char *alsa_mixer)
{
	int count;

	init_alsa(alsa_mixer);

	count = sizeof(modules) / sizeof(struct output_module *);
	if (count == 0) {
		Log_error("output", "No output module available");
		return -1;
	}
	if (shortname == NULL) {
		output_module = modules[0];
	} else {
		int i;
		for (i=0; i<count; i++) {
			if (strcmp(modules[i]->shortname, shortname)==0) {
				output_module = modules[i];
				break;
			}
		}
	}

	if (output_module == NULL) {
		Log_error("error", "ERROR: No such output module: '%s'",
			  shortname);
		return -1;
	}

	Log_info("output", "Using output module: %s (%s)",
		 output_module->shortname, output_module->description);

	if (output_module->init) {
		return output_module->init();
	}

	return 0;
}

static GMainLoop *main_loop_ = NULL;
static void exit_loop_sighandler(int sig) {
	if (main_loop_) {
		// TODO(hzeller): revisit - this is not safe to do.
		g_main_loop_quit(main_loop_);
	}
}

int output_loop()
{
        /* Create a main loop that runs the default GLib main context */
        main_loop_ = g_main_loop_new(NULL, FALSE);

	signal(SIGINT, &exit_loop_sighandler);
	signal(SIGTERM, &exit_loop_sighandler);

        g_main_loop_run(main_loop_);

        return 0;
}

int output_add_options(GOptionContext *ctx)
{
  	int count, i;

	count = sizeof(modules) / sizeof(struct output_module *);
	for (i = 0; i < count; ++i) {
		if (modules[i]->add_options) {
			int result = modules[i]->add_options(ctx);
			if (result != 0) {
				return result;
			}
		}
	}

	return 0;
}

void output_set_uri(const char *uri, output_update_meta_cb_t meta_cb) {
	if (output_module && output_module->set_uri) {
		output_module->set_uri(uri, meta_cb);
	}
}
void output_set_next_uri(const char *uri) {
	if (output_module && output_module->set_next_uri) {
		output_module->set_next_uri(uri);
	}
}

int output_play(output_transition_cb_t transition_callback) {
	if (output_module && output_module->play) {
		return output_module->play(transition_callback);
	}
	return -1;
}

int output_pause(void) {
	if (output_module && output_module->pause) {
		return output_module->pause();
	}
	return -1;
}

int output_stop(void) {
	if (output_module && output_module->stop) {
		return output_module->stop();
	}
	return -1;
}

int output_seek(gint64 position_nanos) {
	if (output_module && output_module->seek) {
		return output_module->seek(position_nanos);
	}
	return -1;
}

int output_get_position(gint64 *track_dur, gint64 *track_pos) {
	if (output_module && output_module->get_position) {
		return output_module->get_position(track_dur, track_pos);
	}
	return -1;
}

int output_get_volume(float *value) {
	/* Try ALSA first */
	float vol = get_alsa_volume();
	if (vol >= 0) {
		return vol;
	}
	/* Go on if ALSA volume isn't supported */
	if (output_module && output_module->get_volume) {
		return output_module->get_volume(value);
	}
	return -1;
}
int output_set_volume(float value) {
	/* Try ALSA first */
	float vol = set_alsa_volume(value);
        if (vol >= 0) {
                return vol;
        }
	/* Go on if ALSA volume isn't supported */
	if (output_module && output_module->set_volume) {
		return output_module->set_volume(value);
	}
	return -1;
}
int output_get_mute(int *value) {
	if (output_module && output_module->get_mute) {
		return output_module->get_mute(value);
	}
	return -1;
}
int output_set_mute(int value) {
	if (output_module && output_module->set_mute) {
		return output_module->set_mute(value);
	}
	return -1;
}
