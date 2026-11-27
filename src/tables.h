/*
 * Copyright © 2013-2017  Rinat Ibragimov
 *
 * This file is part of FreshPlayerPlugin.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include "glx.h"
#include <ppapi/c/pp_instance.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <npapi/npapi.h>
#include <npapi/npfunctions.h>
#include <npapi/npruntime.h>
#include <pango/pango.h>
#include <pthread.h>
#include <stdint.h>

#define NPString_literal(str) { .UTF8Characters = str, .UTF8Length = strlen(str) }

typedef GLXContext
(*glx_create_context_attribs_arb_f)(Display *dpy, GLXFBConfig config,
                                    GLXContext share_context, Bool direct,
                                    const int *attrib_list);

typedef int
(*glx_get_video_sync_sgi_f)(unsigned int *count);

typedef int
(*glx_wait_video_sync_sgi_f)(int divisor, int remainder, unsigned int *count);


struct display_s {
    Display                            *x;
    Cursor                              transparent_cursor;
    pthread_mutexattr_t                 mutex_attr_recursive;
    pthread_mutex_t                     lock;
    uint32_t                            have_xrender;
    XRenderPictFormat                  *pictfmt_rgb24;
    XRenderPictFormat                  *pictfmt_argb32;
    uint32_t                            min_width;  ///< smallest screen width
    uint32_t                            min_height; ///< smallest screen height
    uint32_t                            screensaver_types;
    glx_create_context_attribs_arb_f    glXCreateContextAttribsARB;
    glx_get_video_sync_sgi_f            glXGetVideoSyncSGI;
    glx_wait_video_sync_sgi_f           glXWaitVideoSyncSGI;
    uint32_t                            glx_arb_create_context;
    uint32_t                            glx_arb_create_context_profile;
    uint32_t                            glx_ext_create_context_es2_profile;
    int                                 dri_fd;
};

extern NPNetscapeFuncs  npn;
extern struct display_s display;

int         tables_get_urandom_fd(void);

struct pp_instance_s   *tables_get_pp_instance(PP_Instance instance);
void                    tables_add_pp_instance(PP_Instance instance, struct pp_instance_s *pp_i);
void                    tables_remove_pp_instance(PP_Instance instance);
struct pp_instance_s   *tables_get_some_pp_instance(void);
PP_Instance             tables_generate_new_pp_instance_id(void);

PangoContext   *tables_get_pango_ctx(void);
PangoFontMap   *tables_get_pango_font_map(void);

void    tables_add_npobj_npp_mapping(NPObject *npobj, NPP npp);
NPP     tables_get_npobj_npp_mapping(NPObject *npobj);
void    tables_remove_npobj_npp_mapping(NPObject *npobj);

int     tables_open_display(void);
void    tables_close_display(void);
