#ifndef SKIFF_ERROR_H
#define SKIFF_ERROR_H

/*
 * Every fallible function in Skiff returns a skiff_err. The table below is the single source of
 * truth: the enum, the stable machine name (used in logs and bug reports) and the message shown
 * to the player are all generated from it, so they can never drift apart.
 *
 * Codes are grouped by layer. Append new codes at the end of their group; never renumber, because
 * the numeric value is printed in logs that users paste into issues.
 */
#define SKIFF_ERROR_TABLE(X)                                                                       \
    X(SKIFF_OK, 0, "No error")                                                                     \
    X(SKIFF_ERR_INVALID_ARG, 1, "Internal error: invalid argument")                                \
    X(SKIFF_ERR_NO_MEMORY, 2, "The PSP ran out of memory")                                         \
    X(SKIFF_ERR_BUFFER_TOO_SMALL, 3, "Internal error: buffer too small")                           \
    X(SKIFF_ERR_NOT_IMPLEMENTED, 4, "This feature is not available yet")                           \
    X(SKIFF_ERR_NET_UNAVAILABLE, 100, "Wi-Fi is off or no network profile is set up")              \
    X(SKIFF_ERR_NET_DNS, 101, "Could not find the RomM server; check the address")                 \
    X(SKIFF_ERR_NET_CONNECT, 102, "Could not reach the RomM server")                               \
    X(SKIFF_ERR_NET_TIMEOUT, 103, "The RomM server took too long to answer")                       \
    X(SKIFF_ERR_NET_TLS_HANDSHAKE, 104, "Secure connection failed (TLS handshake)")                \
    X(SKIFF_ERR_NET_TLS_UNTRUSTED, 105, "The server certificate is not trusted")                   \
    X(SKIFF_ERR_NET_TLS_CLIENT_CERT, 106, "The client certificate was rejected or is unreadable")  \
    X(SKIFF_ERR_NET_ENTROPY, 107, "Could not gather enough randomness for a secure connection")    \
    X(SKIFF_ERR_NET_TLS_CLOCK, 108,                                                                \
      "The PSP's date and time are wrong; set them in Settings to connect securely")               \
    X(SKIFF_ERR_NET_NEEDS_ARK, 109, "Networking needs ARK custom firmware (ARK-4 or ARK-5)")       \
    X(SKIFF_ERR_NET_WIFI_JOIN, 110, "Could not join the Wi-Fi network")                            \
    X(SKIFF_ERR_NET_CONNECTION_LOST, 111, "The connection to the RomM server was lost")            \
    X(SKIFF_ERR_ROMM_UNAUTHORIZED, 200, "RomM rejected the login; pair this PSP again")            \
    X(SKIFF_ERR_ROMM_FORBIDDEN, 201, "Your RomM user is not allowed to do this")                   \
    X(SKIFF_ERR_ROMM_NOT_FOUND, 202, "RomM could not find that item")                              \
    X(SKIFF_ERR_ROMM_SERVER, 203, "RomM reported a server error")                                  \
    X(SKIFF_ERR_ROMM_BAD_RESPONSE, 204, "RomM sent a response Skiff does not understand")          \
    X(SKIFF_ERR_ROMM_UNSUPPORTED_VERSION, 205, "This RomM version is not supported")               \
    X(SKIFF_ERR_ROMM_CHECKSUM, 206,                                                                \
      "The file does not match RomM's checksum; rescan the platform in RomM")                      \
    X(SKIFF_ERR_STORAGE_NO_MEDIA, 300, "No Memory Stick found")                                    \
    X(SKIFF_ERR_STORAGE_NO_SPACE, 301, "Not enough free space on the Memory Stick")                \
    X(SKIFF_ERR_STORAGE_IO, 302, "Could not read or write the Memory Stick")                       \
    X(SKIFF_ERR_STORAGE_NOT_FOUND, 303, "File or folder not found on the Memory Stick")            \
    X(SKIFF_ERR_STORAGE_FILE_TOO_LARGE, 304,                                                       \
      "File is larger than 4 GB, which the PSP cannot store")                                      \
    X(SKIFF_ERR_CONFIG_PARSE, 400, "The settings file is damaged")                                 \
    X(SKIFF_ERR_CONFIG_MISSING_KEY, 401, "A required setting is missing")                          \
    X(SKIFF_ERR_CONFIG_INVALID_VALUE, 402, "A setting has an invalid value")

typedef enum skiff_err {
#define SKIFF_ERROR_ENUM(name, value, message) name = (value),
    SKIFF_ERROR_TABLE(SKIFF_ERROR_ENUM)
#undef SKIFF_ERROR_ENUM
} skiff_err;

/* Stable identifier such as "SKIFF_ERR_NET_TIMEOUT"; "SKIFF_ERR_UNKNOWN" outside the table. */
const char *skiff_err_name(skiff_err err);

/* Human-readable sentence for the player; a generic message for values outside the table. */
const char *skiff_err_message(skiff_err err);

#endif
