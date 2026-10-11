#include <array>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <filesystem>
#include <iostream>
int main(int argc, char **argv) {
  try {
    if (argc != 4 && argc != 5)
      return 2;
    asio::io_context io;
    asio::ssl::context context(asio::ssl::context::tls);
    context.use_certificate_chain_file(
        (std::filesystem::path(argv[3]) / "certificate.pem").string());
    context.use_private_key_file(
        (std::filesystem::path(argv[3]) / "private-key.pem").string(),
        asio::ssl::context::pem);
    context.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> socket(io, context);
    asio::connect(socket.next_layer(),
                  asio::ip::tcp::resolver(io).resolve(argv[1], argv[2]));
    socket.handshake(asio::ssl::stream_base::client);
    std::array<unsigned char, 4> header{64, 0, 0, 0};
    if (argc == 5) {
      const std::string payload = "{\"version\":1,\"type\":\"ping\"}";
      header = {0, 0, 0, static_cast<unsigned char>(payload.size())};
      asio::write(socket, asio::buffer(header));
      asio::write(socket, asio::buffer(payload));
    } else
      asio::write(socket, asio::buffer(header));
    char byte;
    std::error_code ec;
    socket.read_some(asio::buffer(&byte, 1), ec);
    if (!ec) {
      std::cerr << "malformed frame was accepted\n";
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
