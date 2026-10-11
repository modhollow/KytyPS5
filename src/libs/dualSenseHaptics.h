#ifndef EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_
#define EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_

#include <cstdint>

namespace Libs::Controller::DualSenseHaptics {

struct Stream;

// Vibration ports use the rear channels of an unambiguous wired DualSense audio endpoint.
Stream* Open(uint32_t freq);
void    Close(Stream* stream);
bool    HasDevice(const Stream* stream);
// Returns queued playback time in microseconds, or zero when no controller endpoint took it.
uint64_t Queue(Stream* stream, int controller, const void* data, uint32_t frames, uint32_t channels,
               bool is_float, const int* volume, float gain = 1.0f);
// Coordinates PS5 motor emulation with waveform playback. Other pads retain SDL rumble.
bool SetVibration(int controller, uint8_t large_motor, uint8_t small_motor,
                  uint32_t duration_ms = 0xffff);
void Shutdown();

} // namespace Libs::Controller::DualSenseHaptics

#endif // EMULATOR_INCLUDE_EMULATOR_DUALSENSE_HAPTICS_H_
