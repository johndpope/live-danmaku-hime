/*
  Copyright (c) 2015 StarBrilliant <m13253@hotmail.com>
  All rights reserved.

  Redistribution and use in source and binary forms are permitted
  provided that the above copyright notice and this paragraph are
  duplicated in all such forms and that any documentation,
  advertising materials, and other materials related to such
  distribution and use acknowledge that the software was developed by
  StarBrilliant.
  The name of StarBrilliant may not be used to endorse or promote
  products derived from this software without specific prior written
  permission.

  THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
*/

#include "cairo_render.h"
#include "../utils.h"
#include "../app.h"
#include "../config.h"
#include "../fetcher/fetcher.h"
#include "../presenter/presenter.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <list>
#include <vector>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include "freetype_includer.h"

#ifdef CAIRO_STATIC_WORKAROUND
extern "C" void _cairo_mutex_initialize();
#endif

namespace dmhm {

struct DanmakuAnimator;

struct CairoRendererPrivate {
    Application *app = nullptr;

    /* Stage size */
    uint32_t width = 0;
    uint32_t height = 0;

    FT_Library freetype = nullptr;
    FT_Face ft_font_face = nullptr;
    cairo_font_face_t *cairo_font_face = nullptr;

    cairo_surface_t *cairo_blend_surface = nullptr;
    cairo_t *cairo_blend_layer = nullptr;
    cairo_surface_t *cairo_blur_surface = nullptr;
    cairo_t *cairo_blur_layer = nullptr;
    cairo_surface_t *cairo_text_surface = nullptr;
    cairo_t *cairo_text_layer = nullptr;

    bool is_eof = false;
    std::list<DanmakuAnimator> danmaku_list;

    void create_cairo(cairo_surface_t *&cairo_surface, cairo_t *&cairo);
    static void release_cairo(cairo_surface_t *&cairo_surface, cairo_t *&cairo);
    void fetch_danmaku(std::chrono::steady_clock::time_point now);
    void animate_text(std::chrono::steady_clock::time_point now);
    void paint_text();
    void blend_layers();

    static const uint32_t blur_rounds = 2;
    uint32_t gamma_table[256];
    uint32_t blur_boxes[blur_rounds];
    void generate_blur_boxes();
    void gauss_blur(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h);
    static void box_blur(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h, int32_t r);
    static void box_blur_H(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h, int32_t r);
    static void box_blur_T(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h, int32_t r);

