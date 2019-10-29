/***
  This file is part of PulseAudio.

  Copyright 2019 Sergii Pylypenko
  Copyright 2019 VideoLAN

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/rtclock.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

PA_MODULE_AUTHOR("Sergii Pylypenko, VideoLAN");
PA_MODULE_DESCRIPTION("OpenSL ES Android Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
		"sink_name=<name for the sink> "
		"sink_properties=<properties for the sink> "
		"format=<sample format> "
		"rate=<sample rate> "
		"channels=<number of channels> "
		"channel_map=<channel map>"
);

static const char* const valid_modargs[] = {
	"sink_name",
	"sink_properties",
	"format",
	"rate",
	"channels",
	"channel_map",
	NULL
};

enum {
    SINK_MESSAGE_RENDER = PA_SINK_MESSAGE_MAX,
};

#define DEFAULT_SINK_NAME "opensles"

#define OPENSLES_BUFFERS 11 /* maximum number of buffers */
#define OPENSLES_BUFLEN  10   /* ms */
/*
 * 10ms of precision when mesasuring latency should be enough,
 * with 255 buffers we can buffer 2.55s of audio.
 */

#define CHECK_OPENSL_ERROR(msg)                       \
	if (PA_UNLIKELY(result != SL_RESULT_SUCCESS)) {   \
		pa_log(msg " (%lu)", (unsigned long) result); \
		goto error;                                   \
	}

#define Destroy(a) (*a)->Destroy(a);
#define SetPlayState(a, b) (*a)->SetPlayState(a, b)
#define RegisterCallback(a, b, c) (*a)->RegisterCallback(a, b, c)
#define GetInterface(a, b, c) (*a)->GetInterface(a, b, c)
#define Realize(a, b) (*a)->Realize(a, b)
#define CreateOutputMix(a, b, c, d, e) (*a)->CreateOutputMix(a, b, c, d, e)
#define CreateAudioPlayer(a, b, c, d, e, f, g) \
	(*a)->CreateAudioPlayer(a, b, c, d, e, f, g)
#define Enqueue(a, b, c) (*a)->Enqueue(a, b, c)
#define Clear(a) (*a)->Clear(a)
#define GetState(a, b) (*a)->GetState(a, b)
#define SetPositionUpdatePeriod(a, b) (*a)->SetPositionUpdatePeriod(a, b)
#define SetVolumeLevel(a, b) (*a)->SetVolumeLevel(a, b)
#define SetMute(a, b) (*a)->SetMute(a, b)


struct userdata {
	pa_core *core;
	pa_module *module;
	pa_sink *sink;

	pa_thread *thread;
	pa_thread_mq thread_mq;
	pa_rtpoll *rtpoll;

	size_t buffer_size;
	size_t buffer_count;

	pa_memchunk memchunk;

	pa_rtpoll_item *rtpoll_item;
	pa_asyncmsgq *rtpoll_msgq;

	/* OpenSL objects */
	SLObjectItf                     engineObject;
	SLObjectItf                     outputMixObject;
	SLAndroidSimpleBufferQueueItf   playerBufferQueue;
	SLObjectItf                     playerObject;
	SLEngineItf                     engineEngine;
	SLPlayItf                       playerPlay;

	/* audio buffered through opensles */
	uint8_t                        *buf;
	int                             next_buf;

	int                             rate;
};

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
	struct userdata *u = PA_SINK(o)->userdata;

	switch (code) {
		case PA_SINK_MESSAGE_GET_LATENCY:
		{
			size_t n = 0;
			SLresult result;
			SLAndroidSimpleBufferQueueState st;

			result = GetState(u->playerBufferQueue, &st);
			if (PA_UNLIKELY(result != SL_RESULT_SUCCESS)) {
				pa_log("Could not query buffer queue state in %s (%d)", __func__, (int)result);
				return -1;
			}

			n = u->buffer_size * st.count;

			*((int64_t*) data) = pa_bytes_to_usec(n, &u->sink->sample_spec);

			//pa_log_debug("PA_SINK_MESSAGE_GET_LATENCY %lld", *((int64_t*) data));

			return 0;
		}
		case SINK_MESSAGE_RENDER:
			return 0;
	}

	return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void sink_update_requested_latency_cb(pa_sink *s) {
	struct userdata *u;
	pa_usec_t block_usec;

	pa_sink_assert_ref(s);
	pa_assert_se(u = s->userdata);

	block_usec = pa_sink_get_requested_latency_within_thread(s);

	if (block_usec == (pa_usec_t) -1)
		block_usec = s->thread_info.max_latency;

	u->buffer_count = block_usec / 1000 / OPENSLES_BUFLEN;
	if (u->buffer_count < 1)
		u->buffer_count = 1;
	if (u->buffer_count > OPENSLES_BUFFERS - 1)
		u->buffer_count = OPENSLES_BUFFERS - 1;

	pa_sink_set_max_request_within_thread(s, u->buffer_size * u->buffer_count);

	pa_log("%s: set latency to %d usec = %d buffer chunks of %d ms", __func__, (int)block_usec, (int)u->buffer_count, OPENSLES_BUFLEN);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
	struct userdata *u;

	pa_assert(s);
	pa_assert_se(u = s->userdata);

	if (s->thread_info.state == PA_SINK_SUSPENDED || s->thread_info.state == PA_SINK_INIT) {
		if (PA_SINK_IS_OPENED(new_state)) {
		}
	} else if (PA_SINK_IS_OPENED(s->thread_info.state)) {
		if (new_state == PA_SINK_SUSPENDED) {
			/* Continuously dropping data (clear counter on entering suspended state. */
		}
	}

	return 0;
}

