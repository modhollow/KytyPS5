#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::PipelineCacheFile {

struct Identity {
	// Source revision is diagnostic provenance, not opaque-driver compatibility.
	std::string             revision;
	uint32_t                vendor = 0;
	uint32_t                device = 0;
	uint32_t                driver = 0;
	std::array<uint8_t, 16> uuid {};
	uint32_t                instrumentation = 0;
	uint64_t                layer_settings  = 0;
};

enum class Error { None, Size, Identity, Checksum, VulkanHeader, Legacy };
struct Decoded {
	std::vector<uint8_t> payload;
	Error                error = Error::None;
};

inline constexpr size_t MaxFileBytes    = 1024ull * 1024 * 1024;
inline constexpr size_t MaxPayloadBytes = MaxFileBytes - 512;

std::string           Prefix(std::span<const uint8_t> payload, const Identity& identity,
                             size_t max_bytes = MaxPayloadBytes);
std::filesystem::path Path(std::string_view title, const std::filesystem::path& root,
                           const Identity& identity);
std::string           Signature(const Identity& identity);
Decoded               Decode(std::span<const uint8_t> file, const Identity& identity,
                             size_t max_bytes = MaxFileBytes);
std::vector<uint8_t>  Encode(std::span<const uint8_t> payload, const Identity& identity);
std::string           Compatibility(const Identity& identity);
Decoded               Read(const std::filesystem::path& path, const Identity& identity,
                           size_t max_bytes = MaxFileBytes);
bool                  Current(const std::filesystem::path& path, const Identity& identity,
                              const std::filesystem::path& seeds);
const char*           Reason(Error error);
std::filesystem::path Temporary(const std::filesystem::path& destination);
bool Replace(const std::filesystem::path& temporary, const std::filesystem::path& destination);

} // namespace Libs::Graphics::PipelineCacheFile
