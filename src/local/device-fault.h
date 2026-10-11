#pragma once

// On device loss: what VK_EXT_device_fault reports (the faulting GPU addresses), and the buffers
// with device addresses whose ranges held them, live or destroyed (work recorded against a buffer
// freed under it faults on its address). After a loss the timeline semaphores read UINT64_MAX:
// everything looks complete and is freed, so the validation layer's "destroyed while in use" errors
// that follow are symptoms; the address here is the cause's.

#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace DeviceFault {

struct BufferSpan {
	uint64_t size = 0, cpu = 0, created_ns = 0, destroyed_ns = 0;
};
struct Registry {
	std::atomic<VkDevice>       device {VK_NULL_HANDLE};
	PFN_vkGetDeviceFaultInfoEXT get = nullptr;
	std::mutex                  mutex;
	std::multimap<uint64_t, BufferSpan>
	                  buffers; // by device address (an address reused after a free: both)
	std::atomic<bool> reported {false};
};
inline Registry g_registry;

[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// The device was created with the extension and its feature.
inline void Enable(VkDevice device, PFN_vkGetDeviceProcAddr get_proc) {
	g_registry.get =
	    reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(get_proc(device, "vkGetDeviceFaultInfoEXT"));
	if (g_registry.get != nullptr) g_registry.device.store(device, std::memory_order_release);
}

inline void NoteCreated(uint64_t device, uint64_t size, uint64_t cpu) {
	if (device == 0 || g_registry.device.load(std::memory_order_relaxed) == VK_NULL_HANDLE) return;
	std::lock_guard lock(g_registry.mutex);
	// (Bounded: the destroyed ones leave first.)
	if (g_registry.buffers.size() >= (size_t {1} << 20u))
		std::erase_if(g_registry.buffers,
		              [](const auto& item) { return item.second.destroyed_ns != 0; });
	g_registry.buffers.emplace(device, BufferSpan {size, cpu, NowNs(), 0});
}

inline void NoteDestroyed(uint64_t device) {
	if (device == 0 || g_registry.device.load(std::memory_order_relaxed) == VK_NULL_HANDLE) return;
	std::lock_guard lock(g_registry.mutex);
	const auto [first, last] = g_registry.buffers.equal_range(device);
	for (auto it = first; it != last; ++it)
		if (it->second.destroyed_ns == 0) {
			it->second.destroyed_ns = NowNs();
			break;
		}
}

inline const char* AddressType(VkDeviceFaultAddressTypeEXT type) {
	switch (type) {
		case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read invalid";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write invalid";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute invalid";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
			return "instruction pointer unknown";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
			return "instruction pointer invalid";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
			return "instruction pointer fault";
		default: return "?";
	}
}

// Once, at the first sign of the loss (any thread).
inline void Report() {
	const auto device = g_registry.device.load(std::memory_order_acquire);
	if (device == VK_NULL_HANDLE || g_registry.reported.exchange(true)) return;
	VkDeviceFaultCountsEXT counts {};
	counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
	if (g_registry.get(device, &counts, nullptr) != VK_SUCCESS) {
		std::printf("Device fault: no report\n");
		std::fflush(stdout);
		return;
	}
	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	counts.vendorBinarySize = 0;
	VkDeviceFaultInfoEXT info {};
	info.sType         = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	const auto result  = g_registry.get(device, &counts, &info);
	std::printf("Device fault (%d): %s; %u addresses, %u vendor records\n",
	            static_cast<int>(result), info.description, counts.addressInfoCount,
	            counts.vendorInfoCount);
	const auto      now = NowNs();
	std::lock_guard lock(g_registry.mutex);
	for (uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
		const auto& address = addresses[i];
		// (The precision is a power of two: the fault is somewhere in the aligned range of that
		// size.)
		const auto precision = address.addressPrecision != 0 ? address.addressPrecision : 1;
		const auto begin     = address.reportedAddress & ~(precision - 1);
		const auto end       = begin + precision;
		std::printf("Device fault address %u: %s 0x%llx (precision 0x%llx)\n", i,
		            AddressType(address.addressType),
		            static_cast<unsigned long long>(address.reportedAddress),
		            static_cast<unsigned long long>(address.addressPrecision));
		uint32_t shown = 0;
		for (const auto& [device_address, span]: g_registry.buffers) {
			if (device_address >= end || device_address + span.size <= begin) continue;
			if (++shown > 16) break;
			std::printf("  buffer device 0x%llx size 0x%llx guest 0x%llx: created %.1f ms ago, ",
			            static_cast<unsigned long long>(device_address),
			            static_cast<unsigned long long>(span.size),
			            static_cast<unsigned long long>(span.cpu),
			            static_cast<double>(now - span.created_ns) / 1e6);
			if (span.destroyed_ns != 0)
				std::printf("destroyed %.1f ms ago\n",
				            static_cast<double>(now - span.destroyed_ns) / 1e6);
			else
				std::printf("live\n");
		}
		if (shown == 0) {
			// The nearest buffers below and above.
			auto above = g_registry.buffers.lower_bound(begin);
			if (above != g_registry.buffers.begin()) {
				const auto& [below_address, below] = *std::prev(above);
				std::printf("  nearest below: device 0x%llx size 0x%llx guest 0x%llx %s\n",
				            static_cast<unsigned long long>(below_address),
				            static_cast<unsigned long long>(below.size),
				            static_cast<unsigned long long>(below.cpu),
				            below.destroyed_ns != 0 ? "destroyed" : "live");
			}
			if (above != g_registry.buffers.end())
				std::printf("  nearest above: device 0x%llx size 0x%llx guest 0x%llx %s\n",
				            static_cast<unsigned long long>(above->first),
				            static_cast<unsigned long long>(above->second.size),
				            static_cast<unsigned long long>(above->second.cpu),
				            above->second.destroyed_ns != 0 ? "destroyed" : "live");
		}
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i)
		std::printf("Device fault vendor %u: %s code 0x%llx data 0x%llx\n", i,
		            vendors[i].description,
		            static_cast<unsigned long long>(vendors[i].vendorFaultCode),
		            static_cast<unsigned long long>(vendors[i].vendorFaultData));
	std::fflush(stdout);
}

} // namespace DeviceFault
