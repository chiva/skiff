#ifndef SKIFF_ROMM_PAIRING_H
#define SKIFF_ROMM_PAIRING_H

/*
 * Pairing this PSP with RomM through RomM's device-code flow, so the player never types a password
 * or a token on the PSP (docs/guide/04-connect-to-romm.md):
 *
 *   1. skiff_romm_pairing_start() asks RomM for a pairing (POST /api/auth/device/init) and gets a
 *      short user code and the web page where the player enters it.
 *   2. The app shows both. The player approves this PSP in RomM's web UI, on a phone or computer.
 *   3. The app calls skiff_romm_pairing_poll() (POST /api/auth/device/token) every interval_s
 *      seconds until it reports SKIFF_ROMM_PAIRING_APPROVED with a token, or fails.
 *   4. skiff_romm_pairing_config_text() puts the token, RomM's device id and this PSP's id
 *      into config.ini's text, for skiff_config_save().
 *
 * RomM answers a poll the way RFC 8628 describes: 400 with {"detail": "authorization_pending"},
 * "slow_down", "access_denied" or "expired_token", and 200 with the token once approved. The
 * device_code is a credential until it expires and the token is one for good: neither may be logged
 * (the user code is shown on screen and may be). Neither request sends the token: both endpoints
 * are open.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/config.h"
#include "skiff/error.h"
#include "skiff/romm.h"

/* How Skiff introduces itself in RomM's list of devices. */
#define SKIFF_ROMM_PAIRING_CLIENT "skiff"
#define SKIFF_ROMM_PAIRING_NAME "Skiff on PSP"
#define SKIFF_ROMM_PAIRING_PLATFORM "psp"
/* Random bytes behind a device identifier; it is twice as many hexadecimal digits. */
#define SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES 16
/* RFC 8628 §3.5: a "slow_down" answer adds 5 seconds to the polling interval, for good. */
#define SKIFF_ROMM_PAIRING_SLOW_DOWN_S 5
/* The polling interval RomM may ask for, and the lifetime of a pairing: values outside are not a
 * pairing RomM would offer (it offers 5 s and 600 s). */
#define SKIFF_ROMM_PAIRING_INTERVAL_MAX_S 300
#define SKIFF_ROMM_PAIRING_EXPIRES_MAX_S 86400

/* Buffer sizes, terminator included, from RomM's limits on the codes. */
#define SKIFF_ROMM_DEVICE_CODE_MAX 129
#define SKIFF_ROMM_USER_CODE_MAX 33

typedef struct skiff_romm_pairing {
    /* Secret until it expires: never log it. Wiped by skiff_romm_pairing_clear(). */
    char device_code[SKIFF_ROMM_DEVICE_CODE_MAX];
    /* What the player types in RomM, e.g. "7EGGP3VE". */
    char user_code[SKIFF_ROMM_USER_CODE_MAX];
    /* The page to open: the client's server address plus RomM's verification path, without and
     * with the user code (for a QR code). */
    char verification_url[SKIFF_ROMM_URL_MAX];
    char verification_url_complete[SKIFF_ROMM_URL_MAX];
    /* Seconds the pairing stays valid from the start, and between two polls (more after a
     * slow_down). */
    uint32_t expires_in_s;
    uint32_t interval_s;
} skiff_romm_pairing;

typedef enum skiff_romm_pairing_state {
    /* The player has not approved yet: poll again after interval_s. */
    SKIFF_ROMM_PAIRING_PENDING,
    /* Polled too soon: interval_s has grown; poll again after it. */
    SKIFF_ROMM_PAIRING_SLOW_DOWN,
    /* Approved: token and device_id are set. */
    SKIFF_ROMM_PAIRING_APPROVED,
} skiff_romm_pairing_state;

typedef struct skiff_romm_pairing_result {
    skiff_romm_pairing_state state;
    /* Set only when approved. The token is a secret: wiped by skiff_romm_pairing_clear(). */
    char token[SKIFF_CONFIG_TOKEN_MAX];
    char device_id[SKIFF_CONFIG_DEVICE_ID_MAX];
} skiff_romm_pairing_result;

