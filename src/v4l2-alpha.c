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
#include "xf86str.h"
#include "gcstruct.h"
#include "v4l2.h"

/* ---------------------------------------------------------------------- */
/* For alpha blending, we want the alpha channel value to be 1's for 100%
 * opaque (the normal case for the rest of the UI).  But we want to be able
 * to ourselves set it to 0's where the video should show through.  For
 * that we need to overload...
 */

static Bool enable_alpha = FALSE;

#define wrap(priv, real, mem, func) do {\
    priv->mem = real->mem; \
    real->mem = func; \
} while(0)

#define unwrap(priv, real, mem) {\
    real->mem = priv->mem; \
} while(0)

static int GCPrivateKeyIndex;
static DevPrivateKey GCPrivateKey = &GCPrivateKeyIndex;
static int ScreenPrivateKeyIndex;
static DevPrivateKey ScreenPrivateKey = &ScreenPrivateKeyIndex;

typedef struct {
    CreateGCProcPtr CreateGC;
} ScreenPrivRec, *ScreenPrivPtr;

typedef struct {
//    void (*ChangeGC)(GCPtr pGC, unsigned long mask);
    GCFuncs *funcs;
} GCPrivRec, *GCPrivPtr;


/* ---------------------------------------------------------------------- */
/* GC funcs */

static void V4L2ValidateGC(GCPtr pGC, unsigned long changes, DrawablePtr pDrawable);
static void V4L2ChangeGC(GCPtr pGC, unsigned long mask);
static void V4L2CopyGC(GCPtr pGCSrc, unsigned long mask, GCPtr pGCDst);
static void V4L2DestroyGC(GCPtr pGC);
static void V4L2ChangeClip (GCPtr pGC, int type, pointer pvalue, int nrects);
static void V4L2DestroyClip(GCPtr pGC);
static void V4L2CopyClip(GCPtr pgcDst, GCPtr pgcSrc);

static GCFuncs V4L2GCfuncs = {
    V4L2ValidateGC, V4L2ChangeGC, V4L2CopyGC, V4L2DestroyGC,
    V4L2ChangeClip, V4L2DestroyClip, V4L2CopyClip
};

static void
V4L2ValidateGC(GCPtr pGC, unsigned long changes, DrawablePtr pDrawable)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGC->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: ValidateGC\n"));

    unwrap (pGCPriv, pGC, funcs);
    (*pGC->funcs->ValidateGC)(pGC, changes, pDrawable);
    wrap (pGCPriv, pGC, funcs, &V4L2GCfuncs);
}

static void
V4L2ChangeGC(GCPtr pGC, unsigned long mask)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGC->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: ChangeGC\n"));

    if (enable_alpha) {
        pGC->fgPixel = pGC->fgPixel & 0x00ffffff;
        pGC->bgPixel = pGC->bgPixel & 0x00ffffff;
    } else {
        pGC->fgPixel = pGC->fgPixel | 0xff000000;
        pGC->bgPixel = pGC->fgPixel | 0xff000000;
    }

    unwrap (pGCPriv, pGC, funcs);
    (*pGC->funcs->ChangeGC) (pGC, mask);
    wrap (pGCPriv, pGC, funcs, &V4L2GCfuncs);
}

static void
V4L2CopyGC(GCPtr pGCSrc, unsigned long mask, GCPtr pGCDst)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGCDst->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: CopyGC\n"));

    unwrap (pGCPriv, pGCDst, funcs);
    (*pGCDst->funcs->CopyGC) (pGCSrc, mask, pGCDst);
    wrap (pGCPriv, pGCDst, funcs, &V4L2GCfuncs);
}

static void
V4L2DestroyGC(GCPtr pGC)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGC->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: DestroyGC\n"));

    unwrap (pGCPriv, pGC, funcs);
    (*pGC->funcs->DestroyGC)(pGC);
    wrap (pGCPriv, pGC, funcs, &V4L2GCfuncs);
}

