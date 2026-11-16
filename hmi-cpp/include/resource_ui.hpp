#pragma once
#include "model.hpp"
#include <cctype>

namespace ksphmi {
inline std::vector<std::string> hmi_resource_names(const Snapshot& s) {
    if (!s.systems || !s.systems->native_resources) return {RESOURCE_NAMES.begin(), RESOURCE_NAMES.end()};
    std::vector<std::string> names;
    for (const auto& r : s.systems->resources) if (r.capacity.value_or(0) > 0) names.push_back(r.name);
    return names;
}
inline std::vector<std::string> hmi_resource_labels(const Snapshot& s, const char* const* legacy) {
    if (!s.systems || !s.systems->native_resources) return {legacy, legacy + RESOURCE_NAMES.size()};
    auto labels = hmi_resource_names(s);
    for (auto& label : labels) {
        if (label == "ElectricCharge") label = "ELECTRICITY";
        else if (label == "MonoPropellant") label = "MONOPROPELLANT";
        else for (char& c : label) c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    return labels;
}
}
