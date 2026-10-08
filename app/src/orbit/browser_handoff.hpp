// Orbit Store - local native-to-browser navigation, without starting a transfer.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace orbit
{
struct BrowserSelection
{
    std::string game_id;
    std::string release_id;
    std::string storage_id;
};
inline std::string browser_handoff_url(const BrowserSelection &selection)
{
    const auto valid = [](std::string_view id, std::size_t limit)
    {
        if (id.empty() || id.size() > limit)
            return false;
        for (const char c : id)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return false;
        return true;
    };
    if (!valid(selection.game_id, 63) || !valid(selection.release_id, 23) ||
        !valid(selection.storage_id, 63))
        return {};
    return "http://127.0.0.1:34177/#download?game=" + selection.game_id +
           "&release=" + selection.release_id + "&storage=" + selection.storage_id;
}
} // namespace orbit
