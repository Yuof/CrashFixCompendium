// Jumps::Follow on made-up memory: each jump kind, a loop, an unreadable target.
#include "jumps.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace
{
	std::map<std::uintptr_t, std::uint8_t> g_mem;
	int                                    g_failed = 0;

	void Put(std::uintptr_t a_at, std::initializer_list<std::uint8_t> a_bytes)
	{
		for (const auto b : a_bytes) {
			g_mem[a_at++] = b;
		}
	}

	void PutRel32(std::uintptr_t a_at, std::uint8_t a_op, std::uintptr_t a_to)
	{
		const auto rel = static_cast<std::int32_t>(a_to - (a_at + 5));
		std::uint8_t b[4];
		std::memcpy(b, &rel, 4);
		Put(a_at, { a_op, b[0], b[1], b[2], b[3] });
	}

	void PutAbs(std::uintptr_t a_at, std::uintptr_t a_to)  // jmp [rip+0] / dq a_to
	{
		Put(a_at, { 0xFF, 0x25, 0, 0, 0, 0 });
		for (int i = 0; i < 8; ++i) {
			g_mem[a_at + 6 + i] = static_cast<std::uint8_t>(a_to >> (8 * i));
		}
	}

	bool Read(std::uintptr_t a_at, void* a_out, std::size_t a_size)
	{
		auto* out = static_cast<std::uint8_t*>(a_out);
		for (std::size_t i = 0; i < a_size; ++i) {
			const auto it = g_mem.find(a_at + i);
			if (it == g_mem.end()) {
				return i > 0 && a_size == 16 ? (std::memset(out + i, 0xCC, a_size - i), true) : false;
			}
			out[i] = it->second;
		}
		return true;
	}

	void Check(const char* a_what, const Jumps::Chain& a_chain, std::initializer_list<std::uintptr_t> a_at, Jumps::End a_end)
	{
		const bool ok = a_chain.end == a_end && a_chain.at == std::vector<std::uintptr_t>(a_at);
		if (!ok) {
			++g_failed;
		}
		std::printf("%s %s\n", ok ? "ok  " : "FAIL", a_what);
	}
}

int main()
{
	// entry -> jmp rel32 -> trampoline slot jmp [rip] -> detour code
	PutRel32(0x1000, 0xE9, 0x2000);
	PutAbs(0x2000, 0x3000);
	Put(0x3000, { 0x48, 0x83, 0xEC, 0x28 });  // sub rsp, 28h
	Check("rel32 then jmp [rip] to code", Jumps::Follow(0x1000, Read), { 0x1000, 0x2000, 0x3000 }, Jumps::End::kCode);

	// a write_call site counts its call; a call further on is code
	PutRel32(0x4000, 0xE8, 0x3000);
	Check("call at the site", Jumps::Follow(0x4000, Read), { 0x4000, 0x3000 }, Jumps::End::kCode);
	PutRel32(0x4100, 0xE9, 0x4200);
	PutRel32(0x4200, 0xE8, 0x3000);
	Check("call past the site is code", Jumps::Follow(0x4100, Read), { 0x4100, 0x4200 }, Jumps::End::kCode);

	// two stubs that jump to each other
	PutAbs(0x5000, 0x5100);
	PutRel32(0x5100, 0xE9, 0x5000);
	Check("loop", Jumps::Follow(0x5000, Read), { 0x5000, 0x5100, 0x5000 }, Jumps::End::kLoop);

	// mov rax / jmp rax, mov r11 / jmp r11, push / mov / ret, jmp rel8
	Put(0x6000, { 0x48, 0xB8, 0x00, 0x70, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 });
	Put(0x7000, { 0x49, 0xBB, 0x00, 0x80, 0, 0, 0, 0, 0, 0, 0x41, 0xFF, 0xE3 });
	Put(0x8000, { 0x68, 0x00, 0x90, 0, 0, 0xC7, 0x44, 0x24, 0x04, 0x01, 0, 0, 0, 0xC3 });
	Check("mov rax, mov r11, push / ret", Jumps::Follow(0x6000, Read), { 0x6000, 0x7000, 0x8000, 0x100009000 },
		Jumps::End::kUnreadable);
	Put(0x9000, { 0xEB, 0x10 });
	Put(0x9012, { 0x90 });
	Check("jmp rel8", Jumps::Follow(0x9000, Read), { 0x9000, 0x9012 }, Jumps::End::kCode);

	// jmp [rip] whose pointer can't be read
	Put(0xA000, { 0xFF, 0x25, 0x00, 0x10, 0, 0 });
	Check("unreadable pointer", Jumps::Follow(0xA000, Read), { 0xA000 }, Jumps::End::kUnreadable);

	return g_failed ? 1 : 0;
}
