#if !defined(__SWITCH__) // Nintendo Switch build: no asio / Unix sockets (see src/emu/http.cpp)
// license:BSD-3-Clause
// copyright-holders:Miodrag Milanovic
#include "asio.h"

#if defined(ASIO_SEPARATE_COMPILATION)
#include <asio/impl/src.hpp>
#endif

#endif
