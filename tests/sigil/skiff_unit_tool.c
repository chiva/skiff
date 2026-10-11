/*
 * Skiff's side of the sigil cross-check (tests/sigil/check.sh): skiff/saves.h over the POSIX
 * storage, with <device> standing in for a Memory Stick (saves: is <device>/PSP/SAVEDATA, app: is
 * <device>/PSP/GAME/Skiff).
 *
 *   skiff_unit_tool write <device> <save_target> <unit>            collects the game's folders and
 *                                                                  writes app:/<unit>
 *   skiff_unit_tool read <device> <save_target> <unit> <staging>   checks app:/<unit> and extracts
 *                                                                  it into app:/<staging>
 *
 * Both print the unit's content_hash on success, and the error and problem otherwise.
 */
#include <stdio.h>
#include <string.h>

#include "skiff/saves.h"

#include "storage_posix.h"

#define APP_FOLDER "/PSP/GAME/Skiff"
#define EXIT_USAGE 2

static int report(const char *step, skiff_err err, const skiff_saves_unit *unit) {
    if (err == SKIFF_OK) {
        printf("%s\n", unit->content_hash);
        fprintf(stderr, "skiff %s: %zu folders, %zu files, %llu bytes\n", step, unit->folder_count,
                unit->file_count, (unsigned long long)unit->total_bytes);
        return 0;
    }
    fprintf(stderr, "skiff %s: %s, problem %s \"%s\"\n", step, skiff_err_name(err),
            skiff_saves_problem_name(unit->problem), unit->problem_name);
    return 1;
}

static int logical(const char *name, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, SKIFF_STORAGE_ROOT_APP "/%s", name);
    return written > 0 && (size_t)written < out_size;
}

static int run(skiff_storage *storage, const skiff_storage_roots *roots, int argc, char **argv,
               skiff_saves_unit *unit) {
    char unit_path[SKIFF_STORAGE_PATH_MAX];
    char staging[SKIFF_STORAGE_PATH_MAX];
    if (!logical(argv[4], unit_path, sizeof unit_path)) {
        return EXIT_USAGE;
    }
    if (strcmp(argv[1], "write") == 0 && argc == 5) {
        skiff_err err = skiff_storage_mkdirs(storage, roots->app);
        if (err == SKIFF_OK) {
            err = skiff_saves_collect(storage, roots, SKIFF_STORAGE_ROOT_SAVES, argv[3], unit);
        }
        if (err == SKIFF_OK) {
            err = skiff_saves_write_unit(storage, roots, unit, unit_path);
        }
        return report("write", err, unit);
    }
    if (strcmp(argv[1], "read") == 0 && argc == 6 && logical(argv[5], staging, sizeof staging)) {
        return report(
            "read", skiff_saves_read_unit(storage, roots, unit_path, argv[3], staging, unit), unit);
    }
    return EXIT_USAGE;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: skiff_unit_tool write <device> <save_target> <unit>\n"
                        "       skiff_unit_tool read <device> <save_target> <unit> <staging>\n");
        return EXIT_USAGE;
    }
    char app[SKIFF_STORAGE_PATH_MAX];
    skiff_storage_roots roots;
    skiff_storage *storage = NULL;
    skiff_saves_unit *unit = NULL;
    int status = EXIT_USAGE;
    if (snprintf(app, sizeof app, "%s" APP_FOLDER, argv[2]) < (int)sizeof app &&
        skiff_storage_roots_init(argv[2], app, &roots) == SKIFF_OK &&
        skiff_posix_storage_create(&storage) == SKIFF_OK &&
        skiff_saves_unit_create(&unit) == SKIFF_OK) {
        status = run(storage, &roots, argc, argv, unit);
    }
    if (status == EXIT_USAGE) {
        fprintf(stderr, "skiff_unit_tool: bad arguments or setup\n");
    }
    skiff_saves_unit_destroy(unit);
    skiff_storage_destroy(storage);
    return status;
}