static int process_render(struct userdata *u, bool opened) {
	pa_assert(u);

	struct userdata *sys = u; // Just an alias, so I won't need to modify copypasted code
	void *p;
	SLresult result;

	SLAndroidSimpleBufferQueueState st;
	result = GetState(sys->playerBufferQueue, &st);
	if (PA_UNLIKELY(result != SL_RESULT_SUCCESS)) {
		pa_log("Could not query buffer queue state in %s (%d)", __func__, (int)result);
		return -1;
	}

	if (st.count > u->buffer_count) {
		return 0;
	}

	if (opened) {
		pa_sink_render_full(u->sink, sys->buffer_size, &u->memchunk);

		pa_assert(u->memchunk.length > 0);

		p = pa_memblock_acquire(u->memchunk.memblock);
		memcpy(&sys->buf[sys->buffer_size * sys->next_buf], (uint8_t*) p + u->memchunk.index, u->memchunk.length);
		pa_memblock_release(u->memchunk.memblock);

		pa_memblock_unref(u->memchunk.memblock);
		pa_memchunk_reset(&u->memchunk);
	} else {
		pa_silence_memory(&sys->buf[sys->buffer_size * sys->next_buf], sys->buffer_size, &u->sink->sample_spec);
	}

	result = Enqueue(sys->playerBufferQueue,
		&sys->buf[sys->buffer_size * sys->next_buf], sys->buffer_size);

	//pa_log_debug("Play %d bytes, pos %d result %d data %x st.count %d st.index %d", (int) sys->buffer_size, (int) (sys->buffer_size * sys->next_buf), (int) result, * ((int *) &sys->buf[unit_size * sys->next_buf]), (int) st.count, (int) st.index);

	if (result == SL_RESULT_SUCCESS) {
		sys->next_buf += 1;
		if (sys->next_buf >= OPENSLES_BUFFERS)
			sys->next_buf = 0;
	} else {
		/* XXX : if writing fails, we don't retry */
		pa_log("error %d when writing %d bytes %s",
				(int)result, (int)sys->buffer_size,
				(result == SL_RESULT_BUFFER_INSUFFICIENT) ? " (buffer insufficient)" : "");
		return -1;
	}

	return 0;
}

static void thread_func(void *userdata) {
	struct userdata *u = userdata;

	pa_assert(u);

	pa_log_debug("Thread starting up");

	pa_thread_mq_install(&u->thread_mq);

	for (;;) {
		int ret;

		if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
			pa_sink_process_rewind(u->sink, 0);

		/* Render some data and write it */
		if (process_render(u, PA_SINK_IS_OPENED(u->sink->thread_info.state)) < 0)
			goto fail;

		if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
			goto fail;

		if (ret == 0)
			goto finish;
	}

fail:
	pa_log_debug("pa_rtpoll_run() failed");
	/* If this was no regular exit from the loop we have to continue
	 * processing messages until we received PA_MESSAGE_SHUTDOWN */
	pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
	pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
	pa_log_debug("Thread shutting down");
}

static void PlayedCallback (SLAndroidSimpleBufferQueueItf caller, void *pContext)
{
	(void)caller;
	struct userdata *u = (struct userdata *)pContext;

	pa_assert (caller == sys->playerBufferQueue);

	//pa_log_debug("%s", __func__);
	// Unblock pa_rtpoll_run()
	pa_asyncmsgq_send(u->rtpoll_msgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_RENDER, NULL, 0, NULL);
}

