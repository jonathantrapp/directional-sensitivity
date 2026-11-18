#include "directional/profile_io.h"
#include <array>
#include <cmath>
#include <iomanip>
#include <istream>
#include <locale>
#include <ostream>
#include <sstream>

namespace directional {
namespace {
template<class T> bool number(const std::string& text, T& value) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    return static_cast<bool>(stream >> value) && (stream >> std::ws).eof();
}
std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    return begin == std::string::npos ? std::string{} :
        text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}
}
bool parse_gain(const std::string& text, std::uint32_t& output) {
    double value;
    if (!number(text,value) || !std::isfinite(value) || value < .25 || value > 4) return false;
    output = static_cast<std::uint32_t>(std::llround(value * unity_gain));
    return true;
}
bool matches_setup(const SavedProfile& profile, std::uint32_t dpi, double sensitivity) noexcept {
    return dpi != 0 && profile.dpi == dpi && std::isfinite(sensitivity) &&
        sensitivity > 0 && profile.reference_sensitivity == sensitivity;
}
bool read_profile(std::istream& input, SavedProfile& output, std::string& error) {
    SavedProfile candidate;
    std::array<GainPair,4> gains{};
    unsigned seen = 0;
    std::size_t bytes = 0, line_number = 0;
    std::string line;
    auto fail = [&](const char* message) {
        error = "Profile line " + std::to_string(line_number) + ": " + message;
        return false;
    };
    // Read with an explicit bound rather than getline allocating an unbounded line.
    while (input.peek() != std::char_traits<char>::eof()) {
        line.clear(); ++line_number;
        for (char c; input.get(c);) {
            if (++bytes > 65536) return fail("file exceeds 64 KiB");
            if (c == '\n') break;
            line.push_back(c);
        }
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) return fail("expected key=value");
        const auto key = trim(line.substr(0,separator));
        const auto value = trim(line.substr(separator+1));
        unsigned index = 7;
        if (key == "directional-profile") index = 0;
        if (key == "dpi") index = 1;
        if (key == "reference_sensitivity") index = 2;
        const char* names[] = {"ne","se","sw","nw"};
        for (unsigned q=0;q<4;++q) if (key == names[q]) index=q+3;
        if (index == 7) return fail("unknown key");
        if (seen & (1u<<index)) return fail("duplicate key");
        seen |= 1u<<index;
        if (index == 0 && value != "1") return fail("unsupported version");
        if (index == 1) {
            std::int64_t dpi;
            if (!number(value,dpi) || dpi < 1 || dpi > 1000000) return fail("invalid DPI");
            candidate.dpi=static_cast<std::uint32_t>(dpi);
        }
        if (index == 2 && (!number(value,candidate.reference_sensitivity) ||
            !std::isfinite(candidate.reference_sensitivity) || candidate.reference_sensitivity <= 0))
            return fail("invalid reference sensitivity");
        if (index >= 3) {
            const auto comma=value.find(',');
            if (comma==std::string::npos || !parse_gain(value.substr(0,comma),gains[index-3].x) ||
                !parse_gain(value.substr(comma+1),gains[index-3].y)) return fail("invalid X,Y gains");
        }
    }
    if (input.bad()) return fail("read failure");
    if (seen != 127) return fail("missing required key");
    if (!Profile::create(gains,candidate.transform)) return fail("angular-order validation failed");
    output=candidate; error.clear(); return true;
}
bool write_profile(std::ostream& output, const SavedProfile& profile, std::string& error) {
    if (profile.dpi < 1 || profile.dpi > 1000000 || !std::isfinite(profile.reference_sensitivity) ||
        profile.reference_sensitivity <= 0) { error="Invalid profile metadata"; return false; }
    // Build with the classic locale so decimal separators round-trip everywhere.
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(17) << "directional-profile=1\ndpi=" << profile.dpi
         << "\nreference_sensitivity=" << profile.reference_sensitivity << '\n';
    const char* names[] = {"ne","se","sw","nw"};
    for (unsigned q=0;q<4;++q) {
        const auto g=profile.transform.gain(static_cast<Quadrant>(q));
        text << names[q] << '=' << static_cast<double>(g.x)/unity_gain << ','
             << static_cast<double>(g.y)/unity_gain << '\n';
    }
    output << text.str();
    if (!output) { error="Profile write failed"; return false; }
    error.clear(); return true;
}
} // namespace directional
