// Position command parser. Mirrors bagaturchess.uci.impl.commands.Position.
//
// Forms:
//   position startpos [moves <m1> <m2> ...]
//   position fen <FEN> [moves <m1> <m2> ...]

#pragma once

#include <string>
#include <vector>

namespace uci {

struct Position {
    std::string              fen;   // empty → startpos
    std::vector<std::string> moves;

    static Position parse(const std::string& line);
};

}  // namespace uci
