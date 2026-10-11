#include "libs/avPlayer.cpp"
#include "common/archive.h"
#include "ArchiveTestFixture.h"

#include <array>
#include <chrono>

namespace {
std::filesystem::path content_root;
}

namespace Libs::LibKernel {
int KYTY_SYSV_ABI PthreadCreate(Pthread*, const PthreadAttr*, pthread_entry_func_t, void*, const char*) {
	std::abort();
}
int KYTY_SYSV_ABI PthreadJoin(Pthread, void**) { std::abort(); }
namespace FileSystem {
std::filesystem::path GetRealFilename(const std::string& path) {
	return path.starts_with("/app0/") ? content_root / path.substr(6) : std::filesystem::path {};
}
}
}

namespace {
using namespace Libs::Audio::AvPlayer;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "AvPlayerFileTests: %s\n", message);
		std::abort();
	}
}

void CheckStream(FileStreamer& stream, const std::vector<uint8_t>& data) {
	auto* io = stream.Context();
	Check(io->seek(io->opaque, 0, AVSEEK_SIZE) == static_cast<int64_t>(data.size()), "query size");
	Check(io->seek(io->opaque, 65531, SEEK_SET | AVSEEK_FORCE) == 65531, "seek across block boundary");
	std::array<uint8_t, 32> bytes {};
	Check(io->read_packet(io->opaque, bytes.data(), bytes.size()) == bytes.size() &&
	          std::equal(bytes.begin(), bytes.end(), data.begin() + 65531), "read across block boundary");
	Check(io->seek(io->opaque, -16, SEEK_CUR) == 65547, "seek relative to cursor");
	Check(io->seek(io->opaque, INT64_MIN, SEEK_CUR) < 0 &&
	          io->seek(io->opaque, INT64_MAX, SEEK_END) < 0 &&
	          io->seek(io->opaque, 0, 12345) < 0, "reject invalid and overflowing seeks");
	Check(io->seek(io->opaque, 0, SEEK_CUR) == 65547, "rejected seeks preserve the cursor");
	Check(io->seek(io->opaque, -7, SEEK_END) == static_cast<int64_t>(data.size() - 7), "seek near end");
	Check(io->read_packet(io->opaque, bytes.data(), bytes.size()) == 7 &&
	          std::equal(bytes.begin(), bytes.begin() + 7, data.end() - 7), "short read at end");
	Check(io->read_packet(io->opaque, bytes.data(), bytes.size()) == AVERROR_EOF, "report EOF");
	Check(io->seek(io->opaque, 11, SEEK_END) == static_cast<int64_t>(data.size() + 11) &&
	          io->read_packet(io->opaque, bytes.data(), bytes.size()) == AVERROR_EOF, "read beyond end");
	Check(io->seek(io->opaque, 0, SEEK_SET) == 0, "rewind");
	std::vector<uint8_t> buffered(data.size());
	Check(avio_read(io, buffered.data(), static_cast<int>(buffered.size())) == buffered.size() &&
	          buffered == data, "FFmpeg buffered reads preserve content");
	Check(avio_seek(io, 123, SEEK_SET) == 123 &&
	          avio_read(io, bytes.data(), bytes.size()) == bytes.size() &&
	          std::equal(bytes.begin(), bytes.end(), data.begin() + 123), "FFmpeg seeks reset buffered reads");
}

struct Callbacks {
	const std::vector<uint8_t>& data;
	unsigned opens = 0;
	unsigned closes = 0;
	bool premature_eof = false;
	bool oversized_read = false;
	bool reject_open = false;
	int read_error = 0;
	unsigned reads = 0;
	std::optional<uint64_t> reported_size;
};
int KYTY_SYSV_ABI Open(void* object, const char*) {
	auto& state = *static_cast<Callbacks*>(object);
	++state.opens;
	return state.reject_open ? -1 : 0;
}
int KYTY_SYSV_ABI Close(void* object) {
	++static_cast<Callbacks*>(object)->closes;
	return 0;
}
int KYTY_SYSV_ABI Read(void* object, uint8_t* buffer, uint64_t offset, uint32_t size) {
	auto& state = *static_cast<Callbacks*>(object);
	++state.reads;
	if (state.read_error != 0) return state.read_error;
	if (state.oversized_read) return static_cast<int>(size + 1);
	if (offset >= state.data.size()) return 0;
	if (state.premature_eof) {
		return 0;
	}
	const auto bytes = std::min<uint64_t>(size, state.data.size() - offset);
	std::memcpy(buffer, state.data.data() + offset, bytes);
	return static_cast<int>(bytes);
}
uint64_t KYTY_SYSV_ABI Size(void* object) {
	const auto& state = *static_cast<Callbacks*>(object);
	return state.reported_size.value_or(state.data.size());
}

