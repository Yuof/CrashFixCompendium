# UIMessageQueue

This crash has no fixed address. The game crashes in whatever reads a menu's name next, "Fader Menu" or "HUD Menu" for instance, and finds invalid data where the name should be. In one case it was another plugin's menu handler, hashing the name of the Fader Menu as it opened: `movsx rax, byte ptr [rbx]` on a "string" 6.6 MB long.

## How to reproduce it
The crash itself is too rare to produce on demand. Its cause is not. Put a counter on the game's `AddMessage` and run a script that changes worldspace and waits 168 hours a few times ([repro/longwait.py](repro/longwait.py)). Without the fix, two threads are inside `AddMessage` at once dozens of times per launch, and about twice per launch they are on the same slot and both release "Fader Menu". With the fix neither happens.

## Where I met it
Once, during a worldspace change. The dump showed the game's string pool entry for "Fader Menu" holding 48 bytes of some render structure. The pool had freed the name and reused the memory, while the UI still used the name.

## Why it crashes
Every time something opens, closes or updates a menu, it sends a message through `UIMessageQueue::AddMessage`. To avoid allocating, the queue keeps 64 ready-made messages and a counter that says which one is next.

`AddMessage` reads the counter, writes the menu's name into that message, queues the message and then adds one to the counter. Writing the name releases the name the message held from last time. `AddMessage` takes no lock.

That would be fine if only the main thread sent UI messages. It isn't so. In one test launch the main thread made about 1,400 calls and other threads about 300: animation jobs, HUD plugins that react to them, and the game's own workers.

Two threads that read the same counter value pick the same message. Both write a name into it, and both release the old name. Menu names are shared, reference-counted strings, so the old name loses two references where it should lose one. After enough collisions the count reaches zero while the UI still holds the string. In a single session the count of "Fader Menu" drops from 5 to 1 while at least four real holders are alive.

`ProcessCommands`, which hands all 64 messages back by setting the counter to 0, takes no lock either.

## The code, with the fix in
The fix detours the entry of both functions. Each now runs through a function of mine that takes a lock first.

My functions (`src/fixes/UIMessageQueue.cpp`):

```cpp
std::recursive_mutex g_lock;

void AddMessageThunk(RE::UIMessageQueue* a_queue, const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type,
    RE::IUIMessageData* a_data)
{
    const std::scoped_lock lock{ g_lock };
    g_addMessage(a_queue, a_menu, a_type, a_data);     // the game's AddMessage, below
}

void ProcessCommandsThunk(RE::UIMessageQueue* a_queue)
{
    const std::scoped_lock lock{ g_lock };
    g_processCommands(a_queue);                        // the game's ProcessCommands, below
}
```

The game's two functions, unchanged except that nobody can reach them without the lock any more (decompiled):

```c
// SkyrimSE+1AF260 (Address Library 13631)
void UIMessageQueue::AddMessage(UIMessageQueue* queue, const BSFixedString& menu, UI_MESSAGE_TYPE type,
                                IUIMessageData* data)
{
    if (queue->poolUsed < 64) {                        // queue + 0x378
        UIMessage* msg = &queue->messagePool[queue->poolUsed];  // 1. read the counter, pick the message
        msg->menu = menu;                              // 2. releases the name msg held before
        msg->type = type;
        msg->data = data;
        msg->isPooled = true;

        msg = &queue->messagePool[queue->poolUsed];    //    (reads the counter a second time)
        queue->messages.Push(&msg);                    // 3. queue it
        queue->poolUsed += 1;                          // 4. only now move the counter on

        // Without the lock: a second thread that runs step 1 before this thread reaches step 4
        // picks the same message, and both threads run step 2 on it.
    }
    else {
        UIMessage* msg = new UIMessage;                // pool used up: allocate one
        msg->menu = menu;
        msg->type = type;
        msg->data = data;
        msg->isPooled = false;
        if (!queue->messages.Push(&msg)) {
            delete msg;
        }
    }
}

// SkyrimSE+FA82B0 (Address Library 82167)
void UIMessageQueue::ProcessCommands(UIMessageQueue* queue)
{
    for (uint i = 0; i < queue->poolUsed && i < 64; ++i) {
        IUIMessageData* data = queue->messagePool[i].data;
        if (data != nullptr && data->byte_9) {         // set: the data came from a pool of its own
            data->byte_8 = false;                      // hand it back, keep the pointer
        } else {
            delete data;
            queue->messagePool[i].data = nullptr;
        }
    }
    queue->poolUsed = 0;                               // all 64 free again. The names stay in the messages.

    // Without the lock: this reset can land between another thread's step 1 and step 4.
}
```

Install checks the first bytes of each function and the instruction that reads the counter. If another plugin has already put a jump at an entry, I chain to it, so that plugin's code runs inside my lock. If the entry holds anything else, I patch nothing.

## Why the fix looks like this
One lock around both functions means no two threads can read the same counter value, and the reset can't cut into an add.

The lock cannot deadlock. `AddMessage`'s push takes only the queue's own internal lock and calls nothing else, and the UI's main processing lock is always taken before mine, never after.

The cost is small. In a whole test session a thread had to wait for the lock about ten times.

## What else I considered
Locking `AddMessage` alone isn't enough, because `ProcessCommands` resets the counter without a lock too, from the main thread and from the loading thread.

A spin lock that yields would avoid parking a thread in the kernel. The mutex hasn't shown up as a cost, so it stays.
