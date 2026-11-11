// TrespassPursue: the part of my test plugin that forces the crashing value. Never install this to play.
//
// The game's trespass tree builder (SkyrimSE+498050) maps the Pursue procedure's parameters with three calls to
// SetUidAt (+455150). The third one, at +498101, maps parameter 3 and grows the uid array from 2 to 4 slots. It
// leaves slot 2 with whatever the allocator handed out, and only the value 2 crashes. That happens for about one
// package in sixteen, so an A/B on chance alone needs many runs.
//
// This hook sits on that third call and writes the crashing value itself, every time.

constexpr uintptr_t kSetUid = 0x455150, kSetUidSite = 0x498101;

void HSetUid(void* uidArray, uint32_t index, uint8_t uid)      // uidArray = &pursue->uids (a BSTArray)
{
	reinterpret_cast<void (*)(void*, uint32_t, uint8_t)>(g_base + kSetUid)(uidArray, index, uid);

	auto*          slots = *reinterpret_cast<uint8_t**>(uidArray);                              // 8 bytes a slot
	const uint32_t size = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(uidArray) + 0x10);
	if (slots && size > 2) {
		slots[2 * 8] = 2;                                      // parameter 2 = the uid of the package's Bool
	}
}