void CheckReadFailures(const std::vector<uint8_t>& payload) {
	Callbacks callbacks {payload};
	{
		FileStreamer stream({&callbacks, Open, Close, Read, Size});
		Check(stream.Init("callback"), "open read-error fixture");
		auto* io = stream.Context();
		Check(io->seek(io->opaque, 7, SEEK_SET) == 7, "position read-error fixture");
		uint8_t byte = 0;
		Check(io->read_packet(io->opaque, &byte, 0) == AVERROR(EINVAL) &&
		          io->read_packet(io->opaque, &byte, -1) == AVERROR(EINVAL) && callbacks.reads == 0,
		      "reject nonpositive reads before callback");
		Check(io->seek(io->opaque, 0, SEEK_CUR) == 7, "invalid reads preserve cursor");
		callbacks.read_error = AVERROR(EACCES);
		Check(io->read_packet(io->opaque, &byte, 1) == AVERROR(EACCES) &&
		          io->seek(io->opaque, 0, SEEK_CUR) == 7, "callback errors preserve cursor");
		callbacks.read_error = 0;
		callbacks.oversized_read = true;
		Check(io->read_packet(io->opaque, &byte, 1) == AVERROR(EIO) &&
		          io->seek(io->opaque, 0, SEEK_CUR) == 7, "oversized callback reads preserve cursor");
		callbacks.oversized_read = false;
		callbacks.premature_eof = true;
		Check(io->read_packet(io->opaque, &byte, 1) == AVERROR(EIO) &&
		          io->seek(io->opaque, 0, SEEK_CUR) == 7, "premature EOF preserves cursor");
		callbacks.premature_eof = false;
		Check(io->read_packet(io->opaque, &byte, 1) == 1 && byte == payload[7] &&
		          io->seek(io->opaque, 0, SEEK_CUR) == 8, "read recovers at preserved cursor");
	}
	Check(callbacks.opens == 1 && callbacks.closes == 1, "read-error fixture closes exactly once");
	for (const auto size : {uint64_t {0}, uint64_t {INT64_MAX} + 1}) {
		Callbacks invalid {payload};
		invalid.reported_size = size;
		{
			FileStreamer stream({&invalid, Open, Close, Read, Size});
			Check(!stream.Init("callback") && stream.Context() == nullptr, "reject invalid callback size");
		}
		Check(invalid.opens == 1 && invalid.closes == 1 && invalid.reads == 0,
		      "invalid-size callback closes exactly once");
	}
	Callbacks rejected {payload};
	rejected.reject_open = true;
	{
		FileStreamer stream({&rejected, Open, Close, Read, Size});
		Check(!stream.Init("callback"), "reject callback open failure");
	}
	Check(rejected.opens == 1 && rejected.closes == 0 && rejected.reads == 0,
	      "failed callback open does not close unopened input");
}

std::vector<uint8_t> MakeWave() {
	// A mono 16-bit PCM WAV, generated here; Init only probes it and starts no workers.
	std::vector<uint8_t> bytes;
	const auto text = [&](const char* value) { bytes.insert(bytes.end(), value, value + 4); };
	const auto word = [&](uint32_t value, unsigned count) {
		for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
	};
	text("RIFF"); word(36 + 128, 4); text("WAVE"); text("fmt "); word(16, 4);
	word(1, 2); word(1, 2); word(8000, 4); word(16000, 4); word(2, 2); word(16, 2);
	text("data"); word(128, 4);
	for (uint32_t i = 0; i < 64; ++i) word(i * 113, 2);
	return bytes;
}

void CheckSource(const std::string& path, AvPlayerFileReplacement replacement, bool valid) {
	Source source({}, replacement, 2, false, 0);
	const auto result = source.Init(path, AvPlayerSourceUnknown);
	Check(valid ? result == 0 && source.StreamCount() == 1
	            : result == AVPLAYER_ERROR_OPERATION_FAILED && source.StreamCount() == 0,
	      valid ? "Source::Init probes real custom input" : "Source::Init rejects missing or invalid input");
}

