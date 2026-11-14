"""Find KSPSerialIO on the local IPv4 LAN using its existing scene responder."""
import socket
import time


def discover(port=50002, timeout=0.5, targets=("255.255.255.255",)):
    """Return the first compatible server address, or empty when none answers.

    Only broadcasts to the local network; no subnet scan or stored Pi address is needed.
    The returned address is subsequently validated by the normal kRPC handshake.
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
                if peer[1] == port and data.startswith(b"KSPHMI/1 ") and data[9:].strip():
                    return peer[0]
    except OSError:
        pass
    return ""
