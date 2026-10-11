# RemoteOS 0.1.8 diagnostic session update

Install the updated HP image and run the matching application host. The client
defaults to MPEG-2 and a 40 Mbps connection budget; the old raw60 selection is
removed from the appliance setup. Capture, color conversion, scaling and
compression run on the host. The HP retains software MPEG-2 decoding and
accelerated YUV presentation; GMA 950 full MPEG-2 hardware decoding is not assumed.

After connecting, the local cursor hides. A short power press releases held
host inputs and restores the local cursor for the Carbon overlay. Resume hides
it again; disconnect and failures restore it. Mouse/touchpad forwarding now
uses Xorg's processed pointer coordinates, with one-to-one mapping at native
size and explicit handling for VGA placement, letterboxing and encoded scaling.

The host list combines automatically discovered hosts with saved pairings.
Start sharing in the host app, select its row on the HP, compare the short
verification value on both screens and enter the host's six-digit PIN. The PIN
expires after ten minutes, accepts at most five attempts and is consumed on
successful pairing. Saved hosts reconnect with their stored TLS identities.
Host names, public fingerprints and ports are discovered; PINs are never
sent through discovery. Changed saved identities require re-verification.

**Match host resolution** is enabled on the HP. Xorg can use an existing mode
or a RandR transform performed by the host GPU, then restore the original
desktop on disconnect. Windows uses driver-supported modes. A physical Wayland
monitor must already match or be configured through the host's display settings.
**Correction in host 0.1.9:** PipeWire format renegotiation alone does not prove
that a virtual desktop resized. The standard portal has no resolution setter;
Plasma 6.5's RemoteDesktop virtual output is fixed at 1920×1080. The new host
Display source control explicitly requests a virtual output. An unavailable
exact match reports an error; clearing the HP option explicitly enables
host-side video scaling. See [the host update](virtual-display-0.1.9.md). The supplied HP logs identify a 1024×576 panel;
1366×768 variants are detected automatically.

Two displays share a 32 Mbps encoder ceiling under the default 40 Mbps limit.
VBV/maxrate settings bound MPEG-2 encoding, and a host-side pacer sends each
encrypted datagram separately. Media has a conservative maximum reservation
of 36 Mbps including charged packet/tunnel overhead; audio has a separate
reservation, leaving room for input/control. A loopback saturation test measures
the actual encrypted UDP rate and checks playback continues while video fills
its allocation. This does not constrain other applications on the host.

Validation uses isolated Xvfb desktops and a private portal/PipeWire fixture.
The connected-session test checks actual cursor pixels, GTK pointer forwarding
at adjacent native pixels and the last pixel, numeric pairing, discovery, saved
host persistence, exact native host resolution and release of held Ctrl/mouse
buttons on opening the power overlay. Actual HP touchpad/Intel behavior,
sustained Wi-Fi/dual-screen decoding and real GNOME/Plasma virtual-output behavior
remain hardware/runtime qualification tasks. The supplied Linux bundle is native
Ubuntu 26.04; build locally on Ubuntu 24.04 using the application host guide.
