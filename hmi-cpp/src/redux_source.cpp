#include "redux_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

#include "fmt.hpp"
#include "log.hpp"

namespace ksphmi {
namespace {

constexpr const char* LOG = "ksphmi.redux";
constexpr int REDUX_PORT = 50003;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string unhex(const std::string& text) {
    if (text == "-") return "";
    if (text.size() > 512 || text.size() % 2) throw std::runtime_error("Invalid Redux label");
    std::string result;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("Invalid Redux label hex");
    };
    for (size_t i = 0; i < text.size(); i += 2) {
        char c = char(digit(text[i]) * 16 + digit(text[i + 1]));
        if (static_cast<unsigned char>(c) < 32 || c == 127) throw std::runtime_error("Control in Redux label");
        result += c;
    }
    return result;
}

// Send one request and return the first reply `accept` takes; earlier, late or foreign datagrams are discarded.
template <class Accept>
auto exchange(net::socket_t fd, const std::string& request, double timeout, Accept accept) -> decltype(accept(std::string())) {
    static thread_local char data[16385];
    while (net::recv_some(fd, data, sizeof(data)) >= 0) { /* drop replies to abandoned requests */ }
    if (net::send_some(fd, request.data(), request.size()) < 0) return std::nullopt;
    const double deadline = now() + timeout;
    for (double left = timeout; left > 0; left = deadline - now()) {
        if (net::wait(fd, false, std::max(1, int(left * 1000))) <= 0) return std::nullopt;
        const long count = net::recv_some(fd, data, sizeof(data));
        if (count <= 0) {
            // Windows reports an ICMP port-unreachable (game not running) on the next receive: stop waiting.
            if (count < 0 && !net::would_block(net::last_error())) return std::nullopt;
            continue;
        }
        try {
            if (auto result = accept(std::string(data, size_t(count)))) return result;
        } catch (const std::exception&) { /* reject malformed, delayed or foreign responses; keep waiting */ }
    }
    return std::nullopt;
}

std::string key_name(const Command& c, int& value) {
    value = 0;
    switch (c.kind) {
        case Command::ToggleSas: return "sas";
        case Command::ToggleRcs: return "rcs";
        case Command::Stage: return "stage";
        case Command::ToggleGear: return "gear";
        case Command::ToggleLights: return "lights";
        case Command::SetBrakes: value = c.value ? 1 : 0; return "brakes";
        case Command::Abort: return "abort";
        case Command::ActionGroup: value = c.value == 0 ? 10 : c.value; return "group";  // kRPC group 0 = key 0 = group 10
        case Command::SasMode: value = c.value; return "sasmode";                            // kRPC SASMode
        case Command::ThrottleFull: return "throttle_full";
        case Command::ThrottleCut: return "throttle_cut";
        case Command::WarpUp: return "warp_up";
        case Command::WarpDown: return "warp_down";
        case Command::WarpStop: return "warp_stop";
        case Command::Quicksave: return "quicksave";
        case Command::Quickload: return "quickload";
        case Command::ToggleMap: return "map";
        case Command::ToggleControl:  // cockpit panel controls: no KSP2KEY verb yet (docs/hmi.rst)
        case Command::SpeedMode:
        case Command::DeployChutes:
        case Command::CutChutes:
        case Command::MechJebSet:  // MechJeb is a KSP 1 mod: the Redux source never sends it
        case Command::MechJebAction: return "";
    }
    return "";
}

std::string number(double v) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << v;
    return out.str();
}

}  // namespace

std::string redux_scene(const std::string& state, bool flight) {
    // Redux GameState names -> existing facility view keys. Redux enters the tracking station as Map3DView without
    // an active flight vessel; a flight map keeps its flight snapshot.
    if (state == "KerbalSpaceCenter") return "space_center";
    if (state == "VehicleAssemblyBuilder") return "vab";
    if (state == "TrackingStation" || (state == "Map3DView" && !flight)) return "tracking_station";
    if (state == "MissionControl") return "mission_control";
    if (state == "ResearchAndDevelopment") return "research";
    return state;
}

Quat redux_rotation(Vec3 forward, Vec3 up) {
    auto f = unit(forward), u = unit(up);
    if (!f || !u || std::fabs(dot(*f, *u)) > 1e-5) throw std::runtime_error("Invalid Redux observation axes");
    double h = std::atan2(f->z, f->y), p = std::atan2(f->x, std::hypot(f->y, f->z));
    Vec3 level_right{0, -std::sin(h), std::cos(h)};
    Vec3 level_up{std::cos(p), -std::sin(p) * std::cos(h), -std::sin(p) * std::sin(h)};
    double r = std::atan2(dot(*u, level_right), dot(*u, level_up));
    return attitude(degrees_of(p), degrees_of(h), degrees_of(r));
}

