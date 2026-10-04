#include <pspkernel.h>

PSP_MODULE_INFO("Skiff", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
/* Leave 1 MB for the system; the rest becomes heap for downloads, covers and TLS buffers. */
PSP_HEAP_SIZE_KB(-1024);
