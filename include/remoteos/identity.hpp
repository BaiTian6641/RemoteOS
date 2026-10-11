#pragma once
#include <filesystem>
#include <string>

namespace remoteos {
void ensure_identity(const std::filesystem::path &, const std::string &name);
std::string certificate_fingerprint(const std::filesystem::path &);
std::string read_private_file(const std::filesystem::path &);
void write_private_file(const std::filesystem::path &, const std::string &);
std::string combined_identity_pem(const std::filesystem::path &);
} // namespace remoteos
