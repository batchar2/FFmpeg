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


static const AVOption colorbar_options[] = {
   {"file", "path to colorbars", offsetof(ColorbarContext, file), AV_OPT_TYPE_STRING},
   {"method", "math method", offsetof(ColorbarContext, method_name), AV_OPT_TYPE_STRING},
   {"threshold", "threshold value", offsetof(ColorbarContext, threshold), AV_OPT_TYPE_INT, { .i64 = -1 }, 0, 64},
   {NULL}
};

AVFILTER_DEFINE_CLASS(colorbar);


static int64 haming_distance(uint64_t hash1, uint64_t hash2);
static uint64_t calc_image_hash(IplImage *image);
static uint64_t calc_image_phash(IplImage *image);

static av_cold int init(AVFilterContext *ctx)
{    
    IplImage *colorbar_img = NULL;
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
    //if (colorbar->method == METHOD_HASH) {
        colorbar->hash = calc_image_phash(colorbar_img);
    //} else {
    //    colorbar->hash = calc_image_hash(colorbar_img);
    //}
    cvReleaseImage(&colorbar_img);

    av_log(NULL, AV_LOG_ERROR, "hash_colorbar = %llu\n", colorbar->hash);
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
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
    int x = 0, y = 0;
    double avg = 0;
    uint64_t phash = 0, phash_mask = 1;
    const int DCT_SIZE = 64;
    
    IplImage *mono = NULL;
    IplImage *small = NULL;
    CvMat *dct = NULL;
    CvMat roi;
    /*
    // #1
    small = cvCreateImage(cvSize(DCT_SIZE, DCT_SIZE), image->depth, 1);
    if (image->nChannels == 1) {
        cvCopy(image, mono, 0);
    } else { 
        cvCvtColor(image, mono, CV_RGB2GRAY);
    }
    // #2
    mono = cvCreateImage(cvSize(small->width, small->height), small->depth, 1);
    // #3
    dct = cvCreateMat(DCT_SIZE, DCT_SIZE, CV_64FC1);
    cvConvertScale(small, dct, 1, 0);
    cvDCT(dct, dct, CV_DXT_ROWS);
    cvGetSubRect(dct, &roi, cvRect(0, 0, 8, 8));    

    avg = cvAvg(&roi, 0).val[0] * 64.0 / 63.0;

    for (x = 7; x >= 0; x--) {
        for (y = 7; y >= 0; y--) {
            if (cvGet2D(dct, x, y).val[0] > avg)
                phash |= phash_mask;
            phash_mask = phash_mask << 1ull;
        }
    }
    return phash_mask;
    */
    mono = cvCreateImage(cvSize(image->width, image->height), image->depth, 1);
    small = cvCreateImage(cvSize(DCT_SIZE, DCT_SIZE), image->depth, 1);
    
    if (image->nChannels == 1) {
        cvCopy(image, mono, 0);
    } else { 
        cvCvtColor(image, mono, CV_RGB2GRAY);
    }
    dct = cvCreateMat(DCT_SIZE, DCT_SIZE, CV_64FC1);
    
    cvConvertScale(small, dct, 1, 0);
    cvTranspose(dct, dct);
    cvDCT(dct, dct, CV_DXT_ROWS);
    cvSet2D(dct, 0, 0, cvScalarAll(0));


    cvGetSubRect(dct, &roi, cvRect(0, 0, 8, 8));
    avg = cvAvg(&roi, 0).val[0] * 64.0 / 63.0;

    for (x = 7; x >= 0; x--) {
        for (y = 7; y >= 0; y--) {
            if (cvGet2D(dct, x, y).val[0] > avg)
                phash |= phash_mask;
            phash_mask = phash_mask << 1;
        }
    }

    cvReleaseMat(&dct);
    cvReleaseImage(&mono);
    cvReleaseImage(&small);
    

    av_log(NULL, AV_LOG_ERROR, "phash_colorbar = %llu\n", phash);

    return phash;
}

static uint64_t calc_image_hash(IplImage *image)
{
    uint64_t hash = 0;
    CvSize size_img = cvSize(8, 8);
    CvScalar average;
    int i = 0;
    
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
                hash |= 1ull<<i;  // warning C4334: '<<' : result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)
                //hash |= 1i64<<i; 
            }
            i++;
        }
    }
    cvReleaseImage(&res);
    cvReleaseImage(&gray);
    cvReleaseImage(&bin);
    return hash;
}


static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ColorbarContext *colorbar = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL;
    int direct = 0;
    uint64_t hash_img = 0, calc_hash = 0;
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
        
        //if (colorbar->method == METHOD_HASH) {
            hash_img = calc_image_phash(img);
        //} else {
        //    hash_img = calc_image_hash(img);
        //}
        calc_hash = haming_distance(hash_img, colorbar->hash);
        //av_log(NULL, AV_LOG_ERROR, "calc_hash = %llu\n", calc_hash);    
        //cvReleaseImageHeader(img);
    }
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
