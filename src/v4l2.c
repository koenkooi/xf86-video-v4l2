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
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Pci.h"
#include "xf86PciInfo.h"
#include "xf86fbman.h"
#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include "regionstr.h"
#include "dgaproc.h"
#include "xf86str.h"
#include "v4l2.h"

#include <asm/ioctl.h>		/* _IORW(xxx) #defines are here */

static void     V4L2Identify(int flags);
static Bool     V4L2Probe(DriverPtr drv, int flags);
static const OptionInfoRec * V4L2AvailableOptions(int chipid, int busid);

_X_EXPORT DriverRec V4L2 = {
        40000,
        "v4l2",
        V4L2Identify, /* Identify*/
        V4L2Probe, /* Probe */
        V4L2AvailableOptions,
        NULL,
        0
};

/* Supported options */
typedef enum {
        OPTION_DEBUG,        /* enable debug */
        OPTION_DEVICES,      /* comma separated list of v4l2 devices */
        OPTION_ALPHA,        /* use alpha blending if supported by device */
        OPTION_COLORKEY,     /* colorkey value to use, if not using alpha */
        NUM_OPTIONS
} FBDevOpts;

#define DEFAULT_DEBUG        FALSE
#define DEFAULT_DEVICES      "/dev/video1,/dev/video2,/dev/video3"
#define DEFAULT_ALPHA        TRUE
#define DEFAULT_COLORKEY     0x0000ff00

static const OptionInfoRec V4L2DevOptions[] = {
        { OPTION_DEBUG,         "Debug",        OPTV_BOOLEAN,   {0},  FALSE },
        { OPTION_DEVICES,       "Devices",      OPTV_STRING,    {0},  FALSE },
        { OPTION_ALPHA,         "Alpha",        OPTV_BOOLEAN,   {0},  FALSE },
        { OPTION_COLORKEY,      "ColorKey",     OPTV_INTEGER,   {0},  FALSE },
        { -1,                   NULL,           OPTV_NONE,      {0},  FALSE }
};

V4L2Config config = {
        .debug     = DEFAULT_DEBUG,
        .devices   = DEFAULT_DEVICES,
        .alpha     = DEFAULT_ALPHA,
        .colorKey  = DEFAULT_COLORKEY
};

#ifdef XFree86LOADER

static MODULESETUPPROTO(v4l2Setup);

static XF86ModuleVersionInfo v4l2VersRec =
{
        "v4l2",
        MODULEVENDORSTRING,
        MODINFOSTRING1,
        MODINFOSTRING2,
        XORG_VERSION_CURRENT,
        0, 1, 1,
        ABI_CLASS_VIDEODRV,
        ABI_VIDEODRV_VERSION,
        MOD_CLASS_NONE,
        {0,0,0,0}
};

_X_EXPORT XF86ModuleData v4l2ModuleData = { &v4l2VersRec, v4l2Setup, NULL };

static pointer
v4l2Setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    const char *osname;
    static Bool setupDone = FALSE;

    if (setupDone) {
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }

    setupDone = TRUE;

    /* Check that we're being loaded on a Linux system */
    LoaderGetOS(&osname, NULL, NULL, NULL);
    if (!osname || strcmp(osname, "linux") != 0) {
        if (errmaj)
            *errmaj = LDR_BADOS;
        if (errmin)
            *errmin = 0;
        return NULL;
    } else {
        /* OK */
        OptionInfoPtr options;

        /* handle options */
        if (!(options = xalloc(sizeof(V4L2DevOptions)))) {
            return FALSE;
        }

        memcpy(options, V4L2DevOptions, sizeof(V4L2DevOptions));
        xf86ProcessOptions(0 /*XXX pScrn->scrnIndex */, opts, options);

        config.debug = xf86ReturnOptValBool(options, OPTION_DEBUG, DEFAULT_DEBUG);
        if (!(config.devices = xf86GetOptValString(options, OPTION_DEVICES))) {
            config.devices = DEFAULT_DEVICES;
        }
        config.alpha = xf86ReturnOptValBool(options, OPTION_ALPHA, DEFAULT_ALPHA);
        if (!xf86GetOptValInteger(options, OPTION_COLORKEY, (int *)&config.colorKey)) {
            config.colorKey = DEFAULT_COLORKEY;
        }

        xf86AddDriver (&V4L2, module, 0);

        return (pointer)1;
    }
}

#else

