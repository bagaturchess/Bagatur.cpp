#include "position.h"

#include <sstream>
#include <string>

namespace uci {

Position Position::parse(const std::string& line) {
    Position p;

    std::size_t moves_idx = line.find(" moves ");
    std::string head = (moves_idx == std::string::npos) ? line : line.substr(0, moves_idx);
    std::string tail = (moves_idx == std::string::npos) ? std::string{}
                                                        : line.substr(moves_idx + 7);

    // Head can be "position startpos" or "position fen <FEN-tokens>".
    if (head.find("startpos") != std::string::npos) {
        // leave fen empty → caller treats as starting position
    } else {
        std::size_t fen_pos = head.find("fen ");
        if (fen_pos != std::string::npos) {
            std::string fen = head.substr(fen_pos + 4);
            // trim trailing whitespace
            while (!fen.empty() && (fen.back() == ' ' || fen.back() == '\r' || fen.back() == '\n'))
                fen.pop_back();
            p.fen = fen;
        }
    }

    if (!tail.empty()) {
        std::istringstream iss(tail);
        std::string mv;
        while (iss >> mv) p.moves.push_back(mv);
    }
    return p;
}

}  // namespace uci