    std::chrono::steady_clock::time_point fps_checkpoint;
    uint32_t fps_count;
    void print_fps(std::chrono::steady_clock::time_point now);
};

CairoRenderer::CairoRenderer(Application *app) {
    p->app = app;

    /* Initialize fonts */
    FT_Error ft_error;
    ft_error = FT_Init_FreeType(&p->freetype);
    dmhm_assert(ft_error == 0);
    ft_error = FT_New_Face(p->freetype, config::font_file, config::font_file_index, &p->ft_font_face);
    if(ft_error == FT_Err_Cannot_Open_Resource) {
        Presenter *presenter = reinterpret_cast<Presenter *>(app->get_presenter());
        dmhm_assert(presenter);
        // Failed to open font file
        presenter->report_error(std::string("\xe6\x89\x93\xe5\xbc\x80\xe5\xad\x97\xe4\xbd\x93\xe6\x96\x87\xe4\xbb\xb6\x20")+config::font_file+std::string("\x20\xe5\xa4\xb1\xe8\xb4\xa5"));
        abort();
    }
    if(ft_error == FT_Err_Unknown_File_Format) {
        Presenter *presenter = reinterpret_cast<Presenter *>(app->get_presenter());
        dmhm_assert(presenter);
        // Unsupported font format
        presenter->report_error(std::string("\xe6\x97\xa0\xe6\xb3\x95\xe8\xaf\x86\xe5\x88\xab\xe7\x9a\x84\xe5\xad\x97\xe4\xbd\x93\xe6\x96\x87\xe4\xbb\xb6\x20")+config::font_file);
        abort();
    }
    dmhm_assert(ft_error == 0);
    ft_error = FT_Set_Char_Size(p->ft_font_face, 0, FT_F26Dot6(config::font_size*64), 72, 72);
    dmhm_assert(ft_error == 0);

#ifdef CAIRO_STATIC_WORKAROUND
    /* There is a bug in _cairo_ft_unscaled_font_map_lock,
       causing mutex not being initialized correctly.
       I will hack it dirtly by calling a private API.
       This problem only occurs on static linking. */
    _cairo_mutex_initialize();
#endif

    p->cairo_font_face = cairo_ft_font_face_create_for_ft_face(p->ft_font_face, 0);

    p->generate_blur_boxes();

    p->fps_checkpoint = std::chrono::steady_clock::now();
    p->fps_count = 0;
}

CairoRenderer::~CairoRenderer() {
    p->release_cairo(p->cairo_text_surface, p->cairo_text_layer);
    p->release_cairo(p->cairo_blend_surface, p->cairo_blend_layer);

    FT_Error ft_error;
    if(p->cairo_font_face) {
        cairo_font_face_destroy(p->cairo_font_face);
        p->cairo_font_face = nullptr;
    }
    if(p->ft_font_face) {
        ft_error = FT_Done_Face(p->ft_font_face);
        dmhm_assert(ft_error == 0);
        p->ft_font_face = nullptr;
    }
    if(p->freetype) {
        ft_error = FT_Done_FreeType(p->freetype);
        dmhm_assert(ft_error == 0);
        p->freetype = nullptr;
    }
}

bool CairoRenderer::paint_frame(uint32_t width, uint32_t height, std::function<void (const uint32_t *bitmap, uint32_t stride)> callback) {
    if(width != p->width || height != p->height) {
        p->width = width;
        p->height = height;
        p->release_cairo(p->cairo_text_surface, p->cairo_text_layer);
        p->release_cairo(p->cairo_blur_surface, p->cairo_blur_layer);
        p->release_cairo(p->cairo_blend_surface, p->cairo_blend_layer);
    }
    if(!p->cairo_blend_layer)
        p->create_cairo(p->cairo_blend_surface, p->cairo_blend_layer);
    if(!p->cairo_blur_layer)
        p->create_cairo(p->cairo_blur_surface, p->cairo_blur_layer);
    if(!p->cairo_text_layer) {
        p->create_cairo(p->cairo_text_surface, p->cairo_text_layer);
        cairo_set_font_face(p->cairo_text_layer, p->cairo_font_face);
        cairo_set_font_size(p->cairo_text_layer, config::font_size);
        cairo_font_options_t *font_options = cairo_font_options_create();
        cairo_get_font_options(p->cairo_text_layer, font_options);
        cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_OFF);
        cairo_set_font_options(p->cairo_text_layer, font_options);
        cairo_font_options_destroy(font_options);
    }

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    p->print_fps(now);
    p->fetch_danmaku(now);
    p->animate_text(now);

    if(!p->danmaku_list.empty()) {
        cairo_set_operator(p->cairo_text_layer, CAIRO_OPERATOR_CLEAR);
        cairo_paint(p->cairo_text_layer);
        cairo_set_operator(p->cairo_text_layer, CAIRO_OPERATOR_OVER);
        p->paint_text();

        cairo_set_operator(p->cairo_blend_layer, CAIRO_OPERATOR_CLEAR);
        cairo_paint(p->cairo_blend_layer);
        cairo_set_operator(p->cairo_blend_layer, CAIRO_OPERATOR_OVER);
        p->blend_layers();
    } else {
        cairo_set_operator(p->cairo_blend_layer, CAIRO_OPERATOR_CLEAR);
        cairo_paint(p->cairo_blend_layer);
        cairo_set_operator(p->cairo_blend_layer, CAIRO_OPERATOR_OVER);
        /* Workaround a wine bug by lighting up a few pixels */
        cairo_set_source_rgba(p->cairo_blend_layer, 0.5, 0.5, 0.5, 0.004);
        cairo_rectangle(p->cairo_blend_layer, 0.5, 0.5, 2, 2);
        cairo_fill(p->cairo_blend_layer);
    }

    cairo_surface_flush(p->cairo_blend_surface);
    callback(reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(p->cairo_blend_surface)), uint32_t(cairo_image_surface_get_stride(p->cairo_blend_surface)/sizeof (uint32_t)));

    return !p->is_eof || !p->danmaku_list.empty();
}

void CairoRendererPrivate::create_cairo(cairo_surface_t *&cairo_surface, cairo_t *&cairo) {
    cairo_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo = cairo_create(cairo_surface);
}

void CairoRendererPrivate::release_cairo(cairo_surface_t *&cairo_surface, cairo_t *&cairo) {
    if(cairo) {
        cairo_destroy(cairo);
        cairo = nullptr;
    }
    if(cairo_surface) {
        cairo_surface_destroy(cairo_surface);
        cairo_surface = nullptr;
    }
}

