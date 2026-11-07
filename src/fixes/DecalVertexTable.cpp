// SkyrimSE+0E53F5D (1.6.1170), `movss xmm2, [r12+rax*8]` under BSTempEffectSimpleDecal::Initialize, rax far past the
// decal's vertices. Or heap corruption that crashes something else later.
// Cause: the game maps the hit shape's vertices to the decal's through a table of triangleCount * 3 entries, but
// indexes it by vertex index. A shape with unused vertices has indices past the table. The game reads the next heap
// block as decal vertex indices, and the tangent step reads and writes through them.
// Seen with a footprint on Capital Whiterun Expansion's SurScafWalkwayNew.nif (72 triangles, 236 vertices).
// Fix: size the table by the shape's vertex count (+0x15A instead of +0x158). One byte, in the read that feeds both
// the allocation and the 0xFFFF fill. Install finds the read by its bytes; anything else there stays unpatched.

#include "fix.h"

namespace DecalVertexTable
{
	namespace
	{
		constexpr REL::RelocationID kAddPolygon{ 29257, 30112 };  // adds a clipped polygon to a simple decal
		constexpr std::size_t       kSpan = 0x200;                // the read sits at +0x12A (SE), +0x143 / +0x142 (AE)

		// movzx ecx, word ptr [rax+158h]: triangleCount. vertexCount is at +0x15A.
		constexpr auto         kRead = "\x0F\xB7\x88\x58\x01\x00\x00"sv;
		constexpr std::size_t  kDisp = 3;
		constexpr std::uint8_t kVertexCount = 0x5A;

		// lea rbx / ebx, [rcx+rcx*2] follows: the size both the allocation and the fill take
		constexpr auto kLeaAe = "\x48\x8D\x1C\x49"sv;
		constexpr auto kLeaSe = "\x8D\x1C\x49"sv;
	}

	Result Install()
	{
		const auto start = kAddPolygon.address();
		const auto code = Bytes(start, kSpan);

		std::size_t at = 0;
		int         count = 0;
		for (auto i = code.find(kRead); i != code.npos; i = code.find(kRead, i + 1)) {
			at = i;
			++count;
		}

		const auto after = Bytes(start + at + kRead.size(), kLeaAe.size());
		if (count != 1 || !(after.starts_with(kLeaAe) || after.starts_with(kLeaSe))) {
			return { Outcome::kFailed,
				std::format("the table size read is not as expected ({} found), nothing patched", count) };
		}

		REL::safe_write(start + at + kDisp, kVertexCount);
		return { Outcome::kActive,
			std::format("patched the decal vertex table's size (+0x{:X}): sized by the shape's vertex count", at) };
	}

	const bool kRegistered = Register({
		.key = "DecalVertexTable",
		.install = Install,
	});
}
