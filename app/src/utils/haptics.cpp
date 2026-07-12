#include "utils/haptics.hpp"

#include <borealis.hpp>
#if defined(__SWITCH__)
#include <borealis/platforms/switch/switch_input.hpp>
#endif
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace twinx::haptics {
namespace {

std::once_flag loadOnce;
std::atomic<float> keyboardStrength{0.65f};
std::atomic_uint64_t keyboardPulseGeneration{0};

std::string settingsPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/TwiNX/haptics.json";
#else
    return "haptics.json";
#endif
}

void load() {
    try {
        std::ifstream input(settingsPath());
        if (!input.is_open()) return;
        nlohmann::json root;
        input >> root;
        keyboardStrength.store(std::clamp(
            root.value("keyboard_intensity", 0.65f), 0.0f, 1.0f));
    } catch (const std::exception& error) {
        brls::Logger::warning(
            "twiNX could not load haptics settings: {}",
            error.what());
    }
}

#if defined(__SWITCH__)
brls::SwitchInputManager* switchInput() {
    auto* platform = brls::Application::getPlatform();
    if (!platform) return nullptr;
    return dynamic_cast<brls::SwitchInputManager*>(
        platform->getInputManager());
}
#endif

}  // namespace

float keyboardIntensity() {
    std::call_once(loadOnce, load);
    return keyboardStrength.load();
}

bool setKeyboardIntensity(float intensity) {
    std::call_once(loadOnce, load);
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    keyboardStrength.store(intensity);
    try {
        const std::filesystem::path path(settingsPath());
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) return false;
        // Always retire the unsafe audio-reactive experiment, including when
        // upgrading from a profile that previously saved it as enabled.
        output << nlohmann::json{
            {"audio_reactive", false},
            {"keyboard_intensity", intensity},
        }.dump(2);
        return output.good();
    } catch (const std::exception& error) {
        brls::Logger::warning(
            "twiNX could not save haptics settings: {}",
            error.what());
        return false;
    }
}

void keyboardPulse() {
#if defined(__SWITCH__)
    auto* input = switchInput();
    if (!input) return;

    const float intensity = keyboardIntensity();
    if (intensity <= 0.001f) return;
    const float shaped = 0.25f + intensity * 0.75f;

    const uint64_t generation = ++keyboardPulseGeneration;
    input->sendRumbleRawForDevice(
        0,
        160.0f,
        320.0f,
        0.025f + shaped * 0.095f,
        0.045f + shaped * 0.205f);
    brls::delay(18 + static_cast<long>(intensity * 18.0f), [generation]() {
        if (generation != keyboardPulseGeneration.load()) return;
        if (auto* input = switchInput())
            input->sendRumbleRawForDevice(
                0, 160.0f, 320.0f, 0.0f, 0.0f);
    });
#endif
}

void stop() {
#if defined(__SWITCH__)
    ++keyboardPulseGeneration;
    if (auto* input = switchInput())
        input->sendRumbleRaw(120.0f, 240.0f, 0.0f, 0.0f);
#endif
}

}  // namespace twinx::haptics