struct DanmakuAnimator {
    DanmakuAnimator(const DanmakuEntry &entry) :
        entry(entry) {
    }
    DanmakuEntry entry;
    double x;
    double y;
    double height;
    double alpha = 0;
    bool moving = false;
    std::chrono::steady_clock::time_point starttime;
    std::chrono::steady_clock::time_point endtime;
    double starty;
    double endy;
};

void CairoRendererPrivate::print_fps(std::chrono::steady_clock::time_point now) {
    fps_count++;
    if(now-fps_checkpoint > std::chrono::seconds(1)) {
        std::cerr << "FPS: " << fps_count << std::endl;
        fps_count = 0;
        fps_checkpoint = now;
    }
}

void CairoRendererPrivate::fetch_danmaku(std::chrono::steady_clock::time_point now) {
    Fetcher *fetcher = reinterpret_cast<Fetcher *>(app->get_fetcher());
    dmhm_assert(fetcher);

    is_eof = fetcher->is_eof();
    fetcher->pop_messages([&](DanmakuEntry &entry) {
        DanmakuAnimator animator(entry);
        animator.y = height-(config::extra_line_height+config::shadow_radius);
        cairo_text_extents_t text_extents;
        cairo_text_extents(cairo_text_layer, animator.entry.message.c_str(), &text_extents);
        animator.height = text_extents.height+config::extra_line_height;
        for(DanmakuAnimator &i : danmaku_list) {
            if(i.moving) {
                i.starty = i.starty+(i.endy-i.starty)*(now-i.starttime).count()/(i.endtime-i.starttime).count();
                i.endy -= animator.height;
            } else {
                i.starty = i.y;
                i.endy = i.starty-animator.height;
                i.moving = true;
            }
            i.starttime = now;
            i.endtime = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(config::danmaku_attack));
        }
        danmaku_list.push_front(std::move(animator));
    });
}

void CairoRendererPrivate::animate_text(std::chrono::steady_clock::time_point now) {
    danmaku_list.remove_if([&](const DanmakuAnimator &x) -> bool {
        double timespan = double((now-x.entry.timestamp).count())*std::chrono::steady_clock::period::num/std::chrono::steady_clock::period::den;
        return timespan >= config::danmaku_lifetime || x.y < -2*config::shadow_radius;
    });
    for(DanmakuAnimator &i : danmaku_list) {
        double timespan = double((now-i.entry.timestamp).count())*std::chrono::steady_clock::period::num/std::chrono::steady_clock::period::den;
        if(timespan < config::danmaku_attack) {
            double progress = timespan/config::danmaku_attack;
            i.x = (width-config::shadow_radius)*(1-progress*(2-progress))+config::shadow_radius;
            i.alpha = progress;
        } else if(timespan < config::danmaku_lifetime-config::danmaku_decay) {
            i.x = config::shadow_radius;
            i.alpha = 1;
        } else {
            i.x = config::shadow_radius;
            i.alpha = (config::danmaku_lifetime-timespan)/config::danmaku_decay;
        }
        if(i.moving)
            if(now >= i.endtime) {
                i.moving = false;
                i.y = i.endy;
            } else
                i.y = i.starty+(i.endy-i.starty)*(now-i.starttime).count()/(i.endtime-i.starttime).count();
        else;
    }
}

void CairoRendererPrivate::paint_text() {
    for(const DanmakuAnimator &i : danmaku_list) {
        cairo_move_to(cairo_text_layer, i.x, i.y);
        cairo_set_source_rgba(cairo_text_layer, 1, 1, 1, i.alpha);
        cairo_show_text(cairo_text_layer, i.entry.message.c_str());
    }
}

void CairoRendererPrivate::blend_layers() {
    cairo_surface_flush(cairo_text_surface);
    const uint32_t *text_bitmap = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(cairo_text_surface));
    uint32_t text_stride = uint32_t(cairo_image_surface_get_stride(cairo_text_surface)/sizeof (uint32_t));
    cairo_surface_flush(cairo_blur_surface);
    uint32_t *blur_bitmap = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(cairo_blur_surface));
    uint32_t blur_stride = uint32_t(cairo_image_surface_get_stride(cairo_blur_surface)/sizeof (uint32_t));
    cairo_surface_flush(cairo_blend_surface);
    uint32_t *blend_bitmap = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(cairo_blend_surface));
    uint32_t blend_stride = uint32_t(cairo_image_surface_get_stride(cairo_blend_surface)/sizeof (uint32_t));

    dmhm_assert(blur_stride == blend_stride);

    for(uint32_t i = 0; i < height; i++)
        for(uint32_t j = 0; j < width; j++)
            blur_bitmap[i*blend_stride + j] = text_bitmap[i*text_stride + j] >> 24;
    gauss_blur(blur_bitmap, blend_bitmap, blend_stride, height);
    for(uint32_t i = 0; i < height; i++)
        for(uint32_t j = 0; j < width; j++)
            blend_bitmap[i*blend_stride + j] = gamma_table[blur_bitmap[i*blur_stride + j]];

    cairo_surface_mark_dirty(cairo_blur_surface);
    cairo_surface_mark_dirty(cairo_blend_surface);
    cairo_set_source_surface(cairo_blend_layer, cairo_text_surface, 0, 0);
    cairo_paint(cairo_blend_layer);
}

