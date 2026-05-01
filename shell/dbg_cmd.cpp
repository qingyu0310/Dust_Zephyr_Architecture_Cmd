#include <zephyr/shell/shell.h>
#include <string.h>
#include "trd_chassis.hpp"

static int cmd_dbg(const struct shell *shell, size_t argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0) {
            instance::chassis::dbg_enabled = true;
        } else if (strcmp(argv[1], "off") == 0) {
            instance::chassis::dbg_enabled = false;
        } else {
            shell_print(shell, "usage: dbg <on|off|status>");
            return -ENOEXEC;
        }
    }

    shell_print(shell, "dbg %s",
                instance::chassis::dbg_enabled ? "ON" : "OFF");
    return 0;
}

SHELL_CMD_REGISTER(dbg, NULL, "Chassis debug printk: dbg <on|off|status>", cmd_dbg);
