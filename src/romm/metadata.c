/*
 * A ROM's description and facts for the details screen (skiff_romm_rom_metadata in skiff/romm.h):
 * RomM's summary, the year, genres, players, companies and rating of its metadatum, and the
 * regions and languages it read from the file name or a provider. All of it is decoration, so
 * nothing here refuses a ROM: a value Skiff would not show is left out.
 */
#include <string.h>

#include "skiff/romm.h"

#include "romm_internal.h"

/* 2^53: every whole number below it is a double exactly (romm.c reads ids under the same limit). */
#define JSON_INTEGER_LIMIT 9007199254740992.0
#define MS_PER_DAY 86400000LL
/* Years shown; anything else is a value no game has. */
#define RELEASE_YEAR_MIN 1
#define RELEASE_YEAR_MAX 9999
/* What RomM gives a game it knows no player count for, and one for a single player alike. */
#define PLAYERS_UNKNOWN "1"
#define UTF8_CONTINUATION_MASK 0xC0U
#define UTF8_CONTINUATION 0x80U
/* The lowest lead byte of a character past ASCII (C2, for U+0080). */
#define UTF8_LEAD_MIN 0xC2U
/* The most continuation bytes after a UTF-8 lead byte. */
#define UTF8_CONTINUATIONS_MAX 3
#define ROUND_HALF 0.5

/* Howard Hinnant's days_from_civil, inverted (civil_from_days): the proleptic Gregorian calendar
 * in eras of 400 years, day 0 being 1970-01-01. */
#define CIVIL_EPOCH_SHIFT 719468LL
#define CIVIL_DAYS_PER_ERA 146097LL
#define CIVIL_YEARS_PER_ERA 400LL
#define CIVIL_DAYS_PER_4_YEARS 1460LL
#define CIVIL_DAYS_PER_CENTURY 36524LL
#define CIVIL_DAYS_PER_YEAR 365LL
#define CIVIL_MONTH_DAYS_NUMERATOR 153LL
#define CIVIL_MONTH_FACTOR 5LL
#define CIVIL_MONTH_OFFSET 2LL
/* The civil year starts in March: months from January on (index 10, 11) belong to the next year. */
#define CIVIL_FIRST_NEXT_YEAR_MONTH 10LL
#define CENTURY_YEARS 100LL
#define LEAP_EVERY_YEARS 4LL

/* ---- Text the PSP's Latin font draws ---- */

/* A character replaced by the ASCII the PSP's Latin firmware font draws (ltn0.pgf holds Latin-1;
 * these punctuation marks are past it): BREAK starts a new line, "" drops the character. */
#define BREAK "\n"
typedef struct replacement {
    const char *utf8;
    const char *ascii;
} replacement;

static const replacement REPLACEMENTS[] = {
    {"\xC2\xA0", " "},       /* no-break space */
    {"\xC2\xAD", ""},        /* soft hyphen */
    {"\xE2\x80\x8B", ""},    /* zero-width space */
    {"\xE2\x80\x8C", ""},    /* zero-width non-joiner */
    {"\xE2\x80\x8D", ""},    /* zero-width joiner */
    {"\xE2\x80\x90", "-"},   /* hyphen */
    {"\xE2\x80\x91", "-"},   /* non-breaking hyphen */
    {"\xE2\x80\x92", "-"},   /* figure dash */
    {"\xE2\x80\x93", "-"},   /* en dash */
    {"\xE2\x80\x94", "-"},   /* em dash */
    {"\xE2\x80\x95", "-"},   /* horizontal bar */
    {"\xE2\x80\x98", "'"},   /* left single quotation mark */
    {"\xE2\x80\x99", "'"},   /* right single quotation mark, apostrophe */
    {"\xE2\x80\x9A", "'"},   /* single low-9 quotation mark */
    {"\xE2\x80\x9B", "'"},   /* single high-reversed-9 quotation mark */
    {"\xE2\x80\x9C", "\""},  /* left double quotation mark */
    {"\xE2\x80\x9D", "\""},  /* right double quotation mark */
    {"\xE2\x80\x9E", "\""},  /* double low-9 quotation mark */
    {"\xE2\x80\x9F", "\""},  /* double high-reversed-9 quotation mark */
    {"\xE2\x80\xA2", "-"},   /* bullet */
    {"\xE2\x80\xA6", "..."}, /* horizontal ellipsis */
    {"\xE2\x80\xA8", BREAK}, /* line separator */
    {"\xE2\x80\xA9", BREAK}, /* paragraph separator */
    {"\xE2\x80\xAF", " "},   /* narrow no-break space */
    {"\xE2\x80\xB2", "'"},   /* prime */
    {"\xE2\x80\xB3", "\""},  /* double prime */
    {"\xE2\x88\x92", "-"},   /* minus sign */
    {"\xEF\xBB\xBF", ""},    /* byte order mark (zero-width no-break space) */
};

