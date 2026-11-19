#pragma once

#include <cstdlib>
#include <unistd.h>

// ANSI terminal colors. Colors are emitted only when the target stream is a
// TTY and the NO_COLOR environment variable is unset (https://no-color.org),
// so piped output, log files and test harnesses stay plain text.
namespace term
{
    inline bool colorEnabled(int fd)
    {
        static const bool no_color = std::getenv("NO_COLOR") != nullptr;
        return !no_color && isatty(fd);
    }

    inline bool outColor()
    {
        static const bool on = colorEnabled(STDOUT_FILENO);
        return on;
    }

    inline bool errColor()
    {
        static const bool on = colorEnabled(STDERR_FILENO);
        return on;
    }

    // Returns the escape code when stdout supports color, else "".
    inline const char *c(const char *code) { return outColor() ? code : ""; }
    // Same, for text written to stderr.
    inline const char *e(const char *code) { return errColor() ? code : ""; }

    constexpr const char *RESET = "\x1b[0m";
    constexpr const char *BOLD = "\x1b[1m";
    constexpr const char *DIM = "\x1b[2m";
    constexpr const char *ITALIC = "\x1b[3m";

    constexpr const char *RED = "\x1b[31m";
    constexpr const char *GREEN = "\x1b[32m";
    constexpr const char *YELLOW = "\x1b[33m";
    constexpr const char *BLUE = "\x1b[34m";
    constexpr const char *MAGENTA = "\x1b[35m";
    constexpr const char *CYAN = "\x1b[36m";

    constexpr const char *BRED = "\x1b[1;31m";
    constexpr const char *BGREEN = "\x1b[1;32m";
    constexpr const char *BYELLOW = "\x1b[1;33m";
    constexpr const char *BBLUE = "\x1b[1;34m";
    constexpr const char *BMAGENTA = "\x1b[1;35m";
    constexpr const char *BCYAN = "\x1b[1;36m";
}
