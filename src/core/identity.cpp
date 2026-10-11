#include "remoteos/identity.hpp"
#include "remoteos/protocol.hpp"
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace remoteos {
#ifdef _WIN32
namespace {
struct PrivateSecurity {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  PrivateSecurity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      throw std::runtime_error("cannot inspect identity owner");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    const BOOL ok =
        GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
    CloseHandle(token);
    LPSTR sid = nullptr;
    if (!ok ||
        !ConvertSidToStringSidA(
            reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid))
      throw std::runtime_error("cannot resolve identity owner SID");
    const std::string acl = "D:P(A;OICI;FA;;;" + std::string(sid) +
                            ")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
      throw std::runtime_error("cannot create private identity ACL");
    attributes.lpSecurityDescriptor = descriptor;
  }
  ~PrivateSecurity() {
    if (descriptor)
      LocalFree(descriptor);
  }
};
} // namespace
#endif
void write_private_file(const std::filesystem::path &p,
                        const std::string &data) {
  auto parent = p.parent_path();
  std::filesystem::create_directories(parent);
  auto tmp = p.string() + "." + random_token(8) + ".tmp";
#ifndef _WIN32
  int fd = ::open(tmp.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create private file");
  std::size_t n = 0;
  while (n < data.size()) {
    auto w = ::write(fd, data.data() + n, data.size() - n);
    if (w <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      throw std::runtime_error("private file write failed");
    }
    n += std::size_t(w);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw std::runtime_error("private file sync failed");
  }
  ::close(fd);
#else
  PrivateSecurity security;
  const auto temporary = std::filesystem::path(tmp).wstring();
  HANDLE file = CreateFileW(
      temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error("cannot create private file");
  std::size_t offset = 0;
  bool success = true;
  while (offset < data.size()) {
    const auto length = static_cast<DWORD>(
        std::min<std::size_t>(data.size() - offset, MAXDWORD));
    DWORD written = 0;
    if (!WriteFile(file, data.data() + offset, length, &written, nullptr) ||
        !written) {
      success = false;
      break;
    }
    offset += written;
  }
  success = success && FlushFileBuffers(file);
  CloseHandle(file);
  if (!success ||
      !MoveFileExW(temporary.c_str(), p.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw std::runtime_error("private file commit failed");
  }
#endif
#ifndef _WIN32
  std::filesystem::rename(tmp, p);
#endif
}
std::string read_private_file(const std::filesystem::path &p) {
  if (std::filesystem::is_symlink(p))
    throw std::runtime_error("refusing symlink for private state");
  std::ifstream f(p, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot read " + p.string());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void ensure_identity(const std::filesystem::path &dir,
                     const std::string &name) {
  if (std::filesystem::is_symlink(dir))
    throw std::runtime_error("identity directory cannot be a symlink");
  std::filesystem::create_directories(dir);
#ifndef _WIN32
  if (::chmod(dir.c_str(), 0700) != 0)
    throw std::runtime_error("cannot restrict identity directory");
#else
  if (GetFileAttributesW(dir.wstring().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
    throw std::runtime_error("identity directory cannot be a reparse point");
  PrivateSecurity security;
  PACL acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(security.descriptor, &present, &acl,
                                 &defaulted) ||
      !present ||
      SetNamedSecurityInfoW(
          const_cast<wchar_t *>(dir.wstring().c_str()), SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          nullptr, nullptr, acl, nullptr) != ERROR_SUCCESS)
    throw std::runtime_error("cannot restrict identity directory ACL");
#endif
  auto certpath = dir / "certificate.pem", keypath = dir / "private-key.pem";
  if (std::filesystem::exists(certpath) && std::filesystem::exists(keypath))
    return;
  if (std::filesystem::exists(certpath) || std::filesystem::exists(keypath))
    throw std::runtime_error(
        "incomplete identity; restore or explicitly reset it");
  using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  Key key(EVP_EC_gen("prime256v1"), EVP_PKEY_free);
  if (!key)
    throw std::runtime_error("key generation failed");
  std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
  X509_set_version(cert.get(), 2);
  auto serial = random_token(8);
  ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert.get()),
                          std::stoull(serial, nullptr, 16));
  X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300);
  X509_gmtime_adj(X509_getm_notAfter(cert.get()), long(5L * 365 * 24 * 3600));
  X509_set_pubkey(cert.get(), key.get());
  auto subject = X509_get_subject_name(cert.get());
  X509_NAME_add_entry_by_txt(
      subject, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>(name.c_str()), int(name.size()),
      -1, 0);
  X509_set_issuer_name(cert.get(), subject);
  if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0)
    throw std::runtime_error("certificate signing failed");
  auto pem = [](BIO *b) {
    char *data = nullptr;
    long len = BIO_get_mem_data(b, &data);
    return std::string(data, std::size_t(len));
  };
  std::unique_ptr<BIO, decltype(&BIO_free)> b(BIO_new(BIO_s_mem()), BIO_free);
  if (!PEM_write_bio_PrivateKey(b.get(), key.get(), nullptr, nullptr, 0,
                                nullptr, nullptr))
    throw std::runtime_error("key encoding failed");
  write_private_file(keypath, pem(b.get()));
  BIO_reset(b.get());
  if (!PEM_write_bio_X509(b.get(), cert.get()))
    throw std::runtime_error("certificate encoding failed");
  write_private_file(certpath, pem(b.get()));
}
std::string certificate_fingerprint(const std::filesystem::path &path) {
  auto data = read_private_file(path);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(data.data(), int(data.size())), BIO_free);
  std::unique_ptr<X509, decltype(&X509_free)> cert(
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
  if (!cert)
    throw std::runtime_error("invalid certificate");
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned len = 0;
  if (X509_digest(cert.get(), EVP_sha256(), digest, &len) != 1)
    throw std::runtime_error("certificate digest failed");
  const char *hex = "0123456789abcdef";
  std::string fp;
  for (unsigned i = 0; i < len; ++i) {
    fp += hex[digest[i] >> 4];
    fp += hex[digest[i] & 15];
  }
  return fp;
}
std::string combined_identity_pem(const std::filesystem::path &dir) {
  return read_private_file(dir / "certificate.pem") +
         read_private_file(dir / "private-key.pem");
}
} // namespace remoteos
