// framepip: a picture-in-picture view of the Steam Frame's passthrough cameras, pinned to your view or placed in
// the room, with a settings panel in the SteamVR dashboard.
//
//   framepip                   run the overlay (started by the framepip.service user unit)
//   framepip --open            open the settings panel, starting the overlay first if needed (desktop entry)
//   framepip --render-panel F [settings|hover|room|room-unlocked]  draw the panel to a PPM file without SteamVR (layout check)
//   framepip --version
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "autostart.hpp"
#include "calibration.hpp"
#include "canvas.hpp"
#include "fd_protocol.hpp"
#include "input.hpp"
#include "openvr.h"
#include "panel.hpp"
#include "pip_renderer.hpp"
#include "room_grab.hpp"
#include "settings.hpp"
#include "tracking_cameras.hpp"

#ifndef FRAMEPIP_VERSION
#define FRAMEPIP_VERSION "dev"
#endif

namespace fp = framepip;

namespace {

constexpr const char *kDashboardKey = "framepip.dashboard";
constexpr const char *kViewKey = "framepip.view";
constexpr const char *kServiceName = "framepip.service";
// Exit status after the user closed the app (or it was already running): framepip.service doesn't restart it.
constexpr int kExitClosed = 3;
constexpr double kDistance = 1.0;       // metres from the eye to the PiP
constexpr double kStaleFrame = 1.0;     // s without a camera frame before the PiP hides
constexpr double kPanelInterval = 1.0 / 30;  // s between panel redraws (animations run at this rate)
constexpr double kInputInterval = 1.0 / 60;  // s between controller reads when nothing is being carried

volatile sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

double Now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

void Sleep(double s) {
  timespec t{(time_t)s, (long)((s - (time_t)s) * 1e9)};
  nanosleep(&t, nullptr);
}

std::string ExePath() {
  char b[4096];
  ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  if (n <= 0) return "";
  b[n] = 0;
  return b;
}

std::string ExeDir() {
  std::string p = ExePath();
  return p.substr(0, p.rfind('/'));
}

// One overlay process at a time: an exclusive lock held for the process lifetime. -1 if another instance holds it.
int AcquireInstanceLock() {
  mkdir(fp::RuntimeDir().c_str(), 0700);
  const std::string path = fp::RuntimeDir() + "/app.lock";
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  if (flock(fd, LOCK_EX | LOCK_NB)) {
    close(fd);
    return -1;
  }
  return fd;
}

bool InitVr(vr::EVRApplicationType type, double timeout) {
  const double until = Now() + timeout;
  while (!g_stop) {
    vr::EVRInitError e = vr::VRInitError_None;
    vr::VR_Init(&e, type);
    if (e == vr::VRInitError_None) return true;
    if (Now() > until) {
      fprintf(stderr, "VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
      return false;
    }
    Sleep(2);
  }
  return false;
}

// Head-relative pose: the PiP centre yawDeg right and pitchDeg up of straight ahead, kDistance away, facing the eye.
vr::HmdMatrix34_t Placement(const fp::Settings &s) {
  const double a = -s.yawDeg * M_PI / 180, b = s.pitchDeg * M_PI / 180;
  const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
  // R = Ry(a) * Rx(b)
  const double r[3][3] = {{ca, sa * sb, sa * cb}, {0, cb, -sb}, {-sa, ca * sb, ca * cb}};
  vr::HmdMatrix34_t m{};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) m.m[i][j] = (float)r[i][j];
    m.m[i][3] = (float)(-kDistance * r[i][2]);  // R * (0, 0, -d)
  }
  return m;
}

// The PiP's width in metres when it follows the view.
double ViewWidth(const fp::Settings &s) { return 2 * kDistance * std::tan(s.sizeDeg * M_PI / 360); }

vr::HmdMatrix34_t RoomPose(const fp::Settings &s) {
  vr::HmdMatrix34_t m;
  memcpy(m.m, s.roomPose, sizeof m.m);
  return m;
}

void SetRoomPose(fp::Settings *s, const vr::HmdMatrix34_t &m) {
  memcpy(s->roomPose, m.m, sizeof m.m);
  s->hasRoomPose = true;
}