Snapshot decode_redux(const std::string& packet, const std::string& nonce, double received) {
    if (packet.size() > 4096) throw std::runtime_error("Oversized Redux response");
    std::istringstream in(packet);
    in.imbue(std::locale::classic());
    std::string magic, echoed, scene, vessel, body, situation, extra;
    int flight = -1;
    if (!(in >> magic >> echoed >> flight >> scene >> vessel >> body >> situation) || (magic != "KSP2HMI1" && magic != "KSP2HMI2") ||
        echoed != nonce || (flight != 0 && flight != 1))
        throw std::runtime_error("Invalid Redux response header");
    double v[24];
    for (double& value : v)
        if (!(in >> value) || !std::isfinite(value)) throw std::runtime_error("Invalid Redux number");
    Snapshot s;
    s.scene = unhex(scene);
    s.vessel = unhex(vessel);
    s.body = unhex(body);
    s.situation = unhex(situation);
    s.received = received;
    s.state = flight && v[23] == 1 ? State::Flight : State::NoVessel;
    s.detail = "KSP2 Redux: " + s.scene + (v[22] == 1 ? " (paused)" : "");
    s.scene = redux_scene(s.scene, s.state == State::Flight);
    s.ut = v[0];
    s.body_radius = v[1];
    s.atmosphere_depth = v[2];
    s.altitude = v[3];
    s.apoapsis = v[4];
    s.periapsis = v[5];
    s.time_to_apoapsis = v[6];
    s.time_to_periapsis = v[7];
    s.inclination = v[8];
    s.eccentricity = v[9];
    s.period = v[10];
    s.semi_major_axis = v[11];
    s.true_anomaly = v[12];
    s.orbital_speed = v[13];
    s.surface_speed = v[14];
    s.vertical_speed = v[15];
    if (magic == "KSP2HMI2") {
        int flags, stage, count;
        if (!(in >> flags) || flags < 0 || flags > 255) throw std::runtime_error("Invalid navigation flags");
        double n[26];
        for (double& value : n)
            if (!(in >> value) || !std::isfinite(value)) throw std::runtime_error("Invalid navigation value");
        if (!(in >> stage >> count) || stage < -1 || stage > 65535 || count < -1 || count > 16)
            throw std::runtime_error("Invalid system counts");
        if (n[21] < 0 || n[21] > 1 || n[23] < 0 || n[23] > 10 || std::floor(n[23]) != n[23] || v[20] < 0 || v[20] > 65535)
            throw std::runtime_error("Invalid navigation control state");
        Systems systems;
        systems.native_resources = true;
        if (stage >= 0) systems.current_stage = stage;
        std::set<std::string> names;
        for (int i = 0; i < count; ++i) {
            std::string name;
            double amount, capacity;
            if (!(in >> name >> amount >> capacity) || !std::isfinite(amount) || !std::isfinite(capacity) || amount < 0 || capacity < 0)
                throw std::runtime_error("Invalid native resource");
            name = unhex(name);
            if (name.empty() || !names.insert(name).second) throw std::runtime_error("Duplicate or empty resource");
            systems.resources.push_back({name, amount, capacity});
        }
        // Missing totals keep the whole resource panel unavailable. Stage alone is not a stage-performance report.
        if (count >= 0 && s.state == State::Flight) s.systems = std::move(systems);
        if ((flags & 1) && s.state == State::Flight) {
            Navigation nav;
            nav.rotation = redux_rotation({n[0], n[1], n[2]}, {n[3], n[4], n[5]});
            nav.pitch = v[16];
            nav.heading = v[17];
            nav.roll = v[18];
            nav.g_force = v[19];
            nav.sas = (int(v[20]) & 1) != 0;
            nav.rcs = (int(v[20]) & 2) != 0;
            nav.throttle = n[21];
            nav.speed_mode = v[21] == 2 ? "surface" : v[21] == 3 ? "target" : "orbit";
            if (flags & 64) nav.speed = n[22];
            static const char* modes[] = {"unknown", "stability_assist", "prograde", "retrograde", "normal", "anti_normal",
                                          "anti_radial", "radial", "target", "anti_target", "maneuver"};
            nav.sas_mode = modes[int(n[23])];
            auto marker = [&](int bit, int offset) -> std::optional<Vec3> {
                if (!(flags & bit)) return std::nullopt;
                return unit(Vec3{n[offset], n[offset + 1], n[offset + 2]});
            };
            nav.prograde = marker(2, 6);
            nav.normal = marker(4, 9);
            nav.radial = marker(8, 12);
            nav.target = marker(16, 15);
            nav.maneuver = marker(32, 18);
            if (flags & 128) {
                nav.node_dv = n[24];
                nav.node_time = n[25];
            }
            s.nav = std::move(nav);
        }
    }
    if (in >> extra) throw std::runtime_error("Trailing Redux data");
    return s;
}

