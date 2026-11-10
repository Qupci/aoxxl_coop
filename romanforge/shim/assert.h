// romanforge: the editor's asserts throw (caught by the forge) instead of aborting the game.
#pragma once
#include <stdexcept>
#undef assert
#define assert(x) ((x) ? (void)0 : throw std::runtime_error("assert failed: " #x " (" __FILE__ ")"))
