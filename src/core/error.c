#include "skiff/error.h"

static const char UNKNOWN_NAME[] = "SKIFF_ERR_UNKNOWN";
static const char UNKNOWN_MESSAGE[] = "Unexpected error";

const char *skiff_err_name(skiff_err err) {
    switch (err) {
#define SKIFF_ERROR_NAME_CASE(name, value, message)                                                \
    case name:                                                                                     \
        return #name;
        SKIFF_ERROR_TABLE(SKIFF_ERROR_NAME_CASE)
#undef SKIFF_ERROR_NAME_CASE
    }
    return UNKNOWN_NAME;
}

const char *skiff_err_message(skiff_err err) {
    switch (err) {
#define SKIFF_ERROR_MESSAGE_CASE(name, value, message)                                             \
    case name:                                                                                     \
        return message;
        SKIFF_ERROR_TABLE(SKIFF_ERROR_MESSAGE_CASE)
#undef SKIFF_ERROR_MESSAGE_CASE
    }
    return UNKNOWN_MESSAGE;
}
