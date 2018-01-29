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

enum FilterMode {
    MODE_WIRES,
    MODE_COLORMIX,
    NB_MODE
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
    char *method;

    uint64_t hash;
} ColorbarContext;


static const AVOption colorbar_options[] = {
   {"file", "path to colorbars", offsetof(ColorbarContext, file), AV_OPT_TYPE_STRING},
   {"method", "math method", offsetof(ColorbarContext, method), AV_OPT_TYPE_STRING},
   {NULL}
};

AVFILTER_DEFINE_CLASS(colorbar);


static int64 haming_distance(uint64_t hash1, uint64_t hash2);
static uint64_t calc_image_phash(IplImage *image);


static av_cold int init(AVFilterContext *ctx)
{    
    IplImage *colorbar_img = NULL;

    //av_log(NULL, AV_LOG_ERROR, "HELLO, World! Init\n");
    ColorbarContext *colorbar = ctx->priv;

    if (colorbar->file == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Colorbar file not defined! Use option: file\n");
        return 1;
    } 
    
    colorbar_img = cvLoadImage(colorbar->file, 1);
    if (colorbar_img == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Colorbar file not found!\n");
        return 1;
    } 
    colorbar->hash = calc_image_phash(colorbar_img);

    av_log(NULL, AV_LOG_ERROR, "hash_colorbar = %llu\n", colorbar->hash);
    av_log(NULL, AV_LOG_ERROR, "HELLO, World! Init %s\n", colorbar->file);
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    const ColorbarContext *colorbar = ctx->priv;
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_BGR24, AV_PIX_FMT_NONE};
    AVFilterFormats *fmts_list = NULL;

    fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);

}


static int config_props(AVFilterLink *inlink)
{
    /*
    int p;
    AVFilterContext *ctx = inlink->dst;
    ColorbarContext *colorbar = ctx->priv;

    colorbar->nb_planes = inlink->format == AV_PIX_FMT_GRAY8 ? 1 : 3;
    for (p = 0; p < colorbar->nb_planes; p++) {
        struct plane_info *plane = &colorbar->planes[p];

        plane->tmpbuf     = av_malloc(inlink->w * inlink->h);
        plane->gradients  = av_calloc(inlink->w * inlink->h, sizeof(*plane->gradients));
        plane->directions = av_malloc(inlink->w * inlink->h);
        if (!plane->tmpbuf || !plane->gradients || !plane->directions)
            return AVERROR(ENOMEM);
    }
    */
    return 0;
}


enum {
    DIRECTION_45UP,
    DIRECTION_45DOWN,
    DIRECTION_HORIZONTAL,
    DIRECTION_VERTICAL,
};

/*
static void AVFrame2IplImage(AVFrame* avFrame, IplImage* iplImage)
{
    struct SwsContext* img_convert_ctx = 0;
    int linesize[4] = {0, 0, 0, 0};

    img_convert_ctx = sws_getContext(avFrame->width, 
                                        avFrame->height,
                                        (PixelFormat)avFrame->format,
                                        iplImage->width,
                                        iplImage->height,
                                        PIX_FMT_BGR24, 
                                        SWS_BICUBIC, 
                                        0, 0, 0);
    if (img_convert_ctx != 0) {
        linesize[0] = 3 * iplImage->width;
        sws_scale(img_convert_ctx, 
                    avFrame->data,
                    avFrame->linesize,
                    0,
                    avFrame->height, 
                    (uint8_t *const*)(&(iplImage->imageData)), 
                    linesize);
        sws_freeContext(img_convert_ctx);
    }
}
*/

static void fill_iplimage_from_frame(IplImage *img, const AVFrame *frame, enum AVPixelFormat pixfmt)
{
    IplImage *tmpimg = NULL;
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
        return;
    }

    tmpimg = cvCreateImageHeader((CvSize){frame->width, frame->height}, depth, channels_nb);
    *img = *tmpimg;
    img->imageData = img->imageDataOrigin = frame->data[0];
    img->dataOrder = IPL_DATA_ORDER_PIXEL;
    img->origin    = IPL_ORIGIN_TL;
    img->widthStep = frame->linesize[0];
}

