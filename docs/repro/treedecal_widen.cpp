// SkinnedDecal: the part of my test plugin that widens the race. Never install this to play.
//
// The plugin hooks the two calls NiNode::AttachChild makes to the child array's grow (SkyrimSE+D1D18A and +D1D212,
// both to +2CFC30), and it knows which root the current thread is creating a skinned decal node on (t_createRoot,
// set by a hook around the node's constructor).
//
// The game's grow stores the new capacity, then allocates, then stores the new buffer. Between the first and the
// last store the array says "room for more" while it still points at the old, full buffer. That state lasts a few
// instructions. This hook holds it for WIDEN_MS (I used 50), so a second thread attaching to the same root in that
// time takes the wrong path every time instead of once in many thousand hits.

void HGrow(void* arr, uint32_t cap)                      // arr = &root->children, cap = the new capacity
{
	const bool creating = t_createRoot && static_cast<uint8_t*>(arr) == static_cast<uint8_t*>(t_createRoot) + 0x110;
	if (creating && GetCurrentThreadId() != g_mainTid) {
		auto* capField = reinterpret_cast<volatile uint16_t*>(static_cast<uint8_t*>(arr) + 0x10);
		const uint16_t old = *capField;
		*capField = static_cast<uint16_t>(cap);          // the grow's own first store, done early
		Sleep(WIDEN_MS);                                 // ... and held: new capacity, old buffer
		*capField = old;
	}
	reinterpret_cast<void (*)(void*, uint32_t)>(g_base + 0x2CFC30)(arr, cap);   // the real grow
}
