/*
 * v4l2.h
 *
 *  Created on: Jul 19, 2010
 *      Author: robclark
 */

#ifndef __V4L2_H__
#define __V4L2_H__

typedef struct {
    int debug;
    const char *devices;
    int alpha;
    CARD32 colorKey;
} V4L2Config;

extern V4L2Config config;

#define DEBUG(fmt, ...) do {                                         \
    if (config.debug)                                                \
        xf86Msg(X_INFO, "v4l2: "fmt"\n", ##__VA_ARGS__);             \
    } while (0)


typedef struct _PortPrivRec {
    ScrnInfoPtr                 pScrn;

    /* file handle */
    int                         nr;

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


/* used when alpha blending is enabled */
void V4L2SetupAlpha(PortPrivPtr pPPriv);
void V4L2SetClip(PortPrivPtr pPPriv, DrawablePtr pDraw, RegionPtr clipBoxes);
void V4L2ClearClip(PortPrivPtr pPPriv);

#ifndef MAX
#  define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#  define MIN(a,b) ((a) > (b) ? (b) : (a))
#endif

#endif /* __V4L2_H__ */
