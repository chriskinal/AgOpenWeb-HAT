// Stands in for AOG-TaskController's logging_utils.hpp, which third_party/aog-taskcontroller
// includes: log() returns a stream, and a line ends at std::endl.
//
// Each line goes, in full, to tc.log in the ISOBUS directory, and to the daemon's logger at debug
// level. Debug, because an implement's description produces a burst of lines: at info they
// used up the logger's 10 a second and the daemon's own lines were dropped with them. The file
// is what a bug report needs.
#pragma once

#include <ostream>
#include <string>

std::ostream &log();
std::ostream &log(const std::string &tag);