/*
 * This PSP's identifier for pairing: random as lowercase hexadecimal into out. Portable code has no
 * random source, so the caller passes SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES random bytes (on the PSP
 * from KIRK, the source TLS uses). Made once and kept in config.ini ([auth] device_identifier), so
 * RomM sees the same device every time this PSP pairs. SKIFF_ERR_INVALID_ARG for NULL or a size
 * other than SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES, SKIFF_ERR_BUFFER_TOO_SMALL if out cannot hold it.
 */
skiff_err skiff_romm_device_identifier(const unsigned char *random, size_t size, char *out,
                                       size_t out_size);

/*
 * Starts a pairing as device_identifier (from skiff_romm_device_identifier() or config.ini),
 * telling RomM Skiff's name, the platform and skiff_version_string(), and asking for the scopes
 * Skiff needs. SKIFF_OK fills out; otherwise out is empty and the result is the transport's error,
 * the RomM error for the HTTP status (skiff_http_status_error()), SKIFF_ERR_ROMM_BAD_RESPONSE for
 * an answer that is not a pairing (codes missing, too long or with blanks, a verification path that
 * is not a path on the server, an interval or lifetime out of range), or SKIFF_ERR_INVALID_ARG for
 * a NULL argument or a device_identifier that is empty, too long or has blanks.
 */
skiff_err skiff_romm_pairing_start(skiff_romm_client *client, const char *device_identifier,
                                   skiff_romm_pairing *out);

/*
 * Asks once whether the pairing was approved. SKIFF_OK with result->state PENDING, SLOW_DOWN (and
 * pairing->interval_s grown by SKIFF_ROMM_PAIRING_SLOW_DOWN_S; a 429 counts as one) or APPROVED
 * (the token and RomM's device id in result). Otherwise result is empty and the error says why the
 * pairing is over:
 *   - SKIFF_ERR_ROMM_PAIRING_DENIED (207): the player refused it in RomM;
 *   - SKIFF_ERR_ROMM_PAIRING_EXPIRED (208): its code expired or was already used;
 *   - SKIFF_ERR_ROMM_PAIRING_SCOPES (209): approved without reading platforms and ROMs, so the
 *     token could neither browse nor download and is not kept;
 *   - the transport's error, or the RomM error for another HTTP status;
 *   - SKIFF_ERR_ROMM_BAD_RESPONSE: an answer that is neither (an unknown "detail", a token too long
 *     or with blanks, no list of scopes, or a scope Skiff did not ask for);
 *   - SKIFF_ERR_INVALID_ARG: a NULL argument or a pairing without a device code.
 */
skiff_err skiff_romm_pairing_poll(skiff_romm_client *client, skiff_romm_pairing *pairing,
                                  skiff_romm_pairing_result *result);

/*
 * Writes config.ini's text with the approved pairing in [auth]: token, device_id and
 * device_identifier, each through skiff_config_set() (so the rest of the file stays as it was, and
 * the result parses back). out_size should be SKIFF_CONFIG_TEXT_MAX + 1. Returns
 * skiff_config_set()'s errors with issue filled, SKIFF_ERR_NO_MEMORY, or SKIFF_ERR_INVALID_ARG for
 * a NULL argument or a result that is not APPROVED; out is empty on error. Save the text with
 * skiff_config_save().
 */
skiff_err skiff_romm_pairing_config_text(const char *text, size_t length,
                                         const char *device_identifier,
                                         const skiff_romm_pairing_result *result, char *out,
                                         size_t out_size, size_t *out_length,
                                         skiff_config_issue *issue);

/* Wipes the device code from pairing and the token from result. Either may be NULL. */
void skiff_romm_pairing_clear(skiff_romm_pairing *pairing, skiff_romm_pairing_result *result);

#endif
