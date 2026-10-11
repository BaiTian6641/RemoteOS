#pragma once
#include <string>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
namespace remoteos::net {
inline bool hardware_gcm_available() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int registers[4]{}; __cpuid(registers, 1);
  return (registers[2] & (1 << 25)) && (registers[2] & (1 << 1));
#elif defined(__i386__) || defined(__x86_64__)
  unsigned a=0,b=0,c=0,d=0;
  return __get_cpuid(1,&a,&b,&c,&d) && (c & bit_AES) && (c & bit_PCLMUL);
#else
  // Conservative software preference; OpenSSL still dispatches optimized code.
  return false;
#endif
}
inline const char* tls_ciphers(bool accelerated_gcm) {
  return accelerated_gcm
    ? "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384"
    : "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384";
}
}
