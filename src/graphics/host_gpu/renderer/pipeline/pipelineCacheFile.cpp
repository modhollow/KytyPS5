#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <xxhash.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Libs::Graphics::PipelineCacheFile {
namespace {
constexpr std::string_view Format = "KytyDriverCache1:";
bool                       Hex(std::string_view value, size_t length) {
	return value.size() == length && std::ranges::all_of(value, [](char c) {
		       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	       });
}
std::string Uuid(const Identity& identity) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(identity.uuid.size() * 2, '0');
	for (size_t i = 0; i < identity.uuid.size(); i++) {
		uuid[i * 2]     = hex[identity.uuid[i] >> 4u];
		uuid[i * 2 + 1] = hex[identity.uuid[i] & 0xfu];
	}
	return uuid;
}
uint32_t Word(std::span<const uint8_t> bytes) {
	return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
	       (uint32_t(bytes[3]) << 24);
}
bool VulkanHeader(std::span<const uint8_t> payload, const Identity& identity) {
	return payload.size() >= 32 && Word(payload) == 32 && Word(payload.subspan(4)) == 1 &&
	       Word(payload.subspan(8)) == identity.vendor &&
	       Word(payload.subspan(12)) == identity.device &&
	       std::equal(identity.uuid.begin(), identity.uuid.end(), payload.begin() + 16);
}
std::string DeviceSignature(const Identity& identity) {
	return fmt::format("{:08x}:{:08x}:{:08x}:{}:{:08x}:{:016x}\n", identity.vendor, identity.device,
	                   identity.driver, Uuid(identity), identity.instrumentation,
	                   identity.layer_settings);
}
} // namespace

std::string Signature(const Identity& identity) {
	const auto revision = Hex(identity.revision, 40) ? std::string_view(identity.revision)
	                                                 : std::string_view("unknown");
	return fmt::format("{}{}:{}", Format, revision, DeviceSignature(identity));
}

Decoded Decode(std::span<const uint8_t> file, const Identity& identity, size_t max_bytes) {
	if (file.empty() || file.size() > max_bytes || max_bytes > 2ull * MaxFileBytes)
		return {{}, Error::Size};
	const auto line_end =
	    std::find(file.begin(), file.begin() + std::min(file.size(), size_t(512)), uint8_t('\n'));
	if (line_end == file.begin() + std::min(file.size(), size_t(512))) return {{}, Error::Identity};
	const size_t           prefix_size = size_t(line_end - file.begin()) + 1;
	const std::string_view line(reinterpret_cast<const char*>(file.data()), prefix_size);
	if (line.starts_with("KytyPC")) return {{}, Error::Legacy};
	if (!line.starts_with(Format)) return {{}, Error::Identity};
	const auto revision_end = line.find(':', Format.size());
	if (revision_end == std::string_view::npos) return {{}, Error::Identity};
	const auto revision = line.substr(Format.size(), revision_end - Format.size());
	if ((!Hex(revision, 40) && revision != "unknown") ||
	    line.substr(revision_end + 1) != DeviceSignature(identity)) {
		return {{}, Error::Identity};
	}
	if (file.size() < prefix_size + sizeof(uint64_t) + 32) return {{}, Error::Size};
	uint64_t hash = 0;
	for (unsigned i = 0; i < 8; i++)
		hash |= uint64_t(file[prefix_size + i]) << (i * 8);
	const auto payload = file.subspan(prefix_size + sizeof(hash));
	if (XXH3_64bits(payload.data(), payload.size()) != hash) return {{}, Error::Checksum};
	if (!VulkanHeader(payload, identity)) return {{}, Error::VulkanHeader};
	return {{payload.begin(), payload.end()}, Error::None};
}

std::string Prefix(std::span<const uint8_t> payload, const Identity& identity, size_t max_bytes) {
	if (payload.size() > max_bytes || max_bytes > 2ull * MaxFileBytes - 512 ||
	    !VulkanHeader(payload, identity))
		return {};
	auto       prefix = Signature(identity);
	const auto hash   = XXH3_64bits(payload.data(), payload.size());
	for (unsigned i = 0; i < 8; i++)
		prefix.push_back(static_cast<char>(hash >> (i * 8)));
	return prefix;
}

std::vector<uint8_t> Encode(std::span<const uint8_t> payload, const Identity& identity) {
	const auto prefix = Prefix(payload, identity);
	if (prefix.empty()) return {};
	std::vector<uint8_t> file(prefix.begin(), prefix.end());
	file.insert(file.end(), payload.begin(), payload.end());
	return file;
}

std::filesystem::path Path(std::string_view title, const std::filesystem::path& root,
                           const Identity& identity) {
	auto device = DeviceSignature(identity);
	device.pop_back();
	std::replace(device.begin(), device.end(), ':', '-');
	return root / "driver-v1" / device / (std::string(title) + ".bin");
}

std::filesystem::path Temporary(const std::filesystem::path& destination) {
	static std::atomic<uint64_t> sequence {0};
	for (unsigned attempt = 0; attempt < 64; ++attempt) {
		const auto stamp =
		    static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		auto temporary = destination;
		temporary += fmt::format(".tmp.{:016x}.{:016x}", stamp, sequence.fetch_add(1));
		// Reserve in the destination directory without truncating another writer's file.
#ifdef _WIN32
		const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		                              FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			CloseHandle(file);
			return temporary;
		}
		const auto error = GetLastError();
		if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return {};
#else
		const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (file >= 0) {
			::close(file);
			return temporary;
		}
		if (errno != EEXIST) return {};
#endif
	}
	return {};
}

bool Replace(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
	return MoveFileExW(temporary.c_str(), destination.c_str(),
	                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	std::error_code error;
	std::filesystem::rename(temporary, destination, error);
	return !error;
#endif
}

std::string Compatibility(const Identity& identity) {
	return DeviceSignature(identity);
}

Decoded Read(const std::filesystem::path& path, const Identity& identity, size_t max_bytes) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) return {{}, Error::Size};
	const auto end = file.tellg();
	if (end <= 0 || end > std::streamoff(max_bytes) || max_bytes > 2ull * MaxFileBytes)
		return {{}, Error::Size};
	std::vector<uint8_t> bytes(static_cast<size_t>(end));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!file) return {{}, Error::Size};
	return Decode(bytes, identity, max_bytes);
}

bool Current(const std::filesystem::path& path, const Identity& identity,
             const std::filesystem::path& seeds) {
	if (!seeds.empty()) {
		std::error_code error;
		const auto      written = std::filesystem::last_write_time(path, error);
		if (error) return false;
		const auto seeded = std::filesystem::last_write_time(seeds, error);
		if (error || written < seeded) return false;
	}
	const auto decoded = Read(path, identity);
	return decoded.error == Error::None && !decoded.payload.empty();
}

const char* Reason(Error error) {
	switch (error) {
		case Error::None: return "compatible";
		case Error::Size: return "invalid file size";
		case Error::Identity: return "incompatible format, device, driver or instrumentation";
		case Error::Checksum: return "damaged cache payload";
		case Error::VulkanHeader: return "invalid Vulkan cache header";
		case Error::Legacy:
			return "legacy cache lacks verified instrumentation identity; original retained";
	}
	return "invalid cache";
}

} // namespace Libs::Graphics::PipelineCacheFile
