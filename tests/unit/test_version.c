#include <stdio.h>

#include "skiff/version.h"

#include "unity.h"
#include "version_config.h"

void setUp(void) {}

void tearDown(void) {}

static void test_string_matches_project_version(void) {
    TEST_PRINTF("version %s", skiff_version_string());
    TEST_ASSERT_EQUAL_STRING(SKIFF_VERSION_STRING, skiff_version_string());
}

static void test_components_compose_the_string(void) {
    char composed[32];
    snprintf(composed, sizeof composed, "%d.%d.%d", skiff_version_major(), skiff_version_minor(),
             skiff_version_patch());
    TEST_ASSERT_EQUAL_STRING(skiff_version_string(), composed);
}

static void test_components_are_non_negative(void) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, skiff_version_major());
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, skiff_version_minor());
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, skiff_version_patch());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_string_matches_project_version);
    RUN_TEST(test_components_compose_the_string);
    RUN_TEST(test_components_are_non_negative);
    return UNITY_END();
}
