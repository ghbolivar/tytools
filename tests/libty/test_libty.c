/*
 * ty, a collection of GUI and command-line tools to manage Teensy devices
 *
 * Distributed under the MIT license (see LICENSE.txt or http://opensource.org/licenses/MIT)
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 */

#include <stdarg.h>
#include "test_libty.h"

static char current_file[1024];
static char current_fn[256];

static unsigned int current_fails, current_total;
static unsigned int cases_failures, cases_total;

static void conclude_current_test()
{
    if (!current_total)
        return;

    if (current_fails) {
        printf("    [%u of %u assertions failed]\n", current_fails, current_total);
        cases_failures++;
    }
    cases_total++;

    current_fails = 0;
    current_total = 0;
}

void report_test(bool pred, const char *file, unsigned int line, const char *fn,
                 const char *pred_fmt, ...)
{
    if (strncmp(fn, current_fn, sizeof(current_fn)) != 0) {
        conclude_current_test();

        if (strncmp(file, current_file, sizeof(current_file)) != 0) {
            printf("Tests from '%s'\n", file);

            strncpy(current_file, file, sizeof(current_file));
            current_file[sizeof(current_file) - 1] = 0;
        }

        printf("  Test case '%s'\n", fn);
        strncpy(current_fn, fn, sizeof(current_fn));
        current_fn[sizeof(current_fn) - 1] = 0;
    }

    if (!pred) {
        va_list va;

        printf("    - Failed assertion ");
        va_start(va, pred_fmt);
        vprintf(pred_fmt, va);
        va_end(va);
        printf("\n      %s:%u in '%s'\n", file, line, fn);

        current_fails++;
    }
    current_total++;
}

int main(void)
{
    conclude_current_test();
    if (cases_failures) {
        printf("\nFailed %u of %u test case(s)\n", cases_failures, cases_total);
        return 1;
    } else {
        printf("\nSuccessfully passed %u test case(s)\n", cases_total);
        return 0;
    }
}
