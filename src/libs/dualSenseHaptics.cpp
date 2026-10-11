#include "libs/dualSenseHaptics.h"

#include "common/logging/log.h"
#include "common/threads.h"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace Libs::Controller::DualSenseHaptics {

struct Stream {
	uint32_t           freq       = 0;
	int                controller = -1;
	SDL_AudioDeviceID  device     = 0;
	SDL_AudioStream*   conversion = nullptr;
	SDL_AudioSpec      actual {};
	uint64_t           next_check       = 0;
	bool               reported_failure = false;
	std::string        name;
	std::vector<float> buffer;
};

namespace {
constexpr int      FRAME_BYTES  = 4 * sizeof(float);
constexpr uint32_t MAX_QUEUE_MS = 80;
Common::Mutex      g_mutex;
int                g_controller  = -1;
int                g_streams     = 0;
uint8_t            g_large_motor = 0, g_small_motor = 0;
uint64_t           g_rumble_until = 0, g_haptics_until = 0;

void ApplyRumble() { // Caller holds g_mutex; never calls Audio or Controller.
	SDL_LockJoysticks();
	if (auto* pad = SDL_GameControllerFromInstanceID(g_controller); pad != nullptr) {
		const auto now   = SDL_GetTicks64();
		const bool muted = g_haptics_until > now || g_rumble_until <= now;
		(void)SDL_GameControllerRumble(
		    pad, muted ? 0 : g_large_motor * 0x101U, muted ? 0 : g_small_motor * 0x101U,
		    g_rumble_until > now ? static_cast<uint32_t>(g_rumble_until - now) : 0);
	}
	SDL_UnlockJoysticks();
}

void SelectController(int controller) {
	if (g_controller != controller) {
		g_haptics_until = g_rumble_until = 0;
		ApplyRumble();
		g_controller  = controller;
		g_large_motor = g_small_motor = 0;
	}
}

void SDLCALL Play(void* userdata, Uint8* data, int bytes) {
	auto* stream = static_cast<Stream*>(userdata);
	SDL_memset(data, stream->actual.silence, bytes);
	// SDL holds this device's lock. Queue uses the same lock for conversion and arbitration.
	Common::LockGuard lock(g_mutex);
	if (stream->controller != g_controller) {
		SDL_AudioStreamClear(stream->conversion);
		return;
	}
	(void)SDL_AudioStreamGet(stream->conversion, data, bytes);
	if (g_haptics_until != 0 && SDL_GetTicks64() >= g_haptics_until) {
		g_haptics_until = 0;
		ApplyRumble();
	}
}

void CloseDevice(Stream* stream) {
	if (stream->device != 0) {
		// Close waits for the audio callback, so it must run outside g_mutex/device locks.
		SDL_CloseAudioDevice(stream->device);
		stream->device = 0;
		SDL_FreeAudioStream(stream->conversion);
		stream->conversion = nullptr;
		Common::LockGuard lock(g_mutex);
		if (--g_streams == 0 && g_haptics_until != 0) {
			g_haptics_until = 0;
			ApplyRumble();
		}
	}
	stream->name.clear();
}

bool CanUseDevice(int controller) {
	// SDL2 cannot associate an audio endpoint with an input device. Reject multiple PS5 pads,
	// including unopened ones. Its pinned HIDAPI driver reports USB power as WIRED and BT as
	// UNKNOWN.
	SDL_LockJoysticks();
	int count = 0;
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		count += SDL_GameControllerTypeForIndex(i) == SDL_CONTROLLER_TYPE_PS5;
	}
	auto*      pad    = SDL_GameControllerFromInstanceID(controller);
	const bool usable = count == 1 && pad != nullptr &&
	                    SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_PS5 &&
	                    SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(pad)) ==
	                        SDL_JOYSTICK_POWER_WIRED;
	SDL_UnlockJoysticks();
	return usable;
}

