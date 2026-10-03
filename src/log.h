#pragma once

// Writes to xxl_coop.log next to the game executable. The game is a 2004 title
// with no console, so a file is the only way to see what the mod is doing.
void LogOpen();
void LogClose();
void Log(const char* fmt, ...);

// Same, but only emitted when [debug] verbose is on.
void LogV(const char* fmt, ...);

// Bounded line builder for the diagnostic dumps.
//
// These logs assemble variable-length lists (one entry per action, per vtable
// slot, per changed byte) and the counts are engine-controlled, so a fixed
// buffer plus sprintf is a stack overrun waiting for the right game state --
// which is exactly what happened once a level left 25 of the 59 input bindings
// inactive and each one appended "(off)".
struct LineBuf {
	char*  buf;
	size_t cap;
	size_t len = 0;
	bool   truncated = false;

	template <size_t N>
	explicit LineBuf(char (&storage)[N]) : buf(storage), cap(N) { buf[0] = '\0'; }

	// Appends if it fits; otherwise marks the line truncated and stops.
	void Add(const char* fmt, ...);

	const char* c_str() const { return buf; }
};