std::optional<std::string> decode_redux_text(const std::string& packet, const std::string& prefix, const std::string& nonce) {
    const std::string header = prefix + " " + nonce + "\n";
    if (packet.size() > 16384 || packet.compare(0, header.size(), header) != 0) throw std::runtime_error("Invalid Redux text header");
    std::string body = packet.substr(header.size());
    if (body == "-") return std::nullopt;
    return body;
}

std::shared_ptr<const ShipReport> decode_redux_ship(const std::string& packet, const std::string& nonce) {
    auto body = decode_redux_text(packet, "KSP2SHIP", nonce);
    if (!body) return nullptr;
    std::string error;
    auto report = parse_ship_report(*body, &error);
    if (!report || report->facility != "vab") throw std::runtime_error("Invalid Redux ship report: " + error);
    return std::make_shared<const ShipReport>(std::move(*report));
}

std::string decode_redux_ack(const std::string& packet, const std::string& prefix, const std::string& nonce) {
    const std::string header = prefix + " " + nonce + " ";
    if (packet.size() > 64 || packet.compare(0, header.size(), header) != 0) throw std::runtime_error("Invalid Redux acknowledgement");
    std::string status = packet.substr(header.size());
    if (status.empty() || status.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
        throw std::runtime_error("Invalid Redux acknowledgement status");
    return status;
}

std::string redux_key_request(const Command& c, const std::string& nonce) {
    int value = 0;
    const std::string name = key_name(c, value);
    return "KSP2KEY " + nonce + " " + name + " " + std::to_string(value);
}

std::string redux_hold_request(const Held& h, const std::string& nonce) {
    std::string r = "KSP2HOLD " + nonce;
    for (float v : {h.pitch, h.yaw, h.roll, h.x, h.y, h.z}) r += " " + number(std::clamp(double(v), -1.0, 1.0));
    return r + " " + std::to_string(h.throttle > 0 ? 1 : h.throttle < 0 ? -1 : 0);
}

std::optional<ReduxReply> redux_exchange(net::socket_t fd, const std::string& request, const std::string& nonce, double timeout) {
    return exchange(fd, request, timeout, [&](const std::string& packet) -> std::optional<ReduxReply> {
        return ReduxReply{decode_redux(packet, nonce, now()), packet.rfind("KSP2HMI2 ", 0) == 0};
    });
}

std::optional<std::shared_ptr<const ShipReport>> redux_ship_exchange(net::socket_t fd, const std::string& nonce, double timeout) {
    return exchange(fd, "KSP2SHIP " + nonce, timeout, [&](const std::string& packet) -> std::optional<std::shared_ptr<const ShipReport>> {
        return decode_redux_ship(packet, nonce);
    });
}

std::optional<std::optional<std::string>> redux_text_exchange(net::socket_t fd, const std::string& prefix, const std::string& nonce,
                                                              double timeout) {
    return exchange(fd, prefix + " " + nonce, timeout, [&](const std::string& packet) -> std::optional<std::optional<std::string>> {
        return decode_redux_text(packet, prefix, nonce);
    });
}

std::optional<std::string> redux_ack_exchange(net::socket_t fd, const std::string& request, const std::string& prefix,
                                              const std::string& nonce, double timeout) {
    return exchange(fd, request, timeout, [&](const std::string& packet) -> std::optional<std::string> {
        return decode_redux_ack(packet, prefix, nonce);
    });
}

ReduxSource::ReduxSource(KrpcSource::Options options, bool fallback)
    : Source("Redux"), options_(std::move(options)), fallback_(fallback) {}

void ReduxSource::command(const Command& c) {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (!linked_ && krpc_) {
            krpc_->command(c);  // KSP 1 through the fallback
            return;
        }
        if (linked_ && commands_.size() < 16) {
            commands_.push_back(c);
            return;
        }
    }
    log::info(LOG, "key " + command_name(c) + " ignored: no game link");
}

