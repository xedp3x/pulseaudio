/***
 This file is part of PulseAudio.

 Copyright 2015 Mario Krüger

 Some code taken from other parts of PulseAudio, these are
 Copyright 2006 Lennart Poettering
 Copyright 2009 Canonical Ltd

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
#include <string.h>

#include <jack/jack.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>
#include <pulse/channelmap.h>
#include <pulsecore/sink.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/namereg.h>
#include <expat_config.h>

#include "module-jack-symdef.h"
#include "module-jack.h"

PA_MODULE_AUTHOR("Mario Krueger");
PA_MODULE_DESCRIPTION("JACK");
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
        "sink_properties=<properties for the card>"
        "source_properties=<properties for the card>"
        "server_name=<jack server name>"
        "connect=<connect new ports to speaker/mic?>"
        "merge=<merge streams from same application: 0=no, 1=same pid, 2=same binary name, 3=same application name>"
        "delay=<delay before remove unused application bridge, 0=never>"
);

static const char* const valid_modargs[] = {
    "sink_properties",
    "source_properties",
    "server_name",
    "connect",
    "merge",
    "delay",
    NULL
};

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct sCard *card = PA_SOURCE(o)->userdata;
    struct sBase *base = card->base;

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
            pa_asyncmsgq_post(card->thread_mq.outq, PA_MSGOBJECT(base->core), PA_CORE_MESSAGE_UNLOAD_MODULE, base->module, 0, NULL, NULL);
            return 0;

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            jack_latency_range_t r;
            jack_nframes_t l, ft, d;
            size_t n;

            /* This is the "worst-case" latency */
            jack_port_get_latency_range(card->source_port[0], JackCaptureLatency, &r);
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
        default:
            return pa_source_process_msg(o, code, data, offset, chunk);
    }
}

static int pa_process_sink_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *memchunk) {
    struct sCard *card = PA_SINK(o)->userdata;
    struct sBase *base = card->base;

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
                pa_deinterleave(p, card->sink_buffer, card->sink_channels, sizeof(float),(unsigned) offset);
                pa_memblock_release(chunk.memblock);

                pa_memblock_unref(chunk.memblock);
            } else {
                unsigned c;
                pa_sample_spec ss;

                /* Humm, we're not RUNNING, hence let's write some silence */
                /* This can happen if we're paused, or during shutdown (when we're unlinked but jack is still running). */

                ss = card->sink->sample_spec;
                ss.channels = 1;

                for (c = 0; c < card->sink_channels; c++)
                    pa_silence_memory(card->sink_buffer[c],(size_t) offset * pa_sample_size(&ss), &ss);
            }
            card->frames_in_buffer = (jack_nframes_t) offset;
            card->saved_frame_time = *(jack_nframes_t*) data;
            card->saved_frame_time_valid = true;
            return 0;

        case SINK_MESSAGE_BUFFER_SIZE:
            pa_sink_set_max_request_within_thread(card->sink, (size_t) offset * pa_frame_size(&card->sink->sample_spec));
            return 0;

        case SINK_MESSAGE_ON_SHUTDOWN:
            pa_asyncmsgq_post(card->thread_mq.outq, PA_MSGOBJECT(base->core), PA_CORE_MESSAGE_UNLOAD_MODULE, base->module, 0, NULL, NULL);
            return 0;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            jack_nframes_t l, ft, d;
            jack_latency_range_t r;
            size_t n;

            /* This is the "worst-case" latency */
            jack_port_get_latency_range(card->sink_port[0], JackPlaybackLatency, &r);
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

        default:
            return pa_sink_process_msg(o, code, data, offset, memchunk);
    }
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

    if (card->sink) {
        for (c = 0; c < card->sink_channels; c++)
            pa_assert_se(card->sink_buffer[c] = jack_port_get_buffer(card->sink_port[c], nframes));
        frame_time = jack_frame_time(card->jack);
        pa_assert_se(pa_asyncmsgq_send(card->jack_msgq, PA_MSGOBJECT(card->sink), SINK_MESSAGE_RENDER, &frame_time, nframes, NULL) == 0);
    }
    if (card->source) {
        for (c = 0; c < card->source_channels; c++)
            pa_assert_se(buffer[c] = jack_port_get_buffer(card->source_port[c], nframes));

        pa_memchunk_reset(&chunk);
        chunk.length = nframes * pa_frame_size(&card->source->sample_spec);
        chunk.memblock = pa_memblock_new(base->core->mempool, chunk.length);
        p = pa_memblock_acquire(chunk.memblock);
        pa_interleave(buffer, card->source_channels, p, sizeof(float), nframes);
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

        if ((ret = pa_rtpoll_run(card->rtpoll)) < 0) {
            pa_log("fail in thread_func");
            goto fail;
        }

        if (ret == 0)
            goto finish;
    }

    fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(card->thread_mq.outq, PA_MSGOBJECT(base->core), PA_CORE_MESSAGE_UNLOAD_MODULE, base->module, 0, NULL, NULL);
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

    if (card->sink) {
        pa_asyncmsgq_post(card->jack_msgq, PA_MSGOBJECT(card->sink), SINK_MESSAGE_ON_SHUTDOWN, NULL, 0, NULL, NULL);
    }
    if (card->source) {
        pa_asyncmsgq_post(card->jack_msgq, PA_MSGOBJECT(card->source), SOURCE_MESSAGE_ON_SHUTDOWN, NULL, 0, NULL, NULL);
    }
}