#include <fcntl.h>
#include <sys/ioctl.h>

#endif

typedef struct _PortPrivRec {
    ScrnInfoPtr                 pScrn;

    /* file handle */
    int 			nr;

    XF86VideoEncodingPtr        enc;
    int                         *input;
    int                         *norm;
    int                         nenc,cenc;

    /* yuv to offscreen */
    XF86OffscreenImagePtr       format;   /* list */
    int                         nformat;  /* # if list entries */
    XF86OffscreenImagePtr       myfmt;    /* which one is YUY2 (packed) */
    int                         yuv_format;

    /* colorkey */
    CARD32                      colorKey;

} PortPrivRec, *PortPrivPtr;

#define XV_ENCODING	"XV_ENCODING"
#define XV_BRIGHTNESS  	"XV_BRIGHTNESS"
#define XV_CONTRAST 	"XV_CONTRAST"
#define XV_SATURATION  	"XV_SATURATION"
#define XV_HUE		"XV_HUE"

#define XV_FREQ		"XV_FREQ"
#define XV_MUTE		"XV_MUTE"
#define XV_VOLUME      	"XV_VOLUME"

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)

static Atom xvEncoding, xvBrightness, xvContrast, xvSaturation, xvHue;
static Atom xvFreq, xvMute, xvVolume;

static XF86VideoFormatRec
InputVideoFormats[] = {
        { 15, TrueColor },
        { 16, TrueColor },
        { 24, TrueColor },
        { 32, TrueColor },
};

#define V4L2_ATTR (sizeof(Attributes) / sizeof(XF86AttributeRec))

static const XF86AttributeRec Attributes[] = {
        {XvSettable | XvGettable, -1000,    1000, XV_ENCODING},
        {XvSettable | XvGettable, -1000,    1000, XV_BRIGHTNESS},
        {XvSettable | XvGettable, -1000,    1000, XV_CONTRAST},
        {XvSettable | XvGettable, -1000,    1000, XV_SATURATION},
        {XvSettable | XvGettable, -1000,    1000, XV_HUE},
};
static const XF86AttributeRec VolumeAttr = 
{XvSettable | XvGettable, -1000,    1000, XV_VOLUME};
static const XF86AttributeRec MuteAttr = 
{XvSettable | XvGettable,     0,       1, XV_MUTE};
static const XF86AttributeRec FreqAttr = 
{XvSettable | XvGettable,     0, 16*1000, XV_FREQ};

#define V4L2_FD   (v4l2_devices[pPPriv->nr].fd)
#define V4L2_NAME (v4l2_devices[pPPriv->nr].devName)

static struct V4L2_DEVICE {
    int  fd;
    char *devName;
} *v4l2_devices = NULL;

/* ---------------------------------------------------------------------- */
/* forward decl */

static void V4L2QueryBestSize(ScrnInfoPtr pScrn, Bool motion,
        short vid_w, short vid_h, short drw_w, short drw_h,
        unsigned int *p_w, unsigned int *p_h, pointer data);

