/*
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

#ifndef AVFILTER_STITCH_H
#define AVFILTER_STITCH_H

#include "framesync.h"
#include "avfilter.h"

enum var_name {
    VAR_DURATION,
    VAR_VARS_NB
};

typedef struct StitchContext {
    const AVClass *class;

    FFFrameSync fs;
    int64_t remaining;
    int64_t last_ts;

    double duration;
    double var_values[1];
    char *dur_expr;
    AVExpr *dur_pexpr;

    int displaying_alternate;
    int w, h;
} StitchContext;

#endif /* AVFILTER_STITCH_H */
