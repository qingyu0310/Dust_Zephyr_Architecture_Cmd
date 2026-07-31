/**
 * @file buzzer.hpp
 * @author qingyu
 * @brief 蜂鸣器控制 — 基于 PWM 发声
 * @version 0.1
 * @date 2026-07-25
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_CMD_BUZZER

#include <cstdint>
#include <type_traits>
#include <zephyr/drivers/pwm.h>
#include "pwm.hpp"

namespace buzzer {

/**
 * @brief 蜂鸣器控制类
 *
 * 封装 PWM 输出，提供简单的发声接口。
 */
class Buzzer final
{
public:
    Buzzer() = default;

    bool Init(const struct pwm_dt_spec &spec, uint32_t freq_hz = 1000, float volume = 0.5f);
    bool On();
    bool Off();
    bool SetFreq(uint32_t freq_hz);
    bool SetVolume(float vol);
    void Beep(uint32_t duration_ms);
    void Short();
    void Long();
    template<typename F>
    void Err(F&& pattern)
    {
        while (true) { On(); if constexpr (std::is_invocable_v<F>) { pattern(); } }
    }

private:
    Pwm      pwm_ {};
    uint32_t freq_hz_ = 2000;
    float    volume_  = 0.5f;
};

inline Buzzer& Instance()
{
    static Buzzer buzzer;
    return buzzer;
}

} // namespace buzzer

#define EXEC_BUZZER_SHORT()    buzzer::Instance().Short()
#define EXEC_BUZZER_LONG()     buzzer::Instance().Long()
#define EXEC_BUZZER_ERR(...)   buzzer::Instance().Err(__VA_ARGS__)
#else

#define EXEC_BUZZER_SHORT()
#define EXEC_BUZZER_LONG()
#define EXEC_BUZZER_ERR(...)    while(1) {}

#endif
