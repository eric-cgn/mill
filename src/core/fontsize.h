/* Which size and style to draw a run of text in, from the strikes there are */
#ifndef MILL_FONTSIZE_H
#define MILL_FONTSIZE_H

/* has(ctx, n, style) answers for size n in STYLE (0 plain, else QuickDraw's bold and italic
   bits): 0 if there's no real strike of it (a bitmap of the size, or an outline; a style is
   real when the strike has it, not when QuickDraw makes it), else which strike (the caller's
   numbering; the one it would rather have first).
   font_pick takes, in order: a real strike of SIZE in STYLE; the nearest in STYLE within an
   eighth of SIZE, the smaller on a tie; the same two in plain, QuickDraw then making the
   style. It gives the size and sets *which to the answer; SIZE itself, with *which 0, if
   none: the Font Manager then scales. */
short font_pick(short size, int style, int (*has)(void *ctx, short n, int style), void *ctx, int *which);

#endif