// The headset's pose in standing space.
bool HeadPose(vr::HmdMatrix34_t *m) {
  vr::TrackedDevicePose_t p{};
  vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &p, 1);
  if (!p.bPoseIsValid) return false;
  *m = p.mDeviceToAbsoluteTracking;
  return true;
}

void ApplyLook(vr::IVROverlay *ov, vr::VROverlayHandle_t h, const fp::Settings &s) {
  if (s.roomMode) {
    ov->SetOverlayWidthInMeters(h, (float)s.roomWidth);
    if (s.hasRoomPose) {
      const vr::HmdMatrix34_t m = RoomPose(s);
      ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
    }
  } else {
    ov->SetOverlayWidthInMeters(h, (float)ViewWidth(s));
    const vr::HmdMatrix34_t m = Placement(s);
    ov->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
  }
  ov->SetOverlayAlpha(h, (float)s.opacity);
  float r, g, b;
  s.TintColor(&r, &g, &b);
  ov->SetOverlayColor(h, r, g, b);
}

fp::PanelStatus StatusFor(const std::string &setupError, const fp::TrackingCameras *cams, bool enabled) {
  fp::PanelStatus st;
  if (!setupError.empty()) {
    st.level = fp::PanelStatus::Error;
    st.text = setupError;
    return st;
  }
  if (!enabled) {
    st.level = fp::PanelStatus::Ok;
    st.text = "PiP hidden - cameras off";
    return st;
  }
  const fp::CameraStatus cs = cams->Status();
  st.text = fp::CameraStatusText(cs);
  const std::string detail = cams->Detail();
  switch (cs) {
    case fp::CameraStatus::Streaming:
      st.level = fp::PanelStatus::Ok;
      break;
    case fp::CameraStatus::NoDriver:
    case fp::CameraStatus::Failed:
      st.level = fp::PanelStatus::Error;
      if (!detail.empty()) st.text += ": " + detail;
      break;
    default:
      st.level = fp::PanelStatus::Waiting;
      break;
  }
  return st;
}

