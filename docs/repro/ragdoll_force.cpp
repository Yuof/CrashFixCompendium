// RagdollWorldLock: the part of my test plugin that forces the crash. Never install this to play.
//
// The crash needs two things at once. The ragdoll driver has to add a joint back to the physics world, and the
// world's counter of changes in progress (hkpWorld +0xF8) has to be other than 0 at that moment. In a normal game the
// second one is a matter of chance. This plugin makes both happen, on the driver's own code path:
//
// 1. While the driver drives a ragdoll, the plugin removes the driver's joint 0 ("NPC COM" to "NPC L Thigh") from
//    the world with the game's own hkpWorld::removeConstraint.
// 2. On the next frame the driver adds it back (its call to hkpWorld::addConstraint at SkyrimSE+AAD6C9). The hook
//    raises the counter by one around that call, so the world only queues the add. A wrong counter does the same.
// 3. Then the hook calls the game's on / off switch (SkyrimSE+B47870) with "off" for that joint. The driver makes
//    that call for a joint whose two bodies are keyframed. It is the call that crashes.
//
// The crash is at SkyrimSE+B6AA54, with the same faulting read as in the reports. The stack shows this plugin where
// the reports show the driver's own call.
//
// The plugin creates the wrong state itself, so it crashes with RagdollWorldLock on too. The fix keeps the counter
// from going wrong. It does not change what a wrong counter leads to.

constexpr uintptr_t kAddSite = 0xAAD6C9, kAdd = 0xB38C50;            // hkpWorld::addConstraint, called by the driver
constexpr uintptr_t kDrivingSite = 0xAADCDD, kDriving = 0xAAED10;   // a call only the driving branch makes
constexpr uintptr_t kSwitch = 0xB47870, kRemove = 0xB38D70;         // the on / off switch, hkpWorld::removeConstraint

int g_pulled = 0;

void* HAdd(uint8_t* world, uint8_t* joint)
{
	auto& counter = *reinterpret_cast<volatile int32_t*>(world + 0xF8);
	++counter;                                                       // step 2: the add is only queued
	void* result = reinterpret_cast<void* (*)(void*, void*)>(g_base + kAdd)(world, joint);
	--counter;                                                       // the queue runs at the next physics update

	if (*reinterpret_cast<void**>(joint + 0x10) == nullptr) {        // owner still null
		reinterpret_cast<void (*)(void*, bool)>(g_base + kSwitch)(joint, false);   // step 3: crashes without the fix
	}
	return result;
}

uint64_t HDriving(uint8_t* driver, uint64_t b, uint64_t c, uint64_t d)
{
	auto* ragdoll = *reinterpret_cast<uint8_t**>(driver + 0x88);
	if (ragdoll && g_pulled < 5) {
		auto* joint = **reinterpret_cast<uint8_t***>(ragdoll + 0x20);               // joint 0
		auto* body = *reinterpret_cast<uint8_t**>(joint + 0x28);
		void* world = *reinterpret_cast<void**>(body + 0x10);
		if (world && *reinterpret_cast<void**>(joint + 0x10)) {
			bool removed = false;
			reinterpret_cast<bool* (*)(void*, bool*, void*)>(g_base + kRemove)(world, &removed, joint);   // step 1
			++g_pulled;
		}
	}
	return reinterpret_cast<uint64_t (*)(void*, uint64_t, uint64_t, uint64_t)>(g_base + kDriving)(driver, b, c, d);
}
