#ifndef SKIFF_ROMM_INSTALLS_H
#define SKIFF_ROMM_INSTALLS_H

/*
 * RomM's install calls (RomM 5.4 or newer, while its heartbeat says so:
 * skiff_romm_server.installs_enabled), over the client from skiff/romm.h: the games a player sends
 * to this PSP from RomM's web UI ("Install on device"). RomM keeps each send as an install request
 * until the device reports it; it can push them over a socket, which Skiff does not hold, so Skiff
 * claims them instead:
 *
 *   1. skiff_romm_installs_register() (PUT /api/devices/{id}): this PSP's capabilities,
 *      {"remote_install": on}. RomM replaces the device's whole capabilities with what is sent
 *      (Skiff sets no other), keeps them while Skiff is not running, and offers Install on device
 *      only for a device whose remote_install is true.
 *   2. skiff_romm_installs_claim() (POST /api/devices/{id}/installs/claim): RomM takes every
 *      pending request and returns every one this PSP holds taken, oldest first, again on every
 *      claim until it is reported: nothing is lost when a claim is not acted on.
 *   3. skiff_romm_installs_report() (PUT /api/devices/{id}/installs/{request_id}): ends a taken
 *      request. RomM shows done as "Added to downloads" and notifies the player.
 *
 * RomM checks nothing about the platform: a game of any platform but Windows can be sent to the
 * PSP, so the caller refuses what is not a PSP game (SKIFF_ERR_ROMM_SEND_NOT_PSP as the reason).
 *
 * The token needs devices.write and roms.read, and claims and reports need it bound to this device
 * (a pairing's token). Every call returns what skiff/romm.h's calls return, SKIFF_ERR_INVALID_ARG
 * for a NULL argument or a value RomM would refuse, and for RomM's statuses: 403 is
 * SKIFF_ERR_ROMM_SENDS_FORBIDDEN (the token may not receive games, or is not this PSP's), 404 is
 * SKIFF_ERR_ROMM_NOT_FOUND (the device was deleted in RomM, the feature turned off, or, for a
 * report, the request ended: cancelled in the web UI or already reported). Nothing is logged: the
 * token travels only in the Authorization header.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/romm.h"

/* A request id as RomM makes it (Python's str(uuid4())): 36 lowercase hexadecimal digits and
 * hyphens. Buffer sizes include the terminator. */
#define SKIFF_ROMM_INSTALL_REQUEST_ID_LENGTH 36
#define SKIFF_ROMM_INSTALL_REQUEST_ID_MAX (SKIFF_ROMM_INSTALL_REQUEST_ID_LENGTH + 1)
/* Requests one claim keeps, oldest first: the most jobs Skiff's queue holds (SKIFF_JOBS_MAX). The
 * rest stay taken in RomM and come back on the next claim. */
#define SKIFF_ROMM_INSTALL_CLAIM_MAX 64
/* A request's file ids read (and checked) at most; the rest are only counted. */
#define SKIFF_ROMM_INSTALL_FILE_IDS_READ_MAX 64
/* The longest reason a report sends, in bytes: RomM takes 500 characters, and a UTF-8 character is
 * at least one byte. */
#define SKIFF_ROMM_INSTALL_REASON_MAX_BYTES 500

/* How a device ends a request it took. */
typedef enum skiff_romm_install_status {
    /* Queued on this PSP: RomM's web UI says "Added to downloads". */
    SKIFF_ROMM_INSTALL_DONE,
    SKIFF_ROMM_INSTALL_ALREADY_INSTALLED,
    /* Not added, with a reason RomM shows the player. */
    SKIFF_ROMM_INSTALL_FAILED,
} skiff_romm_install_status;

/* One request a claim returned. */
typedef struct skiff_romm_install_request {
    char id[SKIFF_ROMM_INSTALL_REQUEST_ID_MAX];
    uint64_t rom_id;
    /* The ROM's files RomM lists for it (game, update and DLC files on disk), counted only: the
     * caller reads the ROM's details itself. */
    size_t file_count;
} skiff_romm_install_request;

typedef struct skiff_romm_install_claim {
    /* Every request RomM returned, kept or not. */
    size_t total;
    /* Requests whose id is not a request id Skiff would put in a URL: never kept, never reported
     * (they stay taken in RomM until it expires them, two days by default). */
    size_t dropped;
    /* The first SKIFF_ROMM_INSTALL_CLAIM_MAX of the others, in RomM's order (oldest first). */
    size_t count;
    skiff_romm_install_request requests[SKIFF_ROMM_INSTALL_CLAIM_MAX];
} skiff_romm_install_claim;

/*
 * PUT /api/devices/{device_id} with {"capabilities":{"remote_install":<on>}}: on (1) lets RomM
 * offer this PSP in Install on device, off (0) stops it. Needs devices.write only (not a token
 * bound to the device). An answer that is not this device holding that capability is
 * SKIFF_ERR_ROMM_BAD_RESPONSE: it confirms nothing.
 */
skiff_err skiff_romm_installs_register(skiff_romm_client *client, const char *device_id, int on);

/*
 * POST /api/devices/{device_id}/installs/claim: the requests RomM holds taken for this PSP, as
 * skiff_romm_parse_install_claim() reads them. Also SKIFF_ERR_ROMM_SERVER for RomM's 409 (its
 * requests kept changing under the claim: claim again later). out is empty on error.
 */
skiff_err skiff_romm_installs_claim(skiff_romm_client *client, const char *device_id,
                                    skiff_romm_install_claim *out);

/*
 * PUT /api/devices/{device_id}/installs/{request_id} with {"status":...} and, when reason is not
 * NULL or empty, "reason": its first skiff_romm_install_reason_length() bytes. A 404 or 409 (the
 * request is still pending) is SKIFF_ERR_ROMM_NOT_FOUND: there is nothing left to report. An
 * answer that is not that request ended with that status is SKIFF_ERR_ROMM_BAD_RESPONSE.
 * SKIFF_ERR_INVALID_ARG for a request id skiff_romm_install_request_id_valid() refuses, a status
 * outside skiff_romm_install_status or a reason with a control character.
 */
skiff_err skiff_romm_installs_report(skiff_romm_client *client, const char *device_id,
                                     const char *request_id, skiff_romm_install_status status,
                                     const char *reason);

/* 1 if id is a request id as RomM makes it (SKIFF_ROMM_INSTALL_REQUEST_ID_LENGTH lowercase
 * hexadecimal digits with hyphens after the 8th, 12th, 16th and 20th), the only form Skiff puts in
 * a URL; 0 otherwise or for NULL. */
int skiff_romm_install_request_id_valid(const char *id);

/* The bytes of reason a report sends: all of it up to SKIFF_ROMM_INSTALL_REASON_MAX_BYTES,
 * otherwise cut there or before, where a UTF-8 character starts. 0 for NULL. */
size_t skiff_romm_install_reason_length(const char *reason);

/* ---- Parsing, exposed for the tests and the self-test ---- */

/*
 * A claim response body for device_id: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE for anything RomM
 * would not send: not an array; a request that is not an object, whose id is not a string, whose
 * status is not "taken", whose device_id is not device_id, whose rom_id is not an id, or whose
 * file_ids is not an array of ids. A request id that is a string but not a request id is dropped
 * (counted in dropped). SKIFF_ERR_INVALID_ARG for a NULL out or an invalid device_id. out is empty
 * on error.
 */
skiff_err skiff_romm_parse_install_claim(const char *json, size_t length, const char *device_id,
                                         skiff_romm_install_claim *out);

#endif
