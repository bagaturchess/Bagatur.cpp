// UCI protocol — command name constants. Mirrors bagaturchess.uci.impl.Protocol.

#pragma once

namespace uci {

inline constexpr const char* ENGINE_NAME    = "Bagatur.cpp";
inline constexpr const char* ENGINE_VERSION = "1.0";
inline constexpr const char* ENGINE_AUTHOR  = "Krasimir Topchiyski";

// Commands from GUI → engine.
inline constexpr const char* CMD_UCI         = "uci";
inline constexpr const char* CMD_ISREADY     = "isready";
inline constexpr const char* CMD_UCINEWGAME  = "ucinewgame";
inline constexpr const char* CMD_POSITION    = "position";
inline constexpr const char* CMD_GO          = "go";
inline constexpr const char* CMD_STOP        = "stop";
inline constexpr const char* CMD_PONDERHIT   = "ponderhit";
inline constexpr const char* CMD_SETOPTION   = "setoption";
inline constexpr const char* CMD_QUIT        = "quit";

// Replies engine → GUI.
inline constexpr const char* REPLY_ID_NAME      = "id name";
inline constexpr const char* REPLY_ID_AUTHOR    = "id author";
inline constexpr const char* REPLY_UCIOK        = "uciok";
inline constexpr const char* REPLY_READYOK      = "readyok";
inline constexpr const char* REPLY_BESTMOVE     = "bestmove";
inline constexpr const char* REPLY_PONDER       = "ponder";
inline constexpr const char* REPLY_INFO         = "info";

}  // namespace uci
