// RagdollWorldLock: the part of my test plugin that shows the two unlocked writes. Never install this to play.
//
// It puts a hardware write watchpoint (debug register DR0, 4 bytes) on the exterior physics world's counter of
// changes in progress (hkpWorld +0xF8). An interior cell has a world of its own, which this does not watch. A
// background thread sets the watchpoint on every thread once a second, so new threads and a new world are covered.
//
// The exception handler runs on each write to the counter, on the writing thread. It counts the write. When that
// thread does not own the world's write lock (bhkWorld +0xC598 holds the owner's thread id), it keeps the stack.
// Each write costs one trap.
//
// Without RagdollWorldLock, the stacks of the unlocked writes end in two places: SkyrimSE+3F9E47, after the loaded
// area bound's call to hkpAabbPhantom::setAabb, and SkyrimSE+BD7A62, after hkaRagdollInstance::setBodiesToPose's
// call to hkpRigidBody::setPositionAndRotation. With the fix, the thread that makes each of those writes owns the
// lock.

constexpr uintptr_t kTesWorld = 0x20F8928;   // pointer to the TES's exterior bhkWorld pointer

std::atomic<uintptr_t> g_counter{ 0 }, g_world{ 0 };
std::atomic<uint64_t>  g_writes{ 0 }, g_unlocked{ 0 };

LONG CALLBACK OnWrite(EXCEPTION_POINTERS* e)
{
	if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1) ||
		e->ContextRecord->Dr0 != g_counter.load()) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	e->ContextRecord->Dr6 = 0;
	++g_writes;
	const DWORD owner = *reinterpret_cast<volatile DWORD*>(g_world.load() + 0xC598);   // worldLock's writer thread
	if (owner != GetCurrentThreadId()) {
		++g_unlocked;
		KeepStack(e->ContextRecord);   // unwinds from the context: the first frame is the instruction after the write
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

void Arm(uintptr_t a_counter)   // DR0 = the counter, write only, 4 bytes, on every other thread
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te{ sizeof(te) };
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) {
			continue;
		}
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
		if (h && SuspendThread(h) != static_cast<DWORD>(-1)) {
			CONTEXT c{};
			c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(h, &c)) {
				c.Dr0 = a_counter;
				c.Dr7 = (c.Dr7 & ~(DWORD64(0xF) << 16)) | 1 | (DWORD64(1) << 16) | (DWORD64(3) << 18);   // L0, write, 4 bytes
				SetThreadContext(h, &c);
			}
			ResumeThread(h);
		}
		if (h) {
			CloseHandle(h);
		}
	}
	CloseHandle(snap);
}

DWORD WINAPI WatchThread(LPVOID)
{
	AddVectoredExceptionHandler(1, OnWrite);
	for (;;) {
		Sleep(1000);
		const auto holder = *reinterpret_cast<uintptr_t*>(g_base + kTesWorld);
		const auto world = holder ? *reinterpret_cast<uintptr_t*>(holder) : 0;              // bhkWorld
		const auto hk = world ? *reinterpret_cast<uintptr_t*>(world + 0x10) : 0;            // its hkpWorld
		if (!hk) {
			continue;
		}
		g_world = world;
		g_counter = hk + 0xF8;
		Arm(hk + 0xF8);
	}
}
