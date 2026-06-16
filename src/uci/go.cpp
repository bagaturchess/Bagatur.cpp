#include "go.h"

#include <charconv>
#include <sstream>
#include <string>

namespace uci {

namespace {

// Helper: find token `key`, return the next whitespace-separated token, or "".
std::string token_after(const std::string& line, const std::string& key) {
    std::size_t pos = line.find(key);
    if (pos == std::string::npos) return {};
    // Make sure the match is whole-word — check char before is start/space and
    // char after is space.
    if (pos > 0 && line[pos - 1] != ' ') return {};
    std::size_t end_of_key = pos + key.size();
    if (end_of_key < line.size() && line[end_of_key] != ' ') return {};
    std::size_t val_start = end_of_key + 1;
    if (val_start >= line.size()) return {};
    std::size_t val_end = line.find(' ', val_start);
    if (val_end == std::string::npos) val_end = line.size();
    return line.substr(val_start, val_end - val_start);
}

bool has_flag(const std::string& line, const std::string& key) {
    std::size_t pos = line.find(key);
    if (pos == std::string::npos) return false;
    if (pos > 0 && line[pos - 1] != ' ') return false;
    std::size_t end_of_key = pos + key.size();
    return end_of_key == line.size() || line[end_of_key] == ' ';
}

template <typename T>
bool parse_int(const std::string& s, T& out) {
    if (s.empty()) return false;
    long long tmp = 0;
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, tmp);
    if (ec != std::errc{} || ptr != last) return false;
    out = static_cast<T>(tmp);
    return true;
}

}  // namespace

Go Go::parse(const std::string& line) {
    Go g;
    if (has_flag(line, "ponder"))   g.ponder   = true;
    if (has_flag(line, "infinite")) g.infinite = true;

    parse_int(token_after(line, "wtime"),     g.wtime);
    parse_int(token_after(line, "btime"),     g.btime);
    parse_int(token_after(line, "winc"),      g.winc);
    parse_int(token_after(line, "binc"),      g.binc);
    parse_int(token_after(line, "nodes"),     g.nodes);
    parse_int(token_after(line, "movestogo"), g.movestogo);
    parse_int(token_after(line, "movetime"),  g.movetime);
    parse_int(token_after(line, "depth"),     g.depth);
    return g;
}

}  // namespace uci
