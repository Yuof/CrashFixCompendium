// RagdollWorldLock: the part of my test plugin that keeps the physics world lock busy during a main-thread
// broadphase update. Never install this to play.
//
// It hooks the tail jumps into the two functions RagdollWorldLock patches:
//   hkpRigidBody::setPositionAndRotation -> hkpEntity::updateBroadPhaseAfterMove  (SkyrimSE+B3021E -> B30FE0)
//   hkpAabbPhantom::setAabb              -> hkpPhantom::updateBroadPhase          (SkyrimSE+B32238 -> B52FD0)
//
// For the first few calls the main thread makes without the world lock, a helper thread takes the lock with the
// game's own LockForWrite and holds it for 2 ms. The call goes on while the helper holds it. The helper holds
// nothing else and always lets go.
//
// With RagdollWorldLock each such call is queued: the world's counter of changes in progress (hkpWorld +0xF8) stays
// 0, and its count of pending operations (+0xF0) goes up by one. 200 ms later both are 0, so the queued updates
// have run.

constexpr uintptr_t kPoseJmp = 0xB3021E, kUpdateAfterMove = 0xB30FE0;
constexpr uintptr_t kAabbJmp = 0xB32238, kPhantomUpdate = 0xB52FD0;
constexpr uintptr_t kLockForWrite = 0xCC9140, kUnlockForWrite = 0xCC9390;

HANDLE                 g_request;   // auto-reset event
std::atomic<uintptr_t> g_lock{ 0 }, g_world{ 0 };
std::atomic<bool>      g_held{ false };
int                    g_left = 8;  // calls to make busy

DWORD WINAPI Helper(LPVOID)
{
	for (;;) {
		WaitForSingleObject(g_request, INFINITE);
		const uintptr_t lock = g_lock.exchange(0), world = g_world.exchange(0);
		reinterpret_cast<void (*)(uintptr_t)>(g_base + kLockForWrite)(lock);
		g_held = true;
		Sleep(2);
		g_held = false;
		reinterpret_cast<void (*)(uintptr_t)>(g_base + kUnlockForWrite)(lock);
		Sleep(200);
		Log("200 ms later: count %d, pending %d", *reinterpret_cast<int32_t*>(world + 0xF8),
			*reinterpret_cast<int32_t*>(world + 0xF0));
	}
}

// true when the helper holds the world lock of a_object's world now
bool MakeBusy(const uint8_t* a_object)
{
	if (g_left <= 0 || GetCurrentThreadId() != g_mainThread) return false;
	const auto world = *reinterpret_cast<const uintptr_t*>(a_object + 0x10);              // hkpWorldObject::world
	const auto bhk = world ? *reinterpret_cast<const uintptr_t*>(world + 0x430) : 0;       // ahkpWorld::userData
	if (!bhk || *reinterpret_cast<volatile DWORD*>(bhk + 0xC598) == g_mainThread) return false;   // already locked
	--g_left;
	g_world = world;
	g_lock = bhk + 0xC598;                                                                    // bhkWorld::worldLock
	SetEvent(g_request);
	for (const auto start = GetTickCount64(); !g_held && GetTickCount64() - start < 100;) YieldProcessor();
	return g_held;
}

void PoseJmp(uint8_t* a_body)
{
	const bool busy = MakeBusy(a_body);
	const auto world = *reinterpret_cast<uintptr_t*>(a_body + 0x10);
	const int count = busy ? *reinterpret_cast<int32_t*>(world + 0xF8) : 0;     // the count of running changes
	const int pending = busy ? *reinterpret_cast<int32_t*>(world + 0xF0) : 0;   // queued changes
	reinterpret_cast<void (*)(void*)>(g_base + kUpdateAfterMove)(a_body);
	if (busy) {
		Log("pose under a busy lock: count %d -> %d, pending %d -> %d", count, *reinterpret_cast<int32_t*>(world + 0xF8),
			pending, *reinterpret_cast<int32_t*>(world + 0xF0));
	}
}

// AabbJmp(phantom, aabb) is the same with g_base + kPhantomUpdate(phantom, aabb).
