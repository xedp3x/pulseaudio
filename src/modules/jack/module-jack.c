/***
 This file is part of PulseAudio.

 Copyright 2013 Mario Krüger & Giovanni Harting

 PulseAudio is free software; you can redistribute it and/or modify
 it under the terms of the GNU Lesser General Public License as published
 by the Free Software Foundation; either version 2.1 of the License,
 or (at your option) any later version.

 PulseAudio is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 General Public License for more details.

 You should have received a copy of the GNU Lesser General Public License
 along with PulseAudio; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 USA.
 ***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>

#include <pulse/xmalloc.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include "module-jack-symdef.h"

PA_MODULE_AUTHOR("Mario Krueger & Giovanni Harting");
PA_MODULE_DESCRIPTION("JACK");
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
		"sink_properties=<properties for the card> "
		"source_properties=<properties for the card> "
		"server_name=<jack server name> "
		"connect=<connect ports?>"
);

static const char* const valid_modargs[] = {
	"sink_properties",
	"source_properties",
	"server_name",
	"connect",
	NULL
};

struct sBase {
	pa_core *core;
	pa_module *module;
	pa_modargs *ma;

	bool autoconnect;
	char server_name;
};

struct sCard {
	void *base;
	bool is_sink;
	char *name;

	pa_sink *sink;
	pa_source *source;

	pa_rtpoll_item *rtpoll_item;

	pa_thread_mq thread_mq;
	pa_thread *thread;
	pa_asyncmsgq *jack_msgq;
	pa_rtpoll *rtpoll;

	jack_client_t *jack; // ehemals client
	jack_port_t *port[PA_CHANNELS_MAX];
	jack_nframes_t frames_in_buffer;
	jack_nframes_t saved_frame_time;
	bool saved_frame_time_valid;

	unsigned channels;
	unsigned ports[PA_CHANNELS_MAX];
	void *buffer[PA_CHANNELS_MAX];
};

enum {
	SOURCE_MESSAGE_POST = PA_SOURCE_MESSAGE_MAX,
	SOURCE_MESSAGE_ON_SHUTDOWN,
	SINK_MESSAGE_RENDER = PA_SINK_MESSAGE_MAX,
	SINK_MESSAGE_BUFFER_SIZE,
	SINK_MESSAGE_ON_SHUTDOWN
};

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
	struct sCard *card = PA_SOURCE(o)->userdata;
	struct sBase *base = card->base;

	//pa_log_debug("source_process_msg %i", code);

	switch (code) {

	case SOURCE_MESSAGE_POST:

		/* Handle the new block from the JACK thread */
		pa_assert(chunk);
		pa_assert(chunk->length > 0);

		if (card->source->thread_info.state == PA_SOURCE_RUNNING)
			pa_source_post(card->source, chunk);

		card->saved_frame_time = (jack_nframes_t) offset;
		card->saved_frame_time_valid = true;

		return 0;

	case SOURCE_MESSAGE_ON_SHUTDOWN:
		pa_asyncmsgq_post(card->thread_mq.outq, PA_MSGOBJECT(base->core),
				PA_CORE_MESSAGE_UNLOAD_MODULE, base->module, 0, NULL, NULL);
		return 0;

	case PA_SOURCE_MESSAGE_GET_LATENCY: {
		jack_latency_range_t r;
		jack_nframes_t l, ft, d;
		size_t n;

		/* This is the "worst-case" latency */
		jack_port_get_latency_range(card->port[0], JackCaptureLatency, &r);
		l = r.max;

		if (card->saved_frame_time_valid) {
			/* Adjust the worst case latency by the time that
			 * passed since we last handed data to JACK */

			ft = jack_frame_time(card->jack);
			d = ft > card->saved_frame_time ? ft - card->saved_frame_time : 0;
			l += d;
		}

		/* Convert it to usec */
		n = l * pa_frame_size(&card->source->sample_spec);
		*((pa_usec_t*) data) = pa_bytes_to_usec(n, &card->source->sample_spec);

		return 0;
	}
	}

	return pa_source_process_msg(o, code, data, offset, chunk);
}

