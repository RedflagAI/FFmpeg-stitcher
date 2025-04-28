/*
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
 * audio and video lumter
 */

#include <stdio.h>

#include "libavutil/attributes.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"
#include <math.h>

typedef struct LumContext {
    const AVClass *class;

    double period;
    double offset;
    double amplitude;

    int64_t period_ts;
    int64_t offset_ts;
    int64_t last_ts;
    int64_t remaining;

    double *sin_table;
    unsigned int sin_table_size;
    unsigned int idx;
    int last_ts_valid;
} LumContext;

typedef struct ThreadData {
    AVFrame *in, *out_pos, *out_neg;
    double modulation;
} ThreadData;

const double RGBToXYZMatrix[9] = {
  0.412453, 0.357580, 0.180423,
  0.212671, 0.715160, 0.072169,
  0.019334, 0.119193, 0.950227
};

const double XYZToRGBMatrix[9] = {
  3.240481,  -1.537151, -0.498536,
  -0.969254,   1.875990, 0.041555,
  0.055646, -0.204041,   1.057311,
};

static double _rgb_to_lab_f (double t) {
    return (t > 0.008856f) ?
        cbrt(t) :              /* cube root */
        ((7.787f * t) + (16.0f / 116.0f));
};
static void rgb_to_lab(const uint8_t rgb[3], double lab[3]) {
    double R = ((double)rgb[0]) / 255.0;
    double G = ((double)rgb[1]) / 255.0;
    double B = ((double)rgb[2]) / 255.0;

    double X  = RGBToXYZMatrix[0] * R + RGBToXYZMatrix[1] * G + RGBToXYZMatrix[2] * B; // [0,1]
    double Y_ = RGBToXYZMatrix[3] * R + RGBToXYZMatrix[4] * G + RGBToXYZMatrix[5] * B; // [0,1]
    double Z  = RGBToXYZMatrix[6] * R + RGBToXYZMatrix[7] * G + RGBToXYZMatrix[8] * B; // [0,1]

    X  /= 0.950455f;
    Y_ /= 1.0f;
    Z  /= 1.088753f;

    double fX = _rgb_to_lab_f(X);
    double fY = _rgb_to_lab_f(Y_);
    double fZ = _rgb_to_lab_f(Z);

    lab[0] = (Y_ > 0.008856) ?
        (116.0 * cbrt(Y_)) - 16.0 :
        903.3 * Y_;
    lab[1] = (500.0 * (fX - fY));
    lab[2] = (200.0 * (fY - fZ));
}

static void lab_to_rgb(const double lab[3], uint8_t rgb[3]) {

    double fY = (lab[0] + 16.0) / 116.0;
    double fX = lab[1] / 500.0 + fY;
    double fZ = fY - lab[2] / 200.0;

    double fX3 = pow(fX, 3.0);
    double fZ3 = pow(fZ, 3.0);

    double X  = (fX3 > 0.008856) ? fX3 : ((116 * fX) - 16) / 903.3;
    double Y_ = (lab[0] > 903.3*0.008856) ? pow((lab[0] + 16)/116.0, 3.0) : lab[0] / 903.3;
    double Z  = (fZ3 > 0.008856) ? fZ3 : ((116 * fZ) - 16) / 903.3;

    X  *= 0.950455;            // [0,1]
    Y_ *= 1.0;                 // [0,1]
    Z  *= 1.088753;            // [0,1]

    double R = XYZToRGBMatrix[0] * X + XYZToRGBMatrix[1] * Y_ + XYZToRGBMatrix[2] * Z; // [0,1]
    double G = XYZToRGBMatrix[3] * X + XYZToRGBMatrix[4] * Y_ + XYZToRGBMatrix[5] * Z; // [0,1]
    double B = XYZToRGBMatrix[6] * X + XYZToRGBMatrix[7] * Y_ + XYZToRGBMatrix[8] * Z; // [0,1]

    rgb[0] = R * 255.0;
    rgb[1] = G * 255.0;
    rgb[2] = B * 255.0;
}

