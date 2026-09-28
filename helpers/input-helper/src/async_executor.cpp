#include "async_executor.h"

#include <pthread.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "common.h"

namespace InputHelper {
namespace AsyncExecutor {
namespace {

struct Queued {
    TaskKind kind;
    std::function<void()> task;
    std::function<void()> onEvict;
    bool evicted = false;
};

// Heap-allocated and never freed: the detached worker may still be running
// while the process exits, so static destructors must not touch this state.
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Queued> queue;
    bool started = false;
    bool stopping = false;
};
State &S()
{
    static State *state = new State();
    return *state;
}

void WorkerLoop()
{
    pthread_setname_np(pthread_self(), "tl-input-a11y");
    for (;;) {
        Queued item;
        {
            State &st = S();
            std::unique_lock<std::mutex> lock(st.mutex);
            st.cv.wait(lock, [&st] { return st.stopping || !st.queue.empty(); });
            if (st.queue.empty()) return;  // stopping and drained
            item = std::move(st.queue.front());
            st.queue.pop_front();
        }
        // Like the original, nothing calls back into JS once cleanup started:
        // the Node environment may already be tearing down.
        if (IsShuttingDown()) continue;
        try {
            if (item.evicted) {
                if (item.onEvict) item.onEvict();
            } else if (item.task) {
                item.task();
            }
        } catch (...) {
            Log("[AsyncExecutor] task threw");
        }
    }
}

}  // namespace

void PostWithEvict(TaskKind kind, std::function<void()> task, std::function<void()> onEvict)
{
    {
        State &st = S();
        std::lock_guard<std::mutex> lock(st.mutex);
        if (!st.stopping) {
            for (auto &q : st.queue) {
                if (q.kind == kind && !q.evicted) q.evicted = true;
            }
            st.queue.push_back(Queued{kind, std::move(task), std::move(onEvict), false});
            if (!st.started) {
                std::thread(WorkerLoop).detach();
                st.started = true;
            }
            st.cv.notify_one();
            return;
        }
    }
    if (onEvict) onEvict();
}

void Shutdown()
{
    // Pending tasks are dropped by the worker (IsShuttingDown()); never join it:
    // it may be relaying a callback to the JS thread that is calling us.
    State &st = S();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.stopping = true;
    st.cv.notify_all();
}

}  // namespace AsyncExecutor
}  // namespace InputHelper
