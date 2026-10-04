#ifndef SKIFF_PSP_ARK_SYSCTRL_H
#define SKIFF_PSP_ARK_SYSCTRL_H

/* Functions from ARK's SystemCtrlForUser library; stubs in ark_sysctrl.S. */

/* What an import from a library that is not loaded returns:
 * SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED. */
#define SKIFF_PSP_IMPORT_NOT_LINKED ((int)0x8002013A)

/* ARK's version; SKIFF_PSP_IMPORT_NOT_LINKED when no ARK custom firmware is running. */
int sctrlHENGetVersion(void);

/*
 * 32 bits from the KIRK crypto engine's random generator, read by ARK in kernel mode (KIRK command
 * 0xE). Only meaningful when sctrlHENGetVersion() shows ARK is present.
 */
unsigned int sctrlKernelRand(void);

#endif
