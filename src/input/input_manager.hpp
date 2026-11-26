#pragma once

#include "core/config.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fzgx {

enum GcButtonBits : uint16_t {
    kGcBtnLeft  = 0x0001,
    kGcBtnRight = 0x0002,
    kGcBtnDown  = 0x0004,
    kGcBtnUp    = 0x0008,
    kGcBtnZ     = 0x0010,
    kGcBtnR     = 0x0020,
    kGcBtnL     = 0x0040,
    kGcBtnA     = 0x0100,
    kGcBtnB     = 0x0200,
    kGcBtnX     = 0x0400,
    kGcBtnY     = 0x0800,
    kGcBtnStart = 0x1000,
};

struct PadState {
    bool connected{false};
    std::string device_label{"Disconnected"};
    uint16_t buttons{0};
    int8_t stick_x{0};     // -128..127
    int8_t stick_y{0};     // -128..127 (up is positive in GC PADStatus)
    int8_t substick_x{0};  // -128..127
    int8_t substick_y{0};  // -128..127
    uint8_t trigger_l{0};  // 0..255
    uint8_t trigger_r{0};  // 0..255
    float norm_stick_x{0.0f};
    float norm_stick_y{0.0f};
    float norm_substick_x{0.0f};
    float norm_substick_y{0.0f};
    float norm_trigger_l{0.0f};
    float norm_trigger_r{0.0f};
};

// Pure helper for stick radial deadzone/outer gate and dual-stage L/R trigger calibration.
void shape_stick(float raw_x,
                 float raw_y,
                 float deadzone,
                 float outer_gate,
                 float& out_norm_x,
                 float& out_norm_y,
                 int8_t& out_gc_x,
                 int8_t& out_gc_y);

void shape_trigger(float raw_val,
                   float analog_deadzone,
                   float digital_threshold,
                   float& out_norm,
                   uint8_t& out_gc_analog,
                   bool& out_digital_click);

const char* gc_button_name(size_t index);
uint16_t gc_button_bit(size_t index);

class IInputProvider {
public:
    virtual ~IInputProvider() = default;
    virtual const char* name() const = 0;
    virtual void poll(const AppConfig& config, std::array<PadState, 4>& ports) = 0;
    virtual void set_rumble(int port, float low_freq, float high_freq, uint32_t duration_ms) = 0;
};

class SdlInputProvider final : public IInputProvider {
public:
    SdlInputProvider() = default;
    ~SdlInputProvider() override;

    const char* name() const override { return "SDL3 Gamepad + Keyboard"; }
    void handle_sdl_event(const SDL_Event& ev);
    void refresh_devices();
    void poll(const AppConfig& config, std::array<PadState, 4>& ports) override;
    void set_rumble(int port, float low_freq, float high_freq, uint32_t duration_ms) override;

    std::vector<std::string> connected_device_names() const;
    bool overlay_chord_triggered();

private:
    struct OpenPad {
        SDL_JoystickID id{0};
        SDL_Gamepad* handle{nullptr};
        std::string name;
    };
    std::vector<OpenPad> open_pads_;
    bool prev_chord_held_{false};
};

// Future WUP-028 GameCube USB Adapter provider (libusb) behind IInputProvider.
class GcUsbAdapterProvider final : public IInputProvider {
public:
    const char* name() const override { return "GameCube USB Adapter (WUP-028 Stub)"; }
    void poll(const AppConfig&, std::array<PadState, 4>&) override {}
    void set_rumble(int, float, float, uint32_t) override {}
    bool is_adapter_detected() const { return false; }
};

class InputManager {
public:
    InputManager();

    void handle_sdl_event(const SDL_Event& ev);
    void refresh_devices();
    void poll(const AppConfig& config);
    void test_rumble(int port);

    const PadState& port_state(int port) const;
    const std::array<PadState, 4>& all_ports() const { return ports_; }
    std::vector<std::string> connected_device_names() const;
    bool poll_overlay_gamepad_toggle();

private:
    std::unique_ptr<SdlInputProvider> sdl_provider_;
    std::unique_ptr<GcUsbAdapterProvider> gc_adapter_provider_;
    std::array<PadState, 4> ports_{};
};

} // namespace fzgx
