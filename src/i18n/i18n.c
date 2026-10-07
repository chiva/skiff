#include "skiff/i18n.h"

#include <stdio.h>
#include <string.h>

#define PLACEHOLDER_OPEN '{'
#define PLACEHOLDER_CLOSE '}'
#define PLACEHOLDER_FIRST '1'
#define PLACEHOLDER_LAST '9'
#define PLACEHOLDER_LENGTH 3
/* The code after the sentence, as the troubleshooting guide lists them. */
#define ERROR_CODE_DIGITS_MAX 12

/*
 * Spanish error sentences, one row per row of SKIFF_ERROR_TABLE (include/skiff/error.h). Rows name
 * error.h's codes, so a misspelt one does not compile, and the counts must match (checked below),
 * so a code added there without a row here does not compile either.
 */
#define SKIFF_ERROR_TABLE_SPANISH(X)                                                               \
    X(SKIFF_OK, "Sin errores")                                                                     \
    X(SKIFF_ERR_INVALID_ARG, "Error interno: argumento no válido")                                 \
    X(SKIFF_ERR_NO_MEMORY, "La PSP se ha quedado sin memoria")                                     \
    X(SKIFF_ERR_BUFFER_TOO_SMALL, "Error interno: búfer demasiado pequeño")                        \
    X(SKIFF_ERR_NOT_IMPLEMENTED, "Esta función aún no está disponible")                            \
    X(SKIFF_ERR_CANCELLED, "Cancelada")                                                            \
    X(SKIFF_ERR_NET_UNAVAILABLE, "El wifi está desactivado o no hay ninguna conexión configurada") \
    X(SKIFF_ERR_NET_DNS, "No se encuentra el servidor de RomM; revisa la dirección")               \
    X(SKIFF_ERR_NET_CONNECT, "No se puede conectar con el servidor de RomM")                       \
    X(SKIFF_ERR_NET_TIMEOUT, "El servidor de RomM ha tardado demasiado en responder")              \
    X(SKIFF_ERR_NET_TLS_HANDSHAKE, "No se ha podido establecer la conexión segura (TLS)")          \
    X(SKIFF_ERR_NET_TLS_UNTRUSTED, "El certificado del servidor no es de confianza")               \
    X(SKIFF_ERR_NET_TLS_CLIENT_CERT,                                                               \
      "El certificado de cliente se ha rechazado o no se puede leer")                              \
    X(SKIFF_ERR_NET_ENTROPY, "No hay suficiente aleatoriedad para una conexión segura")            \
    X(SKIFF_ERR_NET_TLS_CLOCK,                                                                     \
      "La fecha y la hora de la PSP no son correctas; ajústalas en Ajustes para conectarte de "    \
      "forma segura")                                                                              \
    X(SKIFF_ERR_NET_NEEDS_ARK,                                                                     \
      "Para conectarse a la red hace falta el firmware ARK (ARK-4 o ARK-5)")                       \
    X(SKIFF_ERR_NET_WIFI_JOIN, "No se ha podido conectar a la red wifi")                           \
    X(SKIFF_ERR_NET_CONNECTION_LOST, "Se ha perdido la conexión con el servidor de RomM")          \
    X(SKIFF_ERR_ROMM_UNAUTHORIZED, "RomM ha rechazado el acceso; vuelve a vincular esta PSP")      \
    X(SKIFF_ERR_ROMM_FORBIDDEN, "Tu usuario de RomM no tiene permiso para hacer esto")             \
    X(SKIFF_ERR_ROMM_NOT_FOUND, "RomM no encuentra ese elemento")                                  \
    X(SKIFF_ERR_ROMM_SERVER, "RomM ha devuelto un error del servidor")                             \
    X(SKIFF_ERR_ROMM_BAD_RESPONSE, "RomM ha enviado una respuesta que Skiff no entiende")          \
    X(SKIFF_ERR_ROMM_UNSUPPORTED_VERSION, "Esta versión de RomM no es compatible")                 \
    X(SKIFF_ERR_ROMM_CHECKSUM,                                                                     \
      "El archivo no coincide con la suma de comprobación de RomM; vuelve a escanear la "          \
      "plataforma en RomM")                                                                        \
    X(SKIFF_ERR_STORAGE_NO_MEDIA, "No se encuentra ningún Memory Stick")                           \
    X(SKIFF_ERR_STORAGE_NO_SPACE, "No hay suficiente espacio libre en el Memory Stick")            \
    X(SKIFF_ERR_STORAGE_IO, "No se puede leer o escribir en el Memory Stick")                      \
    X(SKIFF_ERR_STORAGE_NOT_FOUND, "No se encuentra el archivo o la carpeta en el Memory Stick")   \
    X(SKIFF_ERR_STORAGE_FILE_TOO_LARGE,                                                            \
      "El archivo ocupa más de 4 GB y la PSP no puede guardarlo")                                  \
    X(SKIFF_ERR_CONFIG_PARSE, "El archivo de ajustes está dañado")                                 \
    X(SKIFF_ERR_CONFIG_MISSING_KEY, "Falta un ajuste obligatorio")                                 \
    X(SKIFF_ERR_CONFIG_INVALID_VALUE, "Un ajuste tiene un valor no válido")

typedef struct error_row {
    skiff_err code;
    const char *text;
} error_row;

