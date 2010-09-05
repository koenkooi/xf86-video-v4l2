/* xf86-video-v4l2
 *
 * Copyright (C) 2010 Texas Instruments, Inc - http://www.ti.com/
 *
 * Description: video4linux2 Xv driver
 *  Created on: May 29, 2010
 *      Author: Rob Clark <rob@ti.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/fb.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Pci.h"
#include "xf86PciInfo.h"
#include "xf86fbman.h"
#include "xf86xv.h"
#include "regionstr.h"
#include "inputstr.h"
#include "mipointrst.h"
#include "xf86str.h"
#include "gcstruct.h"
#include "shadow.h"
#include "fb.h"
#include "v4l2.h"

static Bool alpha = FALSE;

/* track the clip associated with each xv port indexed by pPPriv->nr
 */
static struct {
    RegionPtr clip;
    Bool updated;
} * regions = NULL;

static int numRegions = 0;
static int activeClips = 0;

/* ---------------------------------------------------------------------- */
/* For alpha blending, we want the alpha channel value to be 1's for 100%
 * opaque (the normal case for the rest of the UI).  But we want to be able
 * to ourselves set it to 0's where the video should show through.  For
 * that we need to overload the function that blits from the shadow
 * framebuffer to framebuffer.
 */

#define LIKELY(x)      __builtin_expect(!!(x), 1)
#define UNLIKELY(x)    __builtin_expect(!!(x), 0)

#define MIPOINTER(dev) \
    (miPointerPtr)dixLookupPrivate(&(dev)->devPrivates, miPointerPrivKey)
#define DevHasCursor(pDev) \
    ((pDev)->spriteInfo && (pDev)->spriteInfo->spriteOwner)

static const void *opTransparent=(void *)1, *opSolid=(void *)3;

static inline void
V4L2ShadowBlitTransparentARGB32(void *winBase, int winStride, int w, int h)
{
    while (h--) {
        memset (winBase, 0x00, w * sizeof(FbBits));
        winBase += winStride;
    }
}

static inline void
V4L2ShadowBlitSolidARGB32(void *winBase, int winStride,
        void *shaBase, int shaStride, int w, int h)
{
    while (h--) {
        FbBits *win = winBase;
        FbBits *sha = shaBase;
        int i = w;
        while (i--)
            *win++ = 0xff000000 | *sha++;
        winBase += winStride;
        shaBase += shaStride;
    }
}

static inline void
V4L2ShadowBlitCursorARGB32(void *winBase, int winStride,
        void *curBase, int curStride, int w, int h)
{
    while (h--) {
        memcpy(winBase, curBase, w * sizeof(FbBits));
        winBase += winStride;
        curBase += curStride;
    }
}

static inline void
V4l2ShadowBlit(void *winBase, int winStride,
        void *shaBase, int shaStride, int shaBpp,
        BoxPtr pbox, const void *op)
{
    int w, h;

    w = (pbox->x2 - pbox->x1) * shaBpp / 32;     /* width in words */
    h = pbox->y2 - pbox->y1;                     /* height in rows */

    if ((w == 0) || (h == 0))
        return;

    winBase += (winStride * pbox->y1) + (pbox->x1 * shaBpp / 8);
    shaBase += (shaStride * pbox->y1) + (pbox->x1 * shaBpp / 8);

    if (op == opTransparent) {
        V4L2ShadowBlitTransparentARGB32(winBase, winStride, w, h);
    } else if (op == opSolid) {
        V4L2ShadowBlitSolidARGB32(winBase, winStride, shaBase, shaStride, w, h);
    } else {
        miPointerPtr pPointer = (miPointerPtr)op;
        CursorBitsPtr bits = pPointer->pCursor->bits;
        if (bits->argb) {
            void *curBase = bits->argb;
            int curStride = bits->width * sizeof(FbBits);
            int xoff = MAX(0, pbox->x1 - pPointer->x + bits->xhot);
            int yoff = MAX(0, pbox->y1 - pPointer->y + bits->yhot);
            curBase += (yoff * curStride) + (xoff * sizeof(FbBits));
            V4L2ShadowBlitCursorARGB32(winBase, winStride, curBase, curStride, w, h);
        } else {
            // TODO: support non-argb cursors..
        }
    }
}

static inline void
V4L2ShadowBlitRegions(ScreenPtr pScreen, shadowBufPtr pBuf,
        RegionPtr damage, const void *op)
{
    PixmapPtr pShadow = pBuf->pPixmap;
    BoxPtr pbox;
    int nbox, shaBpp, shaXoff, shaYoff;
    FbBits *shaBase, *winBase;
    CARD32 shaStride, winStride;

    fbGetDrawable(&pShadow->drawable, shaBase, shaStride, shaBpp, shaXoff, shaYoff);
    shaStride *= sizeof(FbBits);                 /* convert into byte-stride */

    winBase =
        pBuf->window(pScreen, 0, 0, SHADOW_WINDOW_WRITE, &winStride, pBuf->closure);

    nbox = RegionNumRects(damage);
    pbox = RegionRects(damage);

    while (nbox--) {
        V4l2ShadowBlit(winBase, winStride, shaBase, shaStride, shaBpp, pbox, op);
        pbox++;
    }

}

