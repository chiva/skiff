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
    X(SKIFF_ERR_CANCELLED, 5, "Cancelled")                                                         \
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
    X(SKIFF_ERR_ROMM_PAIRING_DENIED, 207, "Pairing was refused in RomM")                           \
    X(SKIFF_ERR_ROMM_PAIRING_EXPIRED, 208, "The pairing code expired; start pairing again")        \
    X(SKIFF_ERR_ROMM_PAIRING_SCOPES, 209,                                                          \
      "Pairing was approved without the permissions Skiff needs; pair again and allow them")       \
    X(SKIFF_ERR_ROMM_COVER_FORMAT, 210, "RomM sent a cover in a format Skiff cannot show")         \
    X(SKIFF_ERR_ROMM_COVER_DAMAGED, 211, "RomM sent a cover Skiff could not read")                 \
    X(SKIFF_ERR_ROMM_SAVE_CONFLICT, 212, "RomM has a newer save since this PSP last synced")       \
    X(SKIFF_ERR_ROMM_SAVE_TOO_LARGE, 213, "The save is too large for the RomM server to accept")   \
    X(SKIFF_ERR_ROMM_COLLECTIONS_TOO_LARGE, 214,                                                   \
      "RomM has too many collections, or too large ones, for the PSP to list")                     \
    X(SKIFF_ERR_ROMM_SAVES_UNSUPPORTED, 215, "Save sync needs RomM 5.4 or newer")                  \
    X(SKIFF_ERR_ROMM_SAVES_FORBIDDEN, 216,                                                         \
      "This PSP may not sync saves with RomM; pair it again and allow save sync")                  \
    X(SKIFF_ERR_ROMM_SAVE_MISMATCH, 217,                                                           \
      "The save from RomM does not match RomM's checksum; sync it again")                          \
    X(SKIFF_ERR_ROMM_SENDS_FORBIDDEN, 218,                                                         \
      "RomM did not let this PSP receive games: pair again and allow it")                          \
    X(SKIFF_ERR_ROMM_SEND_NOT_PSP, 219, "That game is not a PSP game")                             \
    X(SKIFF_ERR_ROMM_CONVERTING, 220, "RomM is still converting the game")                         \
    X(SKIFF_ERR_ROMM_CONVERSION_UNAVAILABLE, 221, "RomM cannot convert this game for the PSP")     \
    X(SKIFF_ERR_STORAGE_NO_MEDIA, 300, "No Memory Stick found")                                    \
    X(SKIFF_ERR_STORAGE_NO_SPACE, 301, "Not enough free space on the Memory Stick")                \
    X(SKIFF_ERR_STORAGE_IO, 302, "Could not read or write the Memory Stick")                       \
    X(SKIFF_ERR_STORAGE_NOT_FOUND, 303, "File or folder not found on the Memory Stick")            \
    X(SKIFF_ERR_STORAGE_FILE_TOO_LARGE, 304,                                                       \
      "File is larger than 4 GB, which the PSP cannot store")                                      \
    X(SKIFF_ERR_STORAGE_NAME_TAKEN, 305,                                                           \
      "A file Skiff did not install already has this game's name")                                 \
    X(SKIFF_ERR_STORAGE_SAVE_DAMAGED, 306, "A save is damaged or unsafe, so Skiff did not use it") \
    X(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE, 307,                                                      \
      "A save folder on the Memory Stick holds something Skiff cannot sync")                       \
    X(SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED, 308,                                                \
      "A save restore was cut short; Skiff has now finished it")                                   \
    X(SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED, 309,                                              \
      "A save restore was cut short and could not be finished; Skiff kept a backup")               \
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
