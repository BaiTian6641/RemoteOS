#include "remoteos/client_session.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
using namespace remoteos;
int main(int argc, char **argv) {
  try {
    ClientOptions o;
    o.identity_dir = "./client-identity";
    o.displays.clear();
    int timeout = 10;
    bool init = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto value = [&] {
        if (++i >= argc)
          throw std::runtime_error("missing argument");
        return std::string(argv[i]);
      };
      if (a == "--host")
        o.host = value();
      else if (a == "--port")
        o.port = unsigned(std::stoul(value()));
      else if (a == "--identity")
        o.identity_dir = value();
      else if (a == "--fingerprint")
        o.host_fingerprint = value();
      else if (a == "--pair-code-file") {
        o.pairing_code = read_private_file(value());
        while (!o.pairing_code.empty() &&
               std::isspace(static_cast<unsigned char>(o.pairing_code.back())))
          o.pairing_code.pop_back();
      } else if (a == "--timeout")
        timeout = std::stoi(value());
      else if (a == "--init")
        init = true;
      else if (a == "--media-capabilities") {
        std::cout << media_capabilities().dump(2) << '\n';
        return 0;
      } else if (a == "--help") {
        std::cout
            << "remoteosctl --host HOST --identity DIR --fingerprint SHA256 "
               "[--port PORT] [--pair-code-file FILE]\nremoteosctl --identity "
               "DIR --init\nremoteosctl --media-capabilities\n";
        return 0;
      } else
        throw std::runtime_error("unknown option " + a);
    }
    if (init) {
      ensure_identity(o.identity_dir, "RemoteOS client");
      std::cout << certificate_fingerprint(
                       std::filesystem::path(o.identity_dir) /
                       "certificate.pem")
                << '\n';
      return 0;
    }
    if (o.host.empty())
      throw std::runtime_error("--host required");
    ClientSession session;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false, ok = false;
    session.connect(o, [&](const Json &j) {
      std::cout << j.dump() << std::endl;
      std::lock_guard<std::mutex> lock(mutex);
      if (j.value("type", "") == "capabilities") {
        done = ok = true;
        cv.notify_one();
      } else if (j.value("type", "") == "error") {
        done = true;
        cv.notify_one();
      }
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_for(lock, std::chrono::seconds(timeout), [&] { return done; });
    }
    session.disconnect();
    return ok ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "remoteosctl: " << e.what() << '\n';
    return 1;
  }
}
