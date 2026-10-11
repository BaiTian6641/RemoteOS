#include "crypto_policy.hpp"
#include "transport.hpp"
#include "remoteos/identity.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
  char path[] = "/tmp/remoteos-cipher-fixture-XXXXXX";
  auto* root = mkdtemp(path); if (!root) return 2;
  struct Cleanup { std::filesystem::path p; ~Cleanup() { std::filesystem::remove_all(p); } } cleanup{root};
  try {
    remoteos::ensure_identity(root,"cipher fixture");
    for (bool hardware : {false,true}) {
      asio::ssl::context client(asio::ssl::context::tls), server(asio::ssl::context::tls);
      remoteos::net::configure(client,root); remoteos::net::configure(server,root);
      require(SSL_CTX_set_ciphersuites(client.native_handle(),remoteos::net::tls_ciphers(hardware)),"Client cipher setup");
      require(SSL_CTX_set_ciphersuites(server.native_handle(),remoteos::net::tls_ciphers(!hardware)),"Server cipher setup");
      SSL* c=SSL_new(client.native_handle()); SSL* s=SSL_new(server.native_handle());
      BIO *c_bio=nullptr,*s_bio=nullptr;
      require(BIO_new_bio_pair(&c_bio,0,&s_bio,0)==1,"BIO pair");
      SSL_set_bio(c,c_bio,c_bio); SSL_set_bio(s,s_bio,s_bio);
      SSL_set_connect_state(c); SSL_set_accept_state(s);
      for (unsigned i=0;i<100 && (!SSL_is_init_finished(c)||!SSL_is_init_finished(s));++i) {
        for (SSL* peer : {c,s}) {
          int result=SSL_do_handshake(peer);
          if (result != 1) {
            int e=SSL_get_error(peer,result);
            require(e==SSL_ERROR_WANT_READ || e==SSL_ERROR_WANT_WRITE,"TLS handshake failed");
          }
        }
      }
      require(SSL_is_init_finished(c) && SSL_is_init_finished(s),"TLS did not finish");
      require(SSL_version(c)==TLS1_3_VERSION,"TLS version changed");
      require(std::string(SSL_get_cipher_name(c)) == (hardware ? "TLS_AES_128_GCM_SHA256" : "TLS_CHACHA20_POLY1305_SHA256"),"Server overrode client CPU preference");
      const char bytes[]="authenticated payload"; char recovered[64]{};
      require(SSL_write(c,bytes,sizeof(bytes))==sizeof(bytes),"TLS write");
      require(SSL_read(s,recovered,sizeof(recovered))==sizeof(bytes),"TLS read");
      require(std::string(recovered)==bytes,"TLS payload changed");
      SSL_free(c); SSL_free(s);
    }
    std::cout << "TLS 1.3 client CPU preference and encrypted payload roundtrip passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
