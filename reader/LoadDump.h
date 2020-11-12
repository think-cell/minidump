// think-cell minidump library
//
// Copyright (C) 2016-2020 think-cell Software GmbH
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

#include "tc/range.h"
#include <lldb/API/LLDB.h>

std::basic_string<char> UuidPath() noexcept;

struct ExLoadFailIgnore final : ExLoadFail {};

struct SDebugger final {
private:
	SDebugger() noexcept;
	
public:
	SDebugger(tc::ptr_range<unsigned char const> rngbyteDump, bool bMountSource) THROW(ExLoadFailIgnore, ExLoadFail);
	~SDebugger();
	
	lldb::SBDebugger m_debugger;
	bool m_bIgnoreLoadFail = false;
};