static int pa_process_sink_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *memchunk) {
	struct sCard *card = PA_SINK(o)->userdata;
	struct sBase *base = card->base;

	/*
	pa_log("%p (card) Line %d",card,__LINE__);
	pa_log("%p (base)",card->base);
	pa_log("Cannesl: %i",card->channels);
	pa_log("Name: %s",card->name);
	*/

	switch (code) {
		case SINK_MESSAGE_RENDER:
			/* Handle the request from the JACK thread */
			if (card->sink->thread_info.state == PA_SINK_RUNNING) {
				pa_memchunk chunk;
				size_t nbytes;
				void *p;
				bool rewind_requested;

				pa_assert(offset > 0);
				nbytes = (size_t) offset * pa_frame_size(&card->sink->sample_spec);

				rewind_requested = card->sink->thread_info.rewind_requested;
				card->sink->thread_info.rewind_requested = false;
				pa_sink_render_full(card->sink, nbytes, &chunk);
				card->sink->thread_info.rewind_requested = rewind_requested;

				p = pa_memblock_acquire_chunk(&chunk);
				pa_deinterleave(p, card->buffer, card->channels, sizeof(float),(unsigned) offset);
				pa_memblock_release(chunk.memblock);

				pa_memblock_unref(chunk.memblock);
			} else {
				unsigned c;
				pa_sample_spec ss;

				/* Humm, we're not RUNNING, hence let's write some silence */
				/* This can happen if we're paused, or during shutdown (when we're unlinked but jack is still running). */

				ss = card->sink->sample_spec;
				ss.channels = 1;

				for (c = 0; c < card->channels; c++)
					pa_silence_memory(card->buffer[c],(size_t) offset * pa_sample_size(&ss), &ss);
			}

			card->frames_in_buffer = (jack_nframes_t) offset;
			card->saved_frame_time = *(jack_nframes_t*) data;
			card->saved_frame_time_valid = true;

			return 0;

		case SINK_MESSAGE_BUFFER_SIZE:
			pa_sink_set_max_request_within_thread(card->sink,
					(size_t) offset * pa_frame_size(&card->sink->sample_spec));
			return 0;

		case SINK_MESSAGE_ON_SHUTDOWN:
			pa_asyncmsgq_post(card->thread_mq.outq, PA_MSGOBJECT(base->core),
					PA_CORE_MESSAGE_UNLOAD_MODULE, base->module, 0, NULL, NULL);
			return 0;

		case PA_SINK_MESSAGE_GET_LATENCY: {
			jack_nframes_t l, ft, d;
			jack_latency_range_t r;
			size_t n;

			/* This is the "worst-case" latency */
			jack_port_get_latency_range(card->port[0], JackPlaybackLatency, &r);
			l = r.max + card->frames_in_buffer;

			if (card->saved_frame_time_valid) {
				/* Adjust the worst case latency by the time that
				 * passed since we last handed data to JACK */

				ft = jack_frame_time(card->jack);
				d = ft > card->saved_frame_time ? ft - card->saved_frame_time : 0;
				l = l > d ? l - d : 0;
			}

			/* Convert it to usec */
			n = l * pa_frame_size(&card->sink->sample_spec);
			*((pa_usec_t*) data) = pa_bytes_to_usec(n, &card->sink->sample_spec);

			return 0;
		}
	}

	return pa_sink_process_msg(o, code, data, offset, memchunk);
}

static int jack_process(jack_nframes_t nframes, void *arg) {
	struct sCard *card = arg;
	struct sBase *base = card->base;
	unsigned c;
	void *p;
	const void *buffer[PA_CHANNELS_MAX];
	jack_nframes_t frame_time;
	pa_memchunk chunk;
	pa_assert(card);

	/*
	pa_log("jack_process");
	pa_log("--- %d",__LINE__);

	pa_log("%p (card-%s) Line %d",card,card->name,__LINE__);
	pa_log("%p (base)",card->base);
	pa_log("Cannesl: %i",card->channels);
	*/

	if (card->is_sink) {
		//pa_log("--- %d",__LINE__);
		for (c = 0; c < card->channels; c++)
			pa_assert_se(card->buffer[c] = jack_port_get_buffer(card->port[c], nframes));
		frame_time = jack_frame_time(card->jack);
		pa_assert_se(pa_asyncmsgq_send(card->jack_msgq, PA_MSGOBJECT(card->sink), SINK_MESSAGE_RENDER, &frame_time, nframes, NULL) == 0);
	} else {
		for (c = 0; c < card->channels; c++){
			pa_assert_se(buffer[c] = jack_port_get_buffer(card->port[c], nframes));
			//pa_log("(jack-%s) port#%d: %p",card->name,c,buffer[c]);
		}
		pa_memchunk_reset(&chunk);
		chunk.length = nframes * pa_frame_size(&card->source->sample_spec);
		chunk.memblock = pa_memblock_new(base->core->mempool, chunk.length);
		p = pa_memblock_acquire(chunk.memblock);
		pa_interleave(buffer, card->channels, p, sizeof(float), nframes);
		pa_memblock_release(chunk.memblock);

		frame_time = jack_frame_time(card->jack);

		pa_asyncmsgq_post(card->jack_msgq, PA_MSGOBJECT(card->source),SOURCE_MESSAGE_POST, NULL, frame_time, &chunk, NULL);

		pa_memblock_unref(chunk.memblock);
	}

	return 0;
}

