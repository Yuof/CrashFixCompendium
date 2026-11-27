// SkyrimSE+03FF101 (1.6.1170), `movups xmm0, [rax+0x30]` with rax = 0 in the trap update (TrapTargetEntry), after a
// trap was disabled, or moved with SetPosition, while it touched an actor.
// Cause: a trap keeps an entry while its collision touches an actor. When a solid body (falling rocks) leaves the
// physics world, the trap listener finds the entry only through the body's reference, and the game clears that link
// first. When a phantom (spike beds) leaves, the listener only clears its trigger list. Either way the entry stays and
// points at freed collision.
// Fix: after each of the listener's two callbacks, remove every entry that still points at the leaving collision, with
// the game's own remove function. Install checks the vtable slots and the instructions that use the listener's offsets;
// a build where they differ stays unpatched.

#include "fix.h"
#include "hook.h"

namespace TrapBodyEntry
{
	namespace
	{
		constexpr REL::RelocationID kEntityRemoved{ 25518, 26062 };   // the listener's entity callback
		constexpr REL::RelocationID kPhantomRemoved{ 25521, 26065 };  // the listener's phantom callback
		constexpr REL::RelocationID kTargetUpdate{ 25492, 26032 };    // updates one target of a trap entry
		constexpr REL::RelocationID kRemoveEntry{ 25190, 25718 };     // (listener, entry): unlink + delete

		constexpr std::ptrdiff_t kEntityListener = 0x10;   // TESTrapListener's hkpEntityListener part
		constexpr std::ptrdiff_t kPhantomListener = 0x28;  // and its hkpPhantomListener part
		constexpr std::ptrdiff_t kEntries = 0x8;           // first trap entry
		constexpr std::ptrdiff_t kNext = 0x10;
		constexpr std::ptrdiff_t kCollidable = 0x18;       // the trap's hkpCollidable

		struct Expect
		{
			REL::RelocationID id;
			std::size_t       span;
			std::string_view  bytes;
			std::string_view  what;
		};

		// identical in 1.5.97, 1.6.1170 and 1.7.104
		constexpr Expect kExpect[]{
			{ kEntityRemoved, 0xA0, "\x48\x8B\x57\xF8\x48\x8D\x4F\xF0"sv, "entity callback: entries at +0x8, listener at -0x10"sv },
			{ kEntityRemoved, 0xA0, "\x48\x8B\x52\x10"sv, "entity callback: next entry at +0x10"sv },
			{ kPhantomRemoved, 0x10, "\x4C\x8D\x4A\x20"sv, "phantom callback: collision at phantom +0x20"sv },
			{ kTargetUpdate, 0x70, "\x49\x8B\x46\x18\x80\x78\x28\x01"sv, "target update: collision at +0x18"sv },
			{ kRemoveEntry, 0x20, "\x48\x8B\x51\x08"sv, "remove: entries at +0x8"sv },
		};

		using EntityRemoved_t = void (*)(void*, RE::hkpEntity*);
		EntityRemoved_t g_entityRemoved = nullptr;

		using PhantomRemoved_t = void (*)(void*, RE::hkpPhantom*);
		PhantomRemoved_t g_phantomRemoved = nullptr;

		using RemoveEntry_t = void (*)(void*, void*);
		RemoveEntry_t g_removeEntry = nullptr;

		std::atomic<std::uint64_t> g_bodies{ 0 };
		std::atomic<std::uint64_t> g_phantoms{ 0 };

		template <class T>
		T& At(void* a_base, std::ptrdiff_t a_off)
		{
			return *reinterpret_cast<T*>(static_cast<std::byte*>(a_base) + a_off);
		}

		// on the thread that removes the collision from the world, right after the game's own callback
		void RemoveEntries(void* a_part, std::ptrdiff_t a_partOffset, const RE::hkpCollidable* a_collidable,
			std::atomic<std::uint64_t>& a_count, std::string_view a_kind)
		{
			const auto layer = a_collidable->GetCollisionLayer();
			if (layer != RE::COL_LAYER::kTrap && layer != RE::COL_LAYER::kCloudTrap) {
				return;
			}
			auto* listener = static_cast<std::byte*>(a_part) - a_partOffset;
			if (At<std::uintptr_t>(listener, 0) != RE::VTABLE_TESTrapListener[0].address()) {
				return;  // not laid out as Install checked
			}

			for (auto* entry = At<void*>(listener, kEntries); entry;) {
				auto* next = At<void*>(entry, kNext);  // the remove function deletes the entry
				if (At<const RE::hkpCollidable*>(entry, kCollidable) == a_collidable) {
					g_removeEntry(listener, entry);
					if (const auto n = ++a_count; Milestone(n)) {
						logger::info("[TrapBodyEntry] removed the entry of a trap {} that left the world ({} so far)"sv, a_kind, n);
					}
				}
				entry = next;
			}
		}

		void EntityRemovedThunk(void* a_part, RE::hkpEntity* a_entity)
		{
			g_entityRemoved(a_part, a_entity);
			RemoveEntries(a_part, kEntityListener, &a_entity->collidable, g_bodies, "body"sv);
		}

		void PhantomRemovedThunk(void* a_part, RE::hkpPhantom* a_phantom)
		{
			g_phantomRemoved(a_part, a_phantom);
			RemoveEntries(a_part, kPhantomListener, &a_phantom->collidable, g_phantoms, "phantom"sv);
		}
	}

	Result Install()
	{
		for (const auto& e : kExpect) {
			if (Bytes(e.id.address(), e.span).find(e.bytes) == std::string_view::npos) {
				return { Outcome::kFailed, std::format("{} not found in this build, nothing patched", e.what) };
			}
		}
		if (!SlotIs(RE::VTABLE_TESTrapListener[1], 2, kEntityRemoved) ||
			!SlotIs(RE::VTABLE_TESTrapListener[2], 2, kPhantomRemoved)) {
			return { Outcome::kFailed, "the trap listener's vtable slots are not its callbacks, nothing patched" };
		}

		g_removeEntry = reinterpret_cast<RemoveEntry_t>(kRemoveEntry.address());
		REL::Relocation<std::uintptr_t> entities{ RE::VTABLE_TESTrapListener[1] };
		g_entityRemoved = reinterpret_cast<EntityRemoved_t>(entities.write_vfunc(2, EntityRemovedThunk));
		REL::Relocation<std::uintptr_t> phantoms{ RE::VTABLE_TESTrapListener[2] };
		g_phantomRemoved = reinterpret_cast<PhantomRemoved_t>(phantoms.write_vfunc(2, PhantomRemovedThunk));
		return { Outcome::kActive, "hooked the trap listener's entity and phantom callbacks" };
	}

	const bool kRegistered = Register({
		.key = "TrapBodyEntry",
		.install = Install,
	});
}
