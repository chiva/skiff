/*
 * The decision rule (skiff/saves_sync.h): a pure function of the hashes on both sides and at the
 * last sync. Each side's change is proven by its own hash against the baseline's, so a transfer
 * only ever goes from the side that changed to the side that did not.
 */
#include <string.h>

#include "skiff/saves_sync.h"

#include "saves_internal.h"

static const char *const ACTION_NAMES[] = {
    "in-sync", "upload", "download", "ask-conflict", "ask-deleted-local", "ask-deleted-server"};

static const char *const REASON_NAMES[] = {"same-content", "no-saves",       "only-local",
                                           "only-server",  "deleted-local",  "server-empty",
                                           "no-baseline",  "server-changed", "local-changed",
                                           "both-changed", "unchanged"};

_Static_assert(sizeof ACTION_NAMES / sizeof ACTION_NAMES[0] == SKIFF_SAVES_ASK_DELETED_SERVER + 1,
               "every skiff_saves_action needs a name for logs");
_Static_assert(sizeof REASON_NAMES / sizeof REASON_NAMES[0] == SKIFF_SAVES_REASON_UNCHANGED + 1,
               "every skiff_saves_reason needs a name for logs");

const char *skiff_saves_action_name(skiff_saves_action action) {
    return (int)action >= 0 && action <= SKIFF_SAVES_ASK_DELETED_SERVER ? ACTION_NAMES[action]
                                                                        : "unknown";
}

const char *skiff_saves_reason_name(skiff_saves_reason reason) {
    return (int)reason >= 0 && reason <= SKIFF_SAVES_REASON_UNCHANGED ? REASON_NAMES[reason]
                                                                      : "unknown";
}

static int is_empty(const char *hash) { return strcmp(hash, SKIFF_SAVES_EMPTY_HASH) == 0; }

static skiff_saves_decision decision(skiff_saves_action action, skiff_saves_reason reason) {
    const skiff_saves_decision d = {action, reason, 0};
    return d;
}

/* Both sides hold a save, and they differ. */
static skiff_saves_decision decide_both(const char *local, const char *server,
                                        const skiff_saves_baseline_entry *baseline) {
    const int local_changed = baseline == NULL || strcmp(local, baseline->local_hash) != 0;
    const int server_changed = baseline == NULL || strcmp(server, baseline->server_hash) != 0;
    if (is_empty(server)) {
        /* Uploading over an empty unit Skiff already synced is a local change like any other;
         * restoring one is never decided here. */
        return baseline != NULL && !server_changed && local_changed
                   ? decision(SKIFF_SAVES_UPLOAD, SKIFF_SAVES_REASON_LOCAL_CHANGED)
                   : decision(SKIFF_SAVES_ASK_DELETED_SERVER, SKIFF_SAVES_REASON_SERVER_EMPTY);
    }
    if (baseline == NULL) {
        return decision(SKIFF_SAVES_ASK_CONFLICT, SKIFF_SAVES_REASON_NO_BASELINE);
    }
    if (!local_changed && server_changed) {
        return decision(SKIFF_SAVES_DOWNLOAD, SKIFF_SAVES_REASON_SERVER_CHANGED);
    }
    if (local_changed && !server_changed) {
        return decision(SKIFF_SAVES_UPLOAD, SKIFF_SAVES_REASON_LOCAL_CHANGED);
    }
    if (local_changed) {
        return decision(SKIFF_SAVES_ASK_CONFLICT, SKIFF_SAVES_REASON_BOTH_CHANGED);
    }
    return decision(SKIFF_SAVES_IN_SYNC, SKIFF_SAVES_REASON_UNCHANGED);
}

static skiff_saves_decision decide(const char *local, const char *server,
                                   const skiff_saves_baseline_entry *baseline) {
    if (local != NULL && server != NULL && strcmp(local, server) == 0) {
        return decision(SKIFF_SAVES_IN_SYNC, SKIFF_SAVES_REASON_SAME_CONTENT);
    }
    if (local == NULL && (server == NULL || is_empty(server))) {
        return decision(SKIFF_SAVES_IN_SYNC, SKIFF_SAVES_REASON_NO_SAVES);
    }
    if (server == NULL) {
        return decision(SKIFF_SAVES_UPLOAD, SKIFF_SAVES_REASON_ONLY_LOCAL);
    }
    if (local == NULL) {
        return baseline != NULL && !is_empty(baseline->local_hash)
                   ? decision(SKIFF_SAVES_ASK_DELETED_LOCAL, SKIFF_SAVES_REASON_DELETED_LOCAL)
                   : decision(SKIFF_SAVES_DOWNLOAD, SKIFF_SAVES_REASON_ONLY_SERVER);
    }
    return decide_both(local, server, baseline);
}

static int baseline_valid(const skiff_saves_baseline_entry *baseline) {
    return baseline == NULL ||
           (memchr(baseline->local_hash, '\0', sizeof baseline->local_hash) != NULL &&
            memchr(baseline->server_hash, '\0', sizeof baseline->server_hash) != NULL &&
            saves_hash_valid(baseline->local_hash) && saves_hash_valid(baseline->server_hash));
}

skiff_err skiff_saves_decide(const skiff_saves_sync_input *in, skiff_saves_decision *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (in == NULL || out == NULL ||
        (in->local_hash != NULL && !saves_hash_valid(in->local_hash)) ||
        (in->server_hash != NULL && !saves_hash_valid(in->server_hash)) ||
        !baseline_valid(in->baseline)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *local = in->local_hash != NULL && !is_empty(in->local_hash) ? in->local_hash : NULL;
    *out = decide(local, in->server_hash, in->baseline);
    out->server_id_moved = in->baseline != NULL && in->server_hash != NULL &&
                           strcmp(in->server_hash, in->baseline->server_hash) == 0 &&
                           in->server_save_id != in->baseline->server_save_id;
    return SKIFF_OK;
}