static void thread_func(void *arg) {
	struct sCard *card = arg;
	struct sBase *base = card->base;

	pa_assert(card);

	pa_log_debug("Thread starting up");

	if (base->core->realtime_scheduling)
		pa_make_realtime(base->core->realtime_priority);

	pa_thread_mq_install(&card->thread_mq);

	for (;;) {
		int ret;


		if ((ret = pa_rtpoll_run(card->rtpoll, true)) < 0)
			goto fail;

		if (ret == 0)
			goto finish;
	}

	fail:
	/* If this was no regular exit from the loop we have to continue
	 * processing messages until we received PA_MESSAGE_SHUTDOWN */
	pa_asyncmsgq_post(card->thread_mq.outq, PA_MSGOBJECT(base->core),
			PA_CORE_MESSAGE_UNLOAD_MODULE, base->module, 0, NULL, NULL);
	pa_asyncmsgq_wait_for(card->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

	finish:
	pa_log_debug("Thread shutting down");
}

static void jack_error_func(const char*t) {
	char *s;

	s = pa_xstrndup(t, strcspn(t, "\n\r"));
	pa_log_warn("JACK error >%s<", s);
	pa_xfree(s);
}

static void jack_init(void *arg) {
	struct sCard *card = arg;
	struct sBase *base = card->base;

	pa_log_info("JACK thread starting up.");

	if (base->core->realtime_scheduling)
		pa_make_realtime(base->core->realtime_priority + 4);
}

static void jack_shutdown(void* arg) {
	struct sCard *card = arg;

	pa_log_info("JACK thread shutting down..");

	if (card->is_sink) {
		pa_asyncmsgq_post(card->jack_msgq, PA_MSGOBJECT(card->sink),
				SINK_MESSAGE_ON_SHUTDOWN, NULL, 0, NULL, NULL);
	} else {
		pa_asyncmsgq_post(card->jack_msgq, PA_MSGOBJECT(card->source),
				SOURCE_MESSAGE_ON_SHUTDOWN, NULL, 0, NULL, NULL);
	}
}

void* init_card(void *arg, const char *name, bool is_sink) {
	struct sCard *card = malloc(sizeof(struct sCard));
	struct sBase *base = arg;
	unsigned i;
	jack_status_t status;
	jack_latency_range_t r;
	const char **ports = NULL, **p;
	pa_sample_spec ss;

	pa_log_debug("init_card");

	card->name = name;
	card->base = base;
	card->is_sink = is_sink;

	pa_log("%p (card) Line %d",card,__LINE__);
	pa_log("%p (base)",card->base);

	card->rtpoll = pa_rtpoll_new();
	card->saved_frame_time_valid = false;

	pa_thread_mq_init(&card->thread_mq, base->core->mainloop, card->rtpoll);

	pa_log_debug("Jack Handler 390");
	/* Jack handler */
	card->jack_msgq = pa_asyncmsgq_new(0);
	card->rtpoll_item = pa_rtpoll_item_new_asyncmsgq_read(card->rtpoll, PA_RTPOLL_EARLY - 1, card->jack_msgq);
	pa_log("--- %d",__LINE__);
	if (!(card->jack = jack_client_open(card->name, base->server_name ? JackServerName : JackNullOption, &status, base->server_name))) {
	//if (!(card->jack = jack_client_open("test", JackNullOption, &status, NULL))) {
		pa_log("--- %d",__LINE__);
		pa_log("jack_client_open() failed.");
		goto fail;
	}
	pa_log("--- %d",__LINE__);
	pa_log_info("Successfully connected as '%s'", jack_get_client_name(card->jack));

	jack_set_process_callback(card->jack, jack_process, card);
	jack_on_shutdown(card->jack, jack_shutdown, &card);
	jack_set_thread_init_callback(card->jack, jack_init, card);

	if (jack_activate(card->jack)) {
		pa_log("jack_activate() failed");
		goto fail;
	}

	pa_log_debug("ss 383");
	/* set sample rate */
	ss.rate = jack_get_sample_rate(card->jack);
	ss.format = PA_SAMPLE_FLOAT32NE;
	card->channels = ss.channels = base->core->default_sample_spec.channels;
	pa_assert(pa_sample_spec_valid(&ss));

	pa_log_debug("PA Handler 389");
	/* PA handler */
	if (card->is_sink) {
		pa_sink_new_data data;
		pa_sink_new_data_init(&data);
		data.driver = __FILE__;
		data.module = base->module;

		pa_sink_new_data_set_name(&data, card->name);
		pa_sink_new_data_set_sample_spec(&data, &ss);

		if (base->server_name)
			pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, &base->server_name);
		pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Jack (%s)", jack_get_client_name(card->jack));
		pa_proplist_sets(data.proplist, "jack.client_name", jack_get_client_name(card->jack));

		if (pa_modargs_get_proplist(base->ma, "sink_properties", data.proplist,
				PA_UPDATE_REPLACE) < 0) {
			pa_log("Invalid properties");
			pa_sink_new_data_done(&data);
			goto fail;
		}

		card->sink = pa_sink_new(base->core, &data, PA_SINK_LATENCY);

		pa_sink_new_data_done(&data);

		if (!card->sink) {
			pa_log("Failed to create sink.");
			goto fail;
		}

		card->sink->parent.process_msg = pa_process_sink_msg;
		card->sink->userdata = card;
		card->source = NULL;

		pa_sink_set_asyncmsgq(card->sink, card->thread_mq.inq);
		pa_sink_set_rtpoll(card->sink, card->rtpoll);
		pa_sink_set_max_request(card->sink,jack_get_buffer_size(card->jack)* pa_frame_size(&card->sink->sample_spec));
	} else {
		pa_source_new_data data;
		data.driver = __FILE__;
		data.module = base->module;
		pa_source_new_data_init(&data);
		pa_source_new_data_set_name(&data, &card->name);
		pa_source_new_data_set_sample_spec(&data, &ss);

		if (base->server_name)
			pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING,
					&base->server_name);
		pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Jack (%s)",
				jack_get_client_name(card->jack));
		pa_proplist_sets(data.proplist, "jack.client_name",
				jack_get_client_name(card->jack));

		if (pa_modargs_get_proplist(base->ma, "source_properties",
				data.proplist, PA_UPDATE_REPLACE) < 0) {
			pa_log("Invalid properties");
			pa_source_new_data_done(&data);
			goto fail;
		}

		card->source = pa_source_new(base->core, &data, PA_SOURCE_LATENCY);
		pa_source_new_data_done(&data);

		if (!card->source) {
			pa_log("Failed to create source.");
			goto fail;
		}
		card->source->parent.process_msg = source_process_msg;
		card->source->userdata = card;
		card->sink = NULL;

		pa_source_set_asyncmsgq(card->source, card->thread_mq.inq);
		pa_source_set_rtpoll(card->source, card->rtpoll);

	}

	pa_log_debug("Jack Ports");
	/* Jack ports */
	ports = jack_get_ports(card->jack, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | (card->is_sink ? JackPortIsInput : JackPortIsOutput));
	for (i = 0; i < ss.channels; i++) {
	        if (!(card->port[i] = jack_port_register(card->jack, pa_channel_position_to_string(base->core->default_channel_map.map[i]), JACK_DEFAULT_AUDIO_TYPE, (card->is_sink ? JackPortIsOutput : JackPortIsInput)|JackPortIsTerminal, 0))) {
	            pa_log("jack_port_register() failed.");
	            goto fail;
	        }
	    }

	if (base->autoconnect) {
		if (ports)
			jack_free(ports);
		for (i = 0, p = ports; i < ss.channels; i++, p++) {

			if (!p || !*p) {
				pa_log("Not enough physical output ports, leaving unconnected.");
				break;
			}

			pa_log_info("Connecting %s to %s", jack_port_name(card->port[i]), *p);

			if (jack_connect(card->jack, *p, jack_port_name(card->port[i]))) {
				pa_log("Failed to connect %s to %s, leaving unconnected.", jack_port_name(card->port[i]), *p);
				break;
			}
		}
	}

	/* init thread */
	pa_log_debug("init thread");
	jack_port_get_latency_range(card->port[0], JackCaptureLatency, &r);
	if (card->is_sink) {
		size_t n;
		n = r.max * pa_frame_size(&card->sink->sample_spec);
		pa_sink_set_fixed_latency(card->sink,pa_bytes_to_usec(n, &card->sink->sample_spec));

		if (!(card->thread = pa_thread_new(jack_get_client_name(card->jack),thread_func, card))) {
			pa_log("Failed to create thread.");
			goto fail;
		}
		pa_sink_put(card->sink);
	} else {
		size_t n;
		n = r.max * pa_frame_size(&card->source->sample_spec);
		pa_source_set_fixed_latency(card->source,pa_bytes_to_usec(n, &card->source->sample_spec));

		if (!(card->thread = pa_thread_new(jack_get_client_name(card->jack),thread_func, card))) {
			pa_log("Failed to create thread.");
			goto fail;
		}
		pa_source_put(card->source);
	}

	if (ports)
		jack_free(ports);


	pa_log("new card lodes \\o/");
	return NULL;

	fail:
	pa_log("card_init Fatal 511");
	abort(); // KILL!!!!
	if (ports)
		jack_free(ports);
	return NULL;
}

