/*
 * Save sync on the browsing thread (app_saves.h): checks, transfers, the session's close and a
 * game's backups. Everything large lives on the heap (the thread's stack is 32 KB), files are
 * opened per step, and every line logged carries ids, sizes, hashes and timings only ("save:").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_internal.h"

#define SAVES_LOG_TAG "save"
/* A PSP ROM holding saves, as RomM lists it. */
typedef struct server_rom {
    uint64_t id;
    /* Valid and in the folder-prefix layout, or empty. */
    char save_target[SKIFF_SAVES_TARGET_SIZE];
    char title[SKIFF_ROMM_NAME_MAX];
} server_rom;

/* A check's working memory. */
typedef struct check_scratch {
    skiff_romm_rom_page page;
    skiff_romm_rom rom;
    skiff_romm_save save;
    skiff_romm_sync_plan plan;
    skiff_saves_unit unit;
    size_t rom_count;
    server_rom roms[APP_SAVES_SERVER_ROMS_MAX];
    size_t folder_count;
    char folders[APP_SAVES_FOLDERS_MAX][SKIFF_STORAGE_NAME_MAX];
    /* Installed games asked about this check for the first time. */
    unsigned char asked[SKIFF_INSTALL_RECORDS_MAX];
    size_t rom_id_count;
    uint64_t rom_ids[SKIFF_ROMM_SYNC_ROMS_MAX];
    size_t state_count;
    skiff_romm_save_state states[SKIFF_ROMM_SYNC_SAVES_MAX];
    char file_names[SKIFF_ROMM_SYNC_SAVES_MAX][SKIFF_SAVES_TARGET_SIZE + sizeof ".zip"];
    /* Whether the baseline changed and must be saved. */
    int dirty;
} check_scratch;

static int64_t elapsed_ms(const app_call *call, int64_t since) {
    return call->now_ms(call->clock_ctx) - since;
}

/* ---- The baseline ---- */

static skiff_err save_baseline(app_call *call) {
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_err err =
        skiff_storage_resolve(&call->roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path);
    if (err == SKIFF_OK) {
        err = skiff_saves_baseline_save(call->saves.baseline, call->storage, path);
    }
    call->saves.baseline_saved = err == SKIFF_OK;
    skiff_log_write(call->saves.log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN,
                    SAVES_LOG_TAG, "baseline saved, %zu game(s), %zu ROM(s): %s (%d)",
                    call->saves.baseline->count, call->saves.baseline->rom_count,
                    skiff_err_name(err), (int)err);
    return err;
}

/* Records the game as synced: S and L now, its folders' summary, the call's clock. */
static skiff_err record(app_call *call, app_save_game *game) {
    skiff_saves_baseline_entry entry;
    memset(&entry, 0, sizeof entry);
    snprintf(entry.save_target, sizeof entry.save_target, "%s", game->save_target);
    entry.rom_id = game->rom_id;
    entry.server_save_id = game->server_save_id;
    snprintf(entry.server_hash, sizeof entry.server_hash, "%s", game->server_hash);
    snprintf(entry.local_hash, sizeof entry.local_hash, "%s",
             game->has_local ? game->local_hash : SKIFF_SAVES_EMPTY_HASH);
    entry.synced_ms = call->saves.now_utc_ms;
    entry.tree = game->tree;
    const skiff_err err = skiff_saves_baseline_record(call->saves.baseline, &entry);
    if (err == SKIFF_OK) {
        game->synced_ms = entry.synced_ms;
    }
    return err;
}

/* Remembers a ROM's save ID; a value the baseline refuses is not one to keep. */
static void remember(app_call *call, check_scratch *s, uint64_t rom_id, const char *save_target) {
    int changed = 0;
    (void)skiff_saves_baseline_remember_rom(call->saves.baseline, rom_id, save_target, &changed);
    s->dirty |= changed;
}

/* The save ID Skiff syncs for a ROM: valid, in the folder-prefix layout; empty otherwise. */
static void syncable_target(const skiff_romm_rom_summary *rom, char *out, size_t size) {
    const int usable = rom->save_layout == SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX &&
                       skiff_saves_target_valid(rom->save_target);
    /* Valid, so at most SKIFF_SAVES_TARGET_MAX characters. */
    snprintf(out, size, "%.*s", SKIFF_SAVES_TARGET_MAX, usable ? rom->save_target : "");
}

/* ---- This PSP's saves ---- */

/* L for the game, from its folders' summary when it matches the baseline's and hashing is not
 * forced, otherwise from its files. An unsyncable folder is the game's problem, not the check's. */
static skiff_err read_local(app_call *call, skiff_saves_unit *unit, int force,
                            app_save_game *game) {
    const int64_t started = call->now_ms(call->clock_ctx);
    game->has_local = 0;
    game->local_hash[0] = '\0';
    game->local_mtime = SKIFF_STORAGE_MTIME_UNKNOWN;
    game->local_bytes = 0;
    skiff_err err = skiff_saves_tree_summarize(
        call->storage, &call->roots, SKIFF_STORAGE_ROOT_SAVES, game->save_target, &game->tree);
    if (err != SKIFF_OK) {
        return err;
    }
    const skiff_saves_baseline_entry *entry =
        skiff_saves_baseline_find(call->saves.baseline, game->save_target);
    if (!force && entry != NULL && !skiff_saves_tree_changed(&entry->tree, &game->tree)) {
        game->has_local = strcmp(entry->local_hash, SKIFF_SAVES_EMPTY_HASH) != 0;
        snprintf(game->local_hash, sizeof game->local_hash, "%s", entry->local_hash);
        skiff_log_write(call->saves.log, SKIFF_LOG_DEBUG, SAVES_LOG_TAG,
                        "%s: folders unchanged, %u file(s), hash %s kept", game->save_target,
                        (unsigned)game->tree.file_count, game->local_hash);
        return SKIFF_OK;
    }
    err = skiff_saves_collect(call->storage, &call->roots, SKIFF_STORAGE_ROOT_SAVES,
                              game->save_target, unit);
    if (err == SKIFF_OK) {
        err = skiff_saves_hash_unit(call->storage, &call->roots, unit);
    }
    if (err == SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE) {
        skiff_log_write(call->saves.log, SKIFF_LOG_WARN, SAVES_LOG_TAG, "%s: unsyncable, %s",
                        game->save_target, skiff_saves_problem_name(unit->problem));
        game->problem = SAVE_PROBLEM_ERROR;
        game->error = err;
        return SKIFF_OK;
    }
    if (err != SKIFF_OK) {
        return err;
    }
    for (size_t i = 0; i < unit->file_count; i++) {
        if (unit->files[i].mtime > game->local_mtime) {
            game->local_mtime = unit->files[i].mtime;
        }
    }
    game->has_local = unit->file_count > 0;
    game->local_bytes = unit->total_bytes;
    snprintf(game->local_hash, sizeof game->local_hash, "%s", unit->content_hash);
    skiff_log_write(call->saves.log, SKIFF_LOG_DEBUG, SAVES_LOG_TAG,
                    "%s: %zu file(s), %llu bytes, hash %s in %lld ms", game->save_target,
                    unit->file_count, (unsigned long long)unit->total_bytes, unit->content_hash,
                    (long long)elapsed_ms(call, started));
    return SKIFF_OK;
}

