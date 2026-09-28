// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <time.h>

static uint64_t mailbox_now() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now)) throw std::runtime_error("clock_gettime failed");
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
}
static void mailbox_require(bool good, const char *message) {
    if (!good) throw std::runtime_error(message);
}