static const error_row SPANISH_ERRORS[] = {
#define SKIFF_ERROR_SPANISH_ROW(name, text) {name, text},
    SKIFF_ERROR_TABLE_SPANISH(SKIFF_ERROR_SPANISH_ROW)
#undef SKIFF_ERROR_SPANISH_ROW
};

#define SKIFF_ERROR_COUNT_ROW(name, value, message) +1
enum { ERROR_COUNT = 0 SKIFF_ERROR_TABLE(SKIFF_ERROR_COUNT_ROW) };
#undef SKIFF_ERROR_COUNT_ROW
_Static_assert(sizeof SPANISH_ERRORS / sizeof SPANISH_ERRORS[0] == ERROR_COUNT,
               "every error in include/skiff/error.h needs a Spanish sentence here");

typedef struct text_row {
    const char *name;
    int placeholders;
    const char *text[SKIFF_LANGUAGE_COUNT];
} text_row;

static const text_row TEXTS[SKIFF_TEXT_COUNT] = {
#define SKIFF_TEXT_ROW(id, placeholders, english, spanish) {#id, placeholders, {english, spanish}},
    SKIFF_TEXT_TABLE(SKIFF_TEXT_ROW)
#undef SKIFF_TEXT_ROW
};

static int known_text(skiff_text_id id) { return (int)id >= 0 && id < SKIFF_TEXT_COUNT; }

static int known_language(skiff_language language) {
    return (int)language >= 0 && language < SKIFF_LANGUAGE_COUNT;
}

skiff_language skiff_language_from_psp(int system_language) {
    return system_language == SKIFF_PSP_SYSTEM_LANGUAGE_SPANISH ? SKIFF_LANGUAGE_SPANISH
                                                                : SKIFF_LANGUAGE_ENGLISH;
}

const char *skiff_text(skiff_language language, skiff_text_id id) {
    if (!known_text(id)) {
        return "";
    }
    return TEXTS[id].text[known_language(language) ? language : SKIFF_LANGUAGE_ENGLISH];
}

const char *skiff_text_name(skiff_text_id id) { return known_text(id) ? TEXTS[id].name : NULL; }

int skiff_text_placeholders(skiff_text_id id) {
    return known_text(id) ? TEXTS[id].placeholders : -1;
}

const char *skiff_error_text(skiff_language language, skiff_err err) {
    if (language == SKIFF_LANGUAGE_SPANISH) {
        for (size_t i = 0; i < sizeof SPANISH_ERRORS / sizeof SPANISH_ERRORS[0]; i++) {
            if (SPANISH_ERRORS[i].code == err) {
                return SPANISH_ERRORS[i].text;
            }
        }
    }
    return skiff_err_message(err);
}

/* A UTF-8 continuation byte (10xxxxxx): never the first byte of a character. */
static int is_continuation(char c) { return ((unsigned char)c & 0xC0U) == 0x80U; }

typedef struct writer {
    char *out;
    size_t size;
    size_t used;
    int fits;
} writer;

/* Appends what fits; once something does not, the text ends at the last whole character. */
static void put(writer *w, const char *text, size_t length) {
    if (!w->fits) {
        return;
    }
    if (length < w->size - w->used) {
        memcpy(w->out + w->used, text, length);
        w->used += length;
        w->out[w->used] = '\0';
        return;
    }
    size_t room = w->size - 1 - w->used;
    while (room > 0 && is_continuation(text[room])) {
        room--;
    }
    memcpy(w->out + w->used, text, room);
    w->used += room;
    w->out[w->used] = '\0';
    w->fits = 0;
}

static int placeholder_at(const char *text, size_t *index) {
    if (text[0] != PLACEHOLDER_OPEN || text[1] < PLACEHOLDER_FIRST || text[1] > PLACEHOLDER_LAST ||
        text[2] != PLACEHOLDER_CLOSE) {
        return 0;
    }
    *index = (size_t)(text[1] - PLACEHOLDER_FIRST);
    return 1;
}

skiff_err skiff_text_format(const char *template_text, const char *const *args, size_t count,
                            char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (template_text == NULL || out == NULL || out_size == 0 || (args == NULL && count > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    writer w = {out, out_size, 0, 1};
    const char *literal = template_text;
    const char *cursor = template_text;
    while (*cursor != '\0') {
        size_t index = 0;
        if (!placeholder_at(cursor, &index)) {
            cursor++;
            continue;
        }
        put(&w, literal, (size_t)(cursor - literal));
        if (index < count && args[index] != NULL) {
            put(&w, args[index], strlen(args[index]));
        }
        cursor += PLACEHOLDER_LENGTH;
        literal = cursor;
    }
    put(&w, literal, (size_t)(cursor - literal));
    return w.fits ? SKIFF_OK : SKIFF_ERR_BUFFER_TOO_SMALL;
}

skiff_err skiff_error_line(skiff_language language, skiff_err err, char *out, size_t out_size) {
    char code[ERROR_CODE_DIGITS_MAX];
    snprintf(code, sizeof code, "%d", (int)err);
    const char *args[] = {skiff_error_text(language, err), code};
    return skiff_text_format(skiff_text(language, SKIFF_TEXT_ERROR_CODE), args,
                             sizeof args / sizeof args[0], out, out_size);
}