/* ---- RomM's side ---- */

/* The slot's operation for rom_id in the plan, or NULL. */
static const skiff_romm_sync_operation *slot_operation(const skiff_romm_sync_plan *plan,
                                                       uint64_t rom_id) {
    for (size_t i = 0; i < plan->count; i++) {
        const skiff_romm_sync_operation *op = &plan->operations[i];
        if (op->rom_id == rom_id && op->has_slot && strcmp(op->slot, APP_SAVES_SLOT) == 0) {
            return op;
        }
    }
    return NULL;
}

/* S from the plan: RomM's head of the slot, compared by its hash and id, never its time. */
static void read_server(const skiff_romm_sync_plan *plan, app_save_game *game) {
    const skiff_romm_sync_operation *op = slot_operation(plan, game->rom_id);
    game->has_server = op != NULL && op->has_save_id && op->server_content_hash[0] != '\0';
    game->server_save_id = game->has_server ? op->save_id : 0;
    snprintf(game->server_hash, sizeof game->server_hash, "%s",
             game->has_server ? op->server_content_hash : "");
    snprintf(game->server_updated_at, sizeof game->server_updated_at, "%s",
             game->has_server ? op->server_updated_at : "");
    game->server_device[0] = '\0';
}

static void add_save_state(check_scratch *s, const app_save_game *game, int64_t now_utc_ms) {
    if (!game->has_local || s->state_count == SKIFF_ROMM_SYNC_SAVES_MAX) {
        return;
    }
    char *name = s->file_names[s->state_count];
    snprintf(name, sizeof s->file_names[0], "%s" SKIFF_SAVES_UNIT_EXTENSION, game->save_target);
    skiff_romm_save_state *state = &s->states[s->state_count++];
    state->rom_id = game->rom_id;
    state->file_name = name;
    state->slot = APP_SAVES_SLOT;
    state->emulator = APP_SAVES_EMULATOR;
    state->content_hash = game->local_hash;
    /* Only RomM's advice uses the time, and Skiff never follows it. */
    state->updated_at_ms = game->local_mtime > 0 ? game->local_mtime * APP_MS_PER_S : now_utc_ms;
    state->file_size_bytes = game->local_bytes;
}

static void add_rom_id(check_scratch *s, uint64_t rom_id) {
    for (size_t i = 0; i < s->rom_id_count; i++) {
        if (s->rom_ids[i] == rom_id) {
            return;
        }
    }
    if (s->rom_id_count < SKIFF_ROMM_SYNC_ROMS_MAX) {
        s->rom_ids[s->rom_id_count++] = rom_id;
    }
}

static skiff_err negotiate(app_call *call, check_scratch *s) {
    const int64_t started = call->now_ms(call->clock_ctx);
    const skiff_romm_sync_query query = {
        call->saves.device_id, s->rom_ids, s->rom_id_count, s->states, s->state_count, 1};
    const skiff_err err = skiff_romm_saves_negotiate(call->romm, &query, &s->plan);
    skiff_log_write(
        call->saves.log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN, SAVES_LOG_TAG,
        "negotiate: %zu ROM(s), %zu save(s): session %llu, %zu operation(s) in %lld "
        "ms: %s (%d)",
        s->rom_id_count, s->state_count, (unsigned long long)s->plan.session_id, s->plan.count,
        (long long)elapsed_ms(call, started), skiff_err_name(err), (int)err);
    return err;
}

/* Decides the game from L, S and its baseline; an identical pair is recorded, and RomM is told
 * this PSP holds that save so its upload guard agrees (a first sync, or after pairing again). */