void* create_card(void* arg, const char *name){
    struct sCard *card = pa_xnew0(struct sCard,1);
    struct sBase *base = arg;
    jack_status_t status;

    card->merge_ref = NULL;
    card->sink = NULL;
    card->source = NULL;

    card->name = name;
    card->base = base;

    card->inputs  = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    card->outputs = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    card->time_event = pa_core_rttime_new(base->core, PA_USEC_INVALID, timeout_cb, card);
    card->rtpoll = pa_rtpoll_new();
    card->saved_frame_time_valid = false;

    pa_thread_mq_init(&card->thread_mq, base->core->mainloop, card->rtpoll);

    /* Jack handler */
    card->jack_msgq = pa_asyncmsgq_new(0);
    card->rtpoll_item = pa_rtpoll_item_new_asyncmsgq_read(card->rtpoll, PA_RTPOLL_EARLY - 1, card->jack_msgq);
    if (!(card->jack = jack_client_open(card->name, base->server_name ? JackServerName : JackNullOption, &status, base->server_name))) {
        pa_log("jack_client_open() failed.");
        goto fail;
    }
    pa_log_info("Successfully connected as '%s'", jack_get_client_name(card->jack));

    jack_set_process_callback(card->jack, jack_process, card);
    jack_on_shutdown(card->jack, jack_shutdown, card);
    jack_set_thread_init_callback(card->jack, jack_init, card);

    if (jack_activate(card->jack)) {
        pa_log("jack_activate() failed");
        goto fail;
    }

    if (!(card->thread = pa_thread_new(jack_get_client_name(card->jack),thread_func, card))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_idxset_put(base->cards, card, NULL);
    return card;

fail:
    pa_log("create_card fatal error");
    abort();
    return NULL;
}

void* add_bridge(void *arg, bool sink, uint8_t channels) {
    struct sCard *card = arg;
    struct sBase *base = card->base;
    const char **ports = NULL, **p;
    char *port_name;
    unsigned i;
    bool autoconnect;
    pa_sample_spec ss;
    pa_channel_map map;
    jack_latency_range_t r;
    jack_port_t *port;

    if (sink){
        if (card->sink) {
            pa_log("This card have already a sink!");
            return NULL;
        }
    } else {
        if (card->source){
            pa_log("This card have already a source!");
            return NULL;
        }
    }

    /* set sample rate */
    ss.rate = jack_get_sample_rate(card->jack);
    ss.format = PA_SAMPLE_FLOAT32NE;
    if (channels == 0)
        ss.channels = base->core->default_sample_spec.channels;
    else
        ss.channels = channels;

    if (sink)
        card->sink_channels = ss.channels;
    else
        card->source_channels = ss.channels;

    pa_assert(pa_sample_spec_valid(&ss));

    if (ss.channels == base->core->default_channel_map.channels)
        map = base->core->default_channel_map;
    else
        pa_channel_map_init_extend(&map, ss.channels, PA_CHANNEL_MAP_AUX);

    /* PA handler */
    if (sink){
        pa_sink_new_data data;
        pa_sink_new_data_init(&data);
        data.driver = __FILE__;
        data.module = base->module;

        pa_sink_new_data_set_name(&data, card->name);
        pa_sink_new_data_set_sample_spec(&data, &ss);
        pa_sink_new_data_set_channel_map(&data, &map);

        if (base->server_name)
            pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, &base->server_name);
        pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Jack (%s)", jack_get_client_name(card->jack));
        pa_proplist_sets(data.proplist, PA_PROP_JACK_CLIENT, jack_get_client_name(card->jack));
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "jack");

        if (pa_modargs_get_proplist(base->ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
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

        pa_sink_set_asyncmsgq(card->sink, card->thread_mq.inq);
        pa_sink_set_rtpoll(card->sink, card->rtpoll);
        pa_sink_set_max_request(card->sink,jack_get_buffer_size(card->jack)* pa_frame_size(&card->sink->sample_spec));
    } else {
        pa_source_new_data data;
        data.driver = __FILE__;
        data.module = base->module;
        pa_source_new_data_init(&data);
        pa_source_new_data_set_name(&data, card->name);
        pa_source_new_data_set_sample_spec(&data, &ss);
        pa_source_new_data_set_channel_map(&data, &map);

        if (base->server_name)
            pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, &base->server_name);
        pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Jack (%s)", jack_get_client_name(card->jack));
        pa_proplist_sets(data.proplist, PA_PROP_JACK_CLIENT, jack_get_client_name(card->jack));
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "jack");

        if (pa_modargs_get_proplist(base->ma, "source_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
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

        pa_source_set_asyncmsgq(card->source, card->thread_mq.inq);
        pa_source_set_rtpoll(card->source, card->rtpoll);
    }

    /* Jack ports */
    autoconnect = base->autoconnect;
    if (autoconnect)
        ports = jack_get_ports(card->jack, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | (sink ? JackPortIsInput : JackPortIsOutput));
    for (i = 0, p = ports; i < ss.channels; i++, p++) {
        switch(i){
            case 0:
                if (ss.channels == 1) {
                    if (sink)
                        port_name = (char *) "out";
                    else
                        port_name = (char *) "in";
                } else {
                    if (sink)
                        port_name = (char *) "left";
                    else
                        port_name = (char *) "left_in";
                }
                break;
            case 1:
                if (sink)
                    port_name = (char*) "right";
                else
                    port_name = (char*) "right_in";
                break;
            default:
                port_name = malloc(10);
                if (sink)
                    sprintf(port_name, "out_%d", i+1);
                else
                    sprintf(port_name, "in_%d", i+1);
        }

        if (!(port = jack_port_register(card->jack, port_name, JACK_DEFAULT_AUDIO_TYPE, (sink ? JackPortIsOutput : JackPortIsInput)|JackPortIsTerminal, 0))) {
            pa_log("jack_port_register() failed.");
            goto fail;
        }

        if (sink){
            card->sink_port[i] = port;
            if (autoconnect){
                if (!p || !*p) {
                    pa_log("Not enough physical output ports, leaving unconnected.");
                    autoconnect = false;
                }else{
                    if (jack_connect(card->jack,jack_port_name(card->sink_port[i]),*p)) {
                        pa_log("Failed to connect %s to %s, leaving unconnected.", jack_port_name(card->sink_port[i]), *p);
                        autoconnect = false;
                    }
                }
            }
        } else {
            card->source_port[i] = port;
            if (autoconnect){
                if (!p || !*p) {
                    pa_log("Not enough physical output ports, leaving unconnected.");
                    autoconnect = false;
                }else{
                    if (jack_connect(card->jack,*p,jack_port_name(card->source_port[i]))) {
                        pa_log("Failed to connect %s to %s, leaving unconnected.", *p, jack_port_name(card->source_port[i]));
                        autoconnect = false;
                    }
                }
            }
        }
    }
    if (ports)
        jack_free(ports);

    if (sink) {
        pa_sink_set_fixed_latency(card->sink,pa_bytes_to_usec((r.max * pa_frame_size(&card->sink->sample_spec)), &card->sink->sample_spec));
        pa_sink_put(card->sink);
    } else {
        pa_source_set_fixed_latency(card->source,pa_bytes_to_usec((r.max * pa_frame_size(&card->source->sample_spec)), &card->source->sample_spec));
        pa_source_put(card->source);
    }

    return NULL;

fail:
    pa_log("add_bridge fatal error");
    abort();
    return NULL;

}

void unload_card(void* arg,bool forced){
    struct sCard* card = arg;
    struct sBase* base = card->base;

    if (!forced){
        pa_usec_t now;
        now = pa_rtclock_now();
        if (base->delay > 0)
            pa_core_rttime_restart(base->core, card->time_event, now + base->delay);
        return;
    }

    if(card->sink){
        if (pa_idxset_size(card->sink->inputs) > 0) {
            pa_sink *def;
            pa_sink_input *i;
            uint32_t idx;

            def = pa_namereg_get_default_sink(base->core);
            PA_IDXSET_FOREACH(i, card->sink->inputs, idx)
                pa_sink_input_move_to(i, def, false);
        }
        pa_sink_unlink(card->sink);
        card->sink = NULL;
    }
    if (card->source){
        if (pa_idxset_size(card->source->outputs) > 0) {
            pa_source *def;
            pa_source_output *o;
            uint32_t idx;

            def = pa_namereg_get_default_source(base->core);
            PA_IDXSET_FOREACH(o, card->source->outputs, idx)
                pa_source_output_move_to(o, def, false);
        }
        pa_source_unlink(card->source);
        card->source = NULL;
    }
    base->core->mainloop->time_free(card->time_event);

    jack_client_close(card->jack);
    pa_asyncmsgq_send(card->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
    pa_thread_free(card->thread);
    pa_thread_mq_done(&card->thread_mq);

    if(card->sink)
        pa_sink_unref(card->sink);
    if(card->source)
        pa_source_unref(card->source);

    pa_rtpoll_item_free(card->rtpoll_item);
    pa_asyncmsgq_unref(card->jack_msgq);
    pa_rtpoll_free(card->rtpoll);

    if (card->inputs)
        pa_idxset_free(card->inputs, NULL);
    if (card->outputs)
        pa_idxset_free(card->outputs, NULL);

    pa_idxset_remove_by_data(base->cards, card, NULL);
    pa_xfree(card);
}

const char *get_merge_ref(pa_proplist *p, struct sBase *base){
    switch (base->merge){
    case 1:
        return pa_strnull(pa_proplist_gets(p, PA_PROP_APPLICATION_PROCESS_ID));
    case 2:
        return pa_strnull(pa_proplist_gets(p, PA_PROP_APPLICATION_PROCESS_BINARY));
    case 3:
        return pa_strnull(pa_proplist_gets(p, PA_PROP_APPLICATION_NAME));
    default:
        return NULL;
    }
}

static pa_hook_result_t sink_input_move_fail_hook_callback(pa_core *c, pa_sink_input *i, void *u) {
    pa_sink *target;
    target = pa_namereg_get_default_sink(c);

    pa_assert(c);
    pa_assert(i);

    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    if (pa_sink_input_finish_move(i, target, false) < 0)
        return PA_HOOK_OK;
    else
        return PA_HOOK_STOP;
}

static pa_hook_result_t source_output_move_fail_hook_callback(pa_core *c, pa_source_output *i, void *u) {
    pa_source *target;
    target = pa_namereg_get_default_source(c);

    pa_assert(c);
    pa_assert(i);

    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    if (pa_source_output_finish_move(i, target, false) < 0)
        return PA_HOOK_OK;
    else
        return PA_HOOK_STOP;
}

static pa_hook_result_t sink_put_hook_callback(pa_core *c, pa_sink_input *sink_input, struct sBase* base) {
    /* Don't want to run during startup or shutdown */
    if (c->state != PA_CORE_RUNNING)
        return PA_HOOK_OK;

    if (sink_input->flags & PA_SINK_INPUT_DONT_MOVE ){
        pa_log_info("%s don't own jack-link...",pa_proplist_gets(sink_input->proplist, PA_PROP_APPLICATION_NAME));
    }else{
        uint32_t idx;
        struct sCard *card, *refCard;
        const char *merge_ref = get_merge_ref(sink_input->proplist, base);

        card = NULL;
        if (merge_ref)
            PA_IDXSET_FOREACH(refCard, base->cards, idx)
                if (refCard->merge_ref)
                    if (!strcmp(refCard->merge_ref, merge_ref)) {
                        if (refCard->sink) {
                            pa_log_info("secend sink from %s...", merge_ref);
                            pa_sink_input_move_to(sink_input, refCard->sink, false);
                            pa_idxset_put(refCard->inputs, sink_input, NULL);
                            return PA_HOOK_OK;
                        } else {
                            card = refCard;
                            break;
                        }
                    }

        if (!card)
            card = create_card(base,pa_proplist_gets(sink_input->proplist, PA_PROP_APPLICATION_NAME));
        add_bridge(card,true,sink_input->sample_spec.channels);

        pa_idxset_put(card->inputs, sink_input, NULL);
        pa_proplist_sets(card->sink->proplist, PA_PROP_JACK_CLIENT, jack_get_client_name(card->jack));
        card->merge_ref = malloc(strlen(merge_ref)+1);
        memcpy(card->merge_ref,merge_ref,strlen(merge_ref)+1);

        if (pa_sink_input_move_to(sink_input, card->sink, false) < 0)
            pa_log_info("Failed to move sink input \"%s\" to %s.", pa_strnull(pa_proplist_gets(sink_input->proplist, PA_PROP_APPLICATION_NAME)), card->sink->name);
        else
            pa_log_info("Successfully create sink input %s via %s.", pa_strnull(pa_proplist_gets(sink_input->proplist, PA_PROP_APPLICATION_NAME)), card->sink->name);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t source_put_hook_callback(pa_core *c, pa_source_output *source_output, struct sBase* base) {
    /* Don't want to run during startup or shutdown */
    if (c->state != PA_CORE_RUNNING)
        return PA_HOOK_OK;

    if (source_output->flags & PA_SOURCE_OUTPUT_DONT_MOVE ){
        pa_log_info("%s don't own jack-link...",pa_proplist_gets(source_output->proplist, PA_PROP_APPLICATION_NAME));
    }else{
        uint32_t idx;
        struct sCard *card, *refCard;
        const char *merge_ref = get_merge_ref(source_output->proplist, base);

        card = NULL;
        if (merge_ref)
            PA_IDXSET_FOREACH(refCard, base->cards, idx)
                if (refCard->merge_ref)
                    if (pa_streq(refCard->merge_ref,merge_ref)){
                        if (refCard->source) {
                            pa_log_info("secend source from %s...", merge_ref);
                            pa_source_output_move_to(source_output, refCard->source, false);
                            pa_idxset_put(refCard->outputs, source_output, NULL);
                            return PA_HOOK_OK;
                        } else {
                            card = refCard;
                            break;
                        }
                    }

        if (!card)
            card = create_card(base,pa_proplist_gets(source_output->proplist, PA_PROP_APPLICATION_NAME));
        add_bridge(card,false,source_output->sample_spec.channels);

        pa_idxset_put(card->outputs, source_output, NULL);
        pa_proplist_sets(card->source->proplist, PA_PROP_JACK_CLIENT, jack_get_client_name(card->jack));
        card->merge_ref = malloc(strlen(merge_ref)+1);
        memcpy(card->merge_ref,merge_ref,strlen(merge_ref)+1);

        if (pa_source_output_move_to(source_output, card->source, false) < 0)
            pa_log_info("Failed to move sink input \"%s\" to %s.", pa_strnull(pa_proplist_gets(source_output->proplist, PA_PROP_APPLICATION_NAME)), card->source->name);
        else
            pa_log_info("Successfully create source input %s via %s.", pa_strnull(pa_proplist_gets(source_output->proplist, PA_PROP_APPLICATION_NAME)), card->source->name);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_unlink_hook_callback(pa_core *c, pa_sink_input *sink_input, struct sBase* base) {
    if (pa_proplist_gets(sink_input->sink->proplist, PA_PROP_JACK_CLIENT) != NULL){
        struct sCard* card = sink_input->sink->userdata;
        pa_idxset_remove_by_data(card->inputs, sink_input, NULL);
        unload_card(card,false);
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t source_unlink_hook_callback(pa_core *c, pa_source_output *source_output, struct sBase* base) {
    if (pa_proplist_gets(source_output->source->proplist, PA_PROP_JACK_CLIENT) != NULL){
        struct sCard* card = source_output->source->userdata;
        pa_idxset_remove_by_data(card->outputs, source_output, NULL);
        unload_card(card,false);
    }
    return PA_HOOK_OK;
}

static void timeout_cb(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct sCard *card = userdata;
    struct sBase *base = card->base;

    pa_assert(card);
    pa_assert(base);

    base->core->mainloop->time_restart(card->time_event, NULL);

    if(card->sink){
        if (pa_idxset_size(card->sink->inputs) > 0)
            return;
    }
    if(card->source){
        if (pa_idxset_size(card->source->outputs) > 0)
            return;
    }
    unload_card(userdata,true);
}

int pa__init(pa_module*m) {
    /* init base */
    struct sBase *base = NULL;
    struct sCard *card;
    const char *server_name;
    uint32_t delay = 5;

    m->userdata = base = pa_xnew0(struct sBase, 1);
    base->core = m->core;
    base->module = m;
    base->cards = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    /* read Config */
    if (!(base->ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        pa__done(m);
        return -1;
    }

    base->autoconnect = true;
    if (pa_modargs_get_value_boolean(base->ma, "connect", &(base->autoconnect)) < 0) {
        pa_log("Failed to parse connect= argument.");
        pa__done(m);
        return -1;
    }

    base->merge = 1;
    if (pa_modargs_get_value_u32(base->ma, "merge", &(base->merge)) < 0) {
        pa_log("Failed to parse merge value.");
        pa__done(m);
        return -1;
    }

    if (pa_modargs_get_value_u32(base->ma, "delay", &delay) < 0) {
        pa_log("Failed to parse delay value. It must be a number > 0 (in sec.).");
        pa__done(m);
        return -1;
    }
    base->delay = delay * PA_USEC_PER_SEC;

    /* init Jack */
    server_name = pa_modargs_get_value(base->ma, "server_name", NULL);
    if (server_name)
        base->server_name = *server_name;
    jack_set_error_function(jack_error_func);

    /* register hooks */
    base->sink_put_slot                = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT],          PA_HOOK_LATE+30, (pa_hook_cb_t) sink_put_hook_callback, base);
    base->sink_unlink_slot             = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK],       PA_HOOK_LATE+30, (pa_hook_cb_t) sink_unlink_hook_callback, base);
    base->source_put_slot              = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT],       PA_HOOK_LATE+30, (pa_hook_cb_t) source_put_hook_callback, base);
    base->source_unlink_slot           = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK],    PA_HOOK_LATE+30, (pa_hook_cb_t) source_unlink_hook_callback, base);
    base->sink_input_move_fail_slot    = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FAIL],    PA_HOOK_LATE+20, (pa_hook_cb_t) sink_input_move_fail_hook_callback, base);
    base->source_output_move_fail_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FAIL], PA_HOOK_LATE+20, (pa_hook_cb_t) source_output_move_fail_hook_callback, base);

    /* fixes the same problems as module-always-sink */
    card = create_card(base,"PulseAudio");
    add_bridge(card,true,0); // sink
    add_bridge(card,false,0);// source

    pa_namereg_set_default_sink(base->core,card->sink);
    pa_namereg_set_default_source(base->core,card->source);

    return 0;
}

void pa__done(pa_module*m) {
    struct sBase *base;
    struct sCard *card;
    uint32_t idx;

    pa_assert(m);

    if (!(base = m->userdata))
        return;

    if (base->sink_put_slot)
        pa_hook_slot_free(base->sink_put_slot);
    if (base->sink_unlink_slot)
        pa_hook_slot_free(base->sink_unlink_slot);
    if (base->source_put_slot)
        pa_hook_slot_free(base->source_put_slot);
    if (base->source_unlink_slot)
        pa_hook_slot_free(base->source_unlink_slot);
    if (base->sink_input_move_fail_slot)
        pa_hook_slot_free(base->sink_input_move_fail_slot);
    if (base->source_output_move_fail_slot)
        pa_hook_slot_free(base->source_output_move_fail_slot);

    PA_IDXSET_FOREACH(card, base->cards, idx){
        unload_card(card,true);
    }

    if (base->cards)
        pa_idxset_free(base->cards, NULL);
    if (base->ma)
        pa_modargs_free(base->ma);

    pa_xfree(base);
}
