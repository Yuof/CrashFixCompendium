# SkinnedDecal

Two arrows, spells or blood drops hit the same tree or the same actor in the same frame, and the game crashes. The log says `SkyrimSE.exe+0D1EEFE`, `lock xadd [rcx+0x08], eax`, with a small number like 0x1 or 0x10000 in RCX, on a job thread. On the stack: `NiNode::AttachChild`, the constructor of `BGSDecalNode`, `BGSDecalManager::ApplyDecal` and a projectile impact. The log names a "Skinned Decal Node".

It needs the object's first decals. Once a tree or an actor has its decal node, the window is closed.

## How to reproduce it
1. Riverwood, noon, `tgm`. Pick a pine. Trees are skinned meshes, like actors.
2. From eight points around the tree, cast Firebolt at it in the same frame. Disable and enable the tree before each volley, so every volley hits a tree without a decal node. 40 volleys.

Script: [repro/treedecal.py](repro/treedecal.py) with `reset=1 volleys=40`.

Run like that, the race happens two to four times per launch: two threads create the node at once, and the tree ends up with two decal nodes. It doesn't crash, though, because the bad state lasts only a few instructions. To crash on demand I stretch it to 50 ms with a test plugin ([repro/treedecal_widen.cpp](repro/treedecal_widen.cpp)). Stretched, the game crashes every time without the fix and never with it, mostly at `+0D1EEFE`, and the thread that crashes is the one the log has just caught inside the overlap.

## Where I met it
In a fight next to a pine: the address above with RCX = 0x10000 and a missile projectile among the objects. A blood mod was active that sprays three projectiles per heavy hit. That is enough to put several projectiles on one tree in the same frame.

## Why it crashes
The game applies decals on skinned objects from job threads. A skinned object keeps its decals under a special child node, a `BGSDecalNode`. The decal code looks for that node under the object's root. If there is none it creates one, and the node's constructor attaches the new node to the root with `AttachChild`.

Nothing on that path takes a lock. Two threads hitting the same object both find no node, and both attach one.

The crash itself comes out of the child array. When it is full, `AttachChild` grows it, and the grow stores the new capacity before it stores the new buffer. A second `AttachChild` that arrives in between sees a capacity with room and skips the grow. It then writes into slot [size] of the old buffer, which is one slot past its end.

`AttachChild` releases whatever the slot held before it stores the new child. Here "the slot" is the first eight bytes of whichever heap object follows the old buffer. If those bytes are a small number, releasing it faults at `+0D1EEFE`. If they don't fault, the thread overwrites them with a pointer to its decal node. The next heap object's first eight bytes change from, for example, 0x1 to a `BGSDecalNode*`. That is silent heap damage, and a later crash inside the memory allocator is consistent with it.

## The code, with the fix in
The fix re-points one call. `ApplyDecal` used to call the decal walk directly. Now it calls my function, which takes a lock and then calls the walk. The decompiled function with that call replaced:

```c
// BGSDecalManager, SkyrimSE+1F9830 (Address Library 15203)
void BGSDecalManager::ApplyDecal(BGSDecalManager* mgr, DecalRequest* request, bool force, void* extra)
{
    mgr->unk14 = true;                                             // mgr + 0x14

    if (request->target == nullptr) {                              // request + 0x28
        return;
    }
    float dist = Distance(request->position, PlayerCharacter::GetSingleton()->position);
    if (dist > 800.0f && !request->flag_0xBB && !force) {          // too far from the player
        return;
    }
    NiAVObject* object = request->target->vfunc_0x18();            // the hit object's 3D
    if (object == nullptr) {
        return;
    }

    DecalWalk walk = { request, force, ... };                      // counters and limits for this one decal

    SkinnedDecal::TargetThunk(object, &walk);                      // PATCHED at 1F995A, was: AttachDecalWalk(...)
}
```

My function (`src/fixes/SkinnedDecal.cpp`):

```cpp
std::recursive_timed_mutex g_lock;       // recursive: the walk can re-enter on the same thread

std::uint64_t TargetThunk(void* a_object, void* a_data)
{
    const std::unique_lock lock{ g_lock, kMaxWait };               // wait up to 100 ms for the lock
    if (!lock.owns_lock()) {
        // never hang the game over a decal: go on without the lock and say so in the log
        if (const auto n = ++g_timeouts; Milestone(n)) {
            logger::warn("[SkinnedDecal] a decal went on unlocked after {} ms ({} so far)"sv, kMaxWait.count(), n);
        }
    }
    return g_target(a_object, a_data);                             // the game's walk
}
```

And what now runs inside the lock. The walk visits every shape under the object. For a skinned shape it ends up here, in the code with the race:

```c
// SkyrimSE+1F9CD0 (Address Library 15206): put one decal on one skinned shape
bool BGSDecalManager::AddSkinnedDecal(BGSDecalManager* mgr, DecalWalk* walk)
{
    BSTriShape* shape = walk->shape->vfunc_0x48();
    NiNode*     root = nullptr;
    if (shape != nullptr && shape->skin != nullptr) {              // shape + 0x128
        root = shape->skin->root;                                  // skin + 0x60: the skeleton's root
    }

    BGSDecalNode* node = nullptr;
    if (root != nullptr) {
        for (uint i = 0; i < root->children.size && node == nullptr; ++i) {       // 1. look for the node
            NiAVObject* child = root->children[i];
            if (child != nullptr && child->GetRTTI() == &BGSDecalNode::Ni_RTTI) {
                node = (BGSDecalNode*)child;
                if (!node->skinned) {                              // node + 0x140
                    node = nullptr;                                // a decal node, but not the skinned one
                }
            }
        }
    }

    if (node == nullptr) {                                         // 2. none found: create one
        node = new BGSDecalNode(root, true);                       // the constructor calls root->AttachChild(this)
        mgr->decalNodes.push_back(node);                           // mgr + 0x48
    }

    // Without the lock, a second thread can run step 1 while this thread is between step 1 and the end
    // of step 2. It finds nothing either and attaches a second node, possibly in the middle of this
    // thread's AttachChild.

    if (node->decalCount < 25) {                                   // node + 0x124
        BuildDecalGeometry(mgr, walk, node, shape);
        ...
    }
    ...
}
```

Install looks for the call in `ApplyDecal` by its target and wants exactly one. Otherwise it patches nothing.

## Why the fix looks like this
`ApplyDecal`'s call is the only way into the walk from outside. One lock around it turns "look for the node, else create and attach one" into a single step. The second hit waits a moment and then finds the first hit's node.

I use one lock for all objects.

The wait has a limit. I don't know every lock the game holds on the paths into this code, and a deadlock would freeze the game. So a thread that has waited 100 ms goes on without the lock and writes a line to the log. In normal play the waits stay in the microseconds.

The lock also covers building the decal's geometry, and the main thread's skinned decals pass through it too. That costs nothing measurable. The main thread's decals don't wait.

## What else I considered
The main thread has decal code of its own that this lock doesn't cover: it removes finished decal nodes from the manager's array every frame, and it handles decals on static objects. I left that alone on purpose. Locking it would make the main thread wait for the workers' decal builds every frame, and the two don't overlap there.

The other place to fix this is the child array's grow, which could store the buffer before the capacity. Every `NiNode` in the game shares that code, though, and a change there affects far more than decals.
