#pragma once

namespace remoteos::client {
// Only navigation preferences belong here. Credentials and host identity remain
// in their existing independently protected stores.
struct SetupState {
  enum Step { Welcome, Network, Tailscale, Host };
  Step step = Welcome;
  bool direct = false;
  bool complete = false;

  static SetupState restore(bool record_exists, bool valid_record,
                            int saved_step, bool saved_direct,
                            bool saved_complete, bool existing_pair) {
    SetupState result;
    if (record_exists) {
      // A corrupt in-progress record must not become completed because pairing
      // happened before streaming succeeded.
      if (valid_record && saved_step >= Welcome && saved_step <= Host) {
        result.step = static_cast<Step>(saved_step);
        result.direct = saved_direct;
        result.complete = saved_complete;
      }
    } else if (existing_pair) {
      result.step = Host;
      result.complete = true;
    }
    return result;
  }
  bool advance(bool tailscale_running) {
    if (complete || step == Host || (step == Tailscale && !tailscale_running))
      return false;
    step = static_cast<Step>(step + 1);
    if (step == Host)
      direct = false;
    return true;
  }
  void back() {
    if (!complete && step > Welcome)
      step = static_cast<Step>(step - 1);
  }
  void use_direct() {
    if (!complete && step == Tailscale) {
      direct = true;
      step = Host;
    }
  }
  bool authenticated_session_ready(bool local_media_verified, bool peer_ready,
                                   bool input_ready) {
    if (complete || !local_media_verified || !peer_ready || !input_ready)
      return false;
    step = Host;
    complete = true;
    return true;
  }
};
} // namespace remoteos::client