int RunOverlay() {
  const int lock = AcquireInstanceLock();
  if (lock < 0) {
    fprintf(stderr, "framepip is already running\n");
    return kExitClosed;
  }
  if (!InitVr(vr::VRApplication_Overlay, 120)) return 1;
  vr::IVROverlay *ov = vr::VROverlay();

  // camera pipeline
  std::string setupError;
  std::string calError;
  const std::vector<fp::CameraModel> models = fp::ForwardCameras(fp::LoadCalibration(fp::kCalibrationPath, &calError));
  std::vector<int> indices;
  for (const fp::CameraModel &m : models) indices.push_back(m.index);
  fp::TrackingCameras cams(indices);
  fp::PipRenderer renderer;
  bool camerasUsable = false, camerasRunning = false;
  std::string gpuError;
  if (!renderer.InitDevice(&gpuError)) {
    setupError = "GPU setup failed: " + gpuError;
  } else if (models.empty()) {
    setupError = "No camera calibration (" + calError + ")";
  } else {
    std::string err;
    if (!renderer.InitCameras(models, &err)) setupError = "GPU setup failed: " + err;
    else camerasUsable = true;
  }
  if (!setupError.empty()) fprintf(stderr, "%s\n", setupError.c_str());

  // overlays
  vr::VROverlayHandle_t view = vr::k_ulOverlayHandleInvalid, dash = vr::k_ulOverlayHandleInvalid,
                        thumb = vr::k_ulOverlayHandleInvalid;
  vr::EVROverlayError oe = ov->CreateOverlay(kViewKey, "Passthrough PiP view", &view);
  if (oe == vr::VROverlayError_None) oe = ov->CreateDashboardOverlay(kDashboardKey, "Passthrough PiP", &dash, &thumb);
  if (oe != vr::VROverlayError_None) {
    fprintf(stderr, "creating overlays: %s\n", ov->GetOverlayErrorNameFromEnum(oe));
    vr::VR_Shutdown();
    return 1;
  }
  ov->SetOverlayWidthInMeters(dash, 2.3f);
  ov->SetOverlayInputMethod(dash, vr::VROverlayInputMethod_Mouse);
  vr::HmdVector2_t scale = {{(float)fp::Panel::kWidth, (float)fp::Panel::kHeight}};
  ov->SetOverlayMouseScale(dash, &scale);
  ov->SetOverlayFromFile(thumb, (ExeDir() + "/../share/icon.png").c_str());
  // a "Close" button under the panel and on its dock icon; pressing it sends VREvent_OverlayClosed
  ov->SetOverlayFlag(dash, vr::VROverlayFlags_EnableControlBarClose, true);
  fp::Autostart autostart;
  autostart.Refresh();
  int exitCode = 0;

  fp::Canvas canvas(fp::Panel::kWidth, fp::Panel::kHeight);
  std::string fontError;
  if (!canvas.LoadFonts(ExeDir() + "/../share/fonts", &fontError)) fprintf(stderr, "%s\n", fontError.c_str());
  fp::Panel panel;
  std::string artError;
  if (!panel.LoadArt(ExeDir() + "/../share", &artError)) fprintf(stderr, "%s\n", artError.c_str());

  fp::Settings settings = fp::LoadSettings();
  // a room position is only kept between sessions when asked to; otherwise the PiP starts in front of you
  if (!settings.rememberRoomPose) settings.hasRoomPose = false;
  ApplyLook(ov, view, settings);
  fp::Input input;
  {
    std::string err;
    if (!input.Init(ExeDir() + "/../share/input/actions.json", &err)) fprintf(stderr, "%s\n", err.c_str());
  }
  fp::RoomGrab grab;
  bool roomApplied = settings.roomMode;
  auto save = [&] {
    if (!fp::SaveSettings(settings)) fprintf(stderr, "can't save %s\n", fp::SettingsPath().c_str());
  };

  uint64_t serial = 0;
  std::vector<fp::CameraImage> images;
  fp::ViewParams viewParams;
  double lastFrame = -1e9, lastPanelDraw = -1e9, lastTick = Now(), lastInput = -1e9;
  bool shown = false, panelDirty = true;
  std::string lastStatus;

  while (!g_stop) {
    const double now = Now();
    vr::VREvent_t ev;
    while (ov->PollNextOverlayEvent(dash, &ev, sizeof ev)) {
      const float x = ev.data.mouse.x, y = fp::Panel::kHeight - ev.data.mouse.y;  // OpenVR's mouse origin is bottom-left
      bool changed = false, commit = false;
      switch (ev.eventType) {
        case vr::VREvent_MouseButtonDown: changed = panel.MouseDown(x, y, &settings); break;
        case vr::VREvent_MouseMove: changed = panel.MouseMove(x, y, &settings); break;
        case vr::VREvent_MouseButtonUp: changed = panel.MouseUp(x, y, &settings, &commit); break;
        case vr::VREvent_FocusLeave: panel.MouseLeave(); break;
        case vr::VREvent_OverlayClosed:
          exitCode = kExitClosed;
          g_stop = 1;
          break;
        default: break;
      }
      if (changed) {
        ApplyLook(ov, view, settings);
        panelDirty = true;
      }
      if (commit) save();
    }
    bool bringHere = false;
    switch (panel.TakeRequest()) {
      case fp::Panel::Request::Quit:
        exitCode = kExitClosed;
        g_stop = 1;
        break;
      case fp::Panel::Request::ToggleAutostart:
        if (autostart.Get() == fp::Autostart::State::Enabled) autostart.Set(false);
        else if (autostart.Get() == fp::Autostart::State::Disabled) autostart.Set(true);
        break;
      case fp::Panel::Request::BringHere: bringHere = true; break;
      case fp::Panel::Request::OpenBindings: {
        std::string err;
        if (!input.OpenBindings(&err)) fprintf(stderr, "%s\n", err.c_str());
        break;
      }
      case fp::Panel::Request::None: break;
    }
    if (panel.TakeRedraw() | autostart.TakeChanged()) panelDirty = true;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
      if (ev.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        g_stop = 1;
      }

    // switching between following the view and staying in the room
    if (settings.roomMode != roomApplied) {
      roomApplied = settings.roomMode;
      vr::HmdMatrix34_t head;
      if (settings.roomMode) {
        settings.hasRoomPose = false;
        if (HeadPose(&head)) {  // it stays exactly where it was in view, now fixed in the room
          SetRoomPose(&settings, fp::Multiply(head, Placement(settings)));
          settings.roomWidth = ViewWidth(settings);
          settings.Clamp();
        }
      }
      ApplyLook(ov, view, settings);
      save();
      panelDirty = true;
    }
    if (settings.roomMode && (bringHere || !settings.hasRoomPose)) {
      vr::HmdMatrix34_t head;
      if (HeadPose(&head)) {
        SetRoomPose(&settings, fp::InFrontOf(head, kDistance));
        ApplyLook(ov, view, settings);
        if (bringHere) save();
      }
    }

    // controllers: every loop while the PiP can be carried, otherwise often enough for the show / hide binding
    if (settings.roomMode || grab.Holding() || now - lastInput >= kInputInterval) {
      input.Update();
      lastInput = now;
    }
    if (input.TakeToggle()) {
      settings.enabled = !settings.enabled;
      save();
      panelDirty = true;
    }
    if (cams.Latest(&serial, &images)) {
      // auto brightness: bring the primary camera's mean luma toward mid grey, smoothly
      const float mean = images[0].mean;
      const double target = std::clamp(110.0 / std::max(mean, 1.0f), 1.0, 5.0);
      viewParams.gain += (target - viewParams.gain) * 0.1;
      vr::Texture_t tex{};
      if (renderer.Render(images, viewParams, &tex)) {
        ov->SetOverlayTexture(view, &tex);
        lastFrame = now;
      }
    }
    const bool wantShown = settings.enabled && now - lastFrame < kStaleFrame && (!settings.roomMode || settings.hasRoomPose);
    if (wantShown != shown) {
      if (wantShown) ov->ShowOverlay(view);
      else ov->HideOverlay(view);
      shown = wantShown;
    }

    // picking the PiP up in the room
    if (settings.roomMode || grab.Holding()) {
      fp::RoomGrab::Window w{RoomPose(settings), settings.roomWidth, settings.roomWidth * fp::Settings::kAspect};
      const bool grabbable = settings.roomMode && settings.hasRoomPose && shown && !settings.roomLocked;
      const fp::RoomGrab::Change change =
          grab.Update(input, grabbable, &w, fp::Settings::kRoomWidthMin, fp::Settings::kRoomWidthMax);
      if (change != fp::RoomGrab::Change::None && settings.roomMode) {
        if (w.width != settings.roomWidth) {
          settings.roomWidth = w.width;
          ov->SetOverlayWidthInMeters(view, (float)w.width);
          panelDirty = true;
        }
        SetRoomPose(&settings, w.pose);
        ov->SetOverlayTransformAbsolute(view, vr::TrackingUniverseStanding, &w.pose);
      }
      if (change == fp::RoomGrab::Change::Released) save();
    }
    // the frame around the PiP fades in while a controller aims at it
    const double aimed = settings.roomMode && grab.Aimed() ? 1 : 0;
    viewParams.highlight += (aimed - viewParams.highlight) * std::min(1.0, (now - lastTick) * 14);
    if (std::fabs(aimed - viewParams.highlight) < 0.01) viewParams.highlight = aimed;

    // the cameras are only read while the PiP is shown
    const bool wantCameras = camerasUsable && settings.enabled;
    if (wantCameras != camerasRunning) {
      if (wantCameras) cams.Start();
      else cams.Stop();
      camerasRunning = wantCameras;
    }
    fp::PanelStatus status = StatusFor(setupError, &cams, settings.enabled);
    status.autostart = autostart.Get();
    status.version = FRAMEPIP_VERSION;
    if (status.text != lastStatus) {
      lastStatus = status.text;
      panelDirty = true;
    }
    if (panel.Tick(now - lastTick, settings, status)) panelDirty = true;
    lastTick = now;
    // drawn only while the panel is on screen; whatever changed meanwhile is drawn when it opens
    if (panelDirty && now - lastPanelDraw >= kPanelInterval && ov->IsOverlayVisible(dash)) {
      panel.Draw(&canvas, settings, status);
      vr::Texture_t tex{};
      if (renderer.UploadPanel(canvas.Pixels(), fp::Panel::kWidth, fp::Panel::kHeight, &tex))
        ov->SetOverlayTexture(dash, &tex);
      else  // no GPU: slower, may flicker while it updates
        ov->SetOverlayRaw(dash, (void *)canvas.Pixels(), fp::Panel::kWidth, fp::Panel::kHeight, 4);
      lastPanelDraw = now;
      panelDirty = false;
    }
    Sleep(0.005);
  }

  cams.Stop();
  vr::VR_Shutdown();
  close(lock);
  return exitCode;
}