static void decide(app_call *call, check_scratch *s, app_save_game *game) {
    const skiff_saves_baseline_entry *entry =
        skiff_saves_baseline_find(call->saves.baseline, game->save_target);
    /* The baseline of another ROM with this save ID (the ISO's, for the CSO) says nothing about
     * this one's saves in RomM: a first sync with it. */
    if (entry != NULL && entry->rom_id != game->rom_id) {
        entry = NULL;
    }
    game->synced_ms = entry != NULL ? entry->synced_ms : 0;
    const skiff_saves_sync_input in = {game->has_local ? game->local_hash : NULL,
                                       game->has_server ? game->server_hash : NULL,
                                       game->server_save_id, entry};
    if (skiff_saves_decide(&in, &game->decision) != SKIFF_OK) {
        game->problem = SAVE_PROBLEM_ERROR;
        game->error = SKIFF_ERR_INVALID_ARG;
        return;
    }
    game->decided = 1;
    skiff_log_write(call->saves.log, SKIFF_LOG_DEBUG, SAVES_LOG_TAG,
                    "%s: ROM %llu, local %s, server %llu %s: %s (%s)", game->save_target,
                    (unsigned long long)game->rom_id, game->has_local ? game->local_hash : "-",
                    (unsigned long long)game->server_save_id,
                    game->has_server ? game->server_hash : "-",
                    skiff_saves_action_name(game->decision.action),
                    skiff_saves_reason_name(game->decision.reason));
    const int in_sync = game->decision.action == SKIFF_SAVES_IN_SYNC && game->has_server;
    const int same = game->decision.reason == SKIFF_SAVES_REASON_SAME_CONTENT;
    if (!in_sync || (!same && !game->decision.server_id_moved)) {
        return;
    }
    const char *local = game->has_local ? game->local_hash : SKIFF_SAVES_EMPTY_HASH;
    if (entry != NULL && entry->server_save_id == game->server_save_id &&
        strcmp(entry->server_hash, game->server_hash) == 0 &&
        strcmp(entry->local_hash, local) == 0) {
        return;
    }
    const skiff_err told = skiff_romm_saves_downloaded(call->romm, game->server_save_id,
                                                       call->saves.device_id, game->server_hash);
    skiff_log_write(call->saves.log, told == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN,
                    SAVES_LOG_TAG, "%s: in step with RomM's save %llu, told RomM: %s (%d)",
                    game->save_target, (unsigned long long)game->server_save_id,
                    skiff_err_name(told), (int)told);
    if (told == SKIFF_OK) {
        s->dirty |= record(call, game) == SKIFF_OK;
    }
}

/* The device that uploaded RomM's side of a question, to show with it. */
static void read_device(app_call *call, check_scratch *s, app_save_game *game) {
    const skiff_err err =
        skiff_romm_saves_get(call->romm, game->server_save_id, call->saves.device_id, &s->save);
    if (err == SKIFF_OK) {
        snprintf(game->server_device, sizeof game->server_device, "%s",
                 s->save.origin_device_name[0] != '\0' ? s->save.origin_device_name
                                                       : s->save.emulator);
    }
    skiff_log_write(call->saves.log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_INFO,
                    SAVES_LOG_TAG, "%s: save %llu's device: %s (%d)", game->save_target,
                    (unsigned long long)game->server_save_id, skiff_err_name(err), (int)err);
}

static int asks(const app_save_game *game) {
    return game->decided && (game->decision.action == SKIFF_SAVES_ASK_CONFLICT ||
                             game->decision.action == SKIFF_SAVES_ASK_DELETED_SERVER);
}

/* ---- One game ---- */

static skiff_err check_one(app_call *call, check_scratch *s) {
    app_save_game *game = &call->saves.game;
    game->problem = SAVE_PROBLEM_NONE;
    game->error = SKIFF_OK;
    game->decided = 0;
    game->recheck = 0;
    remember(call, s, game->rom_id, game->save_target);
    skiff_err err = read_local(call, &s->unit, 1, game);
    if (err == SKIFF_OK && game->problem == SAVE_PROBLEM_NONE) {
        add_rom_id(s, game->rom_id);
        add_save_state(s, game, call->saves.now_utc_ms);
        err = negotiate(call, s);
    }
    if (err == SKIFF_OK && game->problem == SAVE_PROBLEM_NONE) {
        call->saves.session_id = s->plan.session_id;
        read_server(&s->plan, game);
        decide(call, s, game);
        if (asks(game)) {
            read_device(call, s, game);
        }
    }
    return err;
}

/* ---- Every game ---- */

static const server_rom *server_rom_of(const check_scratch *s, uint64_t rom_id) {
    for (size_t i = 0; i < s->rom_count; i++) {
        if (s->roms[i].id == rom_id) {
            return &s->roms[i];
        }
    }
    return NULL;
}

/* The PSP ROMs RomM holds saves for, a page at a time. */
static skiff_err read_server_roms(app_call *call, check_scratch *s) {
    if (!call->saves.has_platform) {
        return SKIFF_OK;
    }
    const skiff_romm_list_query query = {call->saves.platform_id, SKIFF_ROMM_LIST_WITH_SAVES, 0, 0};
    uint64_t offset = 0;
    for (;;) {
        const skiff_err err =
            skiff_romm_list_roms(call->romm, &query, offset, SKIFF_ROMM_PAGE_SIZE, &s->page);
        if (err != SKIFF_OK) {
            return err;
        }
        for (size_t i = 0; i < s->page.count && s->rom_count < APP_SAVES_SERVER_ROMS_MAX; i++) {
            const skiff_romm_rom_summary *item = &s->page.items[i];
            /* A save uploaded meanwhile shifts the pages: a ROM read twice is still one game. */
            if (server_rom_of(s, item->id) != NULL) {
                continue;
            }
            server_rom *rom = &s->roms[s->rom_count++];
            rom->id = item->id;
            syncable_target(item, rom->save_target, sizeof rom->save_target);
            snprintf(rom->title, sizeof rom->title, "%s", app_rom_name(item));
            remember(call, s, rom->id, rom->save_target);
        }
        offset += s->page.count;
        if (s->page.count == 0 || offset >= s->page.total ||
            s->rom_count == APP_SAVES_SERVER_ROMS_MAX) {
            break;
        }
    }
    skiff_log_write(call->saves.log, SKIFF_LOG_DEBUG, SAVES_LOG_TAG, "%zu PSP ROM(s) with saves",
                    s->rom_count);
    return SKIFF_OK;
}

/* The save IDs of installed games not remembered yet, from their details, a few per check. A ROM
 * RomM no longer has is remembered without one. */
/* Asks RomM for one installed game's save ID and remembers the answer. A ROM RomM no longer has,
 * or whose details Skiff refuses (RomM's data, not the connection), is remembered without one: it
 * then costs only that game, and Sync all asks about it again. */
