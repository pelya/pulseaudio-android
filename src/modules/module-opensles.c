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

#define DEFAULT_SINK_NAME "opensles"

#define OPENSLES_BUFFERS 255 /* maximum number of buffers */
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
	size_t bytes_dropped;

	pa_memchunk memchunk;

	pa_usec_t block_usec;
	pa_usec_t timestamp;

	/* OpenSL objects */
	SLObjectItf                     engineObject;
	SLObjectItf                     outputMixObject;
	SLAndroidSimpleBufferQueueItf   playerBufferQueue;
	SLObjectItf                     playerObject;
	SLEngineItf                     engineEngine;
	SLPlayItf                       playerPlay;

	/* audio buffered through opensles */
	uint8_t                        *buf;
	size_t                          samples_per_buf;
	int                             next_buf;

	int                             rate;
};

static int bytesPerSample(void)
{
	return 2 /* S16 */ * 2 /* stereo */;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
	struct userdata *u = PA_SINK(o)->userdata;

	switch (code) {
		case PA_SINK_MESSAGE_GET_LATENCY:
		{
			size_t n = 0;

			// n = latency in bytes

			n += u->memchunk.length;

			*((int64_t*) data) = pa_bytes_to_usec(n, &u->sink->sample_spec);
			return 0;
		}
	}

	return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
	struct userdata *u;

	pa_assert(s);
	pa_assert_se(u = s->userdata);

	if (s->thread_info.state == PA_SINK_SUSPENDED || s->thread_info.state == PA_SINK_INIT) {
		if (PA_SINK_IS_OPENED(new_state))
			u->timestamp = pa_rtclock_now();
	} else if (PA_SINK_IS_OPENED(s->thread_info.state)) {
		if (new_state == PA_SINK_SUSPENDED) {
			/* Continuously dropping data (clear counter on entering suspended state. */
			if (u->bytes_dropped != 0) {
				pa_log_debug("Pipe-sink continuously dropping data - clear statistics (%zu -> 0 bytes dropped)", u->bytes_dropped);
				u->bytes_dropped = 0;
			}
		}
	}

	return 0;
}

static int process_render(struct userdata *u) {
	pa_assert(u);

	struct userdata *sys = u; // Just an alias, so I won't need to modify copypasted code
	const size_t unit_size = sys->samples_per_buf * bytesPerSample();
	void *p;
	SLresult result;

	pa_sink_render_full(u->sink, unit_size, &u->memchunk);

	pa_assert(u->memchunk.length > 0);

	p = pa_memblock_acquire(u->memchunk.memblock);
	memcpy(&sys->buf[unit_size * sys->next_buf], (uint8_t*) p + u->memchunk.index, u->memchunk.length);
	pa_memblock_release(u->memchunk.memblock);

	pa_memblock_unref(u->memchunk.memblock);
	pa_memchunk_reset(&u->memchunk);

	SLAndroidSimpleBufferQueueState st;
	result = GetState(sys->playerBufferQueue, &st);
	if (PA_UNLIKELY(result != SL_RESULT_SUCCESS)) {
		pa_log("Could not query buffer queue state in %s (%d)", __func__, (int)result);
		return -1;
	}

	if (st.count == OPENSLES_BUFFERS)
		return -1;

	result = Enqueue(sys->playerBufferQueue,
		&sys->buf[unit_size * sys->next_buf], unit_size);

	//pa_log_debug("Play %d bytes, pos %d result %d data %x", unit_size, (int)(unit_size * sys->next_buf), (int) result, * ((int *) &sys->buf[unit_size * sys->next_buf]));

	if (result == SL_RESULT_SUCCESS) {
		if (++sys->next_buf == OPENSLES_BUFFERS)
			sys->next_buf = 0;
	} else {
		/* XXX : if writing fails, we don't retry */
		pa_log("error %d when writing %d bytes %s",
				(int)result, (int)unit_size,
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

		/* Render some data and write it to the fifo */
		if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
			if (process_render(u) < 0)
				goto fail;
		}

		if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
			goto fail;

		if (ret == 0)
			goto finish;
	}

fail:
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
	struct userdata *sys = (struct userdata *)pContext;

	pa_assert (caller == sys->playerBufferQueue);

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

	/* XXX: rounding shouldn't affect us at normal sampling rate */
	sys->samples_per_buf = OPENSLES_BUFLEN * u->rate / 1000;
	sys->buf = pa_xmalloc(sys->samples_per_buf * bytesPerSample() * OPENSLES_BUFFERS);

	sys->next_buf = 0;

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

	u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY);
	pa_sink_new_data_done(&data);

	if (!u->sink) {
		pa_log("Failed to create sink.");
		goto fail;
	}

	u->sink->parent.process_msg = sink_process_msg;
	u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
	u->sink->userdata = u;

	pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
	pa_sink_set_rtpoll(u->sink, u->rtpoll);

	u->bytes_dropped = 0;

	int buffer_size_ms = 100;
	if (getenv("AUDIO_BUFFER_SIZE_MS") != NULL && atoi(getenv("AUDIO_BUFFER_SIZE_MS")) > 0) {
		buffer_size_ms = atoi(getenv("AUDIO_BUFFER_SIZE_MS"));
	}
	u->buffer_size = pa_bytes_per_second(&u->sink->sample_spec) * buffer_size_ms / 1000;
	// Align PulseAudio buffer size to OpenSL ES buf chunk size
	u->buffer_size = (u->buffer_size / (sys->samples_per_buf * bytesPerSample())) * (sys->samples_per_buf * bytesPerSample());
	u->buffer_size = pa_frame_align(u->buffer_size, &u->sink->sample_spec);
	if (u->buffer_size % (sys->samples_per_buf * bytesPerSample()) != 0) {
		pa_log("PulseAudio buffer size %d does not divide evenly by OpenSL ES buffer frame size %d", (int)u->buffer_size, (int)sys->samples_per_buf * bytesPerSample());
		goto fail;
	}
	pa_sink_set_fixed_latency(u->sink, pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec));
	thread_routine = thread_func;
	pa_sink_set_max_request(u->sink, u->buffer_size);
	pa_log("OpenSLES buffer size = %d bytes = %d milliseconds, change with setenv AUDIO_BUFFER_SIZE_MS", (int) u->buffer_size, buffer_size_ms);

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

	if (u->rtpoll)
		pa_rtpoll_free(u->rtpoll);

	pa_xfree(u);
}