// Starts the overlay: through its user unit when installed, otherwise as a detached child.
void StartOverlay() {
  const std::string rt = "/run/user/" + std::to_string(getuid());
  pid_t pid = fork();
  if (pid == 0) {
    setenv("XDG_RUNTIME_DIR", rt.c_str(), 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + rt + "/bus").c_str(), 1);
    execlp("systemctl", "systemctl", "--user", "start", kServiceName, (char *)nullptr);
    _exit(127);
  }
  int status = 0;
  if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) return;
  fprintf(stderr, "%s not available, starting the overlay directly\n", kServiceName);
  if (fork() == 0) {
    setsid();
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0), dup2(null, 1), dup2(null, 2);
    const std::string self = ExePath();
    execl(self.c_str(), self.c_str(), (char *)nullptr);
    _exit(127);
  }
}

int OpenDashboard() {
  const int lock = AcquireInstanceLock();
  if (lock >= 0) {  // nothing running yet
    close(lock);
    StartOverlay();
  }
  // Utility apps get no IVROverlay; an overlay-type connection that creates no overlays can still find and show ours.
  if (!InitVr(vr::VRApplication_Overlay, 15)) return 1;
  vr::IVROverlay *ov = vr::VROverlay();
  int rc = 1;
  for (const double until = Now() + 20; ov && Now() < until && !g_stop; Sleep(0.25)) {
    vr::VROverlayHandle_t h;
    if (ov->FindOverlay(kDashboardKey, &h) == vr::VROverlayError_None) {
      ov->ShowDashboard(kDashboardKey);
      rc = 0;
      break;
    }
  }
  if (rc) fprintf(stderr, "the Passthrough PiP overlay didn't come up\n");
  vr::VR_Shutdown();
  return rc;
}