/* U+2000 to U+200A, the typographic spaces from en quad to hair space: each a space. */
#define WIDE_SPACE_LEAD "\xE2\x80"
#define WIDE_SPACE_FIRST 0x80U
#define WIDE_SPACE_LAST 0x8AU
static const replacement WIDE_SPACE = {"\xE2\x80\x80", " "};

/* Text written into a buffer of size bytes (terminator included): spaces collapsed, line breaks
 * kept when breaks is set (at most one empty line in a row), and overflow set once something
 * visible did not fit, after which nothing more is written. */
typedef struct text_out {
    char *out;
    size_t size;
    size_t used;
    int breaks;
    int overflow;
} text_out;

static void put_visible(text_out *t, const char *bytes, size_t length) {
    if (t->overflow) {
        return;
    }
    if (length > t->size - 1 - t->used) {
        t->overflow = 1;
        return;
    }
    memcpy(t->out + t->used, bytes, length);
    t->used += length;
}

static int ends_blank(const text_out *t) {
    return t->used == 0 || t->out[t->used - 1] == ' ' || t->out[t->used - 1] == '\n';
}

/* A space between words; none at the start, after another or after a break. Blanks never set
 * overflow: one that does not fit only matters if something visible follows, which will. */
static void put_space(text_out *t) {
    if (!t->overflow && !ends_blank(t) && t->used + 1 < t->size) {
        t->out[t->used++] = ' ';
    }
}

/* A line break, or a space where breaks are not kept: none at the start, and at most two in a row
 * (one empty line between paragraphs). */
static void put_break(text_out *t) {
    if (!t->breaks) {
        put_space(t);
        return;
    }
    while (t->used > 0 && t->out[t->used - 1] == ' ') {
        t->used--;
    }
    const int doubled = t->used >= 2 && t->out[t->used - 1] == '\n' && t->out[t->used - 2] == '\n';
    if (!t->overflow && t->used > 0 && !doubled && t->used + 1 < t->size) {
        t->out[t->used++] = '\n';
    }
}

static void put_replacement(text_out *t, const char *ascii) {
    if (strcmp(ascii, BREAK) == 0) {
        put_break(t);
    } else if (strcmp(ascii, " ") == 0) {
        put_space(t);
    } else if (ascii[0] != '\0') {
        put_visible(t, ascii, strlen(ascii));
    }
}

static const replacement *replacement_at(const char *text) {
    /* Every one is past ASCII: its UTF-8 starts with a lead byte. */
    if ((unsigned char)text[0] < UTF8_LEAD_MIN) {
        return NULL;
    }
    /* After a matched lead the third byte is inside the string, its terminator at worst. */
    if (strncmp(text, WIDE_SPACE_LEAD, sizeof WIDE_SPACE_LEAD - 1) == 0 &&
        (unsigned char)text[2] >= WIDE_SPACE_FIRST && (unsigned char)text[2] <= WIDE_SPACE_LAST) {
        return &WIDE_SPACE;
    }
    for (size_t i = 0; i < sizeof REPLACEMENTS / sizeof REPLACEMENTS[0]; i++) {
        const size_t length = strlen(REPLACEMENTS[i].utf8);
        if (strncmp(text, REPLACEMENTS[i].utf8, length) == 0) {
            return &REPLACEMENTS[i];
        }
    }
    return NULL;
}

