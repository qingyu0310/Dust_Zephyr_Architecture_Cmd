/**
 * @file buzzer.cpp
 * @author qingyu
 * @brief 蜂鸣器控制 — PWM 发声
 * @version 0.1
 * @date 2026-07-25
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "buzzer.hpp"
#include "Init_entry.hpp"
#include <zephyr/kernel.h>

#pragma message "Compiling Cmd/Buzzer"

namespace buzzer {

/**
 * @brief 绑定 PWM 实例，设置默认频率和音量
 *
 * 初始化时按默认频率计算周期，音量初始化为 0（静音）。
 * 调用 On() 后才发声。
 *
 * @param pwm     PWM 设备引用
 * @param freq_hz 默认频率
 * @param volume  默认音量
 * @return true
 */
bool Buzzer::Init(const struct pwm_dt_spec &spec, uint32_t freq_hz, float volume)
{
    if (!pwm_.init(spec)) return false;

    freq_hz_ = freq_hz;
    volume_  = volume;

    if (volume_ > 1.0f) volume_ = 1.0f;
    if (volume_ < 0.0f) volume_ = 0.0f;

    return Off();
}

/**
 * @brief 按当前频率和音量持续发声
 *
 * 周期 = 1e9 / freq_hz（纳秒），脉冲 = 周期 × volume。
 */
bool Buzzer::On()
{
    if (freq_hz_ == 0) return false;

    const uint32_t period_ns = NSEC_PER_SEC / freq_hz_;
    const uint32_t pulse_ns  = static_cast<uint32_t>(period_ns * volume_);

    return pwm_.SetPeriodAndPulse(period_ns, pulse_ns);
}

/**
 * @brief 停止发声
 */
bool Buzzer::Off()
{
    return pwm_.Stop();
}

/**
 * @brief 设置频率
 *
 * 正在发声时立即生效（下次 On 调用使用新频率）。
 *
 * @param freq_hz 频率 Hz
 */
bool Buzzer::SetFreq(uint32_t freq_hz)
{
    if (freq_hz == 0) return false;
    freq_hz_ = freq_hz;
    return true;
}

/**
 * @brief 设置音量
 *
 * @param vol 0.0～1.0
 */
bool Buzzer::SetVolume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    volume_ = vol;
    return true;
}

/**
 * @brief 短鸣一段时间后停止（阻塞）
 * @param duration_ms 持续毫秒
 */
void Buzzer::Beep(uint32_t duration_ms)
{
    On();
    k_msleep(duration_ms);
    Off();
    k_msleep(duration_ms / 2);
    On();
    k_msleep(duration_ms);
    Off();
}

/**
 * @brief 短鸣两声（阻塞）
 */
void Buzzer::Short()
{
    Beep(100);
}

/**
 * @brief 长鸣两声（阻塞）
 */
void Buzzer::Long()
{
    Beep(1000);
}

static bool buzzer_init()
{
    return buzzer::Instance().Init(PWM_DT_SPEC_GET(DT_NODELABEL(buzzer_pwm)));
}
REGISTER_INIT(buzzer_init, PreInit, Mid, "buzzer");


} // namespace buzzer