void CairoRendererPrivate::generate_blur_boxes() {
    for(uint32_t i = 0; i < 256; i++) {
        double fi = i/255.0;
        gamma_table[i] = uint32_t((1-(1-fi)*(1-fi))*double(0xff000000)) & 0xff000000;
    }

    const uint32_t n = blur_rounds;
    dmhm_assert(config::shadow_radius >= 0);
    double sigma = config::shadow_radius/3;

    double wIdeal = std::sqrt(12*sigma*sigma/n+1);
    uint32_t wl = std::floor(wIdeal);
    if((wl & 1) == 0)
        wl--;
    uint32_t wu = wl+2;
    double mIdeal = (12*sigma*sigma - n*wl*wl - 4*n*wl - 3*n)/(-4*wl - 4);
    uint32_t m = uint32_t(mIdeal);

    for(uint32_t i = 0; i < n; i++)
        blur_boxes[i] = i<m ? wl : wu;
}

/* Thanks to http://blog.ivank.net/fastest-gaussian-blur.html
   I rewrote the original algorithm in C++*/
void CairoRendererPrivate::gauss_blur(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h) {
    const uint32_t *bxs = blur_boxes;
    box_blur(scl, tcl, w, h, int32_t((bxs[0]-1)/2));
    box_blur(tcl, scl, w, h, int32_t((bxs[1]-1)/2));
    // box_blur(scl, tcl, w, h, int32_t((bxs[2]-1)/2)); // Two times are enough, the result is in scl instead
}

void CairoRendererPrivate::box_blur(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h, int32_t r) {
    for(int32_t i = 0; i < w*h; i++)
        tcl[i] = scl[i];
    box_blur_H(tcl, scl, w, h, r);
    box_blur_T(scl, tcl, w, h, r);
}

void CairoRendererPrivate::box_blur_H(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h, int32_t r) {
    uint32_t iarr = r*2+1;
    for(int32_t i = 0; i < h; i++) {
        int32_t ti = i*w, li = ti, ri = ti+r;
        uint32_t fv = scl[ti], lv = scl[ti+w-1], val = (r+1)*fv;
        for(int32_t j = 0; j < r; j++)
            val += scl[ti+j];
        for(int32_t j = 0; j <= r; j++) {
            val += scl[ri++] - fv;
            tcl[ti++] = val/iarr;
        }
        for(int32_t j = r+1; j < w-r; j++) {
            val += scl[ri++] - scl[li++];
            tcl[ti++] = val/iarr;
        }
        for(int32_t j = w-r; j < w; j++) {
            val += lv - scl[li++];
            tcl[ti++] = val/iarr;
        }
    }
}

void CairoRendererPrivate::box_blur_T(uint32_t *scl, uint32_t *tcl, int32_t w, int32_t h, int32_t r) {
    uint32_t iarr = r*2+1;
    for(int32_t i = 0; i < w; i++) {
        int32_t ti = i, li = ti, ri = ti+r*w;
        uint32_t fv = scl[ti], lv = scl[ti+w*(h-1)], val = (r+1)*fv;
        for(int32_t j = 0; j < r; j++)
            val += scl[ti+j*w];
        for(int32_t j = 0; j <= r; j++) {
            val += scl[ri] - fv;
            tcl[ti] = val/iarr;
            ri += w; ti += w;
        }
        for(int32_t j = r+1; j < h-r; j++) {
            val += scl[ri] - scl[li];
            tcl[ti] = val/iarr;
            li += w; ri += w; ti += w;
        }
        for(int32_t j = h-r; j < h; j++) {
            val += lv - scl[li];
            tcl[ti] = val/iarr;
            li += w; ti += w;
        }
    }
}

}
