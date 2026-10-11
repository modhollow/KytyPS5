// Exercise standard guest rumble and output isolation without a physical device.
#if __has_include("libs/dualSenseHaptics.cpp")
#define HAS_CONTROLLER_WAVEFORM_ROUTE 1
#else
#define HAS_CONTROLLER_WAVEFORM_ROUTE 0
#endif
#define SDL_MAIN_HANDLED
#include "loader/timer.h"

#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ControllerSettingsTests: %s\n", message);
		std::abort();
	}
}
struct Rumble {
	int    controller;
	Uint16 large, small;
	Uint32 duration;
};
std::vector<Rumble> rumble;
uint64_t            now = 1000000;
} // namespace
namespace Fake {
SDL_GameController* GameControllerFromInstanceID(SDL_JoystickID id) {
	return id == 1 || id == 2 ? reinterpret_cast<SDL_GameController*>(static_cast<uintptr_t>(id))
	                          : nullptr;
}
int GameControllerRumble(SDL_GameController* pad, Uint16 large, Uint16 small, Uint32 duration) {
	rumble.push_back({static_cast<int>(reinterpret_cast<uintptr_t>(pad)), large, small, duration});
	return 0;
}
SDL_GameControllerType GameControllerGetType(SDL_GameController*) {
	return SDL_CONTROLLER_TYPE_PS5;
}
int GameControllerSendEffect(SDL_GameController*, const void*, int) {
	return 0;
}
int GameControllerSetLED(SDL_GameController*, Uint8, Uint8, Uint8) {
	return 0;
}
void GameControllerClose(SDL_GameController*) {}
void Delay(Uint32) {}
} // namespace Fake
#define SDL_GameControllerFromInstanceID Fake::GameControllerFromInstanceID
#define SDL_GameControllerRumble         Fake::GameControllerRumble
#define SDL_GameControllerGetType        Fake::GameControllerGetType
#define SDL_GameControllerSendEffect     Fake::GameControllerSendEffect
#define SDL_GameControllerSetLED         Fake::GameControllerSetLED
#define SDL_GameControllerClose          Fake::GameControllerClose
#define SDL_Delay                        Fake::Delay
#include "libs/controller.cpp"
#undef SDL_GameControllerFromInstanceID
#undef SDL_GameControllerRumble
#undef SDL_GameControllerGetType
#undef SDL_GameControllerSendEffect
#undef SDL_GameControllerSetLED
#undef SDL_GameControllerClose
#undef SDL_Delay

// The waveform backend is covered by PadHapticsTests. Exercise the standard SDL fallback here.
namespace Libs::Controller::DualSenseHaptics {
bool SetVibration(int, uint8_t, uint8_t, uint32_t) {
	return false;
}
void Shutdown() {}
} // namespace Libs::Controller::DualSenseHaptics

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return now;
}
} // namespace Libs::LibKernel
namespace Loader::Timer {
double GetTimeMs() {
	return static_cast<double>(now) / 1000.0;
}
Common::Time GetTime() {
	return Common::Time(static_cast<int>(GetTimeMs()));
}
} // namespace Loader::Timer
namespace {
void TestStandardRumbleContract() {
	using namespace Libs::Controller;
	Config::Initialize();
	Initialize();
	Connect(1);
#if HAS_CONTROLLER_WAVEFORM_ROUTE
	Check(GetActiveControllerId() == 1, "audio routing did not select the active pad");
#endif
	const PadVibrationParam motors {255, 1};
	Check(PadSetVibration(2, &motors) == PAD_ERROR_INVALID_HANDLE && rumble.empty(),
	      "an invalid pad handle emitted rumble");
	Check(PadSetVibration(1, nullptr) == PAD_ERROR_INVALID_ARG && rumble.empty(),
	      "a null motor request emitted rumble");
	Check(PadSetVibration(1, &motors) == 0 && rumble.size() == 1,
	      "a valid guest motor request did not reach the host");
	Check(rumble.back().controller == 1 && rumble.back().large == 65535 &&
	          rumble.back().small == 257 && rumble.back().duration == 65535,
	      "byte amplitudes did not preserve both motor channels and duration");
#if HAS_CONTROLLER_WAVEFORM_ROUTE
	Check(Config::IsControllerVibrationIntensityValid(0) &&
	          Config::IsControllerVibrationIntensityValid(100) &&
	          !Config::IsControllerVibrationIntensityValid(101) &&
	          !Config::IsControllerVibrationIntensityValid(UINT32_MAX),
	      "intensity validation accepted an out-of-range percentage");
	Config::ConfigOptions config;
	config.controller_vibration_intensity = 50;
	Config::Load(config);
	Check(Config::GetControllerVibrationIntensity() == 50, "configured intensity was not retained");
	(void)PadSetVibration(1, &motors);
	Check(rumble.back().large == 128 * 257 && rumble.back().small == 257,
	      "intensity scaling lost rounding or a small nonzero motor");
	config.controller_vibration_intensity = 1;
	Config::Load(config);
	(void)PadSetVibration(1, &motors);
	Check(rumble.back().large == 3 * 257 && rumble.back().small == 257,
	      "minimum positive intensity lost a requested motor");
	config.controller_vibration_intensity = 0;
	Config::Load(config);
	(void)PadSetVibration(1, &motors);
	Check(rumble.back().large == 0 && rumble.back().small == 0, "global mute left rumble output");
	config.controller_vibration_intensity = 100;
	Config::Load(config);
#endif
	const PadVibrationParam stop {0, 0};
	Check(PadSetVibration(1, &stop) == 0 && rumble.back().large == 0 && rumble.back().small == 0,
	      "zero amplitudes did not stop rumble");
	Connect(2);
	Disconnect(1);
#if HAS_CONTROLLER_WAVEFORM_ROUTE
	Check(GetActiveControllerId() == 2, "audio routing retained the removed pad");
#endif
	rumble.clear();
	Check(PadSetVibration(1, &motors) == 0 && rumble.size() == 1 && rumble.back().controller == 2,
	      "a replacement pad request reached the previous controller");
	Disconnect(2);
#if HAS_CONTROLLER_WAVEFORM_ROUTE
	Check(GetActiveControllerId() == HOST_INPUT_CONTROLLER_ID,
	      "keyboard fallback retained a physical output pad");
#endif
	rumble.clear();
	Check(PadSetVibration(1, &motors) == 0 && rumble.empty(),
	      "keyboard-only input emitted a physical motor request");
	Shutdown();
	Config::Shutdown();
}
} // namespace
int main() {
	TestStandardRumbleContract();
	return 0;
}