/* ---------------------------------------------------------------------- */
static void
V4L2SetupDevice(PortPrivPtr pPPriv, ScrnInfoPtr pScrn)
{
    struct v4l2_framebuffer fbuf;

    /* setup alpha or colorkey if supported */

    memset(&fbuf, 0x00, sizeof(fbuf));

    if (-1 == ioctl(V4L2_FD, VIDIOC_G_FBUF, &fbuf)) {
        perror("ioctl VIDIOC_G_FBUF");
    }

    DEBUG("flags=%08x", fbuf.flags);
    DEBUG("capability=%08x", fbuf.capability);
    DEBUG("pixelformat=%08x", fbuf.fmt.pixelformat);

    if(fbuf.capability & (V4L2_FBUF_CAP_CHROMAKEY | V4L2_FBUF_CAP_LOCAL_ALPHA)) {
        struct v4l2_format format;

        fbuf.flags = V4L2_FBUF_FLAG_OVERLAY;
        fbuf.fmt.pixelformat = V4L2_PIX_FMT_BGR32;  // ???

        /* prefer alpha blending to colorkey, if both are supported: */
        if(config.alpha && (fbuf.capability & V4L2_FBUF_CAP_LOCAL_ALPHA)) {
            xf86Msg(X_INFO, "v4l2: enabling local-alpha for %s\n", V4L2_NAME);
            fbuf.flags |= V4L2_FBUF_FLAG_LOCAL_ALPHA;
            pPPriv->colorKey = 0xff000000;
            V4L2SetupAlpha();
        } else {
            xf86Msg(X_INFO, "v4l2: enabling chromakey for %s\n", V4L2_NAME);
            fbuf.flags |= V4L2_FBUF_FLAG_CHROMAKEY;
        }

        /* @todo do we need to keep track of pixelformat?  At a minimum
         * we could select chromakey or alpha depending on the pixel
         * format, I think..
         */

        if (-1 == ioctl(V4L2_FD, VIDIOC_S_FBUF, &fbuf)) {
            perror("ioctl VIDIOC_S_FBUF");
        }

        memset(&format, 0x00, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;

        if (-1 == ioctl(V4L2_FD, VIDIOC_G_FMT, &format)) {
            perror("ioctl VIDIOC_G_FMT");
        }

        format.fmt.win.chromakey = pPPriv->colorKey;

        if (-1 == ioctl(V4L2_FD, VIDIOC_S_FMT, &format)) {
            perror("ioctl VIDIOC_S_FMT");
        }
    } else {
        xf86Msg(X_INFO, "v4l2: neither chromakey or alpha is supported by %s\n", V4L2_NAME);
    }
}

static int
V4L2OpenDevice(PortPrivPtr pPPriv, ScrnInfoPtr pScrn)
{
    static int first = 1;

    if (-1 == V4L2_FD) {
        V4L2_FD = open(V4L2_NAME, O_RDWR, 0);

        if (first) {
            first = 0;
            xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, 2,
                    "v4l2: memPhysBase=0x%lx\n", pScrn->memPhysBase);
        }
        DEBUG("Xv/OD width=%d, height=%d, depth=%d",
                pScrn->virtualX, pScrn->virtualY, pScrn->bitsPerPixel);

        if (-1 != V4L2_FD) {
            V4L2SetupDevice(pPPriv, pScrn);
        }
    }

    if (-1 == V4L2_FD) {
        DEBUG("failed to open device");
        return errno;
    }

    DEBUG("Xv/OD: fd=%d",V4L2_FD);

    return 0;
}

static void
V4L2CloseDevice(PortPrivPtr pPPriv, ScrnInfoPtr pScrn)
{
    DEBUG("Xv/CD: fd=%d", V4L2_FD);
    if (-1 != V4L2_FD) {
        close(V4L2_FD);
        V4L2_FD = -1;
        DEBUG("Xv/CD: device is closed");
    }
}

static int
V4L2UpdateOverlay(PortPrivPtr pPPriv, ScrnInfoPtr pScrn,
        short drw_x, short drw_y, short drw_w, short drw_h,
        RegionPtr clipBoxes, DrawablePtr pDraw)
{
    struct v4l2_format format;

    /* @todo check if x/y w/h has actually changed to avoid unnecessary ioctl
     */

    /* Open a file handle to the device */
    if (V4L2OpenDevice(pPPriv, pScrn))
        return Success;

    memset(&format, 0x00, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;

    if (-1 == ioctl(V4L2_FD, VIDIOC_G_FMT, &format)) {
        perror("ioctl VIDIOC_G_FMT");
    }

    format.fmt.win.chromakey = pPPriv->colorKey;

    format.fmt.win.w.top = drw_y;
    format.fmt.win.w.left = drw_x;

    if (drw_w != -1) {
        format.fmt.win.w.width = drw_w;
    }
    if (drw_h != -1) {
        format.fmt.win.w.height = drw_h;
    }

    if (-1 == ioctl(V4L2_FD, VIDIOC_S_FMT, &format)) {
        perror("ioctl VIDIOC_S_FMT");
    }

    V4L2EnableAlpha(TRUE);
    xf86XVFillKeyHelper(pDraw->pScreen, pPPriv->colorKey, clipBoxes);
    V4L2EnableAlpha(FALSE);

    return Success;
}

static int
V4L2PutVideo(ScrnInfoPtr pScrn,
        short vid_x, short vid_y, short drw_x, short drw_y,
        short vid_w, short vid_h, short drw_w, short drw_h,
        RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;

    DEBUG("Xv/PV vid_x=%d, vid_y=%d, vid_w=%d, vid_h=%d",
            vid_x, vid_y, vid_w, vid_h);
    DEBUG("Xv/PV drw_x=%d, drw_y=%d, drw_w=%d, drw_h=%d",
            drw_x, drw_y, drw_w, drw_h);

    return V4L2UpdateOverlay(pPPriv, pScrn,
            drw_x, drw_y, drw_w, drw_h,
            clipBoxes, pDraw);
}

static int
V4L2PutStill(ScrnInfoPtr pScrn,
        short vid_x, short vid_y, short drw_x, short drw_y,
        short vid_w, short vid_h, short drw_w, short drw_h,
        RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;

    DEBUG("Xv/PS vid_x=%d, vid_y=%d, vid_w=%d, vid_h=%d",
            vid_x, vid_y, vid_w, vid_h);
    DEBUG("Xv/PS drw_x=%d, drw_y=%d, drw_w=%d, drw_h=%d",
            drw_x, drw_y, drw_w, drw_h);

    return V4L2UpdateOverlay(pPPriv, pScrn,
            drw_x, drw_y, drw_w, drw_h,
            clipBoxes, pDraw);
}

static int
V4L2ReputImage(ScrnInfoPtr pScrn, short drw_x, short drw_y,
        RegionPtr clipBoxes, pointer data, DrawablePtr pDraw )
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;

    DEBUG("Xv/RI drw_x=%d, drw_y=%d", drw_x, drw_y);

    return V4L2UpdateOverlay(pPPriv, pScrn,
            drw_x, drw_y, -1, -1,
            clipBoxes, pDraw);
}

