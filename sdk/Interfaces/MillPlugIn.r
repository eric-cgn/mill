/* MillPlugIn.r: the 'MIfc' resource every Plug-In file needs, ID 128. It says which
   interface the Plug-In was built for. Mill reads it with the Resource Manager before it
   loads any of the Plug-In's code, and skips a file without it or with another number,
   so a Plug-In made for another Mill never runs.

   The format: one integer (2 bytes), the interface number, kMillInterface. Mill reads
   only those 2 bytes; a later interface may add fields after them.

   Rez it into the Plug-In file with -i set to this folder (MillInterface.h is here). */
#include "MillInterface.h"

type 'MIfc' {
    integer;                    /* the interface: kMillInterface */
};

resource 'MIfc' (128, "Interface") {
    kMillInterface
};
