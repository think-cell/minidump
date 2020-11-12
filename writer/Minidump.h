// think-cell minidump library
//
// Copyright (C) 2016-2020 think-cell Software GmbH
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt


#pragma once

#include "tc/range.h"
#include <mach/mach_types.h>
std::basic_string<char> MiniDumpWriteDump(task_t task, std::uint64_t threadid, bool bBig, tc::ptr_range<char const> strExecutable, tc::ptr_range<tc::char16 const> strBundleVersion) THROW(tc::file_failure);
