// Compatibility shim.
//
// matron is compiled as C++ in this build. Debian's liblua5.3-dev headers
// carry an `extern "C"` guard, but Arch's lua53 headers do not, so the C++
// compiler would mangle the Lua C API symbols and fail to link against the
// (plain C) liblua. Wrap the real header so its declarations get C linkage.
//
// This directory is placed first on the include path; #include_next reaches
// the system <lua.h> after it.
#pragma once
extern "C" {
#include_next <lua.h>
}
