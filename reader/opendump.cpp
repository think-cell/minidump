// think-cell minidump library
//
// Copyright (C) 2016-2020 think-cell Software GmbH
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

#include "tc/range.h"

#include "tc/dense_map.h"
#include "LoadDump.h"

#include <spawn.h>
#include <lldb/API/LLDB.h>

std::basic_string<char> UuidPath() noexcept {
	// FIXME: Path where scripts/RebuildUuidDatabase.py has stored your uuid index
	return tc::make_str("path_to_uuids/");
}

int main(int argc, char *argv[]) noexcept { ENTRY
	if(argc<2) {
		tc::append(tc::cerr(), "Syntax: opendump <path to Mac dump file>\n");
		return EXIT_FAILURE;
	}
	
	if(!boost::filesystem::is_regular_file(argv[1])) {
		tc::append(tc::cerr(), "[FAILURE] ", argv[1], " does not exist.\n");
		return EXIT_FAILURE;
	}
	
	char const* pszHome = ::getenv("HOME");
	if (!pszHome || tc::empty(pszHome)) {
		tc::append(tc::cerr(), "[FAILURE] HOME environment variable must be set.\n");
		return EXIT_FAILURE;
	}
	
	auto const strUuidsPath = UuidPath();
	
	// Revert the std streams to default line buffering because we hand the streams over to lldb.
	ERRNO(std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ), tc::err::returned(0));
	ERRNO(std::setvbuf(stdin, nullptr, _IOLBF, BUFSIZ), tc::err::returned(0));
	ERRNO(std::setvbuf(stderr, nullptr, _IOLBF, BUFSIZ), tc::err::returned(0));
	
	try {
		SDebugger debugger(SFileMapping(argv[1]), /*bMountSource*/ true); // THROW(ExLoadFail);
		RETURNS_VOID(debugger.m_debugger.SetInputFileHandle(stdin, false));
		RETURNS_VOID(debugger.m_debugger.SetOutputFileHandle(stdout, false));
		RETURNS_VOID(debugger.m_debugger.SetErrorFileHandle(stderr, false));
		// RETURNS_VOID(debugger.HandleCommand("log enable lldb host"));
		// RETURNS_VOID(debugger.HandleCommand("log enable lldb api"));
	
		RETURNS_VOID(debugger.m_debugger.HandleCommand("bt"));
		
		RETURNS_VOID(debugger.m_debugger.RunCommandInterpreter(true, false));
		return EXIT_SUCCESS;
	} catch(ExLoadFail const&) {
	}
	return EXIT_FAILURE;
EXIT }

