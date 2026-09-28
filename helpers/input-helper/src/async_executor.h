#pragma once

#include <functional>

namespace InputHelper {
namespace AsyncExecutor {

enum class TaskKind { SelectedText, InputState };

// Queue `task` on the single AT-SPI worker thread. If a task of the same kind
// is still waiting, it is evicted: its `onEvict` runs (on the worker) instead.
// When the executor is shut down, `onEvict` runs immediately on the caller;
// queued work is dropped once InputHelper::IsShuttingDown() is set.
void PostWithEvict(TaskKind kind, std::function<void()> task, std::function<void()> onEvict);
void Shutdown();

}  // namespace AsyncExecutor
}  // namespace InputHelper
