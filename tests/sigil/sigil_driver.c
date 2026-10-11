/*
 * argosy-sigil's side of the sigil cross-check (tests/sigil/check.sh): sigil's own collect and
 * restore for a PSP memory stick laid out in a host directory, with layout psp_console, the way a
 * sigil-based client syncs a real PSP. Skiff's code is not linked here.
 *
 *   sigil_driver collect <base> <save_id> <unit>   sigil's unit of the game's folders under
 *                                                  <base>/PSP/SAVEDATA, written to <unit>; prints
 *                                                  its content_hash
 *   sigil_driver restore <base> <save_id> <unit>   puts <unit> back under <base>, an empty folder;
 *                                                  prints the content_hash of what it restored
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sigil.h"

#define SAVEDATA "PSP/SAVEDATA"
#define LISTING_MAX 1024
#define LAYOUT "psp_console"
#define PLATFORM "psp"
#define FOLDER_MODE 0755

typedef struct stick {
    const char *base;
    char *paths[LISTING_MAX];
    size_t count;
} stick;

static int fail(const char *what, int code) {
    fprintf(stderr, "sigil_driver: %s (%d: %s)\n", what, code, sigil_strerror(code));
    return 1;
}

static void full_path(const stick *s, const char *relative, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", s->base, relative);
}

/* Every file under <base>/<relative>, as paths relative to base. */
static int list_files(stick *s, const char *relative) {
    char path[SIGIL_SAVE_PATH_MAX * 2];
    full_path(s, relative, path, sizeof path);
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    int result = 0;
    const struct dirent *entry;
    while (result == 0 && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[SIGIL_SAVE_PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", relative, entry->d_name);
        char child_path[SIGIL_SAVE_PATH_MAX * 2];
        full_path(s, child, child_path, sizeof child_path);
        struct stat info;
        if (stat(child_path, &info) != 0) {
            result = -1;
        } else if (S_ISDIR(info.st_mode)) {
            result = list_files(s, child);
        } else if (s->count < LISTING_MAX) {
            s->paths[s->count++] = strdup(child);
        } else {
            result = -1;
        }
    }
    closedir(dir);
    return result;
}

static int file_read(void *ctx, uint64_t offset, void *buffer, size_t length) {
    FILE *file = ctx;
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        return -1;
    }
    const size_t got = fread(buffer, 1, length, file);
    return ferror(file) ? -1 : (int)got;
}

static int64_t file_size(void *ctx) {
    FILE *file = ctx;
    if (fseeko(file, 0, SEEK_END) != 0) {
        return -1;
    }
    return (int64_t)ftello(file);
}

static void file_close(void *ctx) { fclose(ctx); }

