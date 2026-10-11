// Stands in for AOG-TaskController's settings.hpp. third_party/aog-taskcontroller uses one thing
// from it: where to keep the device descriptions (DDOPs) implements upload.
#pragma once

#include <string>

class Settings
{
public:
	static std::string get_filename_path(std::string fileName);

	// AOW HAT: the directory, set once before the stack starts.
	static void set_directory(const std::string &directory);
	static const std::string &directory();
};
