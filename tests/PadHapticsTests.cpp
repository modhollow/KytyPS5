// Keep the guest route oracle buildable with the pre-fix production files during RED proof.
#if __has_include("libs/dualSenseHaptics.cpp")
#define HAS_CONTROLLER_WAVEFORM_ROUTE 1
#else
#define HAS_CONTROLLER_WAVEFORM_ROUTE 0
#endif

// Exercise guest waveform routing and real SDL conversion with every host output mocked.
#define SDL_MAIN_HANDLED
#include "common/emulatorConfig.h"
#include "loader/timer.h"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PadHapticsTests: %s\n", message);
		std::abort();
	}
}
struct Output {
	SDL_AudioDeviceID    device;
	std::vector<uint8_t> bytes;
};
struct Motors {
	int    controller;
	Uint16 large, small;
	Uint32 duration;
};
struct Endpoint {
	std::string name;
	int         channels;
};
std::vector<Output>                        output;
std::vector<Motors>                        motors;
std::vector<Endpoint>                      endpoints;
std::map<SDL_AudioDeviceID, SDL_AudioSpec> opened;
int                                        controller_count = 1, active_controller = 1;
int                                        actual_channels = 4, actual_frequency = 48000;
SDL_AudioFormat                            actual_format = AUDIO_F32SYS;
SDL_GameControllerType                     pad_type      = SDL_CONTROLLER_TYPE_PS5;
SDL_JoystickPowerLevel                     power         = SDL_JOYSTICK_POWER_WIRED;
bool                                       auto_pump = true, fail_open = false, fail_put = false;
uint64_t                                   now        = 1000000;
int                                        pump_bytes = 256 * 4 * sizeof(float), device_locks = 0;
constexpr SDL_AudioDeviceID                DEFAULT_DEVICE = 1, PAD_DEVICE = 2;

void Pump(SDL_AudioDeviceID device = PAD_DEVICE) {
	const auto it = opened.find(device);
	if (it == opened.end() || it->second.callback == nullptr) {
		return;
	}
	std::vector<uint8_t> bytes(pump_bytes, 0);
	it->second.callback(it->second.userdata, bytes.data(), static_cast<int>(bytes.size()));
	output.push_back({device, std::move(bytes)});
}
} // namespace

