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
    AVFilterContext *ctx = fs->parent;
    StitchContext *s = ctx->priv;
    AVFrame *a = NULL, *b = NULL, *out = NULL;
    int64_t difference;
    AVFilterLink *out_link = ctx->outputs[0];
    int ret = 0;

    ret = ff_framesync_get_frame(fs, 0, &a, 1);
    if (ret < 0)
        return ret;
    ret = ff_framesync_get_frame(fs, 1, &b, 1);
    if (ret < 0)
        return ret;
    a->pts = av_rescale_q(fs->pts, fs->time_base, ctx->outputs[0]->time_base);
    b->pts = av_rescale_q(fs->pts, fs->time_base, ctx->outputs[0]->time_base);

    if (s->last_ts_valid) {
        /* hot path */
        difference = s->fs.pts - s->last_ts;
        s->last_ts = s->fs.pts;

        if (difference < s->remaining)
            s->remaining -= difference;
        else {
            s->current_pattern_offset =
                (s->current_pattern_offset + 1) % s->plen;

            s->displaying_alternate =
                (s->pattern >> s->current_pattern_offset) & 0x1;

            s->remaining = s->period - (difference - s->remaining);
        }
    } else {
        /* this is the first frame, do one time setup.

          This can't be in init because the framesync timebase isn't
          set there. */
        difference = 0;
        s->last_ts = s->fs.pts;
        s->last_ts_valid = 1;

        double seconds_to_ts = fs->time_base.den / (double)fs->time_base.num;
        int64_t offset_ts = llrint(s->offset * seconds_to_ts);

        s->period = llrint(s->duration * seconds_to_ts);
        s->remaining = s->period - offset_ts;
        /* we compensate the onetime difference being zero with a
           manual offset. We can't be sure a->duration is set or that
           it's the appropriate length for the output frame since the
           a frame may be longer than the b frame. */

        s->current_pattern_offset = 0;
        s->displaying_alternate = s->pattern & 0x1;
    }

    if (s->displaying_alternate) {
        out = b;
        av_frame_free(&a);
    } else {
        out = a;
        av_frame_free(&b);
    }
    if (!out)
        return AVERROR(ENOMEM);

    ret = ff_filter_frame(out_link, out);
    if (ret < 0)
        return ret;

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    StitchContext *s = ctx->priv;
    const FFFrameSync fs = s->fs;

    if (s->offset > s->duration) {
        av_log(ctx, AV_LOG_ERROR,
               "offset %f greater than duration %f", s->offset, s->duration);
        return AVERROR_INVALIDDATA;
    } else if (s->offset == s->duration) {
        s->offset = 0.0;
    }
    s->last_ts_valid = 0;
    /* should be optimized out, but just to be explicit */

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
     "number of seconds between next pattern bit",
     OFFSET(duration),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 5.0},
     0.15,
     100.0,
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
    {"offset",
     "offset into initial duration in seconds",
     OFFSET(offset),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 0.0},
     0.0,
     100.0,
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