static inline void
V4L2DrawCursor(ScreenPtr pScreen, shadowBufPtr pBuf, miPointerPtr pPointer)
{
    int i;
    int x = pPointer->x - pPointer->pCursor->bits->xhot;
    int y = pPointer->y - pPointer->pCursor->bits->yhot;
    BoxRec cursorRect;

    cursorRect.x1 = x;
    cursorRect.y1 = y;
    cursorRect.x2 = x + pPointer->pCursor->bits->width;
    cursorRect.y2 = y + pPointer->pCursor->bits->height;

    for (i = 0; i < numRegions; i++) {
        if (regions[i].clip &&
                RegionContainsRect(regions[i].clip, &cursorRect)) {
            /* cursor at least partially intersects:
             */
            RegionPtr limits = RegionCreate(&cursorRect, 1);
            RegionIntersect(limits, limits, regions[i].clip);

            V4L2ShadowBlitRegions(pScreen, pBuf, limits, pPointer);

            RegionUninit(limits);
        }
    }
}

static void
V4L2ShadowUpdatePacked(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    int i;
    RegionPtr damage = DamageRegion(pBuf->pDamage);
    RegionPtr tofree = NULL;
    DeviceIntPtr pDev;

    if (UNLIKELY (activeClips > 0)) {
        /* subtract active regions from damaged regions so they aren't
         * blit to screen:
         */
        for (i = 0; i < numRegions; i++) {
            if (regions[i].clip && regions[i].updated) {
                if (!tofree) {
                    tofree = RegionCreate(NULL, 0);
                }
                RegionSubtract(tofree, damage, regions[i].clip);
                damage = tofree;
            }
        }
    }

    /* blit non-video damaged areas to screen:
     */
    V4L2ShadowBlitRegions(pScreen, pBuf, damage, opSolid);

    if (UNLIKELY (activeClips > 0)) {
        /* fill updated video regions with transparent pixels:
         */
        for (i = 0; i < numRegions; i++) {
            if (regions[i].clip && regions[i].updated) {
                V4L2ShadowBlitRegions(pScreen, pBuf, regions[i].clip, opTransparent);
// XXX workaround.. we need to be able to restore under old cursor..
//                regions[i].updated = FALSE;
            }
        }

        if (tofree) {
            RegionUninit(tofree);
        }

        /* handle any cursors that are over an active region:
         */
        for(pDev = inputInfo.devices; pDev; pDev = pDev->next) {
            miPointerPtr pPointer;
            if (DevHasCursor(pDev) && (pPointer = MIPOINTER(pDev)) &&
                    (pPointer->pScreen == pScreen) &&
                    pPointer->pCursor && pPointer->pCursor->bits) {
                V4L2DrawCursor(pScreen, pBuf, pPointer);
            }
        }
    }
}

/**
 * overload shadow update fxn and handle alpha pixels inline with copy to
 * framebuffer
 */
shadowUpdateProc
shadowUpdatePackedWeak(void)
{
    return V4L2ShadowUpdatePacked;
}

static Bool
V4L2SetupScreen(ScreenPtr pScreen)
{
    int fd;

    DEBUG("SetupScreen, pScreen=%p", pScreen);

    /* configure the corresponding framebuffer for ARGB: */
    fd = fbdevHWGetFD(xf86Screens[pScreen->myNum]);
    if (fd) {
        struct fb_var_screeninfo var;

        DEBUG("reconfiguring fb dev %d to ARGB..", fd);

        if (-1 == ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
            perror("ioctl FBIOGET_VSCREENINFO");
        }

        /* @todo support other color formats than ARGB */
        var.transp.length = 8;
        var.transp.offset = 24;

        if (-1 == ioctl(fd, FBIOPUT_VSCREENINFO, &var)) {
            perror("ioctl FBIOPUT_VSCREENINFO");
        }
    }

    return TRUE;
}

/**
 * Setup alpha blending for a new port.
 */
void
V4L2SetupAlpha(PortPrivPtr pPPriv)
{
    if (!alpha) {
        int i;

        for (i = 0; i < screenInfo.numScreens; i++) {
            V4L2SetupScreen(screenInfo.screens[i]);
        }

        alpha = TRUE;
    }

    if (pPPriv->nr >= numRegions) {
        /* grow array of per-port info */
        regions = realloc(regions, sizeof(regions[0]) * (pPPriv->nr + 1));

        while (numRegions <= pPPriv->nr) {
            regions[numRegions].clip = NULL;
            regions[numRegions].updated = FALSE;
            numRegions++;
        }
    }
}

/**
 * called by core part of xf86-video-v4l2 driver module when he wants to paint
 * pixels with alpha enabled.
 */
void
V4L2SetClip(PortPrivPtr pPPriv, DrawablePtr pDraw, RegionPtr clipBoxes)
{
    if (alpha) {
        V4L2ClearClip(pPPriv);

        DEBUG("Xv/SC: %d", pPPriv->nr);

        regions[pPPriv->nr].clip = RegionCreate(NULL, 0);
        RegionCopy(regions[pPPriv->nr].clip, clipBoxes);
        regions[pPPriv->nr].updated = TRUE;
        activeClips++;

        /* we don't actually have to fill the color key.. just register it as
         * dirty so that our shadow-update function gets run
         */
        DamageRegionAppend(pDraw, clipBoxes);
    } else {
        xf86XVFillKeyHelper(pDraw->pScreen, pPPriv->colorKey, clipBoxes);
    }
}

void
V4L2ClearClip(PortPrivPtr pPPriv)
{
    if (alpha) {
        DEBUG("Xv/CC: %d", pPPriv->nr);

        if (regions[pPPriv->nr].clip) {
            RegionUninit(regions[pPPriv->nr].clip);
            regions[pPPriv->nr].clip = NULL;
            activeClips--;
        }
    }
}
