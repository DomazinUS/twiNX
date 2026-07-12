#pragma once

#include <borealis.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace twinx::portrait {

enum class OrientationMode {
    Auto = 0,
    Landscape = 1,
    PortraitClockwise = 2,
    PortraitCounterClockwise = 3,
};

enum class DisplayOrientation {
    Landscape = 0,
    PortraitClockwise = 1,
    PortraitCounterClockwise = 2,
};

class OrientationController {
public:
    static OrientationController& instance();

    void init();
    void shutdown();

    OrientationMode mode() const { return selectedMode; }
    DisplayOrientation orientation() const { return currentOrientation; }
    bool setMode(OrientationMode mode);

    brls::Event<DisplayOrientation>* getOrientationChanged() {
        return &orientationChanged;
    }

    static const char* orientationName(DisplayOrientation orientation);

private:
    void handleSensor(brls::SensorEvent event);
    void applyOrientation(DisplayOrientation orientation, bool notify);
    void load();
    bool save() const;
    std::string settingsPath() const;

    OrientationMode selectedMode = OrientationMode::Auto;
    DisplayOrientation currentOrientation = DisplayOrientation::Landscape;
    DisplayOrientation candidateOrientation = DisplayOrientation::Landscape;
    float filteredX = 0.0f;
    float filteredY = 0.0f;
    bool filterReady = false;
    bool initialized = false;
    std::chrono::steady_clock::time_point candidateSince{};

    brls::Event<brls::SensorEvent>* sensorEvent = nullptr;
    std::optional<brls::Event<brls::SensorEvent>::Subscription> sensorSubscription;
    brls::Event<DisplayOrientation> orientationChanged;
};

}  // namespace twinx::portrait
