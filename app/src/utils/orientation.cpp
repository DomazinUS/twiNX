#include "utils/orientation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace twinx::portrait {
namespace {

constexpr float FILTER_ALPHA = 0.18f;
constexpr float ENTER_AXIS_MINIMUM = 0.48f;
constexpr float ENTER_DOMINANCE = 1.12f;
constexpr float EXIT_DOMINANCE = 1.06f;
constexpr auto ORIENTATION_HOLD = std::chrono::milliseconds(520);

}  // namespace

OrientationController& OrientationController::instance() {
    static OrientationController controller;
    return controller;
}

void OrientationController::init() {
    if (initialized) return;
    initialized = true;
    load();

    if (selectedMode != OrientationMode::Auto) {
        applyOrientation(
            selectedMode == OrientationMode::Landscape
                ? DisplayOrientation::Landscape
                : selectedMode == OrientationMode::PortraitClockwise
                    ? DisplayOrientation::PortraitClockwise
                    : DisplayOrientation::PortraitCounterClockwise,
            false);
    }

#if defined(__SWITCH__)
    sensorEvent = brls::Application::getPlatform()
                      ->getInputManager()
                      ->getControllerSensorStateChanged();
    sensorSubscription = sensorEvent->subscribe(
        [this](brls::SensorEvent event) { handleSensor(event); });
#endif

    brls::Logger::info(
        "twiNX orientation initialized: mode={} orientation={}",
        static_cast<int>(selectedMode),
        orientationName(currentOrientation));
}

void OrientationController::shutdown() {
    if (sensorEvent && sensorSubscription) {
        sensorEvent->unsubscribe(*sensorSubscription);
        sensorSubscription.reset();
    }
    sensorEvent = nullptr;
    initialized = false;
}

bool OrientationController::setMode(OrientationMode mode) {
    if (static_cast<int>(mode) < static_cast<int>(OrientationMode::Auto) ||
        static_cast<int>(mode) >
            static_cast<int>(OrientationMode::PortraitCounterClockwise))
        return false;

    selectedMode = mode;
    filterReady = false;

    if (mode != OrientationMode::Auto) {
        applyOrientation(
            mode == OrientationMode::Landscape
                ? DisplayOrientation::Landscape
                : mode == OrientationMode::PortraitClockwise
                    ? DisplayOrientation::PortraitClockwise
                    : DisplayOrientation::PortraitCounterClockwise,
            true);
    }

    return save();
}

void OrientationController::handleSensor(brls::SensorEvent event) {
    if (event.type != brls::SensorEventType::ACCEL ||
        selectedMode != OrientationMode::Auto)
        return;

    const float x = event.data[0];
    // Borealis exposes the Joy-Con's depth axis as data[1]. Using it as the
    // landscape axis made orientation depend on how far the screen was tilted
    // toward the viewer. data[2] follows the Switch screen's vertical edge.
    const float screenY = event.data[2];
    const float depth = event.data[1];
    const float magnitude =
        std::sqrt(x * x + screenY * screenY + depth * depth);
    if (!std::isfinite(magnitude) || magnitude < 0.55f) return;

    if (!filterReady) {
        filteredX = x;
        filteredScreenY = screenY;
        filterReady = true;
    } else {
        filteredX += FILTER_ALPHA * (x - filteredX);
        filteredScreenY +=
            FILTER_ALPHA * (screenY - filteredScreenY);
    }

    const float absX = std::abs(filteredX);
    const float absScreenY = std::abs(filteredScreenY);
    DisplayOrientation detected = currentOrientation;

    const bool currentlyPortrait =
        currentOrientation != DisplayOrientation::Landscape;
    const float portraitDominance =
        currentlyPortrait ? EXIT_DOMINANCE : ENTER_DOMINANCE;
    const float landscapeDominance =
        currentlyPortrait ? ENTER_DOMINANCE : EXIT_DOMINANCE;

    if (absX >= ENTER_AXIS_MINIMUM &&
        absX >= absScreenY * portraitDominance) {
        detected = filteredX > 0.0f
            ? DisplayOrientation::PortraitClockwise
            : DisplayOrientation::PortraitCounterClockwise;
    } else if (absScreenY >= ENTER_AXIS_MINIMUM &&
        absScreenY >= absX * landscapeDominance) {
        detected = DisplayOrientation::Landscape;
    } else {
        candidateOrientation = currentOrientation;
        candidateSince = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (detected != candidateOrientation) {
        candidateOrientation = detected;
        candidateSince = now;
        return;
    }

    if (detected != currentOrientation &&
        candidateSince.time_since_epoch().count() != 0 &&
        now - candidateSince >= ORIENTATION_HOLD) {
        applyOrientation(detected, true);
        candidateSince = {};
    }
}

void OrientationController::applyOrientation(
    DisplayOrientation orientation,
    bool notify) {
    (void)notify;
    if (orientation == currentOrientation) return;
    currentOrientation = orientation;
    candidateOrientation = orientation;
    orientationChanged.fire(orientation);
    brls::Logger::info(
        "twiNX orientation changed: {}",
        orientationName(orientation));
}

const char* OrientationController::orientationName(
    DisplayOrientation orientation) {
    switch (orientation) {
        case DisplayOrientation::PortraitClockwise:
            return "portrait clockwise";
        case DisplayOrientation::PortraitCounterClockwise:
            return "portrait counter-clockwise";
        default:
            return "landscape";
    }
}

std::string OrientationController::settingsPath() const {
#if defined(__SWITCH__)
    return "sdmc:/config/TwiNX/orientation.json";
#else
    return "orientation.json";
#endif
}

void OrientationController::load() {
    try {
        std::ifstream input(settingsPath());
        if (!input.is_open()) return;
        nlohmann::json root;
        input >> root;
        const int value = root.value("mode", 0);
        if (value >= static_cast<int>(OrientationMode::Auto) &&
            value <= static_cast<int>(OrientationMode::PortraitCounterClockwise))
            selectedMode = static_cast<OrientationMode>(value);
    } catch (const std::exception& error) {
        brls::Logger::warning(
            "twiNX could not load orientation setting: {}",
            error.what());
    }
}

bool OrientationController::save() const {
    try {
        const std::filesystem::path path(settingsPath());
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) return false;
        output << nlohmann::json{{"mode", static_cast<int>(selectedMode)}}.dump(2);
        return output.good();
    } catch (const std::exception& error) {
        brls::Logger::warning(
            "twiNX could not save orientation setting: {}",
            error.what());
        return false;
    }
}

}  // namespace twinx::portrait
