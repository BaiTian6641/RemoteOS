#pragma once
#include <memory>
#include <string>
namespace remoteos::devices {
// Bound to IPv4 loopback. Files stay owned and mounted by the client.
class FileShare {
public:
  FileShare(const std::string& root,const std::string& capability,bool writable=false);
  ~FileShare();
  unsigned port() const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
class CameraExport {
public:
  // Fixed CNF8031-compatible profile: 320x240 YUY2 at 5 fps (~6.15 Mb/s).
  CameraExport(const std::string& capability,bool synthetic_test=false);
  ~CameraExport();
  unsigned port() const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
class CameraBridge {
public:
  CameraBridge(unsigned importer_port,const std::string& capability);
  ~CameraBridge();
  std::string url() const;
  std::string error() const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
