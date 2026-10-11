# Third-party source notices

RemoteOS includes the Asio 1.36.0 headers under the Boost Software License 1.0
and nlohmann/json 3.12.0 under the MIT License. Original notices are preserved
in their source files; full licenses accompany each vendored directory.
`dependencies.lock.json` records source locations and checked SHA-256 digests.
CMake never downloads these dependencies during configuration or compilation.

The appliance also includes independently licensed Buildroot packages, Linux,
OpenSSL, GTK, WebKitGTK, GStreamer, FFmpeg, Tailscale and fonts. Their source
hashes and license declarations live in the pinned Buildroot tree and RemoteOS
external package recipes. Run `scripts/build-image.sh legal-info` for the
distribution manifest and source/license collection. Review its warnings before
redistribution; the diagnostic selection of `b43-firmware` is explicitly marked
non-redistributable by Buildroot. This private diagnostic image is not a public
release distribution.

Windows driver dependencies remain separate, unmodified signed distributions.
Their pins are in `packaging/windows/dependencies.lock.json`; the host setup
does not replace their licensing or signature validation.