static av_cold int lum_init(AVFilterContext *ctx) {
    LumContext *s = ctx->priv;

    s->sin_table_size = 64;
    if (!s->sin_table) {
        s->sin_table = malloc(s->sin_table_size * sizeof(double));
        if (!s->sin_table) return 1;
        for (unsigned i = 0; i < s->sin_table_size; i++) {
            s->sin_table[i] = s->amplitude * sin(2.0 * M_PI * (double)i / (double) s->sin_table_size);
        }

        av_log(NULL, AV_LOG_DEBUG, "lum init\n");
        s->last_ts_valid = 0;
        s->idx = 0;
    }

    return 0;
}

static av_cold void lum_uninit(AVFilterContext *ctx) {
    LumContext *s = ctx->priv;
    av_log(NULL, AV_LOG_DEBUG, "lum uninit\n");
    free(s->sin_table);
}

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr,
                        int nb_jobs) {
    LumContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    const ThreadData *td = arg;
    const AVFrame *in = td->in;
    const int slice_start = (in->height * jobnr) / nb_jobs;
    const int slice_end = (in->height * (jobnr + 1)) / nb_jobs;
    const uint8_t *src = in->data[0] + slice_start * in->linesize[0];
    uint8_t *dst_pos =
        td->out_pos->data[0] + slice_start * td->out_pos->linesize[0];
    uint8_t *dst_neg =
        td->out_neg->data[0] + slice_start * td->out_neg->linesize[0];
    int x, y;

    for (y = slice_start; y < slice_end; y++) {
        for (x = 0; x < inlink->w; x++) {
            int r = 3*x;
            double lab[3];
            rgb_to_lab(&src[r], lab);
            double hold = lab[0];

            lab[0] = hold + td->modulation;
            lab[0] = (lab[0] > 100.0) ? 100.0 : (lab[0] < 0.0) ? 0.0 : lab[0];
            lab_to_rgb(lab, &dst_pos[r]);

            lab[0] = hold - td->modulation;
            lab[0] = (lab[0] > 100.0) ? 100.0 : (lab[0] < 0.0) ? 0.0 : lab[0];
            lab_to_rgb(lab, &dst_neg[r]);
        }

        dst_pos += td->out_pos->linesize[0];
        dst_neg += td->out_neg->linesize[0];
        src += in->linesize[0];
    }
    return 0;
}
/* TODO query formats for rgb24 */
/* TODO mutate_frame_[pos/neg] */


