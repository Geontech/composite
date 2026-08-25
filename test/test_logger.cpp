/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// 0.5.1 regression: the component id used to be interpolated INTO the spdlog pattern string, so
// an id containing pattern flags ("%v", "%^", ...) was compiled as part of the pattern —
// duplicated message text, broken prefixes, whatever the flags happened to mean. Ids arrive from
// config files and POST /app/components, so this is externally reachable, not just a foot-gun.
// The pattern now renders the id via %n (the logger's own name), which is emitted verbatim.
//
// The check captures real stdout output through a pipe: the id must appear VERBATIM in the
// prefix, and the message must appear exactly once (the pre-fix pattern printed it wherever the
// id's "%v" landed, in addition to the trailing %v).

#include <composite/core/logger.hpp>

#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

auto capture_stdout_line(composite::logger& lg, const char* msg) -> std::string {
    std::fflush(stdout);
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        std::perror("pipe");
        std::abort();
    }
    const int saved = ::dup(STDOUT_FILENO);
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[1]);

    lg.info(msg);
    lg.flush();
    std::fflush(stdout);

    ::dup2(saved, STDOUT_FILENO); // closes the pipe's write end -> reader sees EOF
    ::close(saved);

    std::string out;
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf, sizeof buf)) > 0) {
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    return out;
}

auto count_occurrences(const std::string& haystack, const std::string& needle) -> int {
    int count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

int main() {
    // Every class of spdlog pattern flag: message (%v), color range (%^ %$), name (%n).
    const std::string id = "ev%v%^il%$%ncomp";
    composite::logger lg{id};

    if (lg.name() != id) {
        std::fprintf(stderr, "FAIL: logger name mangled: '%s'\n", lg.name().c_str());
        return 1;
    }

    const std::string marker = "MARKER_5f1c0de";
    const auto out = capture_stdout_line(lg, marker.c_str());

    if (out.find("[" + id + "]") == std::string::npos) {
        std::fprintf(stderr, "FAIL: id not rendered verbatim (pattern-compiled?). Output: %s\n", out.c_str());
        return 1;
    }
    const int hits = count_occurrences(out, marker);
    if (hits != 1) {
        std::fprintf(stderr, "FAIL: message rendered %d times (expected 1). Output: %s\n", hits, out.c_str());
        return 1;
    }
    std::printf("LOGGER OK: pattern-flag id rendered verbatim, message once\n");
    return 0;
}
