/* splash_glyphs FONT.ttf SIZE: each character through the outline Plug-In's engine
   (plugins/fonts), as lines "code advance left top width height hexbits", for
   tools/splash.py. Built and run on the host; the engine draws the same pixels there. */
#include "tt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t tt_peak, tt_cur;
void *tt_alloc(uint32_t n) { return malloc(n ? n : 1); }
void tt_free(void *v) { free(v); }

static unsigned char *data;
static long dlen;

static int rd(void *c, uint32_t off, uint16_t len, void *buf)
{
    (void)c;
    if (off + (long)len > dlen) return -1;
    memcpy(buf, data + off, len);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *fp;
    TTFont f;
    TTRaster r;
    int i, k;
    if (argc != 3 || !(fp = fopen(argv[1], "rb"))) return 2;
    memset(&r, 0, sizeof r);
    fseek(fp, 0, SEEK_END); dlen = ftell(fp); rewind(fp);
    data = malloc(dlen);
    if (fread(data, 1, dlen, fp) != (size_t)dlen) return 2;
    fclose(fp);
    if (tt_open(&f, rd, 0)) return 1;
    tt_scale(&f, atoi(argv[2]));
    for (i = 0; i <= TT_NCH; i++) {
        TTGlyph *g = tt_glyph(&f, &r, i);
        if (!g) continue;
        printf("%d %d %d %d %d %d ", i + 32, g->adv, g->left, g->top, g->w, g->h);
        for (k = 0; k < g->rowBytes * g->h; k++) printf("%02x", g->bits[k]);
        printf("\n");
        free(g);
    }
    tt_rfree(&r);
    tt_close(&f);
    return 0;
}