static void
V4L2StopVideo(ScrnInfoPtr pScrn, pointer data, Bool shutdown)
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;

    DEBUG("Xv/StopVideo shutdown=%d",shutdown);

    if (shutdown) {
        V4L2CloseDevice(pPPriv, pScrn);
    }
}

/* v4l2 uses range 0 - 65535; Xv uses -1000 - 1000 */
static int
v4l2_to_xv(int val) {
    val = val * 2000 / 65536 - 1000;
    if (val < -1000) val = -1000;
    if (val >  1000) val =  1000;
    return val;
}

static int
xv_to_v4l2(int val) {
    val = val * 65536 / 2000 + 32768;
    if (val <    -0) val =     0;
    if (val > 65535) val = 65535;
    return val;
}

static int
V4L2SetPortAttribute(ScrnInfoPtr pScrn,
        Atom attribute, INT32 value, pointer data)
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;
    int ret = Success;

    if (V4L2OpenDevice(pPPriv, pScrn))
        return Success;

    DEBUG("Xv/SPA %lu, %ld", attribute, value);

    if (-1 == V4L2_FD) {
        ret = Success;
    } else {
        ret = BadValue;
    }

    V4L2CloseDevice(pPPriv,pScrn);
    return ret;
}

static int
V4L2GetPortAttribute(ScrnInfoPtr pScrn,
        Atom attribute, INT32 *value, pointer data)
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;
    int ret = Success;

    if (V4L2OpenDevice(pPPriv, pScrn))
        return Success;

    DEBUG("Xv/GPA %lu", attribute);

    if (-1 == V4L2_FD) {
        ret = Success;
    } else {
        ret = BadValue;
    }

    DEBUG("Xv/GPA %lu, %ld", attribute, *value);

    V4L2CloseDevice(pPPriv,pScrn);
    return ret;
}

static void
V4L2QueryBestSize(ScrnInfoPtr pScrn, Bool motion,
        short vid_w, short vid_h, short drw_w, short drw_h,
        unsigned int *p_w, unsigned int *p_h, pointer data)
{
    PortPrivPtr pPPriv = (PortPrivPtr) data;
    int maxx = pPPriv->enc[pPPriv->cenc].width;
    int maxy = pPPriv->enc[pPPriv->cenc].height;

    if (0 != pPPriv->yuv_format) {
        *p_w = pPPriv->myfmt->max_width;
        *p_h = pPPriv->myfmt->max_height;
    } else {
        *p_w = (drw_w < maxx) ? drw_w : maxx;
        *p_h = (drw_h < maxy) ? drw_h : maxy;
    }

    DEBUG("Xv/BS %d %dx%d %dx%d", pPPriv->cenc,drw_w,drw_h,*p_w,*p_h);
}

static const OptionInfoRec *
V4L2AvailableOptions(int chipid, int busid)
{
    return V4L2DevOptions;
}

static void
V4L2Identify(int flags)
{
    xf86Msg(X_INFO, "v4l2 driver for Video4Linux2\n");
}        