namespace Fake {
int InitSubSystem(Uint32) {
	return 0;
}
void   QuitSubSystem(Uint32) {}
Uint32 WasInit(Uint32 flags) {
	return flags;
}
Uint64 GetTicks64() {
	return now / 1000;
}
void LockJoysticks() {}
void UnlockJoysticks() {}
int  NumJoysticks() {
    return controller_count;
}
SDL_GameControllerType GameControllerTypeForIndex(int) {
	return pad_type;
}
SDL_GameController* GameControllerFromInstanceID(SDL_JoystickID id) {
	return controller_count > 0 && id >= 1 &&
	               (id == active_controller || (controller_count > 1 && id <= controller_count))
	           ? reinterpret_cast<SDL_GameController*>(static_cast<uintptr_t>(id))
	           : nullptr;
}
SDL_GameControllerType GameControllerGetType(SDL_GameController*) {
	return pad_type;
}
SDL_Joystick* GameControllerGetJoystick(SDL_GameController* pad) {
	return reinterpret_cast<SDL_Joystick*>(pad);
}
SDL_JoystickPowerLevel JoystickCurrentPowerLevel(SDL_Joystick*) {
	return power;
}
int GameControllerRumble(SDL_GameController* pad, Uint16 large, Uint16 small, Uint32 duration) {
	motors.push_back({static_cast<int>(reinterpret_cast<uintptr_t>(pad)), large, small, duration});
	return 0;
}
SDL_AudioDeviceID OpenAudioDevice(const char* name, int, const SDL_AudioSpec* desired,
                                  SDL_AudioSpec* obtained, int changes) {
	if (fail_open) {
		return 0;
	}
	*obtained         = *desired;
	const auto device = name == nullptr ? DEFAULT_DEVICE : PAD_DEVICE;
	if (name != nullptr) {
		Check((changes & SDL_AUDIO_ALLOW_CHANNELS_CHANGE) == 0,
		      "quad route allowed channel remapping");
		Check(desired->channels == 4, "controller route did not request quad output");
		Check(std::any_of(endpoints.begin(), endpoints.end(),
		                  [name](const auto& endpoint) { return endpoint.name == name; }),
		      "unlisted endpoint was opened");
		obtained->channels = static_cast<Uint8>(actual_channels);
		obtained->freq     = actual_frequency;
		obtained->format   = actual_format;
	}
	opened[device] = *obtained;
	return device;
}
int GetNumAudioDevices(int) {
	return static_cast<int>(endpoints.size());
}
const char* GetAudioDeviceName(int index, int) {
	return endpoints.at(index).name.c_str();
}
int GetAudioDeviceSpec(int index, int, SDL_AudioSpec* spec) {
	*spec          = {};
	spec->freq     = 48000;
	spec->format   = AUDIO_F32SYS;
	spec->channels = static_cast<Uint8>(endpoints.at(index).channels);
	return 0;
}
void PauseAudioDevice(SDL_AudioDeviceID, int) {}
void CloseAudioDevice(SDL_AudioDeviceID device) {
	Check(device_locks == 0, "device closed under its callback lock");
	opened.erase(device);
}
void LockAudioDevice(SDL_AudioDeviceID) {
	device_locks++;
}
void UnlockAudioDevice(SDL_AudioDeviceID device) {
	Check(device_locks == 1, "audio device lock was unbalanced");
	device_locks--;
	if (auto_pump) {
		Pump(device);
	}
}
void   ClearQueuedAudio(SDL_AudioDeviceID) {}
Uint32 GetQueuedAudioSize(SDL_AudioDeviceID) {
	return 0;
}
int QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	output.push_back({device, {bytes, bytes + size}});
	return 0;
}
int AudioStreamPut(SDL_AudioStream* stream, const void* data, int size) {
	return fail_put ? -1 : SDL_AudioStreamPut(stream, data, size);
}
} // namespace Fake

#define SDL_InitSubSystem                Fake::InitSubSystem
#define SDL_QuitSubSystem                Fake::QuitSubSystem
#define SDL_WasInit                      Fake::WasInit
#define SDL_GetTicks64                   Fake::GetTicks64
#define SDL_LockJoysticks                Fake::LockJoysticks
#define SDL_UnlockJoysticks              Fake::UnlockJoysticks
#define SDL_NumJoysticks                 Fake::NumJoysticks
#define SDL_GameControllerTypeForIndex   Fake::GameControllerTypeForIndex
#define SDL_GameControllerFromInstanceID Fake::GameControllerFromInstanceID
#define SDL_GameControllerGetType        Fake::GameControllerGetType
#define SDL_GameControllerGetJoystick    Fake::GameControllerGetJoystick
#define SDL_JoystickCurrentPowerLevel    Fake::JoystickCurrentPowerLevel
#define SDL_GameControllerRumble         Fake::GameControllerRumble
#define SDL_OpenAudioDevice              Fake::OpenAudioDevice
#define SDL_GetNumAudioDevices           Fake::GetNumAudioDevices
#define SDL_GetAudioDeviceName           Fake::GetAudioDeviceName
#define SDL_GetAudioDeviceSpec           Fake::GetAudioDeviceSpec
#define SDL_PauseAudioDevice             Fake::PauseAudioDevice
#define SDL_CloseAudioDevice             Fake::CloseAudioDevice
#define SDL_LockAudioDevice              Fake::LockAudioDevice
#define SDL_UnlockAudioDevice            Fake::UnlockAudioDevice
#define SDL_ClearQueuedAudio             Fake::ClearQueuedAudio
#define SDL_GetQueuedAudioSize           Fake::GetQueuedAudioSize
#define SDL_QueueAudio                   Fake::QueueAudio
#define SDL_AudioStreamPut               Fake::AudioStreamPut
#if HAS_CONTROLLER_WAVEFORM_ROUTE
#include "libs/dualSenseHaptics.cpp"
#endif
#include "libs/audio.cpp"
#undef SDL_InitSubSystem
#undef SDL_QuitSubSystem
#undef SDL_WasInit
#undef SDL_GetTicks64
#undef SDL_LockJoysticks
#undef SDL_UnlockJoysticks
#undef SDL_NumJoysticks
#undef SDL_GameControllerTypeForIndex
#undef SDL_GameControllerFromInstanceID
#undef SDL_GameControllerGetType
#undef SDL_GameControllerGetJoystick
#undef SDL_JoystickCurrentPowerLevel
#undef SDL_GameControllerRumble
#undef SDL_OpenAudioDevice
#undef SDL_GetNumAudioDevices
#undef SDL_GetAudioDeviceName
#undef SDL_GetAudioDeviceSpec
#undef SDL_PauseAudioDevice
#undef SDL_CloseAudioDevice
#undef SDL_LockAudioDevice
#undef SDL_UnlockAudioDevice
#undef SDL_ClearQueuedAudio
#undef SDL_GetQueuedAudioSize
#undef SDL_QueueAudio
#undef SDL_AudioStreamPut