void CheckSourceRoutes(const std::filesystem::path& root, const std::vector<uint8_t>& invalid_payload) {
	const auto wave = MakeWave();
	{
		Common::File output;
		Check(output.Create(root / "probe.wav"), "create synthetic WAV");
		output.Write(wave.data(), static_cast<uint32_t>(wave.size()));
	}
	Check(ArchiveTests::CreateArchive(root / "probe.zar", wave), "create synthetic WAV archive");
	content_root = root;
	CheckSource("/app0/probe.wav", {}, true);
	CheckSource("/app0/missing.wav", {}, false);
	CheckSource("/unmounted/probe.wav", {}, false);
	CheckSource("/app0/movie.bin", {}, false);
	Callbacks ignored {wave};
	// The absence of open selects native/archive input; other callback slots are ignored.
	CheckSource("/app0/probe.wav", {&ignored, nullptr, Close, Read, Size}, true);
	content_root = Common::MakeArchivePath(root / "probe.zar");
	CheckSource("/app0/assets/subdir/data.bin", {}, true);
	CheckSource("/app0/missing.wav", {}, false);
	CheckSource("/app0/assets/subdir/data.bin", {&ignored, nullptr, Close, Read, Size}, true);
	Check(ignored.opens == 0 && ignored.closes == 0 && ignored.reads == 0,
	      "native and archive fallback ignore partial callback slots");
	Callbacks callback {wave};
	CheckSource("callback", {&callback, Open, Close, Read, Size}, true);
	Check(callback.opens == 1 && callback.closes == 1, "Source successful callback closes once");
	Callbacks malformed {invalid_payload};
	CheckSource("callback", {&malformed, Open, Close, Read, Size}, false);
	Check(malformed.opens == 1 && malformed.closes == 1, "Source FFmpeg failure closes callback once");
	for (unsigned missing = 0; missing < 3; ++missing) {
		Callbacks incomplete {wave};
		AvPlayerFileReplacement replacement {&incomplete, Open, Close, Read, Size};
		if (missing == 0) replacement.close = nullptr;
		if (missing == 1) replacement.read_offset = nullptr;
		if (missing == 2) replacement.size = nullptr;
		CheckSource("callback", replacement, false);
		Check(incomplete.opens == 0 && incomplete.closes == 0 && incomplete.reads == 0,
		      "Source rejects incomplete callbacks before opening");
	}
	content_root = root;
}
}

int main() {
	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty_avio_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(std::filesystem::create_directories(root), "create fixture directory");
	std::vector<uint8_t> payload(128 * 1024 + 37);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i * 37);
	}
	{
		Common::File output;
		Check(output.Create(root / "movie.bin"), "create native fixture");
		output.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	}
	Check(ArchiveTests::CreateArchive(root / "game.zar", payload), "create archive fixture");
	content_root = root;
	{
		FileStreamer native({});
		Check(native.Init("/app0/movie.bin"), "open native stream");
		CheckStream(native, payload);
	}
	content_root = Common::MakeArchivePath(root / "game.zar");
	{
		FileStreamer archive({});
		Check(archive.Init("/app0/assets/subdir/data.bin"), "open archived stream");
		CheckStream(archive, payload);
	}
	Callbacks callbacks {payload};
	const AvPlayerFileReplacement replacement {&callbacks, Open, Close, Read, Size};
	{
		FileStreamer callback(replacement);
		Check(callback.Init("callback"), "open guest callback stream");
		CheckStream(callback, payload);
		callbacks.premature_eof = true;
		auto* io = callback.Context();
		Check(io->seek(io->opaque, 0, SEEK_SET) == 0, "rewind callback");
		uint8_t byte = 0;
		Check(io->read_packet(io->opaque, &byte, 1) == AVERROR(EIO), "reject premature EOF");
	}
	Check(callbacks.opens == 1 && callbacks.closes == 1, "close callback exactly once");
	{
		FileStreamer missing({});
		Check(!missing.Init("/unmounted/movie.bin"), "reject unmapped media");
	}
	CheckReadFailures(payload);
	CheckSourceRoutes(root, payload);
	std::error_code error;
	std::filesystem::remove_all(root, error);
	Check(!error, "release all archive handles");
	std::puts("AvPlayerFileTests: all cases passed");
}