static skiff_err look_up(app_call *call, check_scratch *s, uint64_t id) {
    const skiff_err err = skiff_romm_get_rom(call->romm, id, &s->rom);
    if (err == SKIFF_ERR_ROMM_NOT_FOUND || err == SKIFF_ERR_ROMM_BAD_RESPONSE) {
        skiff_log_write(call->saves.log, SKIFF_LOG_WARN, SAVES_LOG_TAG,
                        "ROM %llu: no save ID from its details: %s (%d)", (unsigned long long)id,
                        skiff_err_name(err), (int)err);
        remember(call, s, id, "");
        return SKIFF_OK;
    }
    if (err != SKIFF_OK) {
        return err;
    }
    char target[SKIFF_SAVES_TARGET_SIZE];
    syncable_target(&s->rom.summary, target, sizeof target);
    remember(call, s, id, target);
    return SKIFF_OK;
}

/* The save IDs of installed games, a few per check: first the games never asked about, then on
 * Sync all those remembered without one (RomM may have rescanned them), taken in turn from where
 * the last check stopped so every one is reached however many there are. */
static skiff_err read_installed(app_call *call, check_scratch *s) {
    const size_t count = call->saves.installed_count;
    size_t asked = 0;
    skiff_err err = SKIFF_OK;
    for (size_t i = 0; err == SKIFF_OK && i < count && asked < APP_SAVES_LOOKUPS_MAX; i++) {
        const uint64_t id = call->saves.installed[i];
        s->asked[i] = skiff_saves_baseline_rom_target(call->saves.baseline, id) == NULL;
        if (s->asked[i]) {
            asked++;
            err = look_up(call, s, id);
        }
    }
    size_t step = 0;
    for (;
         err == SKIFF_OK && call->saves.force_hash && step < count && asked < APP_SAVES_LOOKUPS_MAX;
         step++) {
        const size_t i = (call->saves.lookup_cursor + step) % count;
        const char *known =
            skiff_saves_baseline_rom_target(call->saves.baseline, call->saves.installed[i]);
        if (!s->asked[i] && known != NULL && known[0] == '\0') {
            asked++;
            err = look_up(call, s, call->saves.installed[i]);
        }
    }
    call->saves.lookup_cursor = count > 0 ? (call->saves.lookup_cursor + step) % count : 0;
    skiff_log_write(call->saves.log, SKIFF_LOG_DEBUG, SAVES_LOG_TAG,
                    "%zu installed game(s), %zu asked about: %s (%d)", count, asked,
                    skiff_err_name(err), (int)err);
    return err;
}

static skiff_storage_list_step take_folder(void *ctx, const skiff_storage_entry *entry) {
    check_scratch *s = ctx;
    if (entry->info.is_dir && s->folder_count < APP_SAVES_FOLDERS_MAX) {
        snprintf(s->folders[s->folder_count++], sizeof s->folders[0], "%s", entry->name);
    }
    return SKIFF_STORAGE_LIST_CONTINUE;
}

static skiff_err read_folders(app_call *call, check_scratch *s) {
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_err err =
        skiff_storage_resolve(&call->roots, SKIFF_STORAGE_ROOT_SAVES, path, sizeof path);
    size_t skipped = 0;
    if (err == SKIFF_OK) {
        err = skiff_storage_list(call->storage, path, take_folder, s, &skipped);
    }
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        err = SKIFF_OK;
    }
    if (err != SKIFF_OK) {
        s->folder_count = 0;
    }
    return err;
}

/* ASCII case folding, whatever the C library's locale (and whether char is signed). */
static char fold(char c) {
    if (c < 'a' || c > 'z') {
        return c;
    }
    return (char)(c - 'a' + 'A');
}

static int starts_with(const char *name, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        if (fold(name[i]) != prefix[i]) {
            return 0;
        }
    }
    return 1;
}

static int has_folder(const check_scratch *s, const char *save_target) {
    for (size_t i = 0; i < s->folder_count; i++) {
        if (starts_with(s->folders[i], save_target)) {
            return 1;
        }
    }
    return 0;
}

static app_save_game *find_game(app_save_list *list, const char *save_target, uint64_t rom_id) {
    for (size_t i = 0; i < list->count; i++) {
        app_save_game *game = &list->games[i];
        if (save_target[0] != '\0' ? strcmp(game->save_target, save_target) == 0
                                   : game->save_target[0] == '\0' && game->rom_id == rom_id) {
            return game;
        }
    }
    return NULL;
}

static app_save_game *add_game(app_call *call, const char *save_target, uint64_t rom_id) {
    app_save_list *list = call->saves.list;
    app_save_game *game = find_game(list, save_target, rom_id);
    if (game != NULL) {
        return game;
    }
    if (list->count == APP_SAVES_GAMES_MAX) {
        skiff_log_write(call->saves.log, SKIFF_LOG_WARN, SAVES_LOG_TAG,
                        "more than %d games: %s left out", APP_SAVES_GAMES_MAX, save_target);
        return NULL;
    }
    game = &list->games[list->count++];
    memset(game, 0, sizeof *game);
    snprintf(game->save_target, sizeof game->save_target, "%s", save_target);
    game->rom_id = rom_id;
    return game;
}

/* The ROM a save ID's saves sync with: the one the baseline recorded, else the only installed
 * game with it, else the only PSP ROM with saves that has it; 0 with *several set when there is
 * more than one to choose from. */
static uint64_t choose_rom(app_call *call, const check_scratch *s, const char *save_target,
                           int *several) {
    *several = 0;
    const skiff_saves_baseline_entry *entry =
        skiff_saves_baseline_find(call->saves.baseline, save_target);
    if (entry != NULL) {
        return entry->rom_id;
    }
    uint64_t chosen = 0;
    size_t found = 0;
    for (size_t i = 0; i < call->saves.installed_count; i++) {
        const char *target =
            skiff_saves_baseline_rom_target(call->saves.baseline, call->saves.installed[i]);
        if (target != NULL && strcmp(target, save_target) == 0 &&
            call->saves.installed[i] != chosen) {
            chosen = call->saves.installed[i];
            found++;
        }
    }
    if (found == 0) {
        for (size_t i = 0; i < s->rom_count; i++) {
            if (strcmp(s->roms[i].save_target, save_target) == 0) {
                chosen = s->roms[i].id;
                found++;
            }
        }
    }
    *several = found > 1;
    return found == 1 ? chosen : 0;
}

