/* SPDX-License-Identifier: GPL-2.0-only
 *
 * An X11 window onto the level.
 *
 * The original shows a 320x160 window onto the 1584-wide level, with the
 * control panel below it; this shows the same viewport and no panel, scrolled
 * with the arrow keys. It is a way to look at what has been reconstructed, not
 * a reimplementation of the game's own display loop - that has to be
 * transcribed and checked against the original like anything else.
 */
#include <stdlib.h>
#include <string.h>

#include "view.h"

#ifndef HAVE_X11

int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale)
{
    (void)c; (void)palette; (void)title; (void)start_x; (void)scale;
    return -1;
}

#else

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#define VIEW_W 320
#define VIEW_H 160

int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale)
{
    if (scale < 1) scale = 1;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return -1;

    int screen = DefaultScreen(dpy);
    uint16_t win_w = (uint16_t)(VIEW_W * scale);
    uint16_t win_h = (uint16_t)(VIEW_H * scale);

    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                                     win_w, win_h, 0,
                                     BlackPixel(dpy, screen),
                                     BlackPixel(dpy, screen));
    XStoreName(dpy, win, title);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    /* 32-bit BGRX, which is what every modern TrueColor visual wants. */
    uint32_t *fb = malloc((size_t)win_w * win_h * 4);
    if (!fb) { XCloseDisplay(dpy); return -1; }

    XImage *img = XCreateImage(dpy, DefaultVisual(dpy, screen),
                               (unsigned)DefaultDepth(dpy, screen), ZPixmap, 0,
                               (char *)fb, win_w, win_h, 32, 0);
    if (!img) { free(fb); XCloseDisplay(dpy); return -1; }

    int32_t scroll = (int32_t)start_x;
    int32_t max_scroll = (int32_t)c->w - VIEW_W;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;

    int running = 1;
    while (running) {
        /* Blit the viewport, scaled by whole pixels. */
        for (uint16_t y = 0; y < win_h; y++) {
            uint16_t sy = (uint16_t)(y / scale);
            const uint8_t *row = &c->pix[(uint32_t)sy * c->w];
            uint32_t *dst = &fb[(uint32_t)y * win_w];
            for (uint16_t x = 0; x < win_w; x++) {
                int32_t sx = scroll + x / scale;
                lem_rgb col = { 0, 0, 0 };
                if (sx >= 0 && sx < (int32_t)c->w)
                    col = palette[row[sx] & 0x0F];
                dst[x] = ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
            }
        }
        XPutImage(dpy, win, gc, img, 0, 0, 0, 0, win_w, win_h);
        XFlush(dpy);

        XEvent ev;
        XNextEvent(dpy, &ev);
        do {
            if (ev.type == KeyPress) {
                KeySym k = XLookupKeysym(&ev.xkey, 0);
                int32_t step = (ev.xkey.state & ShiftMask) ? 64 : 16;
                if (k == XK_Escape || k == XK_q)      running = 0;
                else if (k == XK_Left)                scroll -= step;
                else if (k == XK_Right)               scroll += step;
                else if (k == XK_Home)                scroll = 0;
                else if (k == XK_End)                 scroll = max_scroll;
                if (scroll < 0)          scroll = 0;
                if (scroll > max_scroll) scroll = max_scroll;
            } else if (ev.type == ClientMessage &&
                       (Atom)ev.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        } while (running && XPending(dpy) && (XNextEvent(dpy, &ev), 1));
    }

    XDestroyImage(img);          /* frees fb too */
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}

#endif /* HAVE_X11 */