static void
V4L2ChangeClip (GCPtr pGC, int type, pointer pvalue, int nrects)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGC->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: ChangeClip\n"));

    unwrap (pGCPriv, pGC, funcs);
    (*pGC->funcs->ChangeClip) (pGC, type, pvalue, nrects);
    wrap (pGCPriv, pGC, funcs, &V4L2GCfuncs);
}

static void
V4L2DestroyClip(GCPtr pGC)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGC->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: DestroyClip\n"));

    unwrap (pGCPriv, pGC, funcs);
    (*pGC->funcs->DestroyClip)(pGC);
    wrap (pGCPriv, pGC, funcs, &V4L2GCfuncs);
}

static void
V4L2CopyClip(GCPtr pgcDst, GCPtr pgcSrc)
{
    GCPrivPtr pGCPriv = dixLookupPrivate(&pgcDst->devPrivates, GCPrivateKey);

    DEBUG(xf86Msg(X_INFO, "v4l2: CopyClip\n"));

    unwrap (pGCPriv, pgcDst, funcs);
    (*pgcDst->funcs->CopyClip)(pgcDst, pgcSrc);
    wrap (pGCPriv, pgcDst, funcs, &V4L2GCfuncs);
}

/* ---------------------------------------------------------------------- */
/* Screen funcs */

static Bool
V4L2CreateGC(GCPtr pGC)
{
    ScreenPtr pScreen = pGC->pScreen;
    ScreenPrivPtr pScreenPriv = dixLookupPrivate(&pScreen->devPrivates, ScreenPrivateKey);
    GCPrivPtr pGCPriv = dixLookupPrivate(&pGC->devPrivates, GCPrivateKey);
    Bool ret;

    DEBUG(xf86Msg(X_INFO, "v4l2: CreateGC\n"));

    unwrap (pScreenPriv, pScreen, CreateGC);
    if((ret = (*pScreen->CreateGC) (pGC)) &&
            (pGC->funcs->ChangeGC != V4L2ChangeGC)) {
        DEBUG(xf86Msg(X_INFO, "v4l2: override ChangeGC..\n"));
        wrap (pGCPriv, pGC, funcs, &V4L2GCfuncs);
    }
    wrap(pScreenPriv, pScreen, CreateGC, V4L2CreateGC);

    return ret;
}

static Bool
V4L2SetupScreen(ScreenPtr pScreen)
{
    ScreenPrivPtr pScreenPriv = dixLookupPrivate(&pScreen->devPrivates, ScreenPrivateKey);
    int fd;

    if (dixLookupPrivate(&pScreen->devPrivates, ScreenPrivateKey))
        return TRUE;

    if (!dixRequestPrivate(GCPrivateKey, sizeof(GCPrivRec)))
        return FALSE;

    DEBUG(xf86Msg(X_INFO, "v4l2: SetupScreen\n"));

    pScreenPriv = malloc(sizeof (ScreenPrivRec));
    if (!pScreenPriv)
        return FALSE;

    wrap (pScreenPriv, pScreen, CreateGC, V4L2CreateGC);

    dixSetPrivate(&pScreen->devPrivates, ScreenPrivateKey, pScreenPriv);

    /* configure the corresponding framebuffer for ARGB: */
    fd = fbdevHWGetFD(xf86Screens[pScreen->myNum]);
    if (fd) {
        struct fb_var_screeninfo var;

        DEBUG(xf86Msg(X_INFO, "v4l2: reconfiguring fb dev %d to ARGB..\n", fd));

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

void
V4L2SetupAlpha(void)
{
    static int initialized = FALSE;
    int i;

    if (initialized) {
        return;
    }

    for (i = 0; i < screenInfo.numScreens; i++) {
        V4L2SetupScreen(screenInfo.screens[i]);
    }

    initialized = TRUE;
}

/**
 * called by core part of xf86-video-v4l2 driver module when he wants to paint
 * pixels with alpha enabled.
 */
void
V4L2EnableAlpha(Bool b)
{
    enable_alpha = b;
}