/* A game for a save ID some ROM has, when there is anything to sync: local folders, saves in
 * RomM, or a baseline. */
static void add_target(app_call *call, const check_scratch *s, const char *save_target) {
    if (find_game(call->saves.list, save_target, 0) != NULL) {
        return;
    }
    int several = 0;
    const uint64_t rom_id = choose_rom(call, s, save_target, &several);
    const int local = has_folder(s, save_target);
    int on_server = 0;
    for (size_t i = 0; i < s->rom_count; i++) {
        on_server |= strcmp(s->roms[i].save_target, save_target) == 0;
    }
    if (!local && !on_server &&
        skiff_saves_baseline_find(call->saves.baseline, save_target) == NULL) {
        return;
    }
    app_save_game *game = add_game(call, save_target, rom_id);
    if (game == NULL) {
        return;
    }
    const server_rom *rom = server_rom_of(s, rom_id);
    if (rom != NULL) {
        snprintf(game->title, sizeof game->title, "%s", rom->title);
    }
    if (several) {
        game->problem = SAVE_PROBLEM_SEVERAL_GAMES;
    }
}

/* A save folder's game ID, "ULUS10064" from "ULUS10064DATA00"; 0 when it does not start with one.
 */
static int game_id_of(const char *folder, char *out, size_t size) {
    if (size <= APP_SAVES_GAME_ID_LENGTH || strlen(folder) < APP_SAVES_GAME_ID_LENGTH) {
        return 0;
    }
    for (size_t i = 0; i < APP_SAVES_GAME_ID_LENGTH; i++) {
        const char c = fold(folder[i]);
        const int letter = c >= 'A' && c <= 'Z';
        const int digit = c >= '0' && c <= '9';
        if (i < APP_SAVES_GAME_ID_LETTERS ? !letter : !digit) {
            return 0;
        }
        out[i] = c;
    }
    out[APP_SAVES_GAME_ID_LENGTH] = '\0';
    return 1;
}

/* Save folders no known game claims, one row per game ID. */
static void add_unclaimed(app_call *call, const check_scratch *s) {
    for (size_t i = 0; i < s->folder_count; i++) {
        int claimed = 0;
        for (size_t g = 0; g < call->saves.list->count && !claimed; g++) {
            const app_save_game *game = &call->saves.list->games[g];
            claimed = game->save_target[0] != '\0' && starts_with(s->folders[i], game->save_target);
        }
        char id[SKIFF_SAVES_TARGET_SIZE];
        if (claimed || !game_id_of(s->folders[i], id, sizeof id) ||
            find_game(call->saves.list, id, 0) != NULL) {
            continue;
        }
        app_save_game *game = add_game(call, id, 0);
        if (game != NULL) {
            game->problem = SAVE_PROBLEM_UNKNOWN_GAME;
        }
    }
}

/* Every distinct save ID worth a row: the baseline's, the ROMs' with saves, the installed
 * games'; then ROMs with saves but no save ID; then folders nobody claims. */
static void list_games(app_call *call, const check_scratch *s) {
    const skiff_saves_baseline *baseline = call->saves.baseline;
    for (size_t i = 0; i < baseline->count; i++) {
        add_target(call, s, baseline->entries[i].save_target);
    }
    for (size_t i = 0; i < s->rom_count; i++) {
        if (s->roms[i].save_target[0] != '\0') {
            add_target(call, s, s->roms[i].save_target);
        }
    }
    for (size_t i = 0; i < call->saves.installed_count; i++) {
        const char *target = skiff_saves_baseline_rom_target(baseline, call->saves.installed[i]);
        if (target != NULL && target[0] != '\0') {
            add_target(call, s, target);
        }
    }
    for (size_t i = 0; i < s->rom_count; i++) {
        if (s->roms[i].save_target[0] == '\0') {
            app_save_game *game = add_game(call, "", s->roms[i].id);
            if (game != NULL) {
                game->problem = SAVE_PROBLEM_NO_ID;
                snprintf(game->title, sizeof game->title, "%s", s->roms[i].title);
            }
        }
    }
    add_unclaimed(call, s);
}

/* Of several ROMs with a save ID, the only one whose slot holds a save in RomM. */
static void resolve_several(const check_scratch *s, app_save_game *game) {
    uint64_t chosen = 0;
    size_t found = 0;
    for (size_t i = 0; i < s->rom_count; i++) {
        const skiff_romm_sync_operation *op = slot_operation(&s->plan, s->roms[i].id);
        if (strcmp(s->roms[i].save_target, game->save_target) == 0 && op != NULL &&
            op->has_save_id) {
            chosen = s->roms[i].id;
            found++;
        }
    }
    if (found == 1) {
        game->rom_id = chosen;
        game->problem = SAVE_PROBLEM_NONE;
        const server_rom *rom = server_rom_of(s, chosen);
        snprintf(game->title, sizeof game->title, "%s", rom != NULL ? rom->title : "");
    }
}

/* The name a row shows, for its order: RomM's title, else the save ID. */
static const char *shown_name(const app_save_game *game) {
    return game->title[0] != '\0' ? game->title : game->save_target;
}

/* The games that need the player first, so the Saves screen opens on them; then by name, ignoring
 * ASCII case, as a player looks a game up. */
static int compare_games(const void *a, const void *b) {
    const int a_waits = app_saves_needs_player(a);
    const int b_waits = app_saves_needs_player(b);
    if (a_waits != b_waits) {
        return b_waits - a_waits;
    }
    const char *x = shown_name(a);
    const char *y = shown_name(b);
    for (; *x != '\0' && fold(*x) == fold(*y); x++, y++) {
    }
    return (int)(unsigned char)fold(*x) - (int)(unsigned char)fold(*y);
}

/* Drops games with nothing to show: neither side holds a save and none ever synced. */
static void drop_empty(app_save_list *list) {
    size_t kept = 0;
    for (size_t i = 0; i < list->count; i++) {
        const app_save_game *game = &list->games[i];
        const int empty = game->problem == SAVE_PROBLEM_NONE && game->decided &&
                          game->decision.reason == SKIFF_SAVES_REASON_NO_SAVES &&
                          game->synced_ms == 0;
        if (!empty) {
            list->games[kept++] = *game;
        }
    }
    list->count = kept;
}

