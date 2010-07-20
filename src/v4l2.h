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

/* used when alpha blending is enabled */
void V4L2SetupAlpha(void);
void V4L2EnableAlpha(Bool b);

#endif /* __V4L2_H__ */
