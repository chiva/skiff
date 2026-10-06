/*
 * Where Skiff's files go (skiff/storage_paths.h): logical roots follow the EBOOT's device, a
 * logical path never resolves outside its root, and a name from RomM comes out safe for FAT and the
 * PSP whatever it held: path separators, device names, broken UTF-8, or a length past FAT's.
 */
#include <stdio.h>
#include <string.h>

#include "skiff/storage_paths.h"

#include "unity.h"

#define MEMORY_STICK_EBOOT "ms0:/PSP/GAME/Skiff/EBOOT.PBP"
#define PSP_GO_EBOOT "ef0:/PSP/GAME/Skiff/EBOOT.PBP"
#define OUT_MAX 512

static skiff_storage_roots roots;
static char out[OUT_MAX];

void setUp(void) {
    memset(&roots, 0, sizeof roots);
    memset(out, 0x5A, sizeof out);
}

void tearDown(void) {}

/* ---- Roots ---- */

static void test_roots_follow_the_memory_stick(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program(MEMORY_STICK_EBOOT, &roots));
    TEST_PRINTF("app %s, games %s, saves %s", roots.app, roots.games, roots.saves);
    TEST_ASSERT_EQUAL_STRING("ms0:/PSP/GAME/Skiff", roots.app);
    TEST_ASSERT_EQUAL_STRING("ms0:/ISO", roots.games);
    TEST_ASSERT_EQUAL_STRING("ms0:/PSP/SAVEDATA", roots.saves);
}

static void test_roots_follow_a_psp_go_internal_storage(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program(PSP_GO_EBOOT, &roots));
    TEST_ASSERT_EQUAL_STRING("ef0:/ISO", roots.games);
    TEST_ASSERT_EQUAL_STRING("ef0:/PSP/GAME/Skiff", roots.app);
}

static void test_an_eboot_at_the_top_of_the_device_has_the_device_as_app(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program("ms0:/EBOOT.PBP", &roots));
    TEST_ASSERT_EQUAL_STRING("ms0:", roots.app);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, "app:/config.ini", out, OUT_MAX));
    TEST_ASSERT_EQUAL_STRING("ms0:/config.ini", out);
}

static void test_program_paths_without_a_device_or_folder_are_refused(void) {
    const char *refused[] = {"EBOOT.PBP", "/PSP/GAME/Skiff/EBOOT.PBP", ":/EBOOT.PBP",
                             "ms0:EBOOT.PBP", "PSP/ms0:/EBOOT.PBP"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        TEST_PRINTF("argv[0] '%s'", refused[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_storage_roots_from_program(refused[i], &roots));
        TEST_ASSERT_EQUAL_STRING("", roots.games);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_roots_from_program(NULL, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_roots_from_program(MEMORY_STICK_EBOOT, NULL));
}

static void test_roots_on_a_host_directory(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_roots_init("/tmp/stick", "/tmp/stick/app", &roots));
    TEST_ASSERT_EQUAL_STRING("/tmp/stick/ISO", roots.games);
    TEST_ASSERT_EQUAL_STRING("/tmp/stick/PSP/SAVEDATA", roots.saves);
    TEST_ASSERT_EQUAL_STRING("/tmp/stick/app", roots.app);
    TEST_PRINTF("a trailing '/' would double the separator in every path");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_roots_init("/tmp/stick/", "/tmp/stick/app", &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_roots_init("/tmp/stick", "/tmp/stick/app/", &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_roots_init("", "a", &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_roots_init("a", "", &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_roots_init(NULL, "a", &roots));
}

static void test_roots_that_do_not_fit_are_refused(void) {
    char device[SKIFF_STORAGE_PATH_MAX];
    memset(device, 'd', sizeof device);
    device[SKIFF_STORAGE_PATH_MAX - strlen(SKIFF_STORAGE_SAVES_FOLDER)] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_storage_roots_init(device, "a", &roots));
    TEST_ASSERT_EQUAL_STRING("", roots.app);
    device[SKIFF_STORAGE_PATH_MAX - 1 - strlen(SKIFF_STORAGE_SAVES_FOLDER)] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(device, "a", &roots));

    TEST_PRINTF("an EBOOT folder of 260 bytes is longer than app: can hold");
    char program[SKIFF_STORAGE_PATH_MAX + 32];
    snprintf(program, sizeof program, "ms0:/%0260d/EBOOT.PBP", 0);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_storage_roots_from_program(program, &roots));
}

/* ---- Resolving ---- */

static void assert_resolves(const char *logical, const char *expected) {
    const skiff_err err = skiff_storage_resolve(&roots, logical, out, OUT_MAX);
    TEST_PRINTF("%s -> %s (%s)", logical, out, skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_logical_paths_resolve_on_their_root(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program(MEMORY_STICK_EBOOT, &roots));
    assert_resolves("games:/Skiff Test.iso", "ms0:/ISO/Skiff Test.iso");
    assert_resolves("games:", "ms0:/ISO");
    assert_resolves("saves:/ULUS10064DATA00/DATA.BIN",
                    "ms0:/PSP/SAVEDATA/ULUS10064DATA00/DATA.BIN");
    assert_resolves("app:/skiff.log", "ms0:/PSP/GAME/Skiff/skiff.log");
    assert_resolves("games:/Pok\xC3\xA9mon.iso", "ms0:/ISO/Pok\xC3\xA9mon.iso");
    assert_resolves("games:/..hidden..iso", "ms0:/ISO/..hidden..iso");
}

static void test_nothing_resolves_outside_its_root(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program(MEMORY_STICK_EBOOT, &roots));
    const char *refused[] = {"games:/../PSP/GAME/x",
                             "games:/a/../../b",
                             "games:/.",
                             "games:/..",
                             "games:/.. ./x.iso",
                             "games:/.. /x.iso",
                             "games:/. /x.iso",
                             "games:/a/.../x.iso",
                             "games:/x.iso.",
                             "games:/x.iso ",
                             "games:..",
                             "games:x.iso",
                             "games://x.iso",
                             "games:/x.iso/",
                             "games:/",
                             "games:/a\\..\\b",
                             "games:/ms0:/x",
                             "games:/a\nb",
                             "isos:/x.iso",
                             "ms0:/ISO/x.iso",
                             "/ISO/x.iso",
                             ""};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        const skiff_err err = skiff_storage_resolve(&roots, refused[i], out, OUT_MAX);
        TEST_PRINTF("'%s' -> %s", refused[i], skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, err);
        TEST_ASSERT_EQUAL_STRING("", out);
    }
}