static int activate(AVFilterContext *ctx) {
    LumContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFrame *in;
    int status, ret, nb_eofs = 0;
    int64_t pts;
    ThreadData td;
    unsigned sin_idx;

    for (int i = 0; i < 2; i++)
        nb_eofs += ff_outlink_get_status(ctx->outputs[i]) == AVERROR_EOF;

    if (nb_eofs == 2) {
        av_log(NULL, AV_LOG_DEBUG, "Reached EOF of both outputs\n");
        ff_inlink_set_status(inlink, AVERROR_EOF);
        return 0;
    }

    ret = ff_inlink_consume_frame(inlink, &in);
    if (ret < 0)
        return ret;
    if (ret > 0) {
        for (int i = 0; i < 2; i++) {
            if (ff_outlink_get_status(ctx->outputs[i])) {
                av_log(NULL, AV_LOG_DEBUG, "output %i status was non zero\n", i);
                continue;
            }
        }
        if (s->last_ts_valid) {
            /* hot path */
            int64_t difference;
            difference = in->pts - s->last_ts;
            s->last_ts = in->pts;

            s->offset_ts += difference;
            while (s->offset_ts >= s->period_ts)
                s->offset_ts -= s->period_ts;
        } else {
            double seconds_to_ts = inlink->time_base.den / (double)inlink->time_base.num;

            s->last_ts = in->pts;
            s->last_ts_valid = 1;

            s->period_ts = llrint(s->period * seconds_to_ts);
            s->offset_ts = llrint(s->offset * seconds_to_ts);
        }

        double idx_d = s->offset_ts / (double)s->period_ts;
        sin_idx = (unsigned) (idx_d * s->sin_table_size);

        td.in = in;
        td.out_pos = av_frame_clone(in);
        td.out_neg = av_frame_clone(in);
        td.modulation = s->sin_table[sin_idx];
        if (!td.out_pos || !td.out_neg) {
            ret = AVERROR(ENOMEM);
            av_frame_free(&in);
            av_frame_free(&td.out_pos);
            av_frame_free(&td.out_neg);
            return ret;
        }
        ret = av_frame_make_writable(td.out_pos);
        if (ret < 0)
            return ret;
        ret = av_frame_make_writable(td.out_neg);
        if (ret < 0)
            return ret;

        /*
         * for (int i = 0; i < ff_filter_get_nb_threads(ctx); i++) {
         *     filter_slice(ctx, &td, i, ff_filter_get_nb_threads(ctx));
         * }
         */
        const int n_slices = FFMIN(in->height, ff_filter_get_nb_threads(ctx));
        int *return_vals = malloc(n_slices * sizeof(int));
        if (!return_vals) {
            ret = AVERROR(ENOMEM);
            av_frame_free(&in);
            av_frame_free(&td.out_pos);
            av_frame_free(&td.out_neg);
            return ret;
        }
        ff_filter_execute(ctx, filter_slice, &td, return_vals, n_slices);
        for (int i = 0; i < n_slices; i++)
            if (return_vals[i] < 0)
                return return_vals[i];
        free(return_vals);

        ret = ff_filter_frame(ctx->outputs[0], td.out_pos);
        if (ret < 0)
            return ret;
        ret = ff_filter_frame(ctx->outputs[1], td.out_neg);
        if (ret < 0)
            return ret;

        av_frame_free(&in);
        if (ret < 0)
            return ret;}


    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        for (int i = 0; i < 2; i++) {
            if (ff_outlink_get_status(ctx->outputs[i]))
                continue;
            ff_outlink_set_status(ctx->outputs[i], status, pts);
        }
        return 0;
    }

    for (int i = 0; i < 2; i++) {
        if (ff_outlink_get_status(ctx->outputs[i]))
            continue;

        if (ff_outlink_frame_wanted(ctx->outputs[i])) {
            ff_inlink_request_frame(inlink);
            return 0;
        }
    }

    return FFERROR_NOT_READY;
}

static int query_formats(AVFilterContext *ctx) {
    int formats[2] = {AV_PIX_FMT_RGB24, -1};
    return ff_set_common_formats_from_list(ctx, formats);
}

static int config_output(AVFilterLink *outlink) {
  AVFilterContext *ctx = outlink->src;
  LumContext *s = ctx->priv;

  ctx->outputs[0]->w = ctx->inputs[0]->w;
  ctx->outputs[0]->h = ctx->inputs[0]->h;
  ctx->outputs[1]->w = ctx->inputs[0]->w;
  ctx->outputs[1]->h = ctx->inputs[0]->h;

  return 0;
}

#define OFFSET(x) offsetof(LumContext, x)
#define OFLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption options[] = {
    {"period",
     "period of the encoded sine wave in seconds",
     OFFSET(period),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 5.0 / 3.0},
     0.15,
     100.0,
     OFLAGS},
    {"offset",
     "offset into initial duration in seconds",
     OFFSET(offset),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 0.0},
     0.0,
     100.0,
     OFLAGS},
    {"amp",
     "amplitude of encoded sin wave",
     OFFSET(amplitude),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 0.10},
     0.0,
     1.0,
     OFLAGS},
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name = "positive",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    {
        .name = "negative",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

AVFILTER_DEFINE_CLASS_EXT(lum, "lum", options);

/* TODO outputs */
const AVFilter ff_vf_lum = {
    .name = "lum",
    .description =
        NULL_IF_CONFIG_SMALL("Produce a positive and negative luma sine encoding"),
    .priv_size = sizeof(LumContext),
    .priv_class = &lum_class,
    .init = lum_init,
    .uninit = lum_uninit,
    .activate = activate,
    FILTER_INPUTS(ff_video_default_filterpad),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
    .flags = AVFILTER_FLAG_SLICE_THREADS,
};
