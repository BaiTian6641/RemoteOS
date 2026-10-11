#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace remoteos::power {
inline bool has_word(const std::string& text, const std::string& word) {
  std::istringstream input(text);
  for (std::string item; input >> item;)
    if (item == word || item == "[" + word + "]") return true;
  return false;
}
inline void suspend(const std::filesystem::path& root = "/sys/power") {
  std::ifstream states(root / "state");
  std::string available; std::getline(states, available);
  if (!has_word(available, "mem"))
    throw std::runtime_error("This machine does not support suspend to RAM");
  std::ifstream modes(root / "mem_sleep");
  std::string supported; std::getline(modes, supported);
  if (has_word(supported, "deep")) {
    std::ofstream selected(root / "mem_sleep");
    selected << "deep"; selected.close();
    if (!selected) throw std::runtime_error("Cannot select suspend to RAM");
  }
  if (root == "/sys/power") ::sync();
  std::ofstream state(root / "state");
  state << "mem"; state.close(); // The kernel write returns after resume.
  if (!state) throw std::runtime_error("Suspend was rejected by the kernel");
}
}
