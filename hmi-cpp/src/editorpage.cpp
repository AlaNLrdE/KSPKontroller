#include "shipreport.hpp"
#include "fmt.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ksphmi {
std::string editor_mode_label(const std::string& mode) {
    if (mode == "actions") return "ACTION GROUPS";
    if (mode == "crew") return "CREW";
    if (mode == "cargo") return "CARGO";
    return "CONSTRUCTION";
}

PageSlice page_slice(size_t count, size_t rows, double received) {
    rows = std::max(size_t(1), rows);
    PageSlice p;
    p.pages = std::max(size_t(1), (count + rows - 1) / rows);
    if (std::isfinite(received) && received > 0)
        p.page = static_cast<size_t>(std::fmod(received / 8.0, static_cast<double>(p.pages)));
    p.begin = p.page * rows;
    p.end = std::min(count, p.begin + rows);
    return p;
}

std::vector<std::string> stage_detail_lines(const StageReport& stage, size_t characters) {
    std::vector<std::string> entries;
    for (const auto& item : stage.items)
        entries.push_back(destination_label(item.kind == "other" ? "parts" : item.kind) + ": " + item.name + " x" + std::to_string(item.count));
    if (entries.empty()) entries.push_back(events_text(stage));
    std::vector<std::string> lines;
    std::string line;
    for (const auto& entry : entries) {
        if (entry.empty()) continue;
        if (!line.empty() && line.size() + entry.size() + 3 > characters) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += " / ";
        line += entry;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

namespace {
std::string value(const std::optional<double>& v, double scale, const char* unit) {
    return v ? fmt::str("%.1f %s", *v * scale, unit) : "UNKNOWN";
}
std::string capacity(const std::optional<double>& used, const std::optional<double>& limit, double scale, const char* unit) {
    return value(used, scale, unit) + " / " + (limit ? (*limit < 0 ? "NO LIMIT" : value(limit, scale, unit)) : "UNKNOWN");
}
}

EditorPage editor_page(const ShipReport& r) {
    EditorPage p;
    p.title = editor_mode_label(r.editor_mode);
    if (r.editor_mode == "actions") {
        static const char* groups[] = {"stage", "gear", "lights", "brakes", "abort", "sas", "rcs", "custom1", "custom2",
            "custom3", "custom4", "custom5", "custom6", "custom7", "custom8", "custom9", "custom10"};
        size_t assigned = 0;
        p.left_title = "ACTION GROUPS / ASSIGNMENTS";
        p.right_title = "SELECTED IN KSP";
        for (const char* group : groups) {
            size_t count = 0;
            for (const auto& a : r.actions) if (a.group == group) ++count;
            if (count) ++assigned;
            const std::string input = std::string(group) == "sas" ? "SAS button (flight)" :
                std::string(group) == "rcs" ? "RCS button (flight)" : "No enabled panel input";
            p.left.push_back({destination_label(group), std::to_string(count), input, false});
        }
        for (const auto& a : r.actions)
            if (a.group == r.selected_group) p.right.push_back({a.part, "", a.name, false});
        const std::pair<const char*, const char*> stock[] = {{"stage", "Activate next stage"}, {"gear", "Toggle landing gear"},
            {"lights", "Toggle lights"}, {"brakes", "Toggle brakes"}, {"abort", "Activate abort group"},
            {"sas", "Toggle stability assist"}, {"rcs", "Toggle reaction control"}};
        for (const auto& command : stock)
            if (r.selected_group == command.first) p.right.insert(p.right.begin(), {"STOCK VESSEL COMMAND", "", command.second, false});
        if (p.right.empty()) p.right.push_back({r.selected_group.empty() ? "NO GROUP SELECTED" : "NO PART ACTIONS ASSIGNED", "", "Select a group in KSP", false});
        p.right.insert(p.right.begin(), {destination_label(r.selected_group), "", "Selected action group", false});
        p.metrics = {{"GROUPS", "17"}, {"ASSIGNED", std::to_string(assigned)}, {"ACTIONS", std::to_string(r.actions.size())}, {"FLIGHT INPUTS", "2"}};
    } else if (r.editor_mode == "crew") {
        size_t assigned = 0, pilots = 0, engineers = 0, scientists = 0;
        p.left_title = "SEAT MANIFEST";
        p.right_title = "CREW COMPLEMENT";
        for (const auto& s : r.manifest) {
            if (!s.name.empty()) {
                ++assigned;
                if (s.role == "Pilot") ++pilots;
                if (s.role == "Engineer") ++engineers;
                if (s.role == "Scientist") ++scientists;
            }
            p.left.push_back({s.part + " / " + std::to_string(s.seat + 1), s.name.empty() ? "EMPTY" : s.name,
                s.name.empty() ? "Free seat" : s.role + " / LEVEL " + std::to_string(s.level), false});
        }
        if (p.left.empty()) p.left.push_back({"NO CREW CAPACITY", "", "Add a crewed part in KSP", false});
        p.right = {{"PILOTS", std::to_string(pilots), "", false}, {"ENGINEERS", std::to_string(engineers), "", false},
                   {"SCIENTISTS", std::to_string(scientists), "", false}};
        p.metrics = {{"ASSIGNED", std::to_string(assigned)}, {"CAPACITY", std::to_string(r.manifest.size())},
                     {"FREE SEATS", std::to_string(r.manifest.size() - assigned)}, {"PILOTS", std::to_string(pilots)}};
    } else {
        size_t quantity = 0;
        double mass = 0, volume = 0;
        bool mass_known = true, volume_known = true;
        p.left_title = "STORED PARTS";
        p.right_title = "INVENTORY CAPACITY";
        for (const auto& i : r.inventories) {
            p.right.push_back({i.part, std::to_string(i.slots) + " SLOTS", "VOLUME " + capacity(i.volume, i.volume_capacity, 1, "L"),
                i.volume && i.volume_capacity && *i.volume_capacity >= 0 && *i.volume > *i.volume_capacity});
            p.right.push_back({"MASS", capacity(i.mass, i.mass_capacity, 1000, "kg"), "", i.mass && i.mass_capacity && *i.mass_capacity >= 0 && *i.mass > *i.mass_capacity});
            if (i.mass) mass += *i.mass; else mass_known = false;
            if (i.volume) volume += *i.volume; else volume_known = false;
        }
        for (const auto& c : r.cargo) {
            quantity += c.quantity;
            std::string owner = c.inventory;
            for (const auto& i : r.inventories) if (i.key == c.inventory) owner = i.part;
            p.left.push_back({c.name, "x" + std::to_string(c.quantity), owner + " / SLOT " + std::to_string(c.slot + 1) +
                " / " + value(c.mass, 1000, "kg") + " / " + value(c.volume, 1, "L"), false});
        }
        if (p.left.empty()) p.left.push_back({r.inventories.empty() ? "NO INVENTORIES" : "EMPTY INVENTORIES", "", "Stored parts appear here", false});
        if (p.right.empty()) p.right.push_back({"NO INVENTORY CAPACITY", "", "Add an inventory part in KSP", false});
        p.metrics = {{"ITEMS", std::to_string(quantity)}, {"MASS kg", mass_known ? fmt::str("%.1f", mass * 1000) : "--"},
                     {"VOLUME L", volume_known ? fmt::str("%.1f", volume) : "--"}, {"INVENTORIES", std::to_string(r.inventories.size())}};
    }
    return p;
}
}  // namespace ksphmi