std::string FindDevice() {
	std::string result;
	const int   count = SDL_GetNumAudioDevices(0);
	for (int i = 0; i < count; i++) {
		const char*   name = SDL_GetAudioDeviceName(i, 0);
		SDL_AudioSpec spec {};
		if (name != nullptr &&
		    (SDL_strcasestr(name, "DualSense") != nullptr ||
		     SDL_strcasestr(name, "Wireless Controller") != nullptr) &&
		    SDL_GetAudioDeviceSpec(i, 0, &spec) == 0 && spec.channels == 4) {
			if (!result.empty()) {
				return {};
			}
			result = name;
		}
	}
	return result;
}

void ReportFailure(Stream* stream, const char* reason) {
	if (!stream->reported_failure) {
		LOGF("DualSenseHaptics: controller %d has no waveform route (%s)\n", stream->controller,
		     reason);
		stream->reported_failure = true;
	}
}

void RefreshDevice(Stream* stream, int controller) {
	if (stream->controller != controller) {
		CloseDevice(stream);
		stream->controller       = controller;
		stream->next_check       = 0;
		stream->reported_failure = false;
	}
	if (!CanUseDevice(controller)) {
		CloseDevice(stream);
		stream->next_check = 0;
		ReportFailure(stream, "requires one wired PS5 pad");
		return;
	}
	const auto now = SDL_GetTicks64();
	if (now < stream->next_check) {
		return;
	}
	stream->next_check = now + 2000;
	const auto name    = FindDevice();
	if (!name.empty() && stream->device != 0 && stream->name == name) {
		return;
	}
	CloseDevice(stream);
	if (name.empty()) {
		ReportFailure(stream, "requires one named quad endpoint");
		return;
	}
	SDL_AudioSpec desired {};
	desired.freq     = static_cast<int>(stream->freq);
	desired.format   = AUDIO_F32SYS;
	desired.channels = 4;
	desired.samples  = 256;
	desired.callback = Play;
	desired.userdata = stream;
	stream->device =
	    SDL_OpenAudioDevice(name.c_str(), 0, &desired, &stream->actual,
	                        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_FORMAT_CHANGE);
	if (stream->device != 0 && stream->actual.channels == 4 && stream->actual.freq > 0 &&
	    SDL_AUDIO_BITSIZE(stream->actual.format) != 0) {
		stream->conversion = SDL_NewAudioStream(AUDIO_F32SYS, 4, desired.freq,
		                                        stream->actual.format, 4, stream->actual.freq);
	}
	if (stream->conversion == nullptr) {
		if (stream->device != 0) {
			SDL_CloseAudioDevice(stream->device);
		}
		stream->device = 0;
		ReportFailure(stream, "quad output open/conversion failed");
		return;
	}
	stream->name = name;
	{
		Common::LockGuard lock(g_mutex);
		g_streams++;
	}
	SDL_PauseAudioDevice(stream->device, 0);
	LOGF("DualSenseHaptics: controller %d USB waveform endpoint '%s' (%d Hz, %u ch)\n", controller,
	     name.c_str(), stream->actual.freq, stream->actual.channels);
}
} // namespace

Stream* Open(uint32_t freq) {
	if (freq == 0 || SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		return nullptr;
	}
	auto* stream = new Stream;
	stream->freq = freq;
	return stream;
}

