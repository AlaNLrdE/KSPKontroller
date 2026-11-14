"""Find KSPSerialIO on the local IPv4 LAN using its existing scene responder."""
import ipaddress
import socket
import time


def is_local(address):
    """Only private, link-local or loopback IPv4 peers may redirect the HMI."""
    try:
        ip = ipaddress.IPv4Address(address)
    except ValueError:
        return False
    return ip.is_private or ip.is_link_local or ip.is_loopback


def discover(port=50002, timeout=0.5, targets=("255.255.255.255",)):
    """Return the first compatible server address, or empty when none answers.

    Only broadcasts to the local network; no subnet scan or stored Pi address is needed.
    Replies from public addresses are ignored. The returned address is subsequently
    validated by the normal kRPC handshake. Set ``discovery = false`` in config.toml
    on an untrusted network: any LAN host that answers the scene protocol could
    otherwise redirect the display.
    """
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            for target in targets:
                sock.sendto(b"KSPHMI/1", (target, port))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                sock.settimeout(max(0.001, deadline - time.monotonic()))
                data, peer = sock.recvfrom(256)
                if (peer[1] == port and is_local(peer[0])
                        and data.startswith(b"KSPHMI/1 ") and data[9:].strip()):
                    return peer[0]
    except OSError:
        pass
    return ""