int pa__init(pa_module *m) {
	struct userdata *u;
	struct userdata *sys = NULL;
	pa_sample_spec ss;
	pa_channel_map map;
	pa_modargs *ma;
	pa_sink_new_data data;
	pa_thread_func_t thread_routine;
	SLresult result;

	pa_assert(m);

	if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
		pa_log("Failed to parse module arguments.");
		goto fail;
	}

	ss = m->core->default_sample_spec;
	map = m->core->default_channel_map;
	if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
		pa_log("Invalid sample format specification or channel map");
		goto fail;
	}

	u = pa_xnew0(struct userdata, 1);
	sys = u; // Just an alias, so I won't need to modify copypasted code
	u->core = m->core;
	u->module = m;
	m->userdata = u;
	pa_memchunk_reset(&u->memchunk);
	u->rtpoll = pa_rtpoll_new();

	if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
		pa_log("pa_thread_mq_init() failed.");
		goto fail;
	}

	u->rtpoll_msgq = pa_asyncmsgq_new(0);
	if (!u->rtpoll_msgq) {
		pa_log("pa_thread_mq_init() failed.");
		goto fail;
	}

	u->rtpoll_item = pa_rtpoll_item_new_asyncmsgq_read(u->rtpoll, PA_RTPOLL_EARLY-1, u->rtpoll_msgq);

	if (getenv("AUDIO_NATIVE_SAMPLE_RATE") != NULL && atoi(getenv("AUDIO_NATIVE_SAMPLE_RATE")) > 0) {
		ss.rate = atoi(getenv("AUDIO_NATIVE_SAMPLE_RATE"));
		pa_log("Native audio sample rate %d", ss.rate);
	}
	u->rate = ss.rate;
	ss.format = PA_SAMPLE_S16LE;
	ss.channels = 2;

	// Init OpenSL ES

	// create engine
	result = slCreateEngine(&sys->engineObject, 0, NULL, 0, NULL, NULL);
	CHECK_OPENSL_ERROR("Failed to create engine");

	// realize the engine in synchronous mode
	result = Realize(sys->engineObject, SL_BOOLEAN_FALSE);
	CHECK_OPENSL_ERROR("Failed to realize engine");

	// get the engine interface, needed to create other objects
	result = GetInterface(sys->engineObject, SL_IID_ENGINE, &sys->engineEngine);
	CHECK_OPENSL_ERROR("Failed to get the engine interface");

	// create output mix
	result = CreateOutputMix(sys->engineEngine, &sys->outputMixObject, 0, NULL, NULL);
	CHECK_OPENSL_ERROR("Failed to create output mix");

	// realize the output mix in synchronous mode
	result = Realize(sys->outputMixObject, SL_BOOLEAN_FALSE);
	CHECK_OPENSL_ERROR("Failed to realize output mix");

	// configure audio source - this defines the number of samples you can enqueue.
	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
		SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
		OPENSLES_BUFFERS
	};

	SLDataFormat_PCM format_pcm;
	format_pcm.formatType       = SL_DATAFORMAT_PCM;
	format_pcm.numChannels      = 2;
	format_pcm.samplesPerSec    = ((SLuint32) u->rate * 1000);
	format_pcm.bitsPerSample    = SL_PCMSAMPLEFORMAT_FIXED_16;
	format_pcm.containerSize    = SL_PCMSAMPLEFORMAT_FIXED_16;
	format_pcm.channelMask      = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
	format_pcm.endianness       = SL_BYTEORDER_LITTLEENDIAN;

	SLDataSource audioSrc = {&loc_bufq, &format_pcm};

	// configure audio sink
	SLDataLocator_OutputMix loc_outmix = {
		SL_DATALOCATOR_OUTPUTMIX,
		sys->outputMixObject
	};
	SLDataSink audioSnk = {&loc_outmix, NULL};

	//create audio player
	const SLInterfaceID ids2[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
	static const SLboolean req2[] = { SL_BOOLEAN_TRUE };

	result = CreateAudioPlayer(sys->engineEngine, &sys->playerObject, &audioSrc,
								&audioSnk, 1, ids2, req2);
	CHECK_OPENSL_ERROR("Failed to create audio player");

	result = Realize(sys->playerObject, SL_BOOLEAN_FALSE);
	CHECK_OPENSL_ERROR("Failed to realize player object.");

	result = GetInterface(sys->playerObject, SL_IID_PLAY, &sys->playerPlay);
	CHECK_OPENSL_ERROR("Failed to get player interface.");

	result = GetInterface(sys->playerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
												  &sys->playerBufferQueue);
	CHECK_OPENSL_ERROR("Failed to get buff queue interface");

	result = RegisterCallback(sys->playerBufferQueue, PlayedCallback, (void*) u);
	CHECK_OPENSL_ERROR("Failed to register buff queue callback.");

	// set the player's state to playing
	result = SetPlayState(sys->playerPlay, SL_PLAYSTATE_PLAYING);
	CHECK_OPENSL_ERROR("Failed to switch to playing state");

	// Finish initializing PulseAudio sink
	pa_sink_new_data_init(&data);
	data.driver = __FILE__;
	data.module = m;
	pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
	pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, "OpenSLES");
	pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "OpenSLES sink %s", "OpenSLES");
	pa_sink_new_data_set_sample_spec(&data, &ss);
	pa_sink_new_data_set_channel_map(&data, &map);

	if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
		pa_log("Invalid properties");
		pa_sink_new_data_done(&data);
		goto fail;
	}

	u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY);
	pa_sink_new_data_done(&data);

	if (!u->sink) {
		pa_log("Failed to create sink.");
		goto fail;
	}

	u->sink->parent.process_msg = sink_process_msg;
	u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
	u->sink->update_requested_latency = sink_update_requested_latency_cb;
	u->sink->userdata = u;

	pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
	pa_sink_set_rtpoll(u->sink, u->rtpoll);

	u->buffer_count = 1;

	int buffer_size_ms = OPENSLES_BUFLEN;
	u->buffer_size = pa_bytes_per_second(&u->sink->sample_spec) * buffer_size_ms / 1000;
	// Align PulseAudio buffer size to OpenSL ES buf chunk size
	u->buffer_size = pa_frame_align(u->buffer_size, &u->sink->sample_spec);
	pa_sink_set_latency_range(u->sink, pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec),
								(OPENSLES_BUFFERS - 1) * pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec));
	pa_sink_set_max_request(u->sink, u->buffer_size * (OPENSLES_BUFFERS - 1));
	pa_log("OpenSLES buffer size = %d bytes = %d milliseconds", (int) u->buffer_size, buffer_size_ms);

	// OpenSL output buffer
	sys->buf = pa_xmalloc(sys->buffer_size * OPENSLES_BUFFERS);
	sys->next_buf = 0;

	thread_routine = thread_func;

	if (!(u->thread = pa_thread_new("opensles-sink", thread_routine, u))) {
		pa_log("Failed to create thread.");
		goto fail;
	}

	pa_sink_put(u->sink);

	pa_modargs_free(ma);

	return 0;

