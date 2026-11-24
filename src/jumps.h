#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Follows the jumps that hook code leaves at a patched site, the way the CPU would, without running anything.
// HookWatch logs the chain; a chain that comes back to an address it passed is a loop that never returns.
namespace Jumps
{
	enum class End
	{
		kCode,        // the last address holds no jump: real code starts there
		kLoop,        // the last address was passed before
		kUnreadable,  // the last address can't be read
		kTooLong
	};

	struct Chain
	{
		std::vector<std::uintptr_t> at;  // the site first, then each jump's target
		End                         end = End::kCode;
	};

	constexpr std::size_t kMaxHops = 16;

	// a_read(address, buffer, size) -> false when the memory can't be read.
	// A call (E8) counts as a jump only at the site itself: a write_call site.
	template <class Read>
	Chain Follow(std::uintptr_t a_site, Read&& a_read)
	{
		Chain chain{ { a_site } };
		for (std::uintptr_t at = a_site;;) {
			std::uint8_t b[16];
			if (!a_read(at, b, sizeof(b))) {
				chain.end = End::kUnreadable;
				return chain;
			}
			const auto i32 = [&](std::size_t a_off) {
				std::int32_t v;
				std::memcpy(&v, b + a_off, sizeof(v));
				return static_cast<std::int64_t>(v);
			};
			const auto u64 = [&](std::size_t a_off) {
				std::uint64_t v;
				std::memcpy(&v, b + a_off, sizeof(v));
				return static_cast<std::uintptr_t>(v);
			};

			std::uintptr_t next = 0;
			if (b[0] == 0xE9 || (b[0] == 0xE8 && at == a_site)) {  // jmp / call rel32
				next = at + 5 + i32(1);
			} else if (b[0] == 0xEB) {  // jmp rel8
				next = at + 2 + static_cast<std::int8_t>(b[1]);
			} else if (b[0] == 0xFF && b[1] == 0x25) {  // jmp [rip+disp32]
				std::uint8_t ptr[8];
				if (!a_read(at + 6 + i32(2), ptr, sizeof(ptr))) {
					chain.end = End::kUnreadable;
					return chain;
				}
				std::memcpy(&next, ptr, sizeof(next));
			} else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {  // mov rax, imm64; jmp rax
				next = u64(2);
			} else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {  // r11
				next = u64(2);
			} else if (b[0] == 0x68 && b[5] == 0xC7 && b[6] == 0x44 && b[7] == 0x24 && b[8] == 0x04 && b[13] == 0xC3) {
				// push imm32; mov dword [rsp+4], imm32; ret
				const auto lo = static_cast<std::uint32_t>(i32(1));
				const auto hi = static_cast<std::uint32_t>(i32(9));
				next = (static_cast<std::uintptr_t>(hi) << 32) | lo;
			} else {
				return chain;  // kCode
			}

			const bool seen = std::ranges::find(chain.at, next) != chain.at.end();
			chain.at.push_back(next);
			if (seen) {
				chain.end = End::kLoop;
				return chain;
			}
			if (chain.at.size() > kMaxHops) {
				chain.end = End::kTooLong;
				return chain;
			}
			at = next;
		}
	}
}
