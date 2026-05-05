#include <zephyr/shell/shell.h>
#include <string.h>
#include <stdlib.h>
#include "pid.hpp"
#include "trd_chassis.hpp"

static auto &pid = instance::chassis::wheel_pid[0].steer_torque;

/* 读当前配置改单参数后 shadow-set */
static void set_one(const struct shell *shell, const char *field, float val)
{
    auto cfg = pid.GetConfig();
    bool ok = true;

    if      (strcmp(field, "kp")      == 0) cfg.kp      = val;
    else if (strcmp(field, "ki")      == 0) cfg.ki      = val;
    else if (strcmp(field, "kd")      == 0) cfg.kd      = val;
    else if (strcmp(field, "kf")      == 0) cfg.kf      = val;
    else if (strcmp(field, "out_max") == 0) cfg.outMax  = val;
    else if (strcmp(field, "iout_max")== 0) cfg.iOutMax = val;
    else ok = false;

    if (!ok) {
        shell_print(shell, "unknown field: %s", field);
        shell_print(shell, "usage: pid set <kp|ki|kd|kf|out_max|iout_max> <value>");
        return;
    }

    pid.SetShadow(cfg);
    shell_print(shell, "pid.%s = %.4f (shadow)", field, (double)val);
}

static int cmd_pid_set(const struct shell *shell, size_t argc, char **argv)
{
    if (argc == 3) {
        set_one(shell, argv[1], strtof(argv[2], nullptr));
        return 0;
    }
    if (argc == 4) {
        auto cfg = pid.GetConfig();
        cfg.kp = strtof(argv[1], nullptr);
        cfg.ki = strtof(argv[2], nullptr);
        cfg.kd = strtof(argv[3], nullptr);

        pid.SetShadow(cfg);
        shell_print(shell, "pid: kp=%.4f ki=%.4f kd=%.4f (shadow)",
                    (double)cfg.kp, (double)cfg.ki, (double)cfg.kd);
        return 0;
    }

    shell_print(shell, "usage:\n"
                "  pid set <field> <value>  — set one param\n"
                "  pid set <kp> <ki> <kd>   — set all");
    return -ENOEXEC;
}

static int cmd_pid_target(const struct shell *shell, size_t argc, char **argv)
{
    if (argc == 2) {
        pid.SetTarget(strtof(argv[1], nullptr));
    }
    shell_print(shell, "target = %.4f", (double)pid.GetTarget());
    return 0;
}

static int cmd_pid_show(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "kp=%.4f  ki=%.4f  kd=%.4f  kf=%.4f",
                (double)pid.GetKp(), (double)pid.GetKi(),
                (double)pid.GetKd(), (double)pid.GetKf());
    shell_print(shell, "outMax=%.1f  iOutMax=%.1f  dt=%.4f",
                (double)pid.GetOutMax(), (double)pid.GetIOutMax(),
                (double)pid.GetDt());
    shell_print(shell, "target=%.4f  out=%.4f  integral=%.4f  error=%.4f",
                (double)pid.GetTarget(), (double)pid.GetOut(),
                (double)pid.GetIntegralError(), (double)pid.GetError());
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pid_cmds,
    SHELL_CMD(set,    NULL,
              "Set PID:  pid set <kp|ki|kd|kf|out_max|iout_max> <val>\n"
              "         or pid set <kp> <ki> <kd> <outMax>",
              cmd_pid_set),
    SHELL_CMD(target, NULL, "Set PID target: pid target <value>", cmd_pid_target),
    SHELL_CMD(show,   NULL, "Show PID state", cmd_pid_show),
);

SHELL_CMD_REGISTER(pid, &pid_cmds, "PID runtime tuning", NULL);
