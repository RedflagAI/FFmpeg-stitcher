/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2007 Bobby Bingham
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * stitch one video on top of another
 */

#include <limits.h>
#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "filters.h"
#include "drawutils.h"
#include "framesync.h"
#include "video.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "vf_stitch.h"

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    StitchContext *s = ctx->priv;
    int ret;

    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = ctx->inputs[0]->time_base;

    return ff_framesync_configure(&s->fs);
}

static int config_input_a(AVFilterLink *inlink)
{
  AVFilterContext *ctx = inlink->dst;
  StitchContext *s = ctx->priv;
  int ret;

  if (s->w == 0) {
      s->w = inlink->w;
      s->h = inlink->h;
      return 0;
  } else if (inlink->w != s->w ||
             inlink->h != s->h) {
    av_log(ctx, AV_LOG_ERROR,
           "video dimmensions must match %ix%i vs %ix%i\n",
           inlink->w, inlink->h, s->w, s->h);
    return AVERROR_INVALIDDATA;
  } else
      return 0;
}

static int config_input_b(AVFilterLink *inlink) {
  AVFilterContext *ctx = inlink->dst;
  StitchContext *s = ctx->priv;
  int ret;

  if (s->w == 0) {
    s->w = inlink->w;
    s->h = inlink->h;
    return 0;
  } else if (inlink->w != s->w || inlink->h != s->h) {
    av_log(ctx, AV_LOG_ERROR, "video dimmensions must match %ix%i vs %ix%i\n",
           s->w, s->h, inlink->w, inlink->h);
    return AVERROR_INVALIDDATA;
  } else
    return 0;
}

static int handle_frame(FFFrameSync *fs)
{
    StitchContext *s = fs->parent->priv;
    AVFrame *a = NULL, *b = NULL;
    int64_t difference;
    AVFilterLink *out_link = fs->parent->outputs[0];
    int ret = 0;
    int64_t period;
    AVRational time_base = fs->parent->inputs[0]->time_base;

    ff_framesync_get_frame(fs, 0, &a, 0);
    if (s->last_ts_valid) {
        /* hot path */
        difference = a->pts - s->last_ts;
        s->last_ts = a->pts;

        if (difference >= s->remaining) {
            s->current_pattern_offset =
                (s->current_pattern_offset + 1) % s->plen;

            s->displaying_alternate =
                (s->pattern >> s->current_pattern_offset) & 0x1;

            period = llrint(s->duration * time_base.den / (double)time_base.num);
            s->remaining = period - (difference - s->remaining);
        } else {
            s->remaining -= difference;
        }
    } else {
        /* this is the first frame, do one time setup */
        difference = 0;
        s->last_ts = a->pts;
        s->last_ts_valid = 1;

        period = llrint(s->duration * time_base.den / (double)time_base.num);
        s->remaining = period;

        s->current_pattern_offset = 0;
        s->displaying_alternate = s->pattern & 0x1;
    }

    if (!s->displaying_alternate) {
        ret = ff_filter_frame(out_link, av_frame_clone(a));
    } else {
        ff_framesync_get_frame(fs, 1, &b, 0);
        ret = ff_filter_frame(out_link, av_frame_clone(b));
    }

    if (ret) {
        av_frame_free(&a);
        av_frame_free(&b);
        return ret;
    }

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    StitchContext *s = ctx->priv;


    s->fs.on_event = handle_frame;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
  StitchContext *s = ctx->priv;

  ff_framesync_uninit(&s->fs);
}

static int activate(AVFilterContext *ctx)
{
    StitchContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

#define OFFSET(x) offsetof(StitchContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define TFLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption stitch_options[] = {
    {"duration",
     "how long between switching variants",
     OFFSET(duration),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 5.0},
     0.15,
     100,
     TFLAGS},

    {"pattern",
     "64bit unsigned integer bitmask representing AB pattern. A=0, B=1",
     OFFSET(pattern),
     AV_OPT_TYPE_UINT64,
     {.i64 = 0},
     0,
     UINT_MAX,
     TFLAGS},
    {"plen",
     "how many bits of pattern to repeat, starting from lsb",
     OFFSET(plen),
     AV_OPT_TYPE_UINT64,
     {.i64 = 1},
     1,
     64,
     TFLAGS},
    {NULL}};

FRAMESYNC_DEFINE_CLASS(stitch, StitchContext, fs);

static const AVFilterPad avfilter_vf_stitch_inputs[] = {
    {
        .name         = "a variant",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_a,
    },
    {
        .name         = "b variant",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_b,
    },
};

static const AVFilterPad avfilter_vf_stitch_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_stitch = {
    .name          = "stitch",
    .description   = NULL_IF_CONFIG_SMALL("Stitch a video source on top of the input."),
    .priv_class    = &stitch_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .preinit       = stitch_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(StitchContext),
    .activate      = activate,
    /*
     * .process_command = process_command,
     */
    FILTER_INPUTS(avfilter_vf_stitch_inputs),
    FILTER_OUTPUTS(avfilter_vf_stitch_outputs),
};
