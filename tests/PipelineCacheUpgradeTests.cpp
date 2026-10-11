#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "common/file.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <xxhash.h>

namespace Cache = Libs::Graphics::PipelineCacheFile;
namespace {
int  failures = 0;
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PipelineCacheUpgradeTests: failed: %s\n", message);
		failures++;
	}
}
void Word(std::vector<uint8_t>& bytes, uint32_t value) {
	for (unsigned i = 0; i < 4; i++)
		bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
std::vector<uint8_t> Fixture(const std::string& signature, const std::vector<uint8_t>& payload) {
	std::vector<uint8_t> file(signature.begin(), signature.end());
	const auto           hash = XXH3_64bits(payload.data(), payload.size());
	for (unsigned i = 0; i < 8; i++)
		file.push_back(static_cast<uint8_t>(hash >> (i * 8)));
	file.insert(file.end(), payload.begin(), payload.end());
	return file;
}
// Synthetic Vulkan version-one header, constructed from its specified little-endian fields.
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

// Synthetic cache bytes only; this uses the application's actual file API.
bool NativeOperation(bool result, const char* message, const char* scenario,
                     const std::filesystem::path& path) {
#ifdef _WIN32
	const auto error = GetLastError();
#else
	const auto error = errno;
#endif
	Check(result, message);
	if (!result) {
		std::fprintf(stderr, "Native cache I/O: %s, %s, logical-length=%zu, native-error=%lu\n",
		             scenario, message, path.native().size(), static_cast<unsigned long>(error));
	}
	return result;
}

std::filesystem::path DestinationLength(std::filesystem::path root, size_t length) {
	const std::filesystem::path name          = "cache.bin";
	const auto                  parent_length = length - name.native().size() - 1;
	while (root.native().size() + 61 < parent_length)
		root /= std::string(60, 'd');
	if (root.native().size() < parent_length) {
		const auto remaining = parent_length - root.native().size();
		root /= std::string(remaining > 1 ? remaining - 1 : 1, 'd');
	}
	return root / name;
}

bool WriteNative(const std::filesystem::path& logical, const std::vector<uint8_t>& bytes) {
	Common::File output;
	if (!output.Create(Cache::NativePath(logical))) return false;
	uint32_t written = 0;
	output.Write(bytes.data(), static_cast<uint32_t>(bytes.size()), &written);
	const bool flushed = output.Flush();
	output.Close();
	return flushed && written == bytes.size();
}

std::vector<uint8_t> ReadNative(const std::filesystem::path& logical) {
	Common::File input;
	if (!input.Open(Cache::NativePath(logical), Common::File::Mode::Read)) return {};
	const auto size = input.Size();
	if (size > Cache::MaxFileBytes) return {};
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	uint32_t             read = 0;
	input.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
	input.Close();
	return read == bytes.size() ? bytes : std::vector<uint8_t> {};
}

void ExerciseNativeIO(const std::filesystem::path& destination, const char* scenario,
                      const Cache::Identity& identity, const std::vector<uint8_t>& bytes,
                      const std::vector<uint8_t>& payload) {
	if (!NativeOperation(Cache::CreateDirectories(destination.parent_path()),
	                     "native cache directory creation must succeed", scenario, destination))
		return;
	const auto first = Cache::Temporary(destination);
	if (!NativeOperation(!first.empty(), "native cache temporary reservation must succeed",
	                     scenario, destination))
		return;
	const auto second = Cache::Temporary(destination);
	Check(!second.empty() && first != second, "native writers must own distinct reserved paths");
	if (second.empty()) return;
	Check(Common::File::IsFileExisting(Cache::NativePath(first)) &&
	          Common::File::IsFileExisting(Cache::NativePath(second)),
	      "native reserved paths must exist");
	if (!NativeOperation(WriteNative(first, bytes),
	                     "native complete cache write and flush must succeed", scenario, first))
		return;
	Common::File unfinished;
	if (!NativeOperation(unfinished.Create(Cache::NativePath(second)),
	                     "native unfinished writer create must succeed", scenario, second))
		return;
	const uint8_t partial         = 0x31;
	uint32_t      partial_written = 0;
	unfinished.Write(&partial, 1, &partial_written);
	Check(partial_written == 1, "second native writer must contain only its unfinished byte");
	Check(unfinished.Flush(), "unfinished native writer flush must succeed");
	if (!NativeOperation(Cache::Replace(first, destination),
	                     "native complete cache replacement must succeed", scenario, destination))
		return;
	Check(Common::File::IsFileExisting(Cache::NativePath(destination)),
	      "native published cache must exist");
	const auto saved = ReadNative(destination);
	Check(saved == bytes && Cache::Decode(saved, identity).payload == payload,
	      "native write/replace/read/decode must preserve exact complete bytes");
	Check(Common::File::IsFileExisting(Cache::NativePath(second)),
	      "publishing writer A must leave writer B's reserved path intact");
	unfinished.Close();
	const auto incomplete = ReadNative(second);
	Check(incomplete == std::vector<uint8_t> {partial} &&
	          Cache::Decode(incomplete, identity).payload.empty(),
	      "native publication must not consume another writer's incomplete bytes");
	Check(!Cache::Replace(destination.parent_path() / "missing-source.tmp", destination),
	      "native missing-source replacement must fail");
	Check(ReadNative(destination) == bytes,
	      "native failed replacement must retain published bytes");
	const auto missing = destination.parent_path() / "missing-parent" / "cache.bin";
	Check(Cache::Temporary(missing).empty(),
	      "native missing-parent reservation must remain a failure");
	Check(!WriteNative(missing, bytes), "native missing-parent write must fail");
	Check(ReadNative(destination) == bytes, "native failed write must retain published bytes");
	const auto rejected = Cache::Temporary(destination);
	Check(!rejected.empty(), "native replacement-failure fixture must reserve its own path");
	if (!rejected.empty()) {
		Check(WriteNative(rejected, bytes),
		      "native replacement-failure fixture write must succeed");
		Check(!Cache::Replace(rejected, destination.parent_path()),
		      "native replace onto an existing directory must fail");
		Check(ReadNative(destination) == bytes && ReadNative(rejected) == bytes,
		      "native failed replace must retain destination and owned complete temporary bytes");
		Check(Common::File::DeleteFile(Cache::NativePath(rejected)),
		      "native owned temporary cleanup must succeed");
	}
	Check(Common::File::DeleteFile(Cache::NativePath(second)),
	      "native unfinished writer cleanup must succeed");
}

// Compare existing directory identities, including alternate separator spelling.
bool OwnsNativeFixtureDirectory(const std::filesystem::path& directory,
                                const std::filesystem::path& temporary_root,
                                std::error_code&             error) {
	error.clear();
	const bool same_parent =
	    std::filesystem::equivalent(directory.parent_path(), temporary_root, error);
	return !error && same_parent && directory.filename().string().starts_with("kyty-cache-native-");
}

void CheckNativeCachePaths(const Cache::Identity& identity, const std::vector<uint8_t>& bytes,
                           const std::vector<uint8_t>& payload) {
	std::error_code error;
	const auto      temporary_path = std::filesystem::temp_directory_path(error);
	Check(!error, "synthetic native fixture temporary directory lookup must succeed");
	if (error) return;
	const auto temporary_root = std::filesystem::absolute(temporary_path, error);
	Check(!error, "synthetic native fixture temporary root must resolve");
	if (error) return;
	const auto directory =
	    temporary_root /
	    ("kyty-cache-native-" +
	     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const bool owned_directory = OwnsNativeFixtureDirectory(directory, temporary_root, error);
	Check(!error, "synthetic native fixture parent directory identity must resolve");
	Check(owned_directory, "synthetic native fixture must stay in its owned temporary directory");
	if (error || !owned_directory) return;
	// Bootstrap a short owned directory before testing potentially malformed native paths.
	Check(directory.native().size() < 240,
	      "native fixture bootstrap must stay below the Windows directory limit");
	if (directory.native().size() >= 240) return;
	if (!NativeOperation(Common::File::CreateDirectories(directory),
	                     "native fixture root creation must succeed", "fixture-root", directory))
		return;
	// The existing owned child must differ from its existing parent directory.
	const bool same_fixture = std::filesystem::equivalent(directory, temporary_root, error);
	Check(!error, "native containment negative directory identity must resolve");
	Check(!error && !same_fixture,
	      "native containment negative directory must have a different identity");
	if (error || same_fixture) return;
	const bool accepted_negative =
	    OwnsNativeFixtureDirectory(directory / "kyty-cache-native-negative", temporary_root, error);
	Check(!error, "native containment negative parent identity must resolve");
	Check(!error && !accepted_negative,
	      "native containment must reject a different existing owned parent");
	if (error || accepted_negative) return;
	// Nothing is created for either negative candidate; identity errors must fail closed.
	const bool accepted_missing = OwnsNativeFixtureDirectory(
	    directory / "missing-containment-parent" / "kyty-cache-native-negative", temporary_root,
	    error);
	Check(error && !accepted_missing,
	      "native containment must fail closed on a missing parent identity");
	if (!error || accepted_missing) return;
	error.clear();
	const auto saved_directory = std::filesystem::current_path(error);
	Check(!error, "relative native fixture must preserve current directory");
	if (error) return;
	std::filesystem::current_path(directory, error);
	Check(!error, "relative native fixture must select its owned current directory");
	if (error) return;
	const auto short_path      = directory / "short" / "cache.bin";
	const auto suffix_crossing = DestinationLength(directory / "suffix-boundary", 240);
	const auto over_limit      = DestinationLength(directory / "destination-boundary", 310);
	Check(suffix_crossing.native().size() < 260 && suffix_crossing.native().size() + 38 > 260,
	      "native suffix-boundary fixture must keep destination below260 and temporary above260");
	Check(over_limit.parent_path().native().size() > 260,
	      "native long-directory fixture must cross260 with legal component lengths");
	ExerciseNativeIO(short_path, "short-control", identity, bytes, payload);
	ExerciseNativeIO(suffix_crossing, "temporary-suffix-crosses260", identity, bytes, payload);
	ExerciseNativeIO(over_limit, "destination-directory-over260", identity, bytes, payload);
	ExerciseNativeIO(directory / std::filesystem::path(u8"unicode-\u6f22-\u03bb") / "cache.bin",
	                 "unicode", identity, bytes, payload);
	ExerciseNativeIO(std::filesystem::path("relative") / "cache.bin", "relative-root", identity,
	                 bytes, payload);
#ifdef _WIN32
	Check(Cache::NativePath(std::filesystem::path("relative") / "." / ".." / "cache.bin") ==
	          Cache::NativePath(directory / "cache.bin"),
	      "native relative normalization must resolve to the same absolute file");
#endif
	std::filesystem::current_path(saved_directory, error);
	Check(!error, "relative native fixture must restore its original current directory");
#ifdef _WIN32
	Check(Cache::NativePath(std::filesystem::path(L"\\\\server\\share\\folder\\..\\cache.bin")) ==
	          std::filesystem::path(L"\\\\?\\UNC\\server\\share\\cache.bin"),
	      "native UNC normalization must produce the extended lexical share form");
	const std::filesystem::path extended_drive(L"\\\\?\\C:\\cache\\..\\file.bin");
	const std::filesystem::path extended_unc(L"\\\\?\\UNC\\server\\share\\cache.bin");
	Check(Cache::NativePath(extended_drive) == extended_drive &&
	          Cache::NativePath(extended_unc) == extended_unc,
	      "native already-extended namespace meaning must remain unchanged");
#endif
	Check(Cache::NativePath({}).empty(), "native empty path must not select another file");
	std::filesystem::remove_all(Cache::NativePath(directory), error);
	Check(!error, "native isolated synthetic fixture cleanup must succeed");
}

} // namespace

int main() {
	Cache::Identity old {std::string(40, 'a'), 0x10de, 0x2c02, 123, {1, 2, 3, 4}, 0};
	auto            current = old;
	current.revision        = std::string(40, 'b');
	const auto payload      = Payload(old);
	const auto file         = Cache::Encode(payload, old);
	const auto original     = file;
	Check(Cache::Decode(file, old).payload == payload, "same build/device baseline must load");
	Check(Cache::Decode(file, current).payload == payload,
	      "unrelated revision change must retain compatible driver data");
	current                 = old;
	current.instrumentation = 1;
	Check(Cache::Decode(file, current).payload.empty(),
	      "instrumented mode must not reuse normal cache");
	current = old;
	current.driver++;
	Check(Cache::Decode(file, current).payload.empty(), "driver change must reject cache");
	current = old;
	current.uuid[0]++;
	Check(Cache::Decode(file, current).payload.empty(), "UUID change must reject cache");
	current                = old;
	current.layer_settings = 1;
	Check(Cache::Decode(file, current).payload.empty(),
	      "validation layer settings must not share driver cache");
	current          = old;
	current.revision = "unknown";
	Check(Cache::Decode(file, current).payload == payload,
	      "missing provenance must not invalidate device-compatible data");
	auto wrong_vendor = payload;
	wrong_vendor[8] ^= 1;
	Check(Cache::Decode(Fixture(Cache::Signature(old), wrong_vendor), old).error ==
	          Cache::Error::VulkanHeader,
	      "outer identity must not hide incompatible Vulkan header vendor");
	auto wrong_uuid = payload;
	wrong_uuid[16] ^= 1;
	Check(Cache::Decode(Fixture(Cache::Signature(old), wrong_uuid), old).error ==
	          Cache::Error::VulkanHeader,
	      "outer identity must not hide incompatible Vulkan header UUID");
	auto version = payload;
	version[4]   = 2;
	Check(Cache::Decode(Fixture(Cache::Signature(old), version), old).error ==
	          Cache::Error::VulkanHeader,
	      "unsupported Vulkan header version must reject");
	const auto legacy =
	    Fixture("KytyPC1:" + old.revision +
	                ":000010de:00002c02:0000007b:01020304000000000000000000000000\n",
	            payload);
	Check(Cache::Decode(legacy, old).error == Cache::Error::Legacy,
	      "unverified legacy instrumentation must reject");
	auto corrupt = file;
	corrupt.back() ^= 1;
	Check(Cache::Decode(corrupt, old).error == Cache::Error::Checksum,
	      "payload checksum must reject damage");
	const auto malformed = Fixture(Cache::Signature(old), std::vector<uint8_t>(36, 0));
	Check(Cache::Decode(malformed, old).payload.empty(),
	      "hash-valid invalid Vulkan header must reject");
	Check(Cache::Decode(std::span(file).first(4), old).payload.empty(), "truncation must reject");
	Check(file == original, "decode must preserve imported original bytes");
	const auto directory =
	    std::filesystem::temp_directory_path() /
	    ("kyty-cache-upgrade-" +
	     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code error;
	if (!std::filesystem::create_directory(directory, error)) {
		Check(false, "could not create isolated synthetic I/O fixture");
		return 1;
	}
	const auto destination = directory / "original.bin";
	const auto retained    = directory / "retained root";
	current                = old;
	current.revision       = std::string(40, 'b');
	Check(Cache::Path("SYNTHETIC", retained, current) == Cache::Path("SYNTHETIC", retained, old),
	      "retained path must survive unrelated source updates");
	Check(Cache::Path("SYNTHETIC", retained, old).string().starts_with(retained.string()),
	      "retained root must contain output");
	current.instrumentation = 1;
	Check(Cache::Path("SYNTHETIC", retained, current) != Cache::Path("SYNTHETIC", retained, old),
	      "mode changes must preserve previous cache in separate output");
	{
		std::ofstream output(destination, std::ios::binary);
		output.write(reinterpret_cast<const char*>(original.data()), original.size());
		Check(bool(output), "could not write original fixture");
	}
	Check(!Cache::Replace(directory / "missing.tmp", destination),
	      "missing temporary source must fail replacement");
	{
		std::ifstream              input(destination, std::ios::binary);
		const std::vector<uint8_t> saved {std::istreambuf_iterator<char>(input),
		                                  std::istreambuf_iterator<char>()};
		Check(saved == original, "failed replacement must preserve existing original cache");
	}
	const auto temporary = directory / "complete.tmp";
	{
		std::ofstream output(temporary, std::ios::binary);
		output.write(reinterpret_cast<const char*>(original.data()), original.size());
		Check(bool(output), "could not write completed replacement fixture");
	}
	Check(Cache::Replace(temporary, destination), "completed replacement must succeed");
	{
		std::ifstream              input(destination, std::ios::binary);
		const std::vector<uint8_t> saved {std::istreambuf_iterator<char>(input),
		                                  std::istreambuf_iterator<char>()};
		Check(saved == original, "completed replacement bytes must be exact");
	}
	// Two builds can share the retained root. Writer A must publish only its own completed bytes.
	const auto first_writer  = Cache::Temporary(destination);
	const auto second_writer = Cache::Temporary(destination);
	Check(!first_writer.empty() && !second_writer.empty() && first_writer != second_writer,
	      "each writer must reserve a distinct temporary file");
	Check(std::filesystem::is_regular_file(first_writer) &&
	          std::filesystem::is_regular_file(second_writer),
	      "temporary files must be reserved before writing");
	Check(Cache::Temporary(directory / "missing-directory" / "cache.bin").empty(),
	      "failed reservation must leave the destination untouched");
	{
		std::ofstream file(first_writer, std::ios::binary);
		file << "completed writer A";
	}
	{
		std::ofstream file(second_writer, std::ios::binary);
		file << "unfinished writer B";
	}
	Check(Cache::Replace(first_writer, destination), "first writer publication must succeed");
	{
		std::ifstream     file(destination, std::ios::binary);
		const std::string saved((std::istreambuf_iterator<char>(file)), {});
		Check(saved == "completed writer A",
		      "one writer must never publish another writer's unfinished bytes");
	}
	std::filesystem::remove_all(directory, error);
	CheckNativeCachePaths(old, original, payload);
	std::printf("PipelineCacheUpgradeTests: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
