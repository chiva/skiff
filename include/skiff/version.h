#ifndef SKIFF_VERSION_H
#define SKIFF_VERSION_H

/* Values come from CMake's project(VERSION ...), which release-please bumps on every release. */
const char *skiff_version_string(void);
int skiff_version_major(void);
int skiff_version_minor(void);
int skiff_version_patch(void);

#endif
