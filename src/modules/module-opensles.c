/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

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
#ifdef __ANDROID__
#include <SLES/OpenSLES_Android.h>
#endif


#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>

#include "module-opensles-symdef.h"

PA_MODULE_AUTHOR("Sergii Pylypenko");
PA_MODULE_DESCRIPTION("OpenSL ES Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
    "format=<sample format> "
    "rate=<sample rate> "
    "channels=<number of channels> "
    "buffer=<buffer size> ");

"opensles_output"

struct userdata {
    pa_sink *sink;
    pa_source *source;
    pa_core *core;
    pa_usec_t poll_timeout;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;


static const char* const valid_modargs[] = {
    "format",
    "rate",
    "channels",
    "buffer",
    NULL
};

static int process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
}

static void sink_get_volume_cb(pa_sink *s) {
}

static void sink_set_volume_cb(pa_sink *s) {
}

int pa__get_n_used(pa_module *m) {
}

int pa__init(pa_module *m) {
}
