/* Client-side image maps: an IMG's USEMAP names a MAP, whose AREAs (rectangles, circles,
   and polygons, in the image's pixels as shown) each go somewhere. */
#ifndef MILL_IMAGEMAP_H
#define MILL_IMAGEMAP_H

#include "html.h"

/* The MAP an IMG's USEMAP ("#name", or "page.html#name") names on this page, or -1 */
long imagemap_for(const Doc *d, long img);
/* The AREA that takes the point (x, y) in a map, or -1: the first that holds it wins,
   and one with NOHREF (or no HREF) takes the point but goes nowhere */
long imagemap_hit(const Doc *d, long map, short x, short y);

#endif
