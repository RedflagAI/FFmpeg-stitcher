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
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include "vf_stitch.h"

static const char *const var_names[] = {
    "duration",
    NULL
};


static void eval_expr(AVFilterContext *ctx)
{
    StitchContext *s = ctx->priv;

    s->duration = av_expr_eval(s->dur_pexpr, s->var_values, NULL);
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names,
                        NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when evaluating the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }

    av_expr_free(old);
    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    StitchContext *s = ctx->priv;
    int ret;

    if (!strcmp(cmd, "duration"))
        ret = set_expr(&s->dur_pexpr, args, cmd, ctx);
    else
        ret = AVERROR(ENOSYS);

    if (ret < 0)
        return ret;

    eval_expr(ctx);
    av_log(ctx, AV_LOG_VERBOSE, "duration:%f\n", s->duration);
    return ret;
}

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

static int handle_frame(FFFrameSync *fs)
{
    StitchContext *s;
    AVFrame *a;
    AVFrame *b;
    int64_t difference;
    AVFilterLink *out_link;
    int ret;
    s = fs->parent->priv;
    ff_framesync_get_frame(fs, 0, &a, 1);
    difference = a->pts - s->last_ts;
    out_link = fs->parent->outputs[0];

    if (!s->displaying_alternate) {
        ret = ff_filter_frame(out_link, a);
    } else {
        av_frame_free(&a);
        ff_framesync_get_frame(fs, 1, &b, 1);
        ret = ff_filter_frame(out_link, b);
    }

    if (difference >= s->remaining) {
        s->displaying_alternate = !s->displaying_alternate;
        s->remaining = difference - s->remaining;
    } else {
        s->remaining -= difference;
    }
    return ret;
}

static av_cold int init(AVFilterContext *ctx)
{
    StitchContext *s = ctx->priv;

    int ret;

    s->var_values[0] = 5.0;
    int aw, ah, bw, bh;
    aw = ctx->inputs[0]->w;
    ah = ctx->inputs[0]->h;
    s->w = aw;
    s->h = ah;
    bw = ctx->inputs[1]->w;
    bh = ctx->inputs[1]->h;

    if (aw != bw ||
        ah != bh) {
        av_log(ctx, AV_LOG_ERROR, "video dimmensions must match %ix%i vs %ix%i\n",
               aw, ah, bw, bh);
        return AVERROR_INVALIDDATA;
    }
    if (ctx->inputs[0]->time_base.num != ctx->inputs[1]->time_base.num ||
        ctx->inputs[0]->time_base.den != ctx->inputs[1]->time_base.den) {
      av_log(ctx, AV_LOG_ERROR, "non-matching timebases in stitcher\n");
      return AVERROR_INVALIDDATA;
    }

    eval_expr(ctx);
    av_log(ctx, AV_LOG_VERBOSE, "duration:%f\n", s->duration);
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
    { "duration", "how long between switching variants", OFFSET(dur_expr), AV_OPT_TYPE_DOUBLE, {.dbl = 5.0}, 0.15, 100, TFLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(stitch, StitchContext, fs);

static const AVFilterPad avfilter_vf_stitch_inputs[] = {
    {
        .name         = "a variant",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name         = "b variant",
        .type         = AVMEDIA_TYPE_VIDEO,
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
    .process_command = process_command,
    FILTER_INPUTS(avfilter_vf_stitch_inputs),
    FILTER_OUTPUTS(avfilter_vf_stitch_outputs),
};
