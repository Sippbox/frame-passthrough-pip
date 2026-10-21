#include "autostart.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace framepip {
namespace {

constexpr const char *kUnit = "framepip.service";

// Runs `systemctl --user <args>` against the user manager in /run/user/<uid> (Desktop Mode's nested session points
// XDG_RUNTIME_DIR elsewhere) and returns its first output line.
std::string Systemctl(const std::string &args) {
  const std::string rt = "/run/user/" + std::to_string(getuid());
  const std::string cmd = "XDG_RUNTIME_DIR=" + rt + " DBUS_SESSION_BUS_ADDRESS=unix:path=" + rt +
                          "/bus systemctl --user " + args + " 2>/dev/null";
  std::string out;
  if (FILE *p = popen(cmd.c_str(), "r")) {
    char buf[256];
    if (fgets(buf, sizeof buf, p)) out = buf;
    pclose(p);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

}  // namespace

Autostart::~Autostart() {
  std::lock_guard<std::mutex> l(mutex_);
  if (worker_.joinable()) worker_.join();
}

void Autostart::Refresh() { Run(nullptr); }

void Autostart::Set(bool on) { Run(on ? "enable" : "disable"); }

void Autostart::Run(const char *action) {
  std::lock_guard<std::mutex> l(mutex_);
  if (worker_.joinable()) worker_.join();
  state_ = State::Busy;
  changed_ = true;
  worker_ = std::thread([this, action] {
    if (action) Systemctl(std::string(action) + " " + kUnit);
    const std::string s = Systemctl(std::string("is-enabled ") + kUnit);
    state_ = s == "enabled" ? State::Enabled : s == "disabled" ? State::Disabled : State::Unavailable;
    changed_ = true;
  });
}

}  // namespace framepip
