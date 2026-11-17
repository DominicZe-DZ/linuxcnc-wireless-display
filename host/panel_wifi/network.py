# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""Wi-Fi discovery and unicast pinned to one interface with Linux IP_PKTINFO.

The socket listens for broadcasts but only accepts the configured Wi-Fi
interface and its subnet. The Mesa Ethernet link is never used as a fallback.
"""
from dataclasses import dataclass
import ipaddress
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys

# IP_PKTINFO socket option (value from linux/in.h; not exported by every
# Python version). With it enabled, every received datagram comes with a
# control message telling which interface it arrived on, and on send we can
# attach one that forces the outgoing interface and source address.
#
# The kernel structure (struct in_pktinfo, 12 bytes, host byte order for the
# int, network byte order for the addresses):
#
#   offset  size  field
#   0       4     ipi_ifindex   interface index (unsigned int)
#   4       4     ipi_spec_dst  local address  (on send: source to use)
#   8       4     ipi_addr      header destination address
#
# In struct notation that is "=I4s4s": native byte order, no padding, one
# unsigned int and two 4-byte strings.
IP_PKTINFO = 8
# Buffer for the ancillary data: CMSG_SPACE adds the cmsghdr header and the
# alignment padding around the 12 payload bytes.
ANCILLARY_SIZE = getattr(socket, "CMSG_SPACE", lambda _: 64)(12)
# recvmsg flags meaning "the datagram (or its control data) didn't fit and
# was cut". A truncated packet is never parsed.
TRUNCATED = getattr(socket, "MSG_TRUNC", 0x20) | getattr(socket, "MSG_CTRUNC", 0x08)


@dataclass(frozen=True)
class Link:
    """The Wi-Fi interface as seen at one moment. Compared as a whole every
    2 s: if any field changes the socket is rebuilt."""
    source: str                     # our IPv4 address on the Wi-Fi
    network: ipaddress.IPv4Network  # its subnet, e.g. 192.168.1.0/24
    index: int                      # kernel interface index (if_nametoindex)


def inspect_link(config, demo=False, run=subprocess.check_output):
    """Reads the IPv4 address of the configured interface with `ip -j`.

    Refuses to continue (exception) if the interface is not wireless (checked
    through /sys/class/net/<if>/wireless, skipped in demo mode so loopback
    can be used in tests), if it has no usable address, or if its subnet
    overlaps the Mesa network. Link-local 169.254.x.x addresses are ignored:
    they mean DHCP failed. `run` is injectable for the unit tests.
    """
    if not sys.platform.startswith("linux"):
        raise RuntimeError("the sender only runs on Linux (the unit tests run anywhere)")
    if not demo and not Path("/sys/class/net", config.interface, "wireless").is_dir():
        raise ValueError("the configured interface is not a Wi-Fi interface")
    links = json.loads(run(["ip", "-j", "-4", "addr", "show", "dev", config.interface], timeout=2))
    if not links:
        raise OSError("interface not available")
    excluded = ipaddress.IPv4Network(config.mesa_network)
    candidates = []
    for entry in links[0].get("addr_info", []):
        source = ipaddress.IPv4Interface(f"{entry['local']}/{entry['prefixlen']}")
        if source.network.overlaps(excluded):
            raise ValueError("Wi-Fi and Mesa subnets overlap")
        if not source.ip.is_link_local and not source.ip.is_unspecified:
            candidates.append(source)
    if not candidates:
        raise OSError("no usable IPv4 address on the Wi-Fi interface")
    source = candidates[0]
    return Link(str(source.ip), source.network, socket.if_nametoindex(config.interface))


class WifiTransport:
    """Learns the display address once a valid hello from the pair arrives."""
    def __init__(self, config, demo=False):
        self.config, self.demo = config, demo
        self.link = inspect_link(config, demo)
        self.destination = None
        self.next_link_check = 0.0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.setsockopt(socket.IPPROTO_IP, IP_PKTINFO, 1)
            self.sock.bind(("0.0.0.0", config.host_port))
            self.sock.setblocking(False)
        except BaseException:
            self.sock.close()
            raise

    def check_link(self, now):
        # Slow check, every 2 s. Never spawn a process per sample.
        if now >= self.next_link_check:
            self.next_link_check = now + 2
            if inspect_link(self.config, self.demo) != self.link:
                raise OSError("Wi-Fi address changed, rebuilding the link")

    def receive(self):
        """Returns None when nothing is waiting, (b"", None) for a datagram
        that was read but rejected, or (packet, (ip, port)) for one that
        passed the filters: right interface, port, subnet, not truncated, and
        not sent from the network or broadcast address itself."""
        try:
            packet, ancillary, flags, address = self.sock.recvmsg(1201, ANCILLARY_SIZE)
        except BlockingIOError:
            return None
        index = None
        for level, kind, data in ancillary:
            if level == socket.IPPROTO_IP and kind == IP_PKTINFO and len(data) >= 12:
                # Only ipi_ifindex is needed: which interface it came in on.
                index = struct.unpack("=I4s4s", data[:12])[0]
        peer = ipaddress.IPv4Address(address[0])
        if (flags & TRUNCATED or len(packet) > 1200
                or index != self.link.index or address[1] != self.config.screen_port
                or peer not in self.link.network
                or peer in (self.link.network.network_address, self.link.network.broadcast_address)):
            return b"", None  # consumed but invalid, still counts towards the 8 per loop
        return packet, address

    def select(self, address):
        self.destination = address

    def send(self, packet):
        if self.destination is None:
            raise ValueError("no display discovered yet")
        # in_pktinfo for sending: the interface index pins the Wi-Fi card and
        # ipi_spec_dst sets our source address. The routing table is not
        # consulted for the interface, so even with a default route through
        # Ethernet, this packet can only leave through the Wi-Fi.
        info = struct.pack("=I4s4s", self.link.index, socket.inet_aton(self.link.source), b"\0" * 4)
        self.sock.sendmsg([packet], [(socket.IPPROTO_IP, IP_PKTINFO, info)], 0, self.destination)

    def close(self):
        self.sock.close()