static int is_continuation(char c) {
    return ((unsigned char)c & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION;
}

/* The bytes of the character text starts with: its lead and the continuation bytes after it. */
static size_t character_length(const char *text) {
    size_t length = 1;
    while (length <= UTF8_CONTINUATIONS_MAX && is_continuation(text[length])) {
        length++;
    }
    return length;
}

/* Writes text into t in the form skiff/romm.h describes, then ends it: blanks trimmed, and when
 * something did not fit, cut at a character boundary with SKIFF_ROMM_DESCRIPTION_CUT_MARKER after
 * it. t->size must exceed the marker. */
static void write_text(text_out *t, const char *text) {
    for (const char *c = text; *c != '\0';) {
        const replacement *mapped = replacement_at(c);
        const size_t control = skiff_romm_control_length(c);
        if (*c == '\n') {
            put_break(t);
            c++;
        } else if (*c == ' ' || *c == '\t' || *c == '\r') {
            put_space(t);
            c++;
        } else if (control > 0) {
            c += control;
        } else if (mapped != NULL) {
            put_replacement(t, mapped->ascii);
            c += strlen(mapped->utf8);
        } else {
            const size_t length = character_length(c);
            put_visible(t, c, length);
            c += length;
        }
    }
    /* Room for the marker after what is kept, cut before a character, never inside one. */
    const size_t marker_room = t->size - sizeof SKIFF_ROMM_DESCRIPTION_CUT_MARKER;
    if (t->overflow && t->used > marker_room) {
        size_t kept = marker_room;
        while (kept > 0 && is_continuation(t->out[kept])) {
            kept--;
        }
        t->used = kept;
    }
    while (ends_blank(t) && t->used > 0) {
        t->used--;
    }
    if (t->overflow) {
        memcpy(t->out + t->used, SKIFF_ROMM_DESCRIPTION_CUT_MARKER,
               sizeof SKIFF_ROMM_DESCRIPTION_CUT_MARKER - 1);
        t->used += sizeof SKIFF_ROMM_DESCRIPTION_CUT_MARKER - 1;
    }
    t->out[t->used] = '\0';
}

/* ---- Fields ---- */

/* RomM's summary, made displayable and cut to out_size; empty when it is not a string. */
static void read_description(const cJSON *root, char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "summary");
    out[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        text_out t = {out, out_size, 0, 1, 0};
        write_text(&t, item->valuestring);
    }
}

/* One fact: a string that fits out whole, holds no control character and is not blank once made
 * displayable (which never lengthens it). */
static int read_fact_value(const cJSON *item, char *out, size_t out_size) {
    out[0] = '\0';
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= out_size || !skiff_romm_is_printable_text(item->valuestring)) {
        return 0;
    }
    text_out t = {out, out_size, 0, 0, 0};
    write_text(&t, item->valuestring);
    return out[0] != '\0';
}

/* The first max facts of the list object holds under name that read; the others are skipped. */
static size_t read_fact_list(const cJSON *object, const char *name,
                             char (*out)[SKIFF_ROMM_FACT_MAX], size_t max) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsArray(list)) {
        return 0;
    }
    size_t count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, list) {
        if (count == max) {
            break;
        }
        count += (size_t)read_fact_value(entry, out[count], SKIFF_ROMM_FACT_MAX);
    }
    return count;
}

/* The first fact of a list into out, or out empty. */
static int read_first_fact(const cJSON *object, const char *name, char *out) {
    char first[1][SKIFF_ROMM_FACT_MAX];
    if (read_fact_list(object, name, first, 1) == 0) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, first[0], SKIFF_ROMM_FACT_MAX);
    return 1;
}

static long long floor_divide(long long value, long long divisor) {
    const long long quotient = value / divisor;
    return quotient * divisor > value ? quotient - 1 : quotient;
}

