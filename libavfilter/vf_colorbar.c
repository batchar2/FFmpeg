/*
 * Copyright (c) 2012-2014 Clément Bœsch <u pkh me>
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
 * Edge detection filter
 *
 * @see https://en.wikipedia.org/wiki/Canny_edge_detector
 */

#include <cv.h>
#include <highgui.h>

#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/avassert.h"


#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum MethdType {
    METHOD_HASH = 0,
    METHOD_PHASH = 1
};

struct plane_info {
    uint8_t  *tmpbuf;
    uint16_t *gradients;
    char     *directions;
};

/** Контекст исполнения, на него вешаются опции */
typedef struct ColorbarContext {
    const AVClass *class;
    char *file;
    char *method_name;

    int threshold;
    int method;
    uint64_t hash;
} ColorbarContext;


#define FLAGS      (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption colorbar_options[] = {
   {"file", "path to colorbars", offsetof(ColorbarContext, file), AV_OPT_TYPE_STRING},
   {"threshold", "threshold value", offsetof(ColorbarContext, threshold), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 64, FLAGS},
   {NULL}
};

AVFILTER_DEFINE_CLASS(colorbar);


static int64 haming_distance(uint64_t hash1, uint64_t hash2);
static uint64_t calc_image_phash(IplImage *image);

static av_cold int init(AVFilterContext *ctx)
{
    IplImage *colorbar_img = NULL;
    ColorbarContext *option = ctx->priv;

    if (option->threshold == -1) {
        av_log(NULL, AV_LOG_ERROR, "Colorbar option error! Not set threshold! Use option: \"threshold\"  \n");
        return AVERROR(EINVAL);
    }

    if (option->file == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Colorbar option error! Not set option file! Use option: \"file\"  \n");
        return AVERROR(EINVAL);
    } 
    
    colorbar_img = cvLoadImage(option->file, 1);
    if (colorbar_img == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Colorbar file not found!\n");
        return AVERROR(EINVAL);
    }
    option->hash = calc_image_phash(colorbar_img);

    cvReleaseImage(&colorbar_img);
    
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_NONE
    };
    return ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
}


static int config_props(AVFilterLink *inlink)
{
    return 0;
}


static IplImage* avframe2image(const AVFrame *frame, enum AVPixelFormat pixfmt)
{
    IplImage *header = NULL;
    int depth = 0, channels_nb = 0;

    if (pixfmt == AV_PIX_FMT_GRAY8) { 
        depth = IPL_DEPTH_8U;  
        channels_nb = 1; 
    } else if (pixfmt == AV_PIX_FMT_BGRA)  { 
        depth = IPL_DEPTH_8U;  
        channels_nb = 4; 
    } else if (pixfmt == AV_PIX_FMT_BGR24) { 
        depth = IPL_DEPTH_8U;  
        channels_nb = 3; 
    } else {
        av_log(NULL, AV_LOG_ERROR, "pixfmt error");
        return NULL;
    }

    header = cvCreateImageHeader((CvSize){frame->width, frame->height}, depth, channels_nb);
    header->imageData = header->imageDataOrigin = frame->data[0];
    header->dataOrder = IPL_DATA_ORDER_PIXEL;
    header->origin    = IPL_ORIGIN_TL;
    header->widthStep = frame->linesize[0];

    return header;
}

static int64 haming_distance(uint64_t hash1, uint64_t hash2) 
{
    int64_t dist = 0;
    int64_t val = hash1 ^ hash2;
    // Count the number of set bits
    while(val) {
        ++dist; 
        val &= val - 1;
    }
    return dist;
}

static uint64_t calc_image_phash(IplImage *img)
{
    const int DCT_SIZE = 32;
    const int ROI_SIZE = 8;

    int x = 0, y = 0;
    double avg = 0;
    uint64_t phash = 0, phash_mask = 1;

    IplImage *mono = NULL;
    IplImage *small = NULL;
    CvMat *dct = NULL;

    mono  = cvCreateImage(cvSize(img->width, img->height), img->depth, 1);
    small = cvCreateImage(cvSize(DCT_SIZE, DCT_SIZE), img->depth, 1);

    if (img->nChannels == 1) {
        cvCopy(img, mono, 0);
    } else { 
        cvCvtColor(img, mono, CV_RGB2GRAY);
    }

    cvResize(mono, small, CV_INTER_CUBIC);

    dct = cvCreateMat(DCT_SIZE, DCT_SIZE, CV_32FC1);
    cvConvertScale(small, dct, 1.0/255.0, 0.0);
   
    cvDCT(dct, dct, CV_DXT_ROWS);
    
    avg = cvAvg(dct, NULL).val[0];

    for (x = ROI_SIZE-1; x >= 0; x--) {
        for (y = ROI_SIZE-1; y >= 0; y--) {
            if (cvGet2D(dct, x, y).val[0] > avg)
                phash |= phash_mask;
            phash_mask = phash_mask << 1;
        }
    }
    cvReleaseMat(&dct);
    cvReleaseImage(&mono);
    cvReleaseImage(&small);

    return phash;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ColorbarContext *option = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL;
    int direct = 0;
    uint64_t hash_img = 0, distance = 0;
    IplImage *img = NULL;

    if (av_frame_is_writable(in)) {
        direct = 1;
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }
    img = avframe2image(in, AV_PIX_FMT_BGR24);

    if (img != NULL) {
        hash_img = calc_image_phash(img);
        distance = haming_distance(hash_img, option->hash);
        if (distance <= option->threshold) {
            av_log(NULL, AV_LOG_ERROR, "Colorbar detect! Distance = %lu\n", distance);
        }
        cvReleaseImageHeader(&img);
    }
    if (!direct)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);    
}

static av_cold void uninit(AVFilterContext *ctx)
{
}
 
static const AVFilterPad colorbar_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad colorbar_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_colorbar = {
    .name          = "colorbar",
    .description   = NULL_IF_CONFIG_SMALL("Detect colorbars."),
    .priv_size     = sizeof(ColorbarContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = colorbar_inputs,
    .outputs       = colorbar_outputs,
    .priv_class    = &colorbar_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