/* The ROMs a game asks negotiate about: its own, or every candidate of a save ID several share. */
static size_t game_roms(const check_scratch *s, const app_save_game *game) {
    if (game->problem == SAVE_PROBLEM_NONE) {
        return 1;
    }
    size_t count = 0;
    for (size_t i = 0; game->problem == SAVE_PROBLEM_SEVERAL_GAMES && i < s->rom_count; i++) {
        count += strcmp(s->roms[i].save_target, game->save_target) == 0;
    }
    return count;
}

static void add_game_roms(const app_call *call, check_scratch *s, const app_save_game *game) {
    if (game->problem == SAVE_PROBLEM_NONE) {
        add_rom_id(s, game->rom_id);
        add_save_state(s, game, call->saves.now_utc_ms);
        return;
    }
    for (size_t i = 0; game->problem == SAVE_PROBLEM_SEVERAL_GAMES && i < s->rom_count; i++) {
        if (strcmp(s->roms[i].save_target, game->save_target) == 0) {
            add_rom_id(s, s->roms[i].id);
        }
    }
}

/* Negotiates the games from first on, as many as fit APP_SAVES_NEGOTIATE_ROMS ROMs (negotiate
 * answers for every slot of each ROM, so fewer ROMs keep its plan within the operations Skiff
 * reads), and decides them. *end is the first game left for the next round. A round before the last
 * has its session closed at once; the last one's stays for the transfers. */
static skiff_err check_round(app_call *call, check_scratch *s, size_t first, size_t *end,
                             size_t *devices) {
    app_save_list *list = call->saves.list;
    s->rom_id_count = 0;
    s->state_count = 0;
    s->plan.count = 0;
    size_t next = first;
    for (; next < list->count; next++) {
        const size_t need = game_roms(s, &list->games[next]);
        if (s->rom_id_count > 0 && s->rom_id_count + need > APP_SAVES_NEGOTIATE_ROMS) {
            break;
        }
        add_game_roms(call, s, &list->games[next]);
    }
    *end = next;
    skiff_err err = SKIFF_OK;
    if (s->rom_id_count > 0) {
        if (list->session_id != 0) {
            (void)skiff_romm_saves_complete(call->romm, list->session_id, 0, 0);
        }
        err = negotiate(call, s);
        list->session_id = err == SKIFF_OK ? s->plan.session_id : 0;
    }
    for (size_t i = first; err == SKIFF_OK && i < next; i++) {
        app_save_game *game = &list->games[i];
        if (game->problem == SAVE_PROBLEM_SEVERAL_GAMES) {
            resolve_several(s, game);
        }
        if (game->problem != SAVE_PROBLEM_NONE) {
            continue;
        }
        read_server(&s->plan, game);
        decide(call, s, game);
        if (asks(game) && *devices < APP_SAVES_DEVICE_LOOKUPS_MAX) {
            (*devices)++;
            read_device(call, s, game);
        }
    }
    return err;
}