static int
v4l2_add_enc(XF86VideoEncodingPtr enc, int i,
        int width, int height, int n, int d)
{
    enc[i].id     = i;
    enc[i].name   = xalloc(12);
    if (NULL == enc[i].name)
        return -1;
    enc[i].width  = width;
    enc[i].height = height;
    enc[i].rate.numerator   = n;
    enc[i].rate.denominator = d;
    sprintf(enc[i].name,"%dx%d", width, height);
    return 0;
}

static void
V4L2BuildEncodings(PortPrivPtr p)
{
    int entries = 4;
    p->enc = xalloc(sizeof(XF86VideoEncodingRec) * entries);
    if (NULL == p->enc)
        goto fail;
    v4l2_add_enc(p->enc, p->nenc++,  320,  240, 1001, 30000);
    v4l2_add_enc(p->enc, p->nenc++,  640,  480, 1001, 30000);
    v4l2_add_enc(p->enc, p->nenc++, 1280,  720, 1001, 30000);
    v4l2_add_enc(p->enc, p->nenc++, 1920, 1080, 1001, 30000);

    return;
fail:
    if (p->input)
        xfree(p->input);
    p->input = NULL;
    if (p->norm)
        xfree(p->norm);
    p->norm = NULL;
    if (p->enc)
        xfree(p->enc);
    p->enc = NULL;
    p->nenc = 0;
}

/* add a attribute a list */
static void
v4l2_add_attr(XF86AttributeRec **list, int *count,
        const XF86AttributeRec *attr)
{
    XF86AttributeRec *oldlist = *list;
    int i;

    for (i = 0; i < *count; i++) {
        if (0 == strcmp((*list)[i].name,attr->name)) {
            DEBUG("skip dup attr %s",attr->name);
            return;
        }
    }

    DEBUG("add attr %s",attr->name);
    *list = xalloc((*count + 1) * sizeof(XF86AttributeRec));
    if (NULL == *list) {
        *count = 0;
        return;
    }
    if (*count)
        memcpy(*list, oldlist, *count * sizeof(XF86AttributeRec));
    memcpy(*list + *count, attr, sizeof(XF86AttributeRec));
    (*count)++;
}

/* setup yuv overlay + hw scaling: look if we find some common video
   format which both v4l2 driver and the X-Server can handle */
static void
v4l2_check_yuv(PortPrivPtr pPPriv, ScrnInfoPtr pScrn)
{
    static const struct {
        unsigned int  v4l2_palette;
        unsigned int  v4l2_depth;
        unsigned int  xv_id;
        unsigned int  xv_format;
    } yuvlist[] = {
            { V4L2_PIX_FMT_YUYV, 16, 0x32595559, XvPacked },
            { V4L2_PIX_FMT_UYVY, 16, 0x59565955, XvPacked },
            { V4L2_PIX_FMT_NV12, 12, 0x59565955, XvPlanar },
            { 0 /* end of list */ },
    };
    ScreenPtr pScreen = screenInfo.screens[pScrn->scrnIndex];
    int fmt,i;

    pPPriv->format = xf86XVQueryOffscreenImages(pScreen,&pPPriv->nformat);
    DEBUG("nformat=%d", pPPriv->nformat);
    for (i = 0; i < pPPriv->nformat; i++) {
        DEBUG("format[i].image->id=%08x", i, pPPriv->format[i].image->id);
        DEBUG("format[i].image->format=%d", i, pPPriv->format[i].image->format);
    }
}

