/*
 * The RomM user's collections (skiff_romm_list_collections() in skiff/romm.h): RomM's regular and
 * smart lists, parsed only for what the collections screen shows. RomM sends each list whole, every
 * collection with all its ROM ids (and a regular one with two cover paths per ROM), so an answer
 * over the caps, or more collections than a list keeps, is refused whole with its own error, which
 * the screen can name, rather than shown in part.
 */
#include <string.h>

#include "skiff/romm.h"

#include "romm_internal.h"

#define PATH_COLLECTIONS "/api/collections"
#define PATH_SMART_COLLECTIONS "/api/collections/smart"

/* What one item of a list is to the screen. */
typedef enum item_result {
    ITEM_KEPT,
    /* A collection the screen does not list (favourites, virtual). */
    ITEM_SKIPPED,
    /* Not a collection as RomM sends one: the whole answer is refused. */
    ITEM_BAD,
} item_result;

static item_result parse_item(const cJSON *item, int smart, skiff_romm_collection *out) {
    memset(out, 0, sizeof *out);
    const cJSON *is_smart = cJSON_GetObjectItemCaseSensitive(item, "is_smart");
    /* RomM numbers collections from 1, and a list asked with id 0 is refused (422). Each list says
     * which kind it holds: one of the other kind would open with the wrong filter, and list another
     * collection's games. */
    if (!cJSON_IsObject(item) || !skiff_romm_read_count(item, "id", &out->id) || out->id == 0 ||
        !skiff_romm_read_count(item, "rom_count", &out->rom_count) ||
        !skiff_romm_read_display_name(item, "name", out->name, sizeof out->name) ||
        !cJSON_IsBool(is_smart) || cJSON_IsTrue(is_smart) != smart) {
        return ITEM_BAD;
    }
    out->smart = smart;
    /* The favourites list holds the favourites' games already; a virtual collection is not opened
     * by a number. */
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_favorite")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_virtual"))) {
        return ITEM_SKIPPED;
    }
    return ITEM_KEPT;
}

/* Appends the collections of a list's tree to out; out->count moves only when all of them fit. */
static skiff_err fill_collections(const cJSON *root, int smart, skiff_romm_collection_list *out) {
    if (!cJSON_IsArray(root)) {
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    size_t count = out->count;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        skiff_romm_collection collection;
        const item_result result = parse_item(item, smart, &collection);
        if (result == ITEM_BAD) {
            return SKIFF_ERR_ROMM_BAD_RESPONSE;
        }
        if (result == ITEM_SKIPPED) {
            continue;
        }
        if (count == SKIFF_ROMM_COLLECTIONS_MAX) {
            return SKIFF_ERR_ROMM_COLLECTIONS_TOO_LARGE;
        }
        out->items[count++] = collection;
    }
    out->count = count;
    return SKIFF_OK;
}

skiff_err skiff_romm_parse_collections(const char *json, size_t length, int smart,
                                       skiff_romm_collection_list *out) {
    if (out == NULL || out->count > SKIFF_ROMM_COLLECTIONS_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    int too_large = 0;
    cJSON *root = skiff_romm_parse_json_capped(json, length, &too_large);
    skiff_err err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    if (root != NULL) {
        err = fill_collections(root, smart, out);
    } else if (too_large) {
        err = SKIFF_ERR_ROMM_COLLECTIONS_TOO_LARGE;
    }
    cJSON_Delete(root);
    return err;
}

/* One of RomM's two lists, appended to out. */
static skiff_err list_kind(skiff_romm_client *client, const char *path, int smart,
                           skiff_romm_collection_list *out) {
    cJSON *root = NULL;
    int too_large = 0;
    skiff_err err = skiff_romm_get_json(client, path, &root, &too_large);
    if (err == SKIFF_ERR_ROMM_BAD_RESPONSE && too_large) {
        err = SKIFF_ERR_ROMM_COLLECTIONS_TOO_LARGE;
    }
    if (err == SKIFF_OK) {
        err = fill_collections(root, smart, out);
    }
    cJSON_Delete(root);
    return err;
}

skiff_err skiff_romm_list_collections(skiff_romm_client *client, skiff_romm_collection_list *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    skiff_err err = list_kind(client, PATH_COLLECTIONS, 0, out);
    if (err == SKIFF_OK) {
        err = list_kind(client, PATH_SMART_COLLECTIONS, 1, out);
    }
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}
