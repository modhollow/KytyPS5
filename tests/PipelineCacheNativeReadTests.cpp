#include "common/file.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace Cache = Libs::Graphics::PipelineCacheFile;
namespace {
int failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PipelineCacheNativeReadTests: failed: %s\n", message);
		++failures;
	}
}

void Word(std::vector<uint8_t>& bytes, uint32_t value) {
	for (unsigned i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

std::vector<uint8_t> Payload(const Cache::Identity& identity) {
	std::vector<uint8_t> bytes;
	Word(bytes, 32);
	Word(bytes, 1);
	Word(bytes, identity.vendor);
	Word(bytes, identity.device);
	bytes.insert(bytes.end(), identity.uuid.begin(), identity.uuid.end());
	bytes.insert(bytes.end(), {11, 22, 33, 44});
	return bytes;
}

bool Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	Common::File output;
	if (!output.Create(Cache::NativePath(path))) return false;
	uint32_t written = 0;
	output.Write(bytes.data(), static_cast<uint32_t>(bytes.size()), &written);
	const bool flushed = output.Flush();
	output.Close();
	return flushed && written == bytes.size();
}

bool ReadCommon(const std::filesystem::path& path, const std::vector<uint8_t>& expected) {
	Common::File input;
	if (!input.Open(Cache::NativePath(path), Common::File::Mode::Read)) return false;
	if (input.Size() != expected.size()) {
		input.Close();
		return false;
	}
	std::vector<uint8_t> saved(expected.size());
	uint32_t             read = 0;
	input.Read(saved.data(), static_cast<uint32_t>(saved.size()), &read);
	input.Close();
	return read == saved.size() && saved == expected;
}

void Exercise(const std::filesystem::path& directory, const char* scenario,
              const Cache::Identity& identity, const std::vector<uint8_t>& payload,
              const std::vector<uint8_t>& bytes) {
	std::printf("Native cache reader: %s, logical-length=%zu\n", scenario,
	            (directory / "cache.bin").native().size());
	Check(Cache::CreateDirectories(directory), "owned reader directory must be created");
	const auto path          = directory / "cache.bin";
	const auto seeds         = directory / "seeds.bin";
	const bool cache_written = Write(path, bytes);
	const bool seeds_written = Write(seeds, {0x51});
	Check(cache_written && seeds_written, "synthetic cache and seed setup must succeed");
	if (!cache_written || !seeds_written) return;
	Check(ReadCommon(path, bytes), "native Common reader must preserve complete setup bytes");

	const auto decoded = Cache::Read(path, identity);
	Check(decoded.error == Cache::Error::None && decoded.payload == payload,
	      "native reader must return the exact complete decoded payload");
	Check(Cache::Current(path, identity, {}), "native cache must be current without seeds");

	const auto      written = std::filesystem::file_time_type::clock::now();
	std::error_code cache_error;
	std::error_code seed_error;
	std::filesystem::last_write_time(Cache::NativePath(path), written, cache_error);
	std::filesystem::last_write_time(Cache::NativePath(seeds), written - std::chrono::minutes(2),
	                                 seed_error);
	Check(!cache_error && !seed_error, "older-seed timestamp setup must succeed");
	if (cache_error || seed_error) return;
	Check(Cache::Current(path, identity, seeds), "cache newer than its seeds must be current");

	std::filesystem::last_write_time(Cache::NativePath(seeds), written + std::chrono::minutes(2),
	                                 seed_error);
	Check(!seed_error, "newer-seed timestamp setup must succeed");
	if (seed_error) return;
	Check(!Cache::Current(path, identity, seeds), "newer seeds must make the cache stale");
	Check(!Cache::Current(path, identity, directory / "missing-seeds.bin"),
	      "missing seeds must fail the freshness check");
	const auto missing = directory / "missing-cache.bin";
	Check(Cache::Read(missing, identity).error == Cache::Error::Size &&
	          !Cache::Current(missing, identity, {}),
	      "missing cache must fail read and freshness checks");
	auto incompatible = identity;
	++incompatible.driver;
	Check(Cache::Read(path, incompatible).error == Cache::Error::Identity &&
	          !Cache::Current(path, incompatible, {}),
	      "incompatible identity must remain rejected");
	Check(Cache::Read(path, identity, bytes.size() - 1).error == Cache::Error::Size,
	      "reader must preserve the caller's file-size bound");
}
} // namespace

int main() {
	Cache::Identity identity {std::string(40, 'a'), 0x10de, 0x2c02, 123, {1, 2, 3, 4}, 0};
	const auto      payload = Payload(identity);
	const auto      bytes   = Cache::Encode(payload, identity);
	Check(!bytes.empty(), "accepted V1 encoder must produce the synthetic setup bytes");
	std::error_code error;
	const auto      temporary = std::filesystem::temp_directory_path(error);
	Check(!error, "owned temporary root lookup must succeed");
	if (error) return 1;
	const auto root = std::filesystem::absolute(temporary, error);
	Check(!error, "owned temporary root resolution must succeed");
	if (error) return 1;
	const auto directory =
	    root / ("kyty-cache-read-native-" +
	            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const bool same_parent = std::filesystem::equivalent(directory.parent_path(), root, error);
	const bool owned       = !error && same_parent &&
	                         directory.filename().string().starts_with("kyty-cache-read-native-");
	Check(owned && directory.native().size() < 240,
	      "reader fixture must remain inside its owned short temporary directory");
	if (!owned || directory.native().size() >= 240) return 1;
	Check(Cache::CreateDirectories(directory), "owned reader fixture bootstrap must succeed");
	Exercise(directory / "short-control", "short-control", identity, payload, bytes);
	// Non-BMP U+1F680 cannot be represented by a legacy Windows ANSI code page.
	const std::filesystem::path unicode_component(u8"unicode-\U0001f680-\u6f22");
	Exercise(directory / unicode_component, "unicode-control", identity, payload, bytes);
	auto long_directory = directory / "long-reader";
	while (long_directory.native().size() <= 300)
		long_directory /= std::string(60, 'd');
	Check(long_directory.native().size() > 260,
	      "native reader fixture must exceed260 using legal component lengths");
	Exercise(long_directory, "long-reader", identity, payload, bytes);
	Exercise(long_directory / unicode_component, "long-unicode-reader", identity, payload, bytes);
	std::filesystem::remove_all(Cache::NativePath(directory), error);
	Check(!error, "owned synthetic reader fixture cleanup must succeed");
	std::printf("PipelineCacheNativeReadTests: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
