#pragma once

namespace twinx::haptics {

float keyboardIntensity();
bool setKeyboardIntensity(float intensity);

void keyboardPulse();
void stop();

}  // namespace twinx::haptics
