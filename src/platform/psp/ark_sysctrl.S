/*
 * Imports from the SystemCtrlForUser library of ARK custom firmware (ARK-4 and ARK-5). pspsdk ships
 * no stubs for it, so they are declared by hand. A NID is the first four bytes of the SHA-1 of the
 * function name, read as a little-endian word; checked against the stubs ARK publishes. Without
 * ARK the library is absent and calls return 0x8002013A (library not linked).
 */

    .set noreorder

#include "pspimport.s"

    IMPORT_START "SystemCtrlForUser",0x40090000
    IMPORT_FUNC  "SystemCtrlForUser",0x1090A2E1,sctrlHENGetVersion
    IMPORT_FUNC  "SystemCtrlForUser",0xB364FBB4,sctrlKernelRand
