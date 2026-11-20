/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdio.h>

static int check_failures, check_count;

static void check(const char *name, int ok, const char *detail)
{
    check_count++;
    if (!ok) {
        check_failures++;
    }
    printf("%s  %s  (%s)\n", ok ? "PASS" : "FAIL", name, detail);
}

static int check_summary(void)
{
    printf("%d of %d checks failed\n", check_failures, check_count);
    return check_failures ? 1 : 0;
}
