// Bagatur.cpp-x64 — UCI engine entrypoint.
//
// The executable speaks UCI on stdin/stdout. Plug into any GUI that
// supports the protocol (cutechess-cli, Arena, ChessBase, …).
//
// When CMake finds `network_bagatur.nnue` at the project root, the bytes
// are embedded into this binary (see cmake/embed_binary.cmake) and pinned
// via `Network::load_from_memory` before any code touches the singleton.
// The exe therefore has no runtime file dependency.

#include <cstdio>
#include <iostream>

#include "state_manager.h"

#ifdef BAGATUR_EMBEDDED_NETWORK
#include <cstdint>
#include "../nnue/nnue.h"

extern "C" {
extern const unsigned char kBagaturNetwork[];
extern const unsigned long kBagaturNetwork_size;
}
#endif

int main() {
    // UCI is line-oriented and order-sensitive — disable any chatty default
    // buffering quirks before the StateManager touches stdio.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffered stdout
    std::setvbuf(stderr, nullptr, _IONBF, 0);   // unbuffered stderr

#ifdef BAGATUR_EMBEDDED_NETWORK
    // Pin the embedded blob so the lazy `Network::instance()` picks it up
    // instead of reading network_bagatur.nnue from the working directory.
    nnue::Network::load_from_memory(
        reinterpret_cast<const std::uint8_t*>(kBagaturNetwork),
        static_cast<std::size_t>(kBagaturNetwork_size));
#endif

    uci::StateManager mgr;
    return mgr.run();
}
