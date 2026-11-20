/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdio.h>

static int check_failures;
static int check_count;

static void check(const char *name, bool ok, const char *detail)
{
    check_count++;
    if (!ok) {
        check_failures++;
    }
    printf("%s  %s  (%s)\n", ok ? "PASS" : "FAIL", name, detail);
    fflush(stdout);
}

static int check_summary(void)
{
    printf("%d of %d checks failed\n", check_failures, check_count);
    return check_failures > 0 || check_count == 0 ? 1 : 0;
}
