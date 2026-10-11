#pragma once
#include <cstdint>
#include <string>
#include <regex>
#include <vector>

namespace remoteos::devices {
struct Approval {
    std::string vid, pid;
    std::string serial; // Empty means the administrator approved this model.
};
struct UsbPolicy {
    std::vector<Approval> approved;
    // Explicit client boot disk and backing disks, e.g. /dev/sda. Required for
    // storage forwarding even when no filesystem on the candidate is mounted.
    std::vector<std::string> protected_block_devices;
};
struct UsbDevice {
    std::string bus_id, vid, pid, serial, label;
    std::vector<unsigned> interface_classes;
    std::vector<std::string> block_nodes;
    bool eligible=false;
    std::string reason;
};
inline bool valid_bus_id(const std::string& bus_id) {
    static const std::regex pattern("[1-9][0-9]*-[1-9][0-9]*(\\.[1-9][0-9]*)*");
    return bus_id.size()<=31 && std::regex_match(bus_id,pattern);
}
std::vector<UsbDevice> enumerate_usb(const UsbPolicy& policy,
    const std::string& sys_root="/sys",const std::string& mountinfo="/proc/self/mountinfo");
// These are privileged operations, intended for the authenticated supervisor.
// Callers must first stop all local camera use and establish the authenticated
// reverse tunnel. No function starts stock usbipd or exposes a network port.
void bind_usb(const std::string& bus_id,const UsbPolicy& policy,
    const std::string& usbip_path="/usr/sbin/usbip");
void unbind_usb(const std::string& bus_id,const std::string& usbip_path="/usr/sbin/usbip");
std::string attach_loopback(const std::string& bus_id,std::uint16_t local_port,
    const std::string& usbip_path="/usr/sbin/usbip");
// Refuses mounted volumes, flushes unmounted imported block devices, then
// detaches the requested VHCI port. Never silently unmounts user filesystems.
void detach_import(unsigned port,const std::string& usbip_path="/usr/sbin/usbip");
unsigned import_device(const std::string& bus_id,std::uint16_t local_port,
    const std::string& usbip_path="/usr/sbin/usbip");
void detach_device(unsigned port,const std::string& usbip_path="/usr/sbin/usbip");
// Abrupt network-loss cleanup only, after revoking the authenticated tunnel.
// Verify the exact owned import record; unplugged storage cannot be flushed.
void abandon_import(unsigned port,const std::string& bus_id,std::uint16_t local_port,
    const std::string& usbip_path="/usr/sbin/usbip");
} // namespace remoteos::devices
