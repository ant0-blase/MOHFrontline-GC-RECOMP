// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <future>

#include "Common/Functional.h"
#include "Common/SPSCQueue.h"

struct EfbPokeData;
class PointerWrap;

class AsyncRequests
{
public:
  AsyncRequests();

  // Called from the Video thread.
  void PullEvents();

  // Cheap hot-path probe for the FIFO loop. PullEvents() contains the full
  // event-drain path and therefore gets a sizeable stack frame/stack canary
  // even when the SPSC queue is empty. Keep the existing acquire load from
  // SPSCQueue::Empty(), but let the compiler inline it at the call site so an
  // empty queue does not enter PullEvents() at all.
  bool HasPendingEvents() const { return !m_queue.Empty(); }

  // The following are called from the CPU thread.
  void WaitForEmptyQueue();

  template <std::invocable<> F>
  void PushEvent(F&& callback)
  {
    if (m_passthrough)
    {
      std::invoke(std::forward<F>(callback));
      return;
    }

    QueueEvent(Event{std::forward<F>(callback)});
  }

  template <std::invocable<> F>
  auto PushBlockingEvent(F&& callback) -> std::invoke_result_t<F>
  {
    if (m_passthrough)
      return std::invoke(std::forward<F>(callback));

    std::packaged_task task{std::forward<F>(callback)};
    QueueEvent(Event{[&] { task(); }});

    return task.get_future().get();
  }

  // Not thread-safe. Only set during initialization.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  using Event = Common::MoveOnlyFunction<void()>;

  void QueueEvent(Event&& event);

  static AsyncRequests s_singleton;

  Common::WaitableSPSCQueue<Event> m_queue;

  bool m_passthrough = true;
};
