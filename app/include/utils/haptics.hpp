#pragma once

namespace twinx::haptics {

enum class AudioReactiveProfile {
    Balanced = 0,
    Quiet = 1,
    Extreme = 2,
    HighPeaksOnly = 3,
};

float keyboardIntensity();
bool setKeyboardIntensity(float intensity);

bool audioReactiveEnabled();
bool setAudioReactiveEnabled(bool enabled);
AudioReactiveProfile audioReactiveProfile();
bool setAudioReactiveProfile(AudioReactiveProfile profile);

void keyboardPulse();
void setAudioReactiveLevel(float intensity);
void stopAudioReactive();
void stop();

}  // namespace twinx::haptics