static int
V4L2Init(ScrnInfoPtr pScrn, XF86VideoAdaptorPtr **adaptors)
{
    PortPrivPtr pPPriv;
    DevUnion *Private;
    XF86VideoAdaptorPtr *VAR = NULL;
    char *dev, *devices;
    int  fd,i,j;

    /* we need devices to be a mutable string that we own */
    devices = strdup(config.devices);

    DEBUG("init start");

    for (i = 0; dev = strsep(&devices, ","); ) {
        fd = open(dev, O_RDWR, 0);
        DEBUG("open %s -> %d", dev, fd);
        if (fd == -1) {
            xf86Msg(X_INFO, "v4l2: could not open '%s'.. skipping\n", dev);
            continue;
        }
        DEBUG("%s open ok", dev);

        /* our private data */
        pPPriv = xalloc(sizeof(PortPrivRec));
        if (!pPPriv)
            return FALSE;
        memset(pPPriv,0,sizeof(PortPrivRec));
        pPPriv->nr = i;

        pPPriv->colorKey = config.colorKey;

        /* check device */
#if 0 /* @todo */
        if (-1 == ioctl(fd,VIDIOCGCAP,&pPPriv->cap) ||
                0 == (pPPriv->cap.type & VID_TYPE_OVERLAY)) {
            DEBUG("%s: no overlay support", dev);
            xfree(pPPriv);
            close(fd);
            continue;
        }
#endif

        /* grow array of devices */
        v4l2_devices = xrealloc(v4l2_devices, sizeof(v4l2_devices[0]) * (i + 1));

        V4L2_NAME = dev;
        V4L2_FD = fd;
        V4L2BuildEncodings(pPPriv);
        if (NULL == pPPriv->enc)
            return FALSE;
        v4l2_check_yuv(pPPriv, pScrn);

        /* alloc VideoAdaptorRec */
        VAR = xrealloc(VAR,sizeof(XF86VideoAdaptorPtr)*(i+1));
        VAR[i] = xalloc(sizeof(XF86VideoAdaptorRec));
        if (!VAR[i])
            return FALSE;
        memset(VAR[i],0,sizeof(XF86VideoAdaptorRec));


        /* build attribute list */
        for (j = 0; j < V4L2_ATTR; j++) {
            /* video attributes */
            v4l2_add_attr(&VAR[i]->pAttributes, &VAR[i]->nAttributes,
                    &Attributes[j]);
        }

        if (0 != pPPriv->yuv_format) {
            /* pass throuth scaler attributes */
            for (j = 0; j < pPPriv->myfmt->num_attributes; j++) {
                v4l2_add_attr(&VAR[i]->pAttributes, &VAR[i]->nAttributes,
                        pPPriv->myfmt->attributes+j);
            }
        }

        /* hook in private data */
        Private = xalloc(sizeof(DevUnion));
        if (!Private)
            return FALSE;
        memset(Private,0,sizeof(DevUnion));
        Private->ptr = (pointer)pPPriv;
        VAR[i]->pPortPrivates = Private;
        VAR[i]->nPorts = 1;

        /* init VideoAdaptorRec */
        VAR[i]->type  = XvInputMask | XvWindowMask | XvVideoMask;
        VAR[i]->name  = "video4linux2";
        VAR[i]->flags = 0;

        VAR[i]->PutVideo = V4L2PutVideo;
        VAR[i]->PutStill = V4L2PutStill;
        VAR[i]->ReputImage = V4L2ReputImage;
        VAR[i]->StopVideo = V4L2StopVideo;
        VAR[i]->SetPortAttribute = V4L2SetPortAttribute;
        VAR[i]->GetPortAttribute = V4L2GetPortAttribute;
        VAR[i]->QueryBestSize = V4L2QueryBestSize;

        VAR[i]->nEncodings = pPPriv->nenc;
        VAR[i]->pEncodings = pPPriv->enc;
        VAR[i]->nFormats =
                sizeof(InputVideoFormats) / sizeof(InputVideoFormats[0]);
        VAR[i]->pFormats = InputVideoFormats;

        /* ensure colorkey is properly configured.. some drivers need this
         * before STREAMON.. */
        V4L2SetupDevice(pPPriv, pScrn);

        V4L2CloseDevice(pPPriv, pScrn);
        DEBUG("%s closed ok", dev);

        i++;
    }

    xvEncoding   = MAKE_ATOM(XV_ENCODING);
    xvHue        = MAKE_ATOM(XV_HUE);
    xvSaturation = MAKE_ATOM(XV_SATURATION);
    xvBrightness = MAKE_ATOM(XV_BRIGHTNESS);
    xvContrast   = MAKE_ATOM(XV_CONTRAST);

    xvFreq       = MAKE_ATOM(XV_FREQ);
    xvMute       = MAKE_ATOM(XV_MUTE);
    xvVolume     = MAKE_ATOM(XV_VOLUME);

    DEBUG("init done, %d device(s) found",i);

    *adaptors = VAR;
    return i;
}

static Bool
V4L2Probe(DriverPtr drv, int flags)
{
    DEBUG("probe, flags=%08x", flags);

    if (flags & PROBE_DETECT)
        return TRUE;

    xf86XVRegisterGenericAdaptorDriver(V4L2Init);
    drv->refCount++;
    return TRUE;
}