namespace Libs::Controller {
int GetActiveControllerId() {
	return active_controller;
}
} // namespace Libs::Controller
namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return now;
}
} // namespace Libs::LibKernel
// Audio3d is outside this waveform test. Fail if its unrelated wait path is reached.
namespace Libs::LibKernel::Semaphore {
int KYTY_SYSV_ABI KernelCreateSema(KernelSema*, const char*, uint32_t, int, int, void*) {
	std::abort();
}
int KYTY_SYSV_ABI KernelWaitSema(KernelSema, int, KernelUseconds*) {
	std::abort();
}
int KYTY_SYSV_ABI KernelSignalSema(KernelSema, int) {
	std::abort();
}
} // namespace Libs::LibKernel::Semaphore
namespace Loader::Timer {
double GetTimeMs() {
	return static_cast<double>(now) / 1000.0;
}
Common::Time GetTime() {
	return Common::Time(static_cast<int>(GetTimeMs()));
}
} // namespace Loader::Timer

namespace {
void Begin() {
	output.clear();
	motors.clear();
	opened.clear();
	endpoints         = {{"Synthetic DualSense", 4}};
	controller_count  = 1;
	active_controller = 1;
	pad_type          = SDL_CONTROLLER_TYPE_PS5;
	power             = SDL_JOYSTICK_POWER_WIRED;
	actual_channels   = 4;
	actual_frequency  = 48000;
	actual_format     = AUDIO_F32SYS;
	auto_pump         = true;
	fail_open = fail_put = false;
	device_locks         = 0;
	pump_bytes           = 256 * 4 * sizeof(float);
	now                  = 1000000;
	Config::Initialize();
	Libs::Audio::Initialize();
}
void End() {
	Libs::Audio::Shutdown();
#if HAS_CONTROLLER_WAVEFORM_ROUTE
	Libs::Controller::DualSenseHaptics::Shutdown();
#endif
	Config::Shutdown();
	Check(opened.empty(), "controller endpoint leaked after shutdown");
}
std::array<float, 4> Frame(const Output& block, size_t index = 0) {
	std::array<float, 4> actual {};
	Check((index + 1) * sizeof(actual) <= block.bytes.size(), "captured output is short");
	std::memcpy(actual.data(), block.bytes.data() + index * sizeof(actual), sizeof(actual));
	return actual;
}
void TestGuestVibrationWaveform() {
	Begin();
	constexpr uint32_t frames = 256;
	// Preserve the original RED input and routing oracle.
	std::array<float, frames * 2> pcm {};
	for (uint32_t frame = 0; frame < frames; frame++) {
		pcm[frame * 2]     = 0.25f;
		pcm[frame * 2 + 1] = -0.5f;
	}
	const int handle = Libs::Audio::AudioOut::AudioOutOpen(1, 10, 0, frames, 48000, 4);
	Check(handle > 0, "guest vibration port was not allocated");
	Check(Libs::Audio::AudioOut::AudioOutOutput(handle, pcm.data()) == frames,
	      "guest waveform submission did not consume the requested frames");
	Check(output.size() == 1,
	      "guest vibration PCM was acknowledged without reaching the controller endpoint");
	const auto& block = output.front();
	Check(block.device == PAD_DEVICE, "vibration PCM reached the default speakers");
	Check(block.bytes.size() == frames * 4 * sizeof(float), "controller output was not quad PCM");
	for (uint32_t frame = 0; frame < frames; frame++) {
		const auto actual = Frame(block, frame);
		Check(actual[0] == 0.0f && actual[1] == 0.0f,
		      "vibration PCM contaminated the speaker channels");
		Check(actual[2] == pcm[frame * 2] && actual[3] == pcm[frame * 2 + 1],
		      "the actuators did not retain the independent guest waveforms");
	}
	Check(Libs::Audio::AudioOut::AudioOutClose(handle) == 0, "vibration port did not close");
	End();
}
void TestRejectedEndpoints() {
	for (int scenario = 0; scenario < 9; scenario++) {
		Begin();
		switch (scenario) {
			case 0: endpoints.clear(); break;
			case 1: endpoints = {{"Speakers", 4}}; break;
			case 2: endpoints[0].channels = 2; break;
			case 3: endpoints.push_back({"Other DualSense", 4}); break;
			case 4: controller_count = 2; break;
			case 5: power = SDL_JOYSTICK_POWER_UNKNOWN; break;
			case 6: pad_type = SDL_CONTROLLER_TYPE_PS4; break;
			case 7: actual_channels = 2; break;
			case 8: fail_open = true; break;
		}
		std::array<float, 512> pcm {};
		pcm.fill(0.5f);
		const int handle = Libs::Audio::AudioOut::AudioOutOpen(1, 10, 0, 256, 48000, 4);
		(void)Libs::Audio::AudioOut::AudioOutOutput(handle, pcm.data());
		Check(output.empty() && opened.empty(),
		      "unsupported/ambiguous endpoint received waveform PCM");
		Check(motors.empty(), "a rejected waveform synthesized motor rumble");
		End();
	}
}
#if HAS_CONTROLLER_WAVEFORM_ROUTE
void TestGainMonoAndSilence() {
	Begin();
	Config::ConfigOptions config;
	config.controller_vibration_intensity = 50;
	Config::Load(config);
	std::array<int16_t, 256> pcm {};
	pcm.fill(-16384);
	const int handle = Libs::Audio::AudioInternal::AudioOutOpen(
	    10, 256, 48000, Libs::Audio::AudioInternal::Format::Signed16bitMono);
	const int volume[1] = {16384};
	(void)Libs::Audio::g_audio->AudioOutSetVolume(Libs::Audio::Audio::Id(handle), 1, volume);
	(void)Libs::Audio::AudioOut::AudioOutOutput(handle, pcm.data());
	Check(Frame(output.back()) == std::array<float, 4> {0, 0, -0.125f, -0.125f},
	      "signed mono/volume/intensity conversion changed amplitude");
	pcm.fill(0);
	motors.clear();
	(void)Libs::Audio::AudioOut::AudioOutOutput(handle, pcm.data());
	Check(Frame(output.back()) == std::array<float, 4> {}, "silence became actuator output");
	Check(motors.empty(), "silence changed standard rumble arbitration");
	config.controller_vibration_intensity = 0;
	Config::Load(config);
	pcm.fill(32767);
	motors.clear();
	(void)Libs::Audio::AudioOut::AudioOutOutput(handle, pcm.data());
	Check(Frame(output.back()) == std::array<float, 4> {}, "global mute left waveform output");
	Check(motors.empty(), "muted waveform enabled rumble emulation");
	End();
}
void TestArbitrationAndIsolation() {
	using namespace Libs::Controller::DualSenseHaptics;
	Begin();
	auto_pump                     = false;
	auto*                  stream = Open(48000);
	std::array<float, 512> pcm {};
	pcm.fill(0.5f);
	const int volume[2] = {32768, 32768};
	Check(SetVibration(1, 100, 20, 1000), "PS5 standard rumble was not handled");
	Check(motors.back().large == 25700 && motors.back().small == 5140, "rumble amplitudes changed");
	Check(Queue(stream, 1, pcm.data(), 256, 2, true, volume) > 0 && motors.back().large == 0,
	      "waveform did not disable motor emulation");
	Check(SetVibration(1, 80, 10, 1000) && motors.back().large == 0,
	      "guest rumble interrupted active waveform");
	now += 300000;
	Pump();
	Check(motors.back().large == 20560 && motors.back().small == 2570 &&
	          motors.back().duration == 700,
	      "callback did not restore the current unexpired rumble");
	std::fill(pcm.begin(), pcm.end(), 0.0f);
	motors.clear();
	(void)Queue(stream, 1, pcm.data(), 256, 2, true, volume);
	Check(motors.empty(), "silent PCM cancelled rumble");
	pcm.fill(0.5f);
	(void)SetVibration(1, 90, 10, 100);
	(void)Queue(stream, 1, pcm.data(), 256, 2, true, volume);
	now += 300000;
	Pump();
	Check(motors.back().large == 0, "expired rumble was resurrected");
	(void)Queue(stream, 1, pcm.data(), 256, 2, true, volume);
	Shutdown();
	Pump();
	Check(Frame(output.back()) == std::array<float, 4> {},
	      "controller release played stale queued PCM");
	controller_count = 0;
	motors.clear();
	Check(Queue(stream, 1, pcm.data(), 256, 2, true, volume) == 0 && opened.empty(),
	      "disconnected controller retained endpoint");
	controller_count = 1;
	now += 2000000;
	Check(Queue(stream, 1, pcm.data(), 256, 2, true, volume) > 0,
	      "reconnected endpoint was not rediscovered");
	Check(motors.back().large == 0, "reconnect inherited an old rumble command");
	Shutdown();
	active_controller = 2;
	motors.clear();
	output.clear();
	Check(Queue(stream, 2, pcm.data(), 256, 2, true, volume) > 0,
	      "replacement controller did not open its route");
	Check(!motors.empty() && motors.back().controller == 2 && motors.back().large == 0,
	      "controller replacement inherited old output state");
	Pump();
	Check(Frame(output.back())[2] == 0.5f, "replacement route lost its guest waveform");
	Close(stream);
	End();
}
void TestDeviceConversion() {
	using namespace Libs::Controller::DualSenseHaptics;
	for (bool signed_output: {false, true}) {
		Begin();
		auto_pump                     = false;
		actual_frequency              = signed_output ? 48000 : 44100;
		actual_format                 = signed_output ? AUDIO_S16SYS : AUDIO_F32SYS;
		auto*                  stream = Open(48000);
		std::array<float, 512> pcm {};
		for (size_t i = 0; i < pcm.size(); i += 2) {
			pcm[i]     = 0.25f;
			pcm[i + 1] = -0.5f;
		}
		const int volume[2] = {32768, 32768};
		// An independent stereo SDL converter establishes the rate/format result. The pinned
		// resampler buffers input for its filter padding before returning any playable frames.
		auto* reference =
		    SDL_NewAudioStream(AUDIO_F32SYS, 2, 48000, actual_format, 2, actual_frequency);
		Check(reference != nullptr, "reference converter could not be allocated");
		const int sample_bytes = signed_output ? sizeof(int16_t) : sizeof(float);
		int       blocks       = 0;
		while (SDL_AudioStreamAvailable(reference) < 256 * 2 * sample_bytes && blocks++ < 8) {
			Check(SDL_AudioStreamPut(reference, pcm.data(), sizeof(pcm)) == 0,
			      "reference converter rejected guest samples");
			Check(Queue(stream, 1, pcm.data(), 256, 2, true, volume) > 0,
			      "supported device resampling/format conversion failed");
		}
		std::vector<uint8_t> expected(256 * 2 * sample_bytes);
		Check(SDL_AudioStreamGet(reference, expected.data(), static_cast<int>(expected.size())) ==
		          expected.size(),
		      "reference resampler did not produce frames within the latency budget");
		pump_bytes = 256 * 4 * sample_bytes;
		Pump();
		if (signed_output) {
			std::array<int16_t, 4> frame {};
			std::array<int16_t, 2> converted {};
			std::memcpy(frame.data(), output.back().bytes.data(), sizeof(frame));
			std::memcpy(converted.data(), expected.data(), sizeof(converted));
			Check(frame[0] == 0 && frame[1] == 0 && converted[0] > 8000 && converted[1] < -16000 &&
			          std::abs(frame[2] - converted[0]) <= 1 &&
			          std::abs(frame[3] - converted[1]) <= 1,
			      "device signed16 conversion changed channel assignment or amplitude");
		} else {
			const auto           frame = Frame(output.back(), 128);
			std::array<float, 2> converted {};
			std::memcpy(converted.data(), expected.data() + 128 * sizeof(converted),
			            sizeof(converted));
			Check(frame[0] == 0 && frame[1] == 0 && converted[0] > 0.1f && converted[1] < -0.2f &&
			          std::fabs(frame[2] - converted[0]) < 0.000001f &&
			          std::fabs(frame[3] - converted[1]) < 0.000001f,
			      "device resampling mixed speaker/actuator channels or changed the SDL waveform");
		}
		SDL_FreeAudioStream(reference);
		Close(stream);
		End();
	}
}

void TestQueueBoundAndFailure() {
	using namespace Libs::Controller::DualSenseHaptics;
	Begin();
	auto_pump                 = false;
	auto*              stream = Open(48000);
	std::vector<float> pcm(5000 * 2, 0.25f);
	const int          volume[2] = {32768, 32768};
	const auto         queued    = Queue(stream, 1, pcm.data(), 5000, 2, true, volume);
	Check(queued > 0 && queued <= 80000, "oversized input exceeded the waveform latency bound");
	pcm.assign(512, -0.5f);
	const auto second = Queue(stream, 1, pcm.data(), 256, 2, true, volume);
	Check(second > 0 && second <= 80000, "stalled endpoint accumulated excess waveform latency");
	pump_bytes = 3840 * 4 * sizeof(float);
	Pump();
	Check(Frame(output.back(), 3839)[2] == -0.5f, "latency trimming lost the newest waveform");
	fail_put = true;
	Check(Queue(stream, 1, pcm.data(), 256, 2, true, volume) == 0 && opened.empty(),
	      "failed conversion left a live route");
	Close(stream);
	End();
}
void TestNoGuestWaveformAndNonfinite() {
	Begin();
	std::array<float, 512> pcm {};
	pcm.fill(0.25f);
	const int main = Libs::Audio::AudioOut::AudioOutOpen(1, 0, 0, 256, 48000, 4);
	(void)Libs::Audio::AudioOut::AudioOutOutput(main, pcm.data());
	Check(output.size() == 1 && output.front().device == DEFAULT_DEVICE && motors.empty(),
	      "main audio synthesized a controller effect");
	output.clear();
	const int haptic = Libs::Audio::AudioOut::AudioOutOpen(1, 10, 0, 256, 48000, 4);
	(void)Libs::Audio::AudioOut::AudioOutOutput(haptic, nullptr);
	Check(output.empty(), "null vibration PCM opened or played a waveform");
	pcm.fill(std::numeric_limits<float>::quiet_NaN());
	(void)Libs::Audio::AudioOut::AudioOutOutput(haptic, pcm.data());
	Check(Frame(output.back()) == std::array<float, 4> {} && motors.empty(),
	      "nonfinite samples reached actuators or enabled arbitration");
	End();
}
#endif
} // namespace

int main() {
	TestGuestVibrationWaveform();
	TestRejectedEndpoints();
#if HAS_CONTROLLER_WAVEFORM_ROUTE
	TestGainMonoAndSilence();
	TestArbitrationAndIsolation();
	TestDeviceConversion();
	TestQueueBoundAndFailure();
	TestNoGuestWaveformAndNonfinite();
#endif
	return 0;
}