static void test_resolving_needs_roots_and_room(void) {
    TEST_PRINTF("roots never set: every root is empty");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_resolve(&roots, "games:", out, OUT_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program(MEMORY_STICK_EBOOT, &roots));
    const size_t exact = strlen("ms0:/ISO/a.iso") + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, "games:/a.iso", out, exact));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_storage_resolve(&roots, "games:/a.iso", out, exact - 1));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_resolve(NULL, "games:", out, OUT_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_resolve(&roots, NULL, out, OUT_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_resolve(&roots, "games:", NULL, 8));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_resolve(&roots, "games:", out, 0));
}

/* ---- Sibling paths ---- */

static void test_sibling_path_uses_the_given_file_name(void) {
    const skiff_err err = skiff_storage_sibling_path("ms0:/PSP/GAME/SkiffKIRKProbe/EBOOT.PBP",
                                                     "kirk-log.txt", out, OUT_MAX);
    TEST_PRINTF("sibling -> %s (%s)", out, skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_ASSERT_EQUAL_STRING("ms0:/PSP/GAME/SkiffKIRKProbe/kirk-log.txt", out);
}

static void test_sibling_path_rejects_a_null_file_name_or_no_folder(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_sibling_path("ms0:/EBOOT.PBP", NULL, out, OUT_MAX));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_sibling_path("EBOOT.PBP", "a.log", out, OUT_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_sibling_path(NULL, "a.log", out, OUT_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_sibling_path("ms0:/EBOOT.PBP", "a.log", NULL, OUT_MAX));
}

/* "ms0:/" + "a.log" + NUL is exactly 11 bytes: the limit follows the file name. */
static void test_sibling_path_sizes_the_buffer_for_its_own_name(void) {
    const size_t exact = strlen("ms0:/a.log") + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_sibling_path("ms0:/EBOOT.PBP", "a.log", out, exact));
    TEST_ASSERT_EQUAL_STRING("ms0:/a.log", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_storage_sibling_path("ms0:/EBOOT.PBP", "a.log", out, exact - 1));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- Safe names ---- */

typedef struct name_case {
    const char *why;
    const char *input;
    const char *expected;
} name_case;

static void test_names_from_romm_come_out_safe(void) {
    static const name_case CASES[] = {
        {"an ordinary name is kept", "Skiff Test (USA).iso", "Skiff Test (USA).iso"},
        {"accents are kept", "Pok\xC3\xA9mon Edici\xC3\xB3n.cso",
         "Pok\xC3\xA9mon Edici\xC3\xB3n.cso"},
        {"CJK and emoji are kept", "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 \xF0\x9F\x8E\xAE.iso",
         "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 \xF0\x9F\x8E\xAE.iso"},
        {"path separators cannot add a folder", "../../PSP/GAME/evil/EBOOT.PBP",
         ".._.._PSP_GAME_evil_EBOOT.PBP"},
        {"backslashes and a device", "ms0:\\ISO\\x.iso", "ms0__ISO_x.iso"},
        {"every FAT-forbidden character", "a*b?c\"d<e>f|g.iso", "a_b_c_d_e_f_g.iso"},
        {"control characters",
         "a\x01"
         "b\x1F"
         "c\x7F.iso",
         "a_b_c_.iso"},
        {"tab becomes '_', not a blank to trim", "\tx.iso", "_x.iso"},
        {"blanks at the start and blanks and dots at the end go", "   Game .iso . . ", "Game .iso"},
        {"a lone stray UTF-8 byte",
         "a\x80"
         "b.iso",
         "a_b.iso"},
        {"an overlong encoding of '/'",
         "a\xC0\xAF"
         "b.iso",
         "a__b.iso"},
        {"a UTF-16 surrogate",
         "a\xED\xA0\x80"
         "b",
         "a___b"},
        {"past U+10FFFF",
         "a\xF4\x90\x80\x80"
         "b",
         "a____b"},
        {"a character cut off by the end", "ab\xE3\x83", "ab__"},
        {"DOS device names get a '_'", "CON", "_CON"},
        {"in any case and with an extension", "nul.iso", "_nul.iso"},
        {"with blanks before the extension", "Com1 .txt", "_Com1 .txt"},
        {"LPT9", "lpt9", "_lpt9"},
        {"not names that only start like one", "CONSOLE.iso", "CONSOLE.iso"},
        {"nor COM0 or COM10", "COM10", "COM10"},
        {"a leading dot is a name, not an extension", ".iso", ".iso"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const skiff_err err = skiff_storage_safe_name(CASES[i].input, out, SKIFF_STORAGE_NAME_MAX);
        TEST_PRINTF("%s: '%s' -> '%s' (%s)", CASES[i].why, CASES[i].input, out,
                    skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, err, CASES[i].why);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(CASES[i].expected, out, CASES[i].why);
    }
}

static void test_names_that_clean_to_nothing_are_refused(void) {
    const char *refused[] = {"", ".", "..", "   ", " . . .", "..."};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        const skiff_err err = skiff_storage_safe_name(refused[i], out, SKIFF_STORAGE_NAME_MAX);
        TEST_PRINTF("'%s' -> %s", refused[i], skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, err);
        TEST_ASSERT_EQUAL_STRING("", out);
    }
}

/* A name of n copies of unit (a character of unit_bytes bytes) followed by extension. */
static void long_name(char *text, size_t text_size, const char *unit, size_t n,
                      const char *extension) {
    text[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        strncat(text, unit, text_size - strlen(text) - 1);
    }
    strncat(text, extension, text_size - strlen(text) - 1);
}

static int valid_utf8(const char *text) {
    const unsigned char *bytes = (const unsigned char *)text;
    while (*bytes != '\0') {
        size_t length = 1;
        if (*bytes >= 0xF0) {
            length = 4;
        } else if (*bytes >= 0xE0) {
            length = 3;
        } else if (*bytes >= 0xC0) {
            length = 2;
        } else if (*bytes >= 0x80) {
            return 0;
        }
        for (size_t i = 1; i < length; i++) {
            if ((bytes[i] & 0xC0) != 0x80) {
                return 0;
            }
        }
        bytes += length;
    }
    return 1;
}

static void test_long_names_keep_their_extension_and_whole_characters(void) {
    char text[SKIFF_STORAGE_NAME_INPUT_MAX + 1];
    TEST_PRINTF("200 ASCII bytes and .iso: cut to 127 bytes, extension kept");
    long_name(text, sizeof text, "a", 200, ".iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_size_t(SKIFF_STORAGE_NAME_MAX - 1, strlen(out));
    TEST_ASSERT_EQUAL_STRING(".iso", out + strlen(out) - 4);

    TEST_PRINTF("four-byte characters: 123 bytes of stem would split one, so the cut backs up");
    long_name(text, sizeof text, "\xF0\x9F\x8E\xAE", 40, ".cso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_PRINTF("-> %zu bytes", strlen(out));
    TEST_ASSERT_EQUAL_size_t(120 + 4, strlen(out));
    TEST_ASSERT_TRUE(valid_utf8(out));
    TEST_ASSERT_EQUAL_STRING(".cso", out + strlen(out) - 4);

    TEST_PRINTF("an ending too long to be an extension is cut like the rest");
    long_name(text, sizeof text, "b", 150, ".this-is-not-an-extension");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_size_t(SKIFF_STORAGE_NAME_MAX - 1, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('b', out[strlen(out) - 1]);

    TEST_PRINTF("dots before the cut are trimmed, not left before the extension");
    long_name(text, sizeof text, "c", 122, "........iso.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_STRING("c.iso", out + strlen(out) - 5);

    TEST_PRINTF("a stem made only of dots becomes '_'");
    long_name(text, sizeof text, ".", 200, "x.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_STRING("_.iso", out);

    TEST_PRINTF("without an extension");
    long_name(text, sizeof text, "d", 300, "");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_size_t(SKIFF_STORAGE_NAME_MAX - 1, strlen(out));
}

static void test_a_name_shortened_into_a_dos_device_still_gets_its_mark(void) {
    char text[SKIFF_STORAGE_NAME_INPUT_MAX + 1];
    TEST_PRINTF("CON, 200 blanks, A.iso: the cut leaves CON before the extension");
    snprintf(text, sizeof text, "CON%200sA.iso", "");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_STRING("_CON.iso", out);
    snprintf(text, sizeof text, "nul%300s", "x");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_STRING("_nul", out);
    TEST_PRINTF("a device name already at the length limit is shortened again after its '_'");
    snprintf(text, sizeof text, "COM1%119s.txt", "");
    TEST_ASSERT_EQUAL_size_t(SKIFF_STORAGE_NAME_MAX - 1, strlen(text));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_PRINTF("-> '%s': the second cut drops the blanks before the extension", out);
    TEST_ASSERT_EQUAL_STRING("_COM1.txt", out);
}

static void test_input_limits_and_arguments(void) {
    char text[SKIFF_STORAGE_NAME_INPUT_MAX + 2];
    memset(text, 'x', sizeof text);
    text[SKIFF_STORAGE_NAME_INPUT_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    text[SKIFF_STORAGE_NAME_INPUT_MAX] = 'x';
    text[SKIFF_STORAGE_NAME_INPUT_MAX + 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_safe_name(text, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_STRING("", out);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name("abc", out, 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL, skiff_storage_safe_name("abc", out, 3));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_safe_name(NULL, out, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_safe_name("a", NULL, SKIFF_STORAGE_NAME_MAX));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_safe_name("a", out, 0));
}

static void test_a_safe_name_resolves_under_its_root(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_from_program(MEMORY_STICK_EBOOT, &roots));
    char name[SKIFF_STORAGE_NAME_MAX];
    char logical[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_safe_name("../../PSP/GAME/x/EBOOT.PBP", name, sizeof name));
    snprintf(logical, sizeof logical, "games:/%s", name);
    assert_resolves(logical, "ms0:/ISO/.._.._PSP_GAME_x_EBOOT.PBP");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_roots_follow_the_memory_stick);
    RUN_TEST(test_roots_follow_a_psp_go_internal_storage);
    RUN_TEST(test_an_eboot_at_the_top_of_the_device_has_the_device_as_app);
    RUN_TEST(test_program_paths_without_a_device_or_folder_are_refused);
    RUN_TEST(test_roots_on_a_host_directory);
    RUN_TEST(test_roots_that_do_not_fit_are_refused);
    RUN_TEST(test_logical_paths_resolve_on_their_root);
    RUN_TEST(test_nothing_resolves_outside_its_root);
    RUN_TEST(test_resolving_needs_roots_and_room);
    RUN_TEST(test_sibling_path_uses_the_given_file_name);
    RUN_TEST(test_sibling_path_rejects_a_null_file_name_or_no_folder);
    RUN_TEST(test_sibling_path_sizes_the_buffer_for_its_own_name);
    RUN_TEST(test_names_from_romm_come_out_safe);
    RUN_TEST(test_names_that_clean_to_nothing_are_refused);
    RUN_TEST(test_long_names_keep_their_extension_and_whole_characters);
    RUN_TEST(test_a_name_shortened_into_a_dos_device_still_gets_its_mark);
    RUN_TEST(test_input_limits_and_arguments);
    RUN_TEST(test_a_safe_name_resolves_under_its_root);
    return UNITY_END();
}
