// Finding XRService (the Steam Frame's tracking service) and the V4L2 nodes its tracking cameras stream from.
// Used by driver_framepip to find what it may lend.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace framepip {

inline std::string ReadSmallFile(const std::string &path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "";
  char buf[512];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  return n > 0 ? std::string(buf, (size_t)n) : "";
}

// The basename of a process's argv[0].
inline std::string ProcessName(int pid) {
  const std::string cmd = ReadSmallFile("/proc/" + std::to_string(pid) + "/cmdline");
  const std::string argv0 = cmd.c_str();  // up to the first NUL
  const size_t slash = argv0.rfind('/');
  return slash == std::string::npos ? argv0 : argv0.substr(slash + 1);
}

inline int ParentPid(int pid) {
  const std::string stat = ReadSmallFile("/proc/" + std::to_string(pid) + "/stat");
  const size_t r = stat.rfind(')');  // "pid (comm) state ppid ..."; comm may hold spaces
  char state;
  int ppid = 0;
  if (r == std::string::npos || sscanf(stat.c_str() + r + 1, " %c %d", &state, &ppid) != 2) return 0;
  return ppid;
}

// The XRService started by vrserver (the real one: a same-named impostor elsewhere is ignored). If parentPid is
// given, only a child of that process counts. 0 if none.
inline int FindXrService(int parentPid = 0) {
  DIR *d = opendir("/proc");
  if (!d) return 0;
  int found = 0;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const int pid = atoi(e->d_name);
    if (ProcessName(pid) != "XRService") continue;
    const int ppid = ParentPid(pid);
    if (parentPid ? ppid != parentPid : ProcessName(ppid) != "vrserver") continue;
    found = pid;
    break;
  }
  closedir(d);
  return found;
}

// XRService logs which node each camera index streams from, e.g.
//   "TrackingCameraInit: index: 0. video device: /dev/video0. v4l subdevice: /dev/v4l-subdev31"
// The log it has open is the current one; the last start's line wins.
inline std::vector<std::string> XrServiceCameraNodes(int xr) {
  std::vector<std::string> nodes;
  const std::string dir = "/proc/" + std::to_string(xr) + "/fd";
  std::string log;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      char l[512];
      ssize_t n = readlink((dir + "/" + e->d_name).c_str(), l, sizeof l - 1);
      if (n <= 4) continue;
      l[n] = 0;
      if (strstr(l, "/XRService-") && !strcmp(l + n - 4, ".log")) {
        log = l;
        break;
      }
    }
    closedir(d);
  }
  std::ifstream f(log);
  std::string line;
  while (std::getline(f, line)) {
    size_t at = line.find("TrackingCameraInit: index: ");
    if (at == std::string::npos) continue;
    int i;
    char dev[64];
    if (sscanf(line.c_str() + at, "TrackingCameraInit: index: %d. video device: %63[^. \n]", &i, dev) != 2) continue;
    if (i < 0 || i > 16 || strncmp(dev, "/dev/video", 10) != 0) continue;
    if ((int)nodes.size() <= i) nodes.resize(i + 1);
    nodes[i] = dev;
  }
  return nodes;
}

// XRService's fd numbers of the V4L2 capture buffers queued on a camera node (any process may query a queue).
inline std::vector<int> QueueBufferFds(const std::string &node, int maxBuffers = 16) {
  std::vector<int> fds;
  int vfd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (vfd < 0) return fds;
  for (int i = 0; i < maxBuffers; i++) {
    v4l2_buffer b{};
    v4l2_plane pl[VIDEO_MAX_PLANES]{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.index = i;
    b.m.planes = pl;
    b.length = VIDEO_MAX_PLANES;
    if (ioctl(vfd, VIDIOC_QUERYBUF, &b) || b.memory != V4L2_MEMORY_DMABUF) break;
    fds.push_back(pl[0].m.fd);
  }
  close(vfd);
  return fds;
}

}  // namespace framepip
