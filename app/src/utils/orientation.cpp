#include "utils/orientation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace twinx::portrait {
namespace {

constexpr float FILTER_ALPHA = 0.16f;
constexpr float PORTRAIT_THRESHOLD = 0.72f;
constexpr float LANDSCAPE_THRESHOLD = 0.62f;
constexpr float AXIS_MARGIN = 0.16f;
constexpr auto ORIENTATION_HOLD = std::chrono::milliseconds(650);

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
        "Portrait Lab orientation initialized: mode={} orientation={}",
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
    const float y = event.data[1];
    const float magnitude = std::sqrt(x * x + y * y + event.data[2] * event.data[2]);
    if (!std::isfinite(magnitude) || magnitude < 0.55f) return;

    if (!filterReady) {
        filteredX = x;
        filteredY = y;
        filterReady = true;
    } else {
        filteredX += FILTER_ALPHA * (x - filteredX);
        filteredY += FILTER_ALPHA * (y - filteredY);
    }

    const float absX = std::abs(filteredX);
    const float absY = std::abs(filteredY);
    DisplayOrientation detected = currentOrientation;

    if (absX >= PORTRAIT_THRESHOLD && absX >= absY + AXIS_MARGIN) {
        detected = filteredX > 0.0f
            ? DisplayOrientation::PortraitClockwise
            : DisplayOrientation::PortraitCounterClockwise;
    } else if (absY >= LANDSCAPE_THRESHOLD && absY >= absX + AXIS_MARGIN) {
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
    if (orientation == currentOrientation) return;
    currentOrientation = orientation;
    candidateOrientation = orientation;
    orientationChanged.fire(orientation);
    brls::Logger::info(
        "Portrait Lab orientation changed: {}",
        orientationName(orientation));
    if (notify) {
        brls::Application::notify(
            std::string("Portrait Lab: ") + orientationName(orientation) +
            " detected");
    }
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
    return "sdmc:/config/TwiNXPortraitExperimental/orientation.json";
#else
    return "orientation-portrait-experimental.json";
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
            "Portrait Lab could not load orientation setting: {}",
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
            "Portrait Lab could not save orientation setting: {}",
            error.what());
        return false;
    }
}

}  // namespace twinx::portrait
