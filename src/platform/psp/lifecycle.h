#ifndef SKIFF_PSP_LIFECYCLE_H
#define SKIFF_PSP_LIFECYCLE_H

/* Registers the HOME-menu exit callback; the main loop polls skiff_psp_exit_requested(). */
void skiff_psp_install_exit_callback(void);

int skiff_psp_exit_requested(void);

#endif
