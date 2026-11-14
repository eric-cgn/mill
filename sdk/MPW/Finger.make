# Finger.make: the Finger example Plug-In, built with MPW 3.5 (SC and Link).
# In the MPW Shell, set the directory to this folder, then:
#     Make -f Finger.make > Finger.out; Finger.out

Kit = ::
Example = {Kit}Examples:Finger:
# -b: strings in the code, not in globals (a code resource has none). The glue
# goes first: its MillMain is the first function, and has to be at byte 0.
COptions = -b -mbg off -opt space -i {Kit}Interfaces:

Finger ƒ MillGlue.c.o Finger.c.o
	Link -rt MPlg=128 -m MILLMAIN -sg Finger -t MPlg -c MiLL ∂
		MillGlue.c.o Finger.c.o ∂
		"{Libraries}Interface.o" "{CLibraries}StdCLib.o" ∂
		-o Finger

Finger.c.o ƒ {Example}Finger.c {Kit}Interfaces:MillPlugIn.h
	SC {COptions} {Example}Finger.c -o Finger.c.o

MillGlue.c.o ƒ {Kit}Glue:MillGlue.c {Kit}Interfaces:MillPlugIn.h
	SC {COptions} {Kit}Glue:MillGlue.c -o MillGlue.c.o