fail:
error:
	if (ma)
		pa_modargs_free(ma);

	pa__done(m);

	if (sys && sys->playerObject) {
		Destroy(sys->playerObject);
		sys->playerObject = NULL;
		sys->playerBufferQueue = NULL;
		sys->playerPlay = NULL;
	}

	if (sys->outputMixObject)
		Destroy(sys->outputMixObject);
	if (sys->engineObject)
		Destroy(sys->engineObject);

	return -1;
}

int pa__get_n_used(pa_module *m) {
	struct userdata *u;

	pa_assert(m);
	pa_assert_se(u = m->userdata);

	return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module *m) {
	struct userdata *u;

	pa_assert(m);

	if (!(u = m->userdata))
		return;

	// Stop OpenSL ES audio playback
	struct userdata *sys = u; // Just an alias, so I won't need to modify copypasted code

	SetPlayState(sys->playerPlay, SL_PLAYSTATE_STOPPED);
	//Flush remaining buffers if any.
	Clear(sys->playerBufferQueue);

	Destroy(sys->playerObject);
	sys->playerObject = NULL;

	if (u->sink)
		pa_sink_unlink(u->sink);

	if (u->thread) {
		pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
		pa_thread_free(u->thread);
	}

	pa_xfree(sys->buf);

	pa_thread_mq_done(&u->thread_mq);

	if (u->sink)
		pa_sink_unref(u->sink);

	if (u->memchunk.memblock)
		pa_memblock_unref(u->memchunk.memblock);

	if (u->rtpoll_item)
		pa_rtpoll_item_free(u->rtpoll_item);

	if (u->rtpoll_msgq)
		pa_asyncmsgq_unref(u->rtpoll_msgq);

	if (u->rtpoll)
		pa_rtpoll_free(u->rtpoll);

	pa_xfree(u);
}
