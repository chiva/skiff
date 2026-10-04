#include "skiff/version.h"

#include "version_config.h"

const char *skiff_version_string(void) { return SKIFF_VERSION_STRING; }

int skiff_version_major(void) { return SKIFF_VERSION_MAJOR; }

int skiff_version_minor(void) { return SKIFF_VERSION_MINOR; }

int skiff_version_patch(void) { return SKIFF_VERSION_PATCH; }
