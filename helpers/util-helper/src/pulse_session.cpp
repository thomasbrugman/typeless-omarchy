#include <poll.h>

#include "common.h"

namespace {

struct ContextWaitState {
  bool ready = false;
  bool failed = false;
};

void ContextStateCallback(pa_context* c, void* userdata) {
  auto* st = static_cast<ContextWaitState*>(userdata);
  switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
      st->ready = true;
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      st->failed = true;
      break;
    default:
      break;
  }
}

void OperationCallback(pa_operation* op, void* userdata) {
  if (pa_operation_get_state(op) != PA_OPERATION_RUNNING) *static_cast<bool*>(userdata) = true;
}

}  // namespace

PulseSession::~PulseSession() { Cleanup(); }

void PulseSession::Cleanup() {
  if (context_) {
    pa_context_set_state_callback(context_, nullptr, nullptr);
    pa_context_disconnect(context_);
    if (mainloop_) {
      int retval = 0;
      for (int i = 0; i < 50; ++i) {
        pa_context_state_t st = pa_context_get_state(context_);
        if (st == PA_CONTEXT_TERMINATED || st == PA_CONTEXT_UNCONNECTED) break;
        if (pa_mainloop_iterate(mainloop_, 0, &retval) < 0 || retval != 0) break;
      }
    }
    pa_context_unref(context_);
    context_ = nullptr;
  }
  if (mainloop_) {
    pa_mainloop_free(mainloop_);
    mainloop_ = nullptr;
  }
}

bool PulseSession::Connect(const char* appName) {
  Cleanup();
  error_.clear();
  mainloop_ = pa_mainloop_new();
  if (!mainloop_) {
    error_ = "Failed to create PulseAudio mainloop";
    return false;
  }
  pa_mainloop_api* api = pa_mainloop_get_api(mainloop_);
  if (!appName || !*appName) appName = "Typeless";
  context_ = pa_context_new(api, appName);
  if (!context_) {
    error_ = "Failed to create PulseAudio context";
    Cleanup();
    return false;
  }
  ContextWaitState st;
  pa_context_set_state_callback(context_, ContextStateCallback, &st);
  if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    error_ = pa_strerror(pa_context_errno(context_));
    Cleanup();
    return false;
  }
  if (!IterateUntil([&] { return st.ready || st.failed; }, std::chrono::milliseconds(5000))) {
    Cleanup();
    return false;
  }
  if (st.failed || !st.ready) {
    error_ = pa_strerror(pa_context_errno(context_));
    Cleanup();
    return false;
  }
  pa_context_set_state_callback(context_, nullptr, nullptr);
  return true;
}

bool PulseSession::IterateUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
  if (!mainloop_) {
    error_ = "PulseAudio mainloop is not initialized";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (pred()) return true;
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      error_ = "Timed out waiting for PulseAudio";
      return false;
    }
    // Like pa_mainloop_iterate(block=1) but never sleeps past the deadline.
    long long remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
    if (remainingUs < 1000) remainingUs = 1000;
    int retval = 0;
    if (pa_mainloop_prepare(mainloop_, (int)remainingUs) < 0 || pa_mainloop_poll(mainloop_) < 0 ||
        pa_mainloop_dispatch(mainloop_) < 0) {
      if (pa_mainloop_get_retval(mainloop_) != 0) {
        error_ = "PulseAudio mainloop quit unexpectedly";
      } else {
        error_ = "PulseAudio mainloop iteration failed";
      }
      return false;
    }
    (void)retval;
    if (context_ && pa_context_get_state(context_) == PA_CONTEXT_FAILED) {
      error_ = pa_strerror(pa_context_errno(context_));
      return false;
    }
  }
}

bool PulseSession::WaitForOperation(pa_operation* op) {
  if (!op) {
    error_ = "PulseAudio operation is null";
    return false;
  }
  bool done = false;
  pa_operation_set_state_callback(op, OperationCallback, &done);
  if (!IterateUntil([&] { return done; }, std::chrono::milliseconds(10000))) {
    pa_operation_set_state_callback(op, nullptr, nullptr);
    pa_operation_cancel(op);
    pa_operation_unref(op);
    return false;
  }
  pa_operation_state_t st = pa_operation_get_state(op);
  pa_operation_set_state_callback(op, nullptr, nullptr);
  pa_operation_unref(op);
  if (st != PA_OPERATION_DONE) {
    if (error_.empty()) error_ = "PulseAudio operation failed";
    return false;
  }
  return true;
}
