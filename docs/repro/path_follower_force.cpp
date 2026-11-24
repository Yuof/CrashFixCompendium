// PathFollowerRace: the part of my test plugin that forces the crash. Never install this to play.
//
// The crash needs another thread to clear an actor's path between two reads of the path in the actor's update. In a
// normal game that is a matter of chance. This plugin makes it happen on every fast travel, with the game's own code:
//
// 1. It hooks the virtual follower's update (planner slot 3, SkyrimSE+120D2F0) to know which follower the thread is
//    updating, and ProcessLists::UpdateNonHighActors's calls (SkyrimSE+731E3D, +736AE0) to know when the far-away
//    actors' batch runs.
// 2. It hooks the update's call of FindClosestPoint (SkyrimSE+120DA03), the call right after the first read of the
//    path. The first three times a batch update gets there, the plugin starts a thread that calls that follower's own
//    ClearPath (path manager slot 6, SkyrimSE+120D550), and holds the update there for 30 ms.
// 3. The update then reads the path a second time, for its call of SegmentForParam (SkyrimSE+120DA36).
//
// Without the fix, ClearPath runs at once: the second read gets null and the game crashes at SkyrimSE+11BCBB0, with
// the faulting read of the reports. With the fix, the update holds the follower's lock, so ClearPath waits until the
// update ends, and the game runs on.

constexpr uintptr_t kNonHigh = 0x768ED0, kNonHighSites[] = { 0x731E3D, 0x736AE0 };   // UpdateNonHighActors
constexpr uintptr_t kClosest = 0x11BC650, kClosestSite = 0x120DA03;                 // FindClosestPoint

std::atomic<int>   g_inBatch{ 0 };
std::atomic<int>   g_forced{ 0 };
thread_local void* t_follower = nullptr;  // the follower this thread is updating

void HNonHigh(void* lists, float dt, uint8_t a, bool b)
{
	++g_inBatch;
	reinterpret_cast<void (*)(void*, float, uint8_t, bool)>(g_base + kNonHigh)(lists, dt, a, b);
	--g_inBatch;
}

// planner slot 3; a_planner is the follower's IMovementPlannerAgent at +0x18
void HUpdate(void* a_planner, void* a_dt, void* a3, void* a4)
{
	void* const outer = t_follower;
	t_follower = static_cast<char*>(a_planner) - 0x18;
	g_nextUpdate(a_planner, a_dt, a3, a4);
	t_follower = outer;
}

DWORD WINAPI ForceClear(void* a_follower)
{
	void* agent = static_cast<char*>(a_follower) + 0x20;  // its IMovementPathManagerAgent
	using ClearPath_t = void (*)(void*);
	(*reinterpret_cast<ClearPath_t**>(agent))[6](agent);  // the game's ClearPath: takes the follower's lock
	return 0;
}

bool HClosest(void* a_path, void* a_from, void* a_out, float* a_t)
{
	const bool found = reinterpret_cast<bool (*)(void*, void*, void*, float*)>(g_base + kClosest)(a_path, a_from, a_out, a_t);
	if (found && g_inBatch && t_follower && g_forced++ < 3) {
		if (HANDLE h = CreateThread(nullptr, 0, &ForceClear, t_follower, 0, nullptr)) {
			CloseHandle(h);
		}
		Sleep(30);  // the update never waits for that thread itself
	}
	return found;
}

// Install: the update through the follower's vtable (slot 3 of its IMovementPlannerAgent table), the two
// UpdateNonHighActors calls and the FindClosestPoint call as call-site hooks, each after checking its bytes.
