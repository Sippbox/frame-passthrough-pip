// "Start with SteamVR": whether the framepip.service user unit is enabled (it is WantedBy=steamvr.service).
// systemctl runs on a worker thread so the overlay loop never waits on it.
#pragma once

#include <atomic>
#include <mutex>
#include <thread>

namespace framepip {

class Autostart {
 public:
  enum class State { Unknown, Enabled, Disabled, Unavailable, Busy };

  ~Autostart();
  void Refresh();      // query the unit's state
  void Set(bool on);   // enable / disable the unit
  State Get() const { return state_.load(); }
  // True once after the state changed (the panel needs a redraw).
  bool TakeChanged() { return changed_.exchange(false); }

 private:
  void Run(const char *action);
  std::atomic<State> state_{State::Unknown};
  std::atomic<bool> changed_{false};
  std::mutex mutex_;  // one systemctl at a time
  std::thread worker_;
};

}  // namespace framepip
