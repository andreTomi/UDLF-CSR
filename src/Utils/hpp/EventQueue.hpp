#ifndef EVENT_QUEUE_HPP
#define EVENT_QUEUE_HPP

#include <omp.h>

#include <any>
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <vector>

#include "Singleton.hpp"

enum class EventType {
  LOAD_IMAGE_LIST,
  LOAD_CLASSES_FILE,
  LOAD_RANKED_LISTS,
  COMPUTE_PRECISION,
  COMPUTE_RECALL,
  COMPUTE_MAP,
};

class EventQueue : public Singleton<EventQueue> {
 public:
  friend class Singleton<EventQueue>;
  using EventCallback = std::function<void()>;

  template <typename T>
  void registerEvent(EventType eventType, T callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingEvents_[eventType].push_back(std::move(callback));
  }

  void processEvents() {
    std::map<EventType, std::vector<EventCallback>> eventsToProcess;

    // Move all pending events to local storage
    {
      std::lock_guard<std::mutex> lock(mutex_);
      eventsToProcess = std::move(pendingEvents_);
      pendingEvents_.clear();
    }

    if (eventsToProcess.empty()) return;

    // Convert map to vector for parallel processing
    std::vector<std::pair<EventType, std::vector<EventCallback>>> eventTypesToProcess;
    for (auto& [eventType, callbacks] : eventsToProcess) {
      if (!callbacks.empty()) {
        eventTypesToProcess.emplace_back(eventType, std::move(callbacks));
      }
    }

    if (eventTypesToProcess.empty()) return;

    // Process event types in parallel
#pragma omp parallel for schedule(dynamic) if (eventTypesToProcess.size() > 1)
    for (int i = 0; i < static_cast<int>(eventTypesToProcess.size()); ++i) {
      auto& [eventType, callbacks] = eventTypesToProcess[i];
      // Execute all callbacks for this event type (they handle their own parallelism)
      for (auto& callback : callbacks) {
        processEvent(callback);
      }
    }
  }

 private:
  void processEvent(EventCallback callback) {
    try {
      callback();
    } catch (const std::exception& e) {
      std::cerr << "[EventQueue] Exception in callback: " << e.what() << std::endl;
    } catch (...) {
      std::cerr << "[EventQueue] Unknown exception in callback" << std::endl;
    }
  }

  mutable std::mutex mutex_;
  std::map<EventType, std::vector<EventCallback>> pendingEvents_;

  EventQueue() = default;
  ~EventQueue() = default;

  // Prevent copy/move
  EventQueue(const EventQueue&) = delete;
  EventQueue& operator=(const EventQueue&) = delete;
  EventQueue(EventQueue&&) = delete;
  EventQueue& operator=(EventQueue&&) = delete;
};

#endif  // EVENT_QUEUE_HPP