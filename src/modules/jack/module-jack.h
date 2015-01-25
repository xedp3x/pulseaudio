#define PA_PROP_JACK_CLIENT "jack.name"
#define PA_USEC_INVALID ((pa_usec_t) -1)
#define PA_USEC_PER_SEC ((pa_usec_t) 1000000ULL)

struct sBase {
	pa_core *core;
	pa_module *module;
	pa_modargs *ma;

	bool autoconnect;
	char server_name;
	uint32_t merge;
	pa_usec_t delay;

	pa_idxset *cards;

    pa_hook_slot
		*sink_put_slot,
		*sink_unlink_slot,
		*source_put_slot,
		*source_unlink_slot,
		*sink_input_move_fail_slot,
		*source_output_move_fail_slot;
};

struct sCard {
	void *base;
	char const *name;
    char *merge_ref;

	pa_sink *sink;
	pa_source *source;

	pa_idxset *inputs, *outputs;

	pa_time_event *time_event;
	pa_rtpoll_item *rtpoll_item;

	pa_thread_mq thread_mq;
	pa_thread *thread;
	pa_asyncmsgq *jack_msgq;
	pa_rtpoll *rtpoll;

	jack_client_t *jack;
	jack_port_t *sink_port[PA_CHANNELS_MAX], *source_port[PA_CHANNELS_MAX];
	jack_nframes_t frames_in_buffer;
	jack_nframes_t saved_frame_time;
	bool saved_frame_time_valid;

	unsigned sink_channels, source_channels;
	void *sink_buffer[PA_CHANNELS_MAX], *source_buffer[PA_CHANNELS_MAX];
};


enum {
	SOURCE_MESSAGE_POST = PA_SOURCE_MESSAGE_MAX,
	SOURCE_MESSAGE_ON_SHUTDOWN,
	SINK_MESSAGE_RENDER = PA_SINK_MESSAGE_MAX,
	SINK_MESSAGE_BUFFER_SIZE,
	SINK_MESSAGE_ON_SHUTDOWN
};

void* create_card(void* arg, const char *name);
void* add_bridge(void *arg, bool sink, uint8_t channels);
void unload_card(void* arg,bool forced);
static void timeout_cb(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata);
const char* get_merge_ref(pa_proplist *p, struct sBase *base);
