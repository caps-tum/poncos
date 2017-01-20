#include "poncos/poncos.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <locale>
#include <string>
#include <vector>

// trim from start
static inline std::string &ltrim(std::string &s) {
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
	return s;
}

// trim from end
static inline std::string &rtrim(std::string &s) {
	s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
	return s;
}

// trim from both ends
static inline std::string &trim(std::string &s) { return ltrim(rtrim(s)); }

void read_file(const std::string& filename, std::vector<std::string> &command_queue) {
	std::ifstream file(filename);
	assert(file.good());
	std::string command;
	while (std::getline(file, command)) {
		assert(file.good());

		trim(command);
		if (command[0] != '#') command_queue.push_back(command);
	}
}
