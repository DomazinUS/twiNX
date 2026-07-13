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
#include <cmath>
#include <mutex>

namespace twinx::haptics {
namespace {

std::once_flag loadOnce;
std::atomic<float> keyboardStrength{0.65f};
std::atomic_bool audioReactive{false};
std::atomic_int audioProfile{
    static_cast<int>(AudioReactiveProfile::Balanced)};
std::atomic_uint64_t keyboardPulseGeneration{0};
std::mutex settingsMutex;

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
        audioReactive.store(
            root.value("audio_reactive_version", 0) >= 2 &&
            root.value("audio_reactive", false));
        audioProfile.store(std::clamp(
            root.value(
                "audio_reactive_profile",
                static_cast<int>(AudioReactiveProfile::Balanced)),
            static_cast<int>(AudioReactiveProfile::Balanced),
            static_cast<int>(AudioReactiveProfile::HighPeaksOnly)));
    } catch (const std::exception& error) {
        brls::Logger::warning(
            "twiNX could not load haptics settings: {}",
            error.what());
    }
}

bool saveSettings() {
    std::lock_guard<std::mutex> lock(settingsMutex);
    try {
        const std::filesystem::path path(settingsPath());
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) return false;
        output << nlohmann::json{
            {"audio_reactive", audioReactive.load()},
            {"audio_reactive_profile", audioProfile.load()},
            {"audio_reactive_version", 2},
            {"keyboard_intensity", keyboardStrength.load()},
        }.dump(2);
        return output.good();
    } catch (const std::exception& error) {
        brls::Logger::warning(
            "twiNX could not save haptics settings: {}",
            error.what());
        return false;
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
    return saveSettings();
}

bool audioReactiveEnabled() {
    std::call_once(loadOnce, load);
    return audioReactive.load();
}

bool setAudioReactiveEnabled(bool enabled) {
    std::call_once(loadOnce, load);
    audioReactive.store(enabled);
    if (!enabled) stopAudioReactive();
    return saveSettings();
}

AudioReactiveProfile audioReactiveProfile() {
    std::call_once(loadOnce, load);
    return static_cast<AudioReactiveProfile>(audioProfile.load());
}

bool setAudioReactiveProfile(AudioReactiveProfile profile) {
    std::call_once(loadOnce, load);
    const int bounded = std::clamp(
        static_cast<int>(profile),
        static_cast<int>(AudioReactiveProfile::Balanced),
        static_cast<int>(AudioReactiveProfile::HighPeaksOnly));
    audioProfile.store(bounded);
    return saveSettings();
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

void setAudioReactiveLevel(float intensity) {
#if defined(__SWITCH__)
    auto* input = switchInput();
    if (!input) return;

    intensity = std::clamp(intensity, 0.0f, 1.0f);
    if (intensity <= 0.001f) {
        stopAudioReactive();
        return;
    }

    const float shaped = std::pow(intensity, 0.75f);
    input->sendRumbleRaw(
        90.0f + shaped * 45.0f,
        180.0f + shaped * 140.0f,
        shaped * 0.24f,
        shaped * 0.48f);
#endif
}

void stopAudioReactive() {
#if defined(__SWITCH__)
    if (auto* input = switchInput())
        input->sendRumbleRaw(120.0f, 240.0f, 0.0f, 0.0f);
#endif
}

void stop() {
#if defined(__SWITCH__)
    ++keyboardPulseGeneration;
    stopAudioReactive();
#endif
}

}  // namespace twinx::haptics