// variant: "" (main page), "settings" (settings page), "hover" (main page with the Position card highlighted),
// "room" (main page in Local space, locked) or "room-unlocked"
int RenderPanel(const char *path, const std::string &variant) {
  fp::Canvas canvas(fp::Panel::kWidth, fp::Panel::kHeight);
  std::string err;
  if (!canvas.LoadFonts(ExeDir() + "/../share/fonts", &err)) fprintf(stderr, "%s\n", err.c_str());
  fp::PanelStatus st;
  st.level = fp::PanelStatus::Ok;
  st.text = fp::CameraStatusText(fp::CameraStatus::Streaming);
  st.autostart = fp::Autostart::State::Enabled;
  st.version = FRAMEPIP_VERSION;
  fp::Panel panel;
  if (!panel.LoadArt(ExeDir() + "/../share", &err)) fprintf(stderr, "%s\n", err.c_str());
  fp::Settings settings = fp::LoadSettings();
  if (variant == "settings") panel.SetPage(fp::Panel::Page::Settings);
  if (variant == "hover") panel.MouseMove(300, 400, &settings);
  if (variant == "room" || variant == "room-unlocked") {
    settings.roomMode = true;
    settings.roomLocked = variant == "room";
  }
  for (int i = 0; i < 100; i++) panel.Tick(0.05, settings, st);  // let the animations settle
  panel.Draw(&canvas, settings, st);
  FILE *f = fopen(path, "wb");
  if (!f) return 1;
  fprintf(f, "P6\n%d %d\n255\n", fp::Panel::kWidth, fp::Panel::kHeight);
  const uint8_t *p = canvas.Pixels();
  for (int i = 0; i < fp::Panel::kWidth * fp::Panel::kHeight; i++) fwrite(p + i * 4, 1, 3, f);
  fclose(f);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  const std::string arg = argc > 1 ? argv[1] : "";
  if (arg.empty()) return RunOverlay();
  if (arg == "--open") return OpenDashboard();
  if (arg == "--render-panel" && argc > 2) return RenderPanel(argv[2], argc > 3 ? argv[3] : "");
  if (arg == "--version") {
    printf("framepip %s\n", FRAMEPIP_VERSION);
    return 0;
  }
  fprintf(stderr, "usage: %s [--open | --render-panel FILE.ppm [settings|hover|room|room-unlocked] | --version]\n", argv[0]);
  return 2;
}