int pa__init(pa_module*m) {
	/* init base */
	struct sBase *base = NULL;
	pa_log_debug("PA init");

	m->userdata = base = pa_xnew0(struct sBase, 1);
	base->core = m->core;
	base->module = m;

	/* read Config */
	if (!(base->ma = pa_modargs_new(m->argument, valid_modargs))) {
		pa_log("Failed to parse module arguments.");
		pa__done(m);
		return -1;
	}

	if (pa_modargs_get_value_boolean(base->ma, "connect", &(base->autoconnect)) < 0) {
		pa_log("Failed to parse connect= argument.");
		pa__done(m);
		return -1;
	}

	/* init Jack */
	//pa_modargs_get_value(base->ma, "server_name", &base->server_name);
	base->server_name = pa_modargs_get_value(base->ma, "server_name", NULL);
	jack_set_error_function(jack_error_func);

	pa_log_debug("create sink 1");
	init_card(base,"sink1",true);
	pa_log_debug("create sink 2");
	init_card(base,"sink2", true);

	pa_log_debug("create source 1");
	init_card(base, "source1", false);
	pa_log_debug("create source 2");
	init_card(base,"source2", false);

	return 0;
}

/*
 int pa__get_n_used(pa_module *m) {
 struct sCard *card;

 pa_assert(m);
 pa_assert_se(card = m->userdata);

 return pa_source_linked_by(card->source);
 }
 */

void pa__done(pa_module*m) {
	/*
	 struct sBase *base;
	 pa_assert(m);

	 if (!(base = m->userdata))
	 return;

	 if (base->ma)
	 pa_modargs_free(ma);

	 // Arrays!
	 if (base.source)
	 pa_source_unlink(base.source);

	 // Arrays!
	 if (base->sink)
	 pa_source_unlink(base->sink);


	 if (base->client)
	 jack_client_close(base->client);

	 if (base->thread) {
	 pa_asyncmsgq_send(base->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
	 pa_thread_free(base->thread);
	 }

	 pa_thread_mq_done(&base->thread_mq);

	 // Arrays!
	 if (base.source)
	 pa_source_unref(base.source);

	 // Arrays!
	 if (base->sink)
	 pa_source_unref(base->sink);

	 if (base->rtpoll_item)
	 pa_rtpoll_item_free(base->rtpoll_item);

	 if (base.jack_msgq)
	 pa_asyncmsgq_unref(base.jack_msgq);

	 if (base->rtpoll)
	 pa_rtpoll_free(base->rtpoll);
	 */
	//pa_xfree(base);
	//goto hell;
}