static int64 haming_distance(uint64_t hash1, uint64_t hash2) 
{
    int64 dist = 0, val = hash1 ^ hash2;
    // Count the number of set bits
    while(val) {
        ++dist; 
        val &= val - 1;
    }
    return dist;
}

static uint64_t calc_image_phash(IplImage *image)
{
    uint64_t hash = 0;
    CvSize size_img = cvSize(8, 8);
    CvScalar average;
    int i = 0;
    uint64_t one = 1;

    //
    IplImage *res = cvCreateImage(size_img, image->depth, image->nChannels);
    IplImage *gray = cvCreateImage(size_img, IPL_DEPTH_8U, 1);
    IplImage *bin = cvCreateImage(size_img, IPL_DEPTH_8U, 1);

    // resize image
    cvResize(image, res, CV_INTER_LINEAR);
    
    // to gray
    cvCvtColor(res, gray, CV_BGR2GRAY);
    // среднее-арифметическое
    average = cvAvg(gray, NULL);

    // получим бинарное изображение относительно среднего
    // для этого воспользуемся пороговым преобразованием
    cvThreshold(gray, bin, average.val[0], 255, CV_THRESH_BINARY);

    // пробегаемся по всем пикселям изображения
    for( int y=0; y<bin->height; y++ ) {
        uchar* ptr = (uchar*) (bin->imageData + y * bin->widthStep);
        for( int x=0; x<bin->width; x++ ) {
            // 1 канал
            if(ptr[x]){
                hash |= one<<i;  // warning C4334: '<<' : result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)
                //hash |= 1i64<<i; 
            }
            i++;
        }
    }   
    return hash;
}


static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    av_log(NULL, AV_LOG_ERROR, "FILTER!\n");
    AVFilterContext *ctx = inlink->dst;
    ColorbarContext *colorbar = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int direct = 0;
    AVFrame *out = NULL;

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
    
    int depth = IPL_DEPTH_8U, channels_nb = 3;
    IplImage *img = cvCreateImageHeader((CvSize){in->width, in->height}, depth, channels_nb);
    img->imageData = img->imageDataOrigin = in->data[0];
    img->dataOrder = IPL_DATA_ORDER_PIXEL;
    img->origin    = IPL_ORIGIN_TL;
    img->widthStep = in->linesize[0];


    uint64_t hash_img = calc_image_phash(img);
    
    //uint64_t hash_colorbar = calc_image_phash(colorbar_img);
    uint64_t calc_hash =  haming_distance(hash_img, colorbar->hash);

    //av_log(NULL, AV_LOG_ERROR, "Hash = " PRId64 "\n", hash);
    av_log(NULL, AV_LOG_ERROR, "hash_img = %llu\n", hash_img);
    av_log(NULL, AV_LOG_ERROR, "calc_hash = %llu\n", calc_hash);    

    /*
    int p[3];
    p[0] = CV_IMWRITE_JPEG_QUALITY;
    p[1] = 100;
    p[2] = 0;
    cvSaveImage("WOw.jpg", img, p);
    */
    if (!direct)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);    
}

static av_cold void uninit(AVFilterContext *ctx)
{
    /*
    int p;
    ColorbarContext *colorbar = ctx->priv;

    for (p = 0; p < colorbar->nb_planes; p++) {
        struct plane_info *plane = &colorbar->planes[p];
        av_freep(&plane->tmpbuf);
        av_freep(&plane->gradients);
        av_freep(&plane->directions);
    }
    */
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
    .description   = NULL_IF_CONFIG_SMALL("Detect and draw edge."),
    .priv_size     = sizeof(ColorbarContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = colorbar_inputs,
    .outputs       = colorbar_outputs,
    .priv_class    = &colorbar_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