void ReduxSource::hold(const Held& h) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    held_ = h;
    if (krpc_) krpc_->hold(linked_ ? Held{} : h);
}

void ReduxSource::set_cockpit(bool shown) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    cockpit_ = shown;
    if (krpc_) krpc_->set_cockpit(shown);  // KSP 1 through the fallback
}

void ReduxSource::send_inputs(net::socket_t fd, uint64_t& sequence) {
    std::vector<Command> todo;
    Held held;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        todo.swap(commands_);
        held = held_;
    }
    for (const Command& c : todo) {
        int unused = 0;
        if (key_name(c, unused).empty()) {
            log::warning(LOG, "key " + command_name(c) + " not applied: not supported by the KSP 2 Redux plugin");
            continue;
        }
        // Never resent: the game acknowledges a queued press, and a lost reply must not apply it twice.
        const std::string nonce = std::to_string(++sequence);
        auto status = redux_ack_exchange(fd, redux_key_request(c, nonce), "KSP2KEY", nonce, 0.08);
        if (status && *status == "queued") log::info(LOG, "key: " + command_name(c) + " sent to KSP 2");
        else log::warning(LOG, "key " + command_name(c) + " not applied: " + (status ? *status : std::string("no answer")) +
                                   (status && *status == "disabled" ? " (HmiCommands is false in the plugin settings)" : ""));
    }
    // Held keys every tick while held, then once with zeros; the plugin releases them 0.5 s after the last one.
    if (!(held.axes() || held.throttle || sent_.axes() || sent_.throttle)) return;
    const std::string nonce = std::to_string(++sequence);
    auto status = redux_ack_exchange(fd, redux_hold_request(held, nonce), "KSP2HOLD", nonce, 0.05);
    const std::string problem = status && *status == "queued" ? "" : status ? *status : "no answer";
    if (problem != held_problem_) {
        if (problem.empty()) log::info(LOG, "held keys applied again");
        else log::warning(LOG, "held keys not applied: " + problem);
        held_problem_ = problem;
    }
    sent_ = held;
}

