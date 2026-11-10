#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace GamePatcher {
	struct GamePatcherException : std::exception { using std::exception::exception; };

	struct GamePatcherThread {
		std::filesystem::path inputPath, elbPath, outputPath;
		int progress = 0;
		bool done = false;
		std::string status;
		std::mutex statusMutex;
		virtual ~GamePatcherThread() = default;
		virtual void start() = 0;
		std::string getStatus();
		void setStatus(const std::string& text);
		void setStatusFmt(const char* format, ...);
	};

	struct GamePatcherThreadX1 : GamePatcherThread {
		void start();
	};

	struct GamePatcherThreadX2 : GamePatcherThread {
		void start();
	};

	struct GamePatcherThreadCopy : GamePatcherThread {
		int platform;
		GamePatcherThreadCopy(int platform) : platform(platform) {}
		void start();
	};
}

// romanforge additions (the XXL co-op mod)
namespace GamePatcher {
	struct ModulePatch { size_t offset; const unsigned char* bytes; size_t size; };
	bool ConvertLevelX1(const void* elb, size_t elbSize, const std::filesystem::path& inputPath,
	                    const std::filesystem::path& outputPath, int lvlnum, std::string& error);
	size_t ModulePatchesX1(ModulePatch* out, size_t max);
}
