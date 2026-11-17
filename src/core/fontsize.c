/* Which size and style to draw a run of text in, from the strikes there are (fontsize.h) */
#include "fontsize.h"

/* SIZE in STYLE, or the nearest within an eighth of it (the smaller on a tie); 0 if none */
static short near(short size, int style, int (*has)(void *ctx, short n, int style), void *ctx, int *which)
{
    short d;
    int w;
    for (d = 0; d * 8 <= size; d++) {
        if (size - d >= 1 && (w = has(ctx, (short)(size - d), style))) { *which = w; return (short)(size - d); }
        if (d && size + d <= 255 && (w = has(ctx, (short)(size + d), style))) { *which = w; return (short)(size + d); }
    }
    return 0;
}

short font_pick(short size, int style, int (*has)(void *ctx, short n, int style), void *ctx, int *which)
{
    short n;
    if (style && (n = near(size, style, has, ctx, which))) return n;
    if ((n = near(size, 0, has, ctx, which))) return n;
    *which = 0;
    return size;
}
