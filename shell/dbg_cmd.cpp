#include <zephyr/shell/shell.h>
#include <string.h>
#include "debug.hpp"

static int cmd_dbg(const struct shell *shell, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(shell, "chassis debug: turn=%s run=%s",
                    debug::chassis::pwr_turn ? "ON" : "OFF",
                    debug::chassis::pwr_run  ? "ON" : "OFF");
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        debug::chassis::pwr_turn = false;
        debug::chassis::pwr_run  = false;
    } else if (strcmp(argv[1], "turn") == 0) {
        debug::chassis::pwr_turn = true;
        debug::chassis::pwr_run  = false;
    } else if (strcmp(argv[1], "run") == 0) {
        debug::chassis::pwr_turn = false;
        debug::chassis::pwr_run  = true;
    } else {
        shell_print(shell, "usage: dbg <off|turn|run>");
        return -ENOEXEC;
    }

    shell_print(shell, "chassis debug: %s", argv[1]);
    return 0;
}

SHELL_CMD_REGISTER(dbg, NULL, "Chassis debug: dbg <off|turn|run>", cmd_dbg);