/* The UTC year of a day counted from 1970-01-01 (Hinnant's civil_from_days). */
static long long year_of_day(long long days) {
    const long long shifted = days + CIVIL_EPOCH_SHIFT;
    const long long era = floor_divide(shifted, CIVIL_DAYS_PER_ERA);
    const long long day_of_era = shifted - era * CIVIL_DAYS_PER_ERA;
    const long long year_of_era =
        (day_of_era - day_of_era / CIVIL_DAYS_PER_4_YEARS + day_of_era / CIVIL_DAYS_PER_CENTURY -
         day_of_era / (CIVIL_DAYS_PER_ERA - 1)) /
        CIVIL_DAYS_PER_YEAR;
    const long long day_of_year =
        day_of_era - (CIVIL_DAYS_PER_YEAR * year_of_era + year_of_era / LEAP_EVERY_YEARS -
                      year_of_era / CENTURY_YEARS);
    const long long month_index =
        (CIVIL_MONTH_FACTOR * day_of_year + CIVIL_MONTH_OFFSET) / CIVIL_MONTH_DAYS_NUMERATOR;
    return year_of_era + era * CIVIL_YEARS_PER_ERA +
           (month_index >= CIVIL_FIRST_NEXT_YEAR_MONTH ? 1 : 0);
}

/* first_release_date, Unix milliseconds (RomM scales every provider's date to them), as a year; 0
 * when absent, not a whole number, 0 (RomM's own "none") or outside the years shown. */
static int read_release_year(const cJSON *metadatum) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(metadatum, "first_release_date");
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    if (!(value > -JSON_INTEGER_LIMIT && value < JSON_INTEGER_LIMIT) ||
        value != (double)(long long)value || value == 0.0) {
        return 0;
    }
    const long long year = year_of_day(floor_divide((long long)value, MS_PER_DAY));
    return year >= RELEASE_YEAR_MIN && year <= RELEASE_YEAR_MAX ? (int)year : 0;
}

/* average_rating rounded to a whole score out of SKIFF_ROMM_RATING_SCALE; 0 when absent, 0 or out
 * of range. */
static unsigned read_rating(const cJSON *metadatum) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(metadatum, "average_rating");
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    if (!(value > 0.0 && value <= (double)SKIFF_ROMM_RATING_SCALE)) {
        return 0;
    }
    return (unsigned)(value + ROUND_HALF);
}

static void read_players(const cJSON *metadatum, char *out, size_t out_size) {
    if (!read_fact_value(cJSON_GetObjectItemCaseSensitive(metadatum, "player_count"), out,
                         out_size) ||
        strcmp(out, PLAYERS_UNKNOWN) == 0) {
        out[0] = '\0';
    }
}

void skiff_romm_read_metadata(const cJSON *root, skiff_romm_rom_metadata *out) {
    memset(out, 0, sizeof *out);
    read_description(root, out->description, sizeof out->description);
    out->region_count = read_fact_list(root, "regions", out->regions, SKIFF_ROMM_REGIONS_MAX);
    out->language_count =
        read_fact_list(root, "languages", out->languages, SKIFF_ROMM_LANGUAGES_MAX);
    const cJSON *metadatum = cJSON_GetObjectItemCaseSensitive(root, "metadatum");
    if (!cJSON_IsObject(metadatum)) {
        return;
    }
    out->release_year = read_release_year(metadatum);
    out->rating = read_rating(metadatum);
    read_players(metadatum, out->players, sizeof out->players);
    out->genre_count = read_fact_list(metadatum, "genres", out->genres, SKIFF_ROMM_GENRES_MAX);
    const int developer = read_first_fact(metadatum, "developers", out->developer);
    const int publisher = read_first_fact(metadatum, "publishers", out->publisher);
    /* Scans before RomM split companies into developers and publishers list only companies. */
    if (!developer && !publisher) {
        (void)read_first_fact(metadatum, "companies", out->developer);
    }
}