void ReduxSource::run() {
    // The PC address already configured for kRPC (KSPHMI_REDUX_ADDRESS overrides it), a name or a number. No
    // discovery. A name is resolved again while Redux stays silent, so a DHCP change or a name that does not resolve
    // yet at boot never needs a restart; the kRPC fallback keeps running meanwhile (user, 2026-11-16).
    const char* address = std::getenv("KSPHMI_REDUX_ADDRESS");
    const std::string host = address ? address : options_.address;
    net::Fd socket;
    std::string endpoint;  // the address the socket is connected to
    double next_resolve = 0;
    std::string resolve_problem;
    auto open_endpoint = [&] {
        auto remote = net::resolve_ipv4(host, REDUX_PORT);
        if (!remote) {
            if (resolve_problem != "dns") log::warning(LOG, "Redux address does not resolve yet: " + host);
            resolve_problem = "dns";
            return;
        }
        const std::string ip = net::to_string(remote->sin_addr);
        if (socket && ip == endpoint) return;  // same address: keep the socket
        net::Fd fresh(net::open_socket(SOCK_DGRAM));
        if (!fresh || ::connect(fresh.get(), reinterpret_cast<const sockaddr*>(&*remote), sizeof(*remote)) != 0) {
            if (resolve_problem != "socket")
                log::warning(LOG, "cannot open the Redux UDP socket: " + net::error_text(net::last_error()));
            resolve_problem = "socket";
            return;
        }
        net::set_nonblocking(fresh.get(), true);
        socket = std::move(fresh);
        if (ip != host) log::info(LOG, "Redux endpoint " + host + " is " + ip + (endpoint.empty() ? "" : " (was " + endpoint + ")"));
        endpoint = ip;
        resolve_problem.clear();
    };
    std::unique_ptr<KrpcSource> krpc;
    if (fallback_) {
        krpc = std::make_unique<KrpcSource>(options_);
        krpc->start();
        std::lock_guard<std::mutex> lock(input_mutex_);
        krpc_ = krpc.get();
        krpc_->set_cockpit(cockpit_);
    }
    log::info(LOG, "KSP 2 Redux UDP source at " + host + ":" + std::to_string(REDUX_PORT) + (fallback_ ? ", kRPC fallback" : ""));
    uint64_t sequence = 0;
    double last_redux = -1e9;
    bool version2 = false, linked = false, in_vab = false, in_flight = false;
    std::shared_ptr<const ShipReport> ship;
    double ship_at = -1e9, next_ship = 0;
    std::optional<VesselView> shape;
    std::shared_ptr<const VesselView> view;
    double view_at = -1e9, next_view = 0;
    std::string view_problem;
    while (!stopping()) {
        const double started = now();
        // While Redux is silent, look the address up again: every 3 s before the first socket, every 10 s after.
        if (!socket || (started - last_redux > 10 && started >= next_resolve)) {
            if (started >= next_resolve) {
                next_resolve = started + (socket ? 10 : 3);
                open_endpoint();
            }
        }
        if (socket) {
            // Editor report at 2 Hz, only while the last snapshot was the VAB; older servers never answer it.
            if (in_vab && started >= next_ship) {
                next_ship = started + 0.5;
                if (auto reply = redux_ship_exchange(socket.get(), std::to_string(++sequence), 0.05)) {
                    ship = *reply;
                    ship_at = now();
                }
            }
            // VESSEL panel at 2 Hz in flight: state every time, shape only when the state's revision is not the shape's.
            if (in_flight && started >= next_view) {
                next_view = started + 0.5;
                std::string why;
                auto state = redux_text_exchange(socket.get(), "KSP2VSTA", std::to_string(++sequence), 0.05);
                if (!state) why = "no answer (plugin older than beta.11?)";
                else if (!*state) why = "no vessel view in the game";
                else {
                    VesselView next = shape ? *shape : VesselView{};
                    if (!shape || !apply_vessel_state(next, **state)) {
                        auto text = redux_text_exchange(socket.get(), "KSP2VSHP", std::to_string(++sequence), 0.05);
                        auto parsed = text && *text ? parse_vessel_shape(**text) : std::nullopt;
                        if (!parsed) why = "the vessel shape does not parse";
                        else {
                            if (!shape || shape->revision != parsed->revision)
                                log::info(LOG, fmt::str("vessel view: %zu parts (shape revision %u)", parsed->parts.size(), parsed->revision));
                            shape = *parsed;
                            next = *parsed;
                            if (!apply_vessel_state(next, **state)) why = "the vessel state does not match its shape";
                        }
                    }
                    if (why.empty()) {
                        view = std::make_shared<const VesselView>(std::move(next));
                        view_at = now();
                    }
                }
                if (why != view_problem && !why.empty()) log::warning(LOG, "vessel view unavailable: " + why);
                view_problem = why;
            }
            const std::string nonce = std::to_string(++sequence);
            // Alternate during discovery so beta.6 servers remain usable; prefer v2 once observed.
            const std::string request = (version2 || sequence % 2 ? "KSP2HMI2 " : "KSP2HMI1 ") + nonce;
            if (auto reply = redux_exchange(socket.get(), request, nonce, 0.08)) {
                version2 = reply->version2;
                reply->snapshot.server = "KSP2 Redux UDP :50003";
                reply->snapshot.game = "ksp2";  // skin auto picks ksp2
                in_vab = reply->snapshot.state == State::NoVessel && reply->snapshot.scene == "vab";
                in_flight = reply->snapshot.state == State::Flight;
                if (!in_vab) ship.reset();
                else if (ship && now() - ship_at <= 2) reply->snapshot.ship = ship;
                if (!in_flight) {
                    shape.reset();
                    view.reset();
                } else if (view && now() - view_at <= 2) reply->snapshot.vessel_view = view;
                publish(std::move(reply->snapshot));
                last_redux = now();
                if (!linked) {
                    log::info(LOG, std::string("Redux UDP replies received (") + (version2 ? "v2" : "v1") + ")");
                    linked = true;
                    std::lock_guard<std::mutex> lock(input_mutex_);
                    linked_ = true;
                    if (krpc_) krpc_->hold(Held{});
                }
                send_inputs(socket.get(), sequence);
            }
        }
        if (now() - last_redux > 1) {
            if (linked) {
                log::warning(LOG, "no valid Redux UDP reply for 1 s; using fallback");
                linked = false;
                std::lock_guard<std::mutex> lock(input_mutex_);
                linked_ = false;
                commands_.clear();
            }
            version2 = in_vab = in_flight = false;
            shape.reset();
            view.reset();
            if (krpc) publish(*krpc->latest());
            else {
                Snapshot s;
                s.detail = "Waiting for KSP2 Redux UDP :50003";
                publish(std::move(s));
            }
        }
        if (wait(std::max(0.0, 0.1 - (now() - started)))) break;
    }
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        krpc_ = nullptr;
    }
    if (krpc) krpc->stop();
}

}  // namespace ksphmi
