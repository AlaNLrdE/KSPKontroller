// Which of the game PC's IPv4 addresses the HMI talks to when the PC is on both Ethernet and Wi-Fi.
//
// mDNS answers a dual-homed PC's name with one address at a time (2026-11-18: Ethernet .17, then Wi-Fi .29), so
// resolving the name again flipped the endpoint. With [krpc] prefer_mac (the PC's Ethernet MAC) set, the address the
// Linux neighbour table (/proc/net/arp) shows with that MAC wins, and the choice is sticky: equivalent answers never
// move a working endpoint. Unset prefer_mac keeps the old behaviour (the resolver's first answer). docs/hmi.rst.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ksphmi {

/// One complete /proc/net/arp row.
struct ArpEntry {
    std::string ip, mac, device;  ///< mac normalised (lower case, ':' separators)
};

/// "AA-BB-CC-DD-EE-FF", "aa:bb:cc:dd:ee:ff" -> "aa:bb:cc:dd:ee:ff"; "" when it is not a 6-byte MAC address.
std::string normalise_mac(const std::string& text);

/// The complete rows of /proc/net/arp text (header skipped; flags 0x0 and 00:00:00:00:00:00 entries dropped).
std::vector<ArpEntry> parse_arp_table(const std::string& text);

/// /proc/net/arp now; empty where it does not exist or cannot be read (macOS, Windows): no preference then.
std::vector<ArpEntry> read_arp_table();

/// True for a dotted-quad IPv4 literal (no preference applies to those).
bool is_ipv4_literal(const std::string& host);

/// What the chooser remembers between resolutions (one per source).
struct EndpointMemory {
    std::string current;  ///< the address in use ("" before the first choice)
    std::string learned;  ///< the last address seen with the preferred MAC ("" never seen)
};

struct EndpointChoice {
    std::string address;  ///< "" when there is nothing to use yet
    std::string reason;   ///< "preferred MAC", "remembered preferred MAC", "kept", "resolved" or "" (no prefer_mac)
};

/// Pure rule. `resolved`: the name's IPv4 answers now (may be empty); `arp`: the neighbour table now;
/// `prefer_mac`: normalised or ""; `current_ok`: the current address answered recently (a complete neighbour entry
/// for it counts too). Updates `memory`; an empty address (nothing resolves, nothing answers) leaves it unchanged.
///   1. prefer_mac unset: the first answer (old behaviour).
///   2. the neighbour table shows prefer_mac on the same /24 as an answer, the current or the learned address: it.
///   3. the learned preferred address while it is current and answers (an expired ARP entry does not move it).
///   4. the current address while it answers or is still among the answers (never flip between equivalent ones).
///   5. otherwise the first answer (the current one stopped answering and the name points elsewhere), else "".
EndpointChoice choose_endpoint(const std::vector<std::string>& resolved, const std::vector<ArpEntry>& arp,
                               const std::string& prefer_mac, bool current_ok, EndpointMemory& memory);

/// One empty UDP datagram to `ip`:9 (discard), so the kernel learns that address's MAC for the next choice.
void poke_neighbour(const std::string& ip);

/// " (preferred MAC, was 192.168.100.29)" style suffix for the existing "endpoint NAME is IP" log lines.
std::string endpoint_note(const EndpointChoice& choice, const std::string& previous);

/// One source's PC endpoint: resolve `host`, read the neighbour table, choose, and poke the other answers so their
/// MACs are known next time. A numeric `host` or an empty `prefer_mac` is the old behaviour (first answer).
class EndpointResolver {
public:
    EndpointResolver(std::string host, std::string prefer_mac)
        : host_(std::move(host)), prefer_mac_(normalise_mac(prefer_mac)) {}
    /// `current_ok`: the address in use answered recently. Empty address: the name does not resolve yet.
    EndpointChoice resolve(bool current_ok);
    const std::string& current() const { return memory_.current; }
    const std::string& host() const { return host_; }
    bool preferring() const { return !prefer_mac_.empty() && !is_ipv4_literal(host_); }

private:
    std::string host_, prefer_mac_;
    EndpointMemory memory_;
};

}  // namespace ksphmi