static sigil_io *stick_open(void *ctx, const char *relative) {
    const stick *s = ctx;
    char path[SIGIL_SAVE_PATH_MAX * 2];
    full_path(s, relative, path, sizeof path);
    FILE *file = fopen(path, "rb");
    sigil_io *io = file != NULL ? malloc(sizeof *io) : NULL;
    if (io == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    io->read = file_read;
    io->size = file_size;
    io->close = file_close;
    io->ctx = file;
    return io;
}

static int make_parents(char *path) {
    for (char *slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        const int made = mkdir(path, FOLDER_MODE) == 0 || errno == EEXIST;
        *slash = '/';
        if (!made) {
            return -1;
        }
    }
    return 0;
}

static int stick_write(void *ctx, const char *relative, const uint8_t *data, size_t length) {
    const stick *s = ctx;
    char path[SIGIL_SAVE_PATH_MAX * 2];
    full_path(s, relative, path, sizeof path);
    if (make_parents(path) != 0) {
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    const int ok = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok ? 0 : -1;
}

static int stick_remove(void *ctx, const char *relative) {
    const stick *s = ctx;
    char path[SIGIL_SAVE_PATH_MAX * 2];
    full_path(s, relative, path, sizeof path);
    return remove(path) == 0 ? 0 : -1;
}

static void request_for(stick *s, const char *save_id, sigil_result *result,
                        sigil_sync_request *request) {
    memset(result, 0, sizeof *result);
    result->struct_version = SIGIL_RESULT_V3;
    result->platform = SIGIL_PLATFORM_PSP;
    snprintf(result->title_id, sizeof result->title_id, "%s", save_id);
    snprintf(result->save_id, sizeof result->save_id, "%s", save_id);
    memset(request, 0, sizeof *request);
    request->struct_version = SIGIL_SYNC_REQUEST_V1;
    request->save.struct_version = SIGIL_SAVE_REQUEST_V1;
    request->save.layout = LAYOUT;
    request->save.platform = PLATFORM;
    request->save.content_path = "game.iso";
    request->save.result = result;
    request->save.listing = (const char *const *)s->paths;
    request->save.listing_count = s->count;
    request->save.open = stick_open;
    request->save.open_ctx = s;
    request->write = stick_write;
    request->remove = stick_remove;
    request->write_ctx = s;
    request->overwrite_local = 1;
}

static int collect(stick *s, const char *save_id, const char *unit_path) {
    sigil_result result;
    sigil_sync_request request;
    request_for(s, save_id, &result, &request);
    sigil_sync_result *out = NULL;
    const int code = sigil_collect(&request, &out);
    if (code != SIGIL_OK || out == NULL || out->data == NULL) {
        sigil_sync_result_free(out);
        return fail("collect", code);
    }
    FILE *file = fopen(unit_path, "wb");
    const int written = file != NULL && fwrite(out->data, 1, out->len, file) == out->len;
    if (file == NULL || fclose(file) != 0 || !written) {
        sigil_sync_result_free(out);
        return fail("writing the unit", SIGIL_ERR_IO);
    }
    printf("%s\n", out->content_hash);
    fprintf(stderr, "sigil collect: %s, %zu bytes, artifact %s\n", unit_path, out->len,
            out->artifact);
    sigil_sync_result_free(out);
    return 0;
}

static int restore(stick *s, const char *save_id, const char *unit_path) {
    FILE *file = fopen(unit_path, "rb");
    if (file == NULL || fseeko(file, 0, SEEK_END) != 0) {
        return fail("opening the unit", SIGIL_ERR_IO);
    }
    const long size = (long)ftello(file);
    uint8_t *unit = size > 0 ? malloc((size_t)size) : NULL;
    rewind(file);
    const int read_ok = unit != NULL && fread(unit, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read_ok) {
        free(unit);
        return fail("reading the unit", SIGIL_ERR_IO);
    }
    sigil_result result;
    sigil_sync_request request;
    request_for(s, save_id, &result, &request);
    sigil_sync_result *out = NULL;
    const int code = sigil_restore(&request, unit, (size_t)size, &out);
    free(unit);
    if (code != SIGIL_OK) {
        fprintf(stderr, "sigil restore problem: %s\n", out != NULL ? out->problem : "");
        sigil_sync_result_free(out);
        return fail("restore", code);
    }
    printf("%s\n", out != NULL ? out->content_hash : "");
    sigil_sync_result_free(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5 || (strcmp(argv[1], "collect") != 0 && strcmp(argv[1], "restore") != 0)) {
        fprintf(stderr, "usage: sigil_driver collect|restore <base> <save_id> <unit>\n");
        return 2;
    }
    stick s = {argv[2], {NULL}, 0};
    if (list_files(&s, SAVEDATA) != 0) {
        return fail("listing the stick", SIGIL_ERR_IO);
    }
    fprintf(stderr, "sigil %s: %zu files under %s/" SAVEDATA "\n", sigil_version(), s.count,
            s.base);
    const int status = strcmp(argv[1], "collect") == 0 ? collect(&s, argv[3], argv[4])
                                                       : restore(&s, argv[3], argv[4]);
    for (size_t i = 0; i < s.count; i++) {
        free(s.paths[i]);
    }
    return status;
}