static skiff_err check_all(app_call *call, check_scratch *s) {
    app_save_list *list = call->saves.list;
    memset(list, 0, sizeof *list);
    skiff_err err = read_server_roms(call, s);
    if (err == SKIFF_OK) {
        err = read_installed(call, s);
    }
    if (err == SKIFF_OK) {
        err = read_folders(call, s);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    list_games(call, s);
    for (size_t i = 0; err == SKIFF_OK && i < list->count; i++) {
        app_save_game *game = &list->games[i];
        if (game->save_target[0] == '\0' || game->problem == SAVE_PROBLEM_UNKNOWN_GAME) {
            continue;
        }
        err = read_local(call, &s->unit, call->saves.force_hash, game);
    }
    size_t devices = 0;
    for (size_t first = 0; err == SKIFF_OK && first < list->count;) {
        size_t next = first;
        err = check_round(call, s, first, &next, &devices);
        first = next;
    }
    if (err == SKIFF_OK) {
        drop_empty(list);
        qsort(list->games, list->count, sizeof list->games[0], compare_games);
    }
    return err;
}

skiff_err app_saves_check_run(app_call *call) {
    const int64_t started = call->now_ms(call->clock_ctx);
    check_scratch *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const int one = call->saves.op == SAVES_OP_CHECK_ONE;
    skiff_err err = one ? check_one(call, s) : check_all(call, s);
    call->saves.baseline_error = SKIFF_OK;
    if (s->dirty || call->saves.baseline_pending) {
        call->saves.baseline_error = save_baseline(call);
    }
    skiff_log_write(call->saves.log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN,
                    SAVES_LOG_TAG, "check%s: %zu game(s), %zu folder(s) in %lld ms: %s (%d)",
                    one ? " of one game" : "", one ? (size_t)1 : call->saves.list->count,
                    s->folder_count, (long long)elapsed_ms(call, started), skiff_err_name(err),
                    (int)err);
    free(s);
    return err;
}

/* ---- Transfers ---- */

/* A restore that failed may have stopped while replacing live files: its journal is replayed now,
 * so no check reads a half-restored save as this PSP's. */
static void settle_restore(app_call *call) {
    skiff_saves_recovery recovery;
    call->saves.recovery = skiff_saves_recover(call->storage, &call->roots, &recovery);
    skiff_log_write(call->saves.log,
                    call->saves.recovery == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN,
                    SAVES_LOG_TAG, "restore settled: outcome %d: %s (%d)", (int)recovery.outcome,
                    skiff_err_name(call->saves.recovery), (int)call->saves.recovery);
}

/* A transfer's working memory. */
typedef struct apply_scratch {
    skiff_saves_unit unit;
    skiff_romm_save save;
    skiff_saves_restore_report report;
    char unit_path[SKIFF_STORAGE_PATH_MAX];
    char real_path[SKIFF_STORAGE_PATH_MAX];
} apply_scratch;

/* The game's saves now, hashed: whatever the check saw must still hold, or nothing is sent or
 * restored and the game is checked again. */
static skiff_err confirm_local(app_call *call, apply_scratch *a, app_save_game *game) {
    app_save_game now = *game;
    const skiff_err err = read_local(call, &a->unit, 1, &now);
    if (err != SKIFF_OK) {
        return err;
    }
    if (now.problem != SAVE_PROBLEM_NONE) {
        game->problem = now.problem;
        game->error = now.error;
        return now.error;
    }
    const int same = now.has_local == game->has_local &&
                     (!now.has_local || strcmp(now.local_hash, game->local_hash) == 0);
    call->saves.stale = !same;
    if (same) {
        game->tree = now.tree;
        game->local_mtime = now.local_mtime;
        game->local_bytes = now.local_bytes;
    }
    return SKIFF_OK;
}

static skiff_err work_paths(app_call *call, apply_scratch *a, const char *save_target) {
    char folder[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_saves_work_unit_path(save_target, a->unit_path, sizeof a->unit_path);
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(&call->roots, a->unit_path, a->real_path, sizeof a->real_path);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(&call->roots, SKIFF_SAVES_WORK_FOLDER, folder, sizeof folder);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_mkdirs(call->storage, folder);
    }
    return err;
}

static skiff_err upload(app_call *call, apply_scratch *a, int overwrite) {
    app_save_game *game = &call->saves.game;
    const int64_t started = call->now_ms(call->clock_ctx);
    skiff_err err = work_paths(call, a, game->save_target);
    if (err == SKIFF_OK) {
        err = skiff_saves_tree_summarize(call->storage, &call->roots, SKIFF_STORAGE_ROOT_SAVES,
                                         game->save_target, &game->tree);
    }
    if (err == SKIFF_OK) {
        err = skiff_saves_collect(call->storage, &call->roots, SKIFF_STORAGE_ROOT_SAVES,
                                  game->save_target, &a->unit);
    }
    if (err == SKIFF_OK) {
        err = skiff_saves_write_unit(call->storage, &call->roots, &a->unit, a->unit_path);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    if (a->unit.file_count == 0 || !game->has_local ||
        strcmp(a->unit.content_hash, game->local_hash) != 0) {
        call->saves.stale = 1;
        (void)skiff_storage_remove(call->storage, a->real_path);
        return SKIFF_OK;
    }
    char file_name[SKIFF_SAVES_TARGET_SIZE + sizeof SKIFF_SAVES_UNIT_EXTENSION];
    snprintf(file_name, sizeof file_name, "%s" SKIFF_SAVES_UNIT_EXTENSION, game->save_target);
    const skiff_romm_upload request = {
        .rom_id = game->rom_id,
        .device_id = call->saves.device_id,
        .emulator = APP_SAVES_EMULATOR,
        .slot = APP_SAVES_SLOT,
        .content_hash = game->local_hash,
        .overwrite = overwrite,
        .keep_versions = APP_SAVES_KEEP_VERSIONS,
        .storage = call->storage,
        .path = a->real_path,
        .file_name = file_name,
    };
    err = skiff_romm_saves_upload(call->romm, &request, &a->save);
    (void)skiff_storage_remove(call->storage, a->real_path);
    skiff_log_write(
        call->saves.log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, SAVES_LOG_TAG,
        "%s: upload to ROM %llu%s, %llu bytes, hash %s: save %llu in %lld ms: %s (%d)",
        game->save_target, (unsigned long long)game->rom_id, overwrite ? " over RomM's" : "",
        (unsigned long long)a->unit.total_bytes, game->local_hash, (unsigned long long)a->save.id,
        (long long)elapsed_ms(call, started), skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        return err;
    }
    game->has_server = 1;
    game->server_save_id = a->save.id;
    snprintf(game->server_hash, sizeof game->server_hash, "%s",
             a->save.content_hash[0] != '\0' ? a->save.content_hash : game->local_hash);
    snprintf(game->server_updated_at, sizeof game->server_updated_at, "%s", a->save.updated_at);
    return record(call, game);
}

static skiff_err download(app_call *call, apply_scratch *a) {
    app_save_game *game = &call->saves.game;
    const int64_t started = call->now_ms(call->clock_ctx);
    skiff_err err = confirm_local(call, a, game);
    if (err != SKIFF_OK || call->saves.stale) {
        return err;
    }
    err = work_paths(call, a, game->save_target);
    uint64_t size = 0;
    if (err == SKIFF_OK) {
        const skiff_romm_download request = {game->server_save_id, call->saves.device_id,
                                             call->storage, a->real_path,
                                             SKIFF_SAVES_UNIT_FILE_BYTES_MAX};
        err = skiff_romm_saves_download(call->romm, &request, &size);
    }
    const int64_t fetched = call->now_ms(call->clock_ctx);
    if (err == SKIFF_OK) {
        err = skiff_saves_read_unit(call->storage, &call->roots, a->unit_path, game->save_target,
                                    NULL, &a->unit);
    }
    if (err == SKIFF_OK && strcmp(a->unit.content_hash, game->server_hash) != 0) {
        skiff_log_write(call->saves.log, SKIFF_LOG_WARN, SAVES_LOG_TAG,
                        "%s: save %llu hashes to %s, not RomM's %s", game->save_target,
                        (unsigned long long)game->server_save_id, a->unit.content_hash,
                        game->server_hash);
        err = SKIFF_ERR_ROMM_SAVE_MISMATCH;
    }
    /* Checked again right before anything is replaced: the download can take a while. */
    if (err == SKIFF_OK) {
        err = confirm_local(call, a, game);
    }
    if (err == SKIFF_OK && call->saves.stale) {
        (void)skiff_storage_remove(call->storage, a->real_path);
        return SKIFF_OK;
    }
    if (err == SKIFF_OK) {
        err = skiff_saves_restore(call->storage, &call->roots, a->unit_path, game->save_target,
                                  &a->report);
        if (err != SKIFF_OK) {
            settle_restore(call);
        }
    }
    (void)skiff_storage_remove(call->storage, a->real_path);
    skiff_log_write(
        call->saves.log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, SAVES_LOG_TAG,
        "%s: download of save %llu, %llu bytes in %lld ms, restore in %lld ms "
        "(backup %u, %zu replaced, %zu removed, %zu installs kept): %s (%d)",
        game->save_target, (unsigned long long)game->server_save_id, (unsigned long long)size,
        (long long)(fetched - started), (long long)elapsed_ms(call, fetched),
        (unsigned)a->report.backup, a->report.folders_replaced, a->report.folders_removed,
        a->report.installs_kept, skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        return err;
    }
    /* RomM records what this PSP holds only now; without it, the next check finds both sides the
     * same and records the baseline then. */
    err = skiff_romm_saves_downloaded(call->romm, game->server_save_id, call->saves.device_id,
                                      game->server_hash);
    if (err != SKIFF_OK) {
        return err;
    }
    /* L as the folders now are: a folder an install holds stays out of the unit. */
    err = read_local(call, &a->unit, 1, game);
    if (err == SKIFF_OK && game->problem != SAVE_PROBLEM_NONE) {
        err = game->error;
    }
    return err == SKIFF_OK ? record(call, game) : err;
}

/* "Leave it": the deleted save stays deleted while RomM's stays what it is. */
static skiff_err leave_deleted(app_call *call, apply_scratch *a) {
    app_save_game *game = &call->saves.game;
    /* Still deleted, or saves came back meanwhile (over USB): then compare again instead. */
    game->has_local = 0;
    const skiff_err err = confirm_local(call, a, game);
    if (err != SKIFF_OK || call->saves.stale) {
        return err;
    }
    return record(call, game);
}

static skiff_err restore_backup(app_call *call, apply_scratch *a) {
    app_save_game *game = &call->saves.game;
    const int64_t started = call->now_ms(call->clock_ctx);
    const skiff_err err = skiff_saves_restore_backup(call->storage, &call->roots, game->save_target,
                                                     call->saves.backup, &a->report);
    if (err != SKIFF_OK) {
        settle_restore(call);
    }
    skiff_log_write(call->saves.log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN,
                    SAVES_LOG_TAG, "%s: backup %u restored (backup %u made) in %lld ms: %s (%d)",
                    game->save_target, (unsigned)call->saves.backup, (unsigned)a->report.backup,
                    (long long)elapsed_ms(call, started), skiff_err_name(err), (int)err);
    return err;
}

/* A restore that failed across a suspend, settled once the Memory Stick answers again: its journal
 * replayed or its stage removed (settle_restore()), then the unit it downloaded removed, which the
 * transfer's next run downloads again. The settle's result is call->saves.recovery. */
static skiff_err settle_again(app_call *call, apply_scratch *a) {
    settle_restore(call);
    const skiff_err settled = call->saves.recovery;
    if (settled != SKIFF_OK && settled != SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED &&
        settled != SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED) {
        return SKIFF_OK;
    }
    /* One the player asked for after a launch's recovery failed has no transfer behind it. */
    if (call->saves.game.save_target[0] == '\0') {
        return SKIFF_OK;
    }
    skiff_err removed = work_paths(call, a, call->saves.game.save_target);
    if (removed == SKIFF_OK) {
        removed = skiff_storage_remove(call->storage, a->real_path);
    }
    skiff_log_write(call->saves.log, SKIFF_LOG_DEBUG, SAVES_LOG_TAG,
                    "%s: downloaded unit removed: %s (%d)", call->saves.game.save_target,
                    skiff_err_name(removed), (int)removed);
    return SKIFF_OK;
}

skiff_err app_saves_apply_run(app_call *call) {
    apply_scratch *a = calloc(1, sizeof *a);
    if (a == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    call->saves.stale = 0;
    call->saves.baseline_error = SKIFF_OK;
    call->saves.recovery = SKIFF_OK;
    skiff_err err = SKIFF_ERR_INVALID_ARG;
    switch (call->saves.op) {
    case SAVES_OP_UPLOAD:
    case SAVES_OP_UPLOAD_OVERWRITE:
        err = upload(call, a, call->saves.op == SAVES_OP_UPLOAD_OVERWRITE);
        break;
    case SAVES_OP_DOWNLOAD:
        err = download(call, a);
        break;
    case SAVES_OP_LEAVE_DELETED:
        err = leave_deleted(call, a);
        break;
    case SAVES_OP_RESTORE_BACKUP:
        err = restore_backup(call, a);
        break;
    case SAVES_OP_SETTLE:
        err = settle_again(call, a);
        break;
    case SAVES_OP_CHECK:
    case SAVES_OP_CHECK_ONE:
        break;
    }
    free(a);
    const int recorded = err == SKIFF_OK && !call->saves.stale &&
                         call->saves.op != SAVES_OP_RESTORE_BACKUP &&
                         call->saves.op != SAVES_OP_SETTLE;
    if (recorded || call->saves.baseline_pending) {
        call->saves.baseline_error = save_baseline(call);
    }
    if (recorded) {
        call->saves.game.decided = 1;
        call->saves.game.decision.action = SKIFF_SAVES_IN_SYNC;
        call->saves.game.decision.reason = SKIFF_SAVES_REASON_UNCHANGED;
        call->saves.game.problem = SAVE_PROBLEM_NONE;
        call->saves.game.error = SKIFF_OK;
    }
    return err;
}

skiff_err app_saves_complete_run(app_call *call) {
    const skiff_err err = skiff_romm_saves_complete(call->romm, call->saves.session_id,
                                                    call->saves.completed, call->saves.failed);
    skiff_log_write(call->saves.log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN,
                    SAVES_LOG_TAG, "session %llu closed, %u done, %u failed: %s (%d)",
                    (unsigned long long)call->saves.session_id, (unsigned)call->saves.completed,
                    (unsigned)call->saves.failed, skiff_err_name(err), (int)err);
    return err;
}

skiff_err app_saves_backups_run(app_call *call) {
    return skiff_saves_backups_list(call->storage, &call->roots, call->saves.game.save_target,
                                    &call->saves.backups);
}