void Close(Stream* stream) {
	if (stream != nullptr) {
		CloseDevice(stream);
		delete stream;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

bool HasDevice(const Stream* stream) {
	return stream != nullptr && stream->device != 0;
}

uint64_t Queue(Stream* stream, int controller, const void* data, uint32_t frames, uint32_t channels,
               bool is_float, const int* volume, float gain) {
	if (stream == nullptr || data == nullptr || frames == 0 || channels == 0 || volume == nullptr) {
		return 0;
	}
	const auto max_frames =
	    static_cast<uint32_t>(static_cast<uint64_t>(stream->freq) * MAX_QUEUE_MS / 1000);
	const auto first_frame = frames > max_frames ? frames - max_frames : 0;
	frames -= first_frame;
	if (frames == 0) {
		return 0;
	}
	RefreshDevice(stream, controller);
	if (stream->device == 0) {
		return 0;
	}
	stream->buffer.assign(static_cast<size_t>(frames) * 4, 0.0f);
	bool audible = false;
	for (uint32_t frame = 0; frame < frames; frame++) {
		for (uint32_t ch = 0; ch < 2; ch++) {
			const auto src_ch = channels == 1 ? 0 : ch;
			const auto index  = (static_cast<size_t>(first_frame) + frame) * channels + src_ch;
			float      value  = is_float ? static_cast<const float*>(data)[index]
			                             : static_cast<const int16_t*>(data)[index] / 32768.0f;
			value *= volume[src_ch] / 32768.0f * gain;
			if (!std::isfinite(value)) {
				value = 0.0f;
			}
			stream->buffer[static_cast<size_t>(frame) * 4 + ch + 2] = value;
			audible |= std::fabs(value) > 1.0f / 1024;
		}
	}
	SDL_LockAudioDevice(stream->device);
	const int frame_bytes = 4 * (SDL_AUDIO_BITSIZE(stream->actual.format) / 8);
	const int max_bytes   = static_cast<int>(static_cast<uint64_t>(stream->actual.freq) *
	                                         frame_bytes * MAX_QUEUE_MS / 1000);
	int       queued      = std::max(0, SDL_AudioStreamAvailable(stream->conversion));
	const int incoming =
	    static_cast<int>((static_cast<uint64_t>(frames) * stream->actual.freq + stream->freq - 1) /
	                     stream->freq) *
	    frame_bytes;
	std::array<uint8_t, 16384> discarded {};
	while (queued + incoming > max_bytes && queued > 0 && frame_bytes > 0) {
		const int excess        = queued + incoming - max_bytes;
		const int excess_frames = (excess + frame_bytes - 1) / frame_bytes;
		const int drop          = std::min({queued / frame_bytes, excess_frames,
		                                    static_cast<int>(discarded.size()) / frame_bytes}) *
		                          frame_bytes;
		if (drop == 0 || SDL_AudioStreamGet(stream->conversion, discarded.data(), drop) <= 0) {
			break;
		}
		queued = std::max(0, SDL_AudioStreamAvailable(stream->conversion));
	}
	if (queued + incoming > max_bytes) {
		SDL_AudioStreamClear(stream->conversion);
	}
	const bool success = SDL_AudioStreamPut(stream->conversion, stream->buffer.data(),
	                                        static_cast<int>(frames * FRAME_BYTES)) == 0;
	// SDL resampling can retain a short input block until the next submission. Count that accepted
	// block as queued even when no converted output is available yet.
	const auto available_us =
	    static_cast<uint64_t>(std::max(0, SDL_AudioStreamAvailable(stream->conversion))) * 1000000 /
	    (stream->actual.freq * frame_bytes);
	const auto queued_us = success
	                           ? std::max<uint64_t>(available_us, static_cast<uint64_t>(frames) *
	                                                                  1000000 / stream->freq)
	                           : 0;
	if (success) {
		Common::LockGuard lock(g_mutex);
		SelectController(controller);
		if (audible) {
			const auto now      = SDL_GetTicks64();
			const bool starting = g_haptics_until <= now;
			g_haptics_until =
			    std::max(g_haptics_until, now + std::max<uint64_t>(queued_us / 1000, 250));
			if (starting) {
				ApplyRumble();
			}
		}
	}
	SDL_UnlockAudioDevice(stream->device);
	if (!success) {
		CloseDevice(stream);
		stream->next_check = SDL_GetTicks64() + 2000;
		ReportFailure(stream, "waveform conversion failed");
	}
	return queued_us;
}

bool SetVibration(int controller, uint8_t large_motor, uint8_t small_motor, uint32_t duration_ms) {
	SDL_LockJoysticks();
	auto*      pad    = SDL_GameControllerFromInstanceID(controller);
	const bool is_ps5 = pad != nullptr && SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_PS5;
	SDL_UnlockJoysticks();
	if (!is_ps5) {
		return false;
	}
	Common::LockGuard lock(g_mutex);
	SelectController(controller);
	g_large_motor  = large_motor;
	g_small_motor  = small_motor;
	g_rumble_until = SDL_GetTicks64() + duration_ms;
	ApplyRumble();
	return true;
}

void Shutdown() {
	Common::LockGuard lock(g_mutex);
	SelectController(-1);
}

} // namespace Libs::Controller::DualSenseHaptics
