// think-cell minidump library
//
// Copyright (C) 2016-2020 think-cell Software GmbH
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

#include "tc/range.h"

#include "LoadDump.h"
#include "tc/dense_map.h"

#include <copyfile.h>
#include <spawn.h>
#include <lldb/API/LLDB.h>

namespace {
	struct SDumpMetaInformation final {
		std::basic_string<char> m_strExecutable;
		std::basic_string<char> m_strBundleVersion;
		int m_nThread;

		struct SModule final {
			std::basic_string<char> m_strPath;
			std::uint64_t m_pvStartAddress;
			tc::native_module_version m_modver;
			std::basic_string<char> m_strUuid;
		};
		tc::vector<SModule> m_vecmodule;
	};
	
	std::basic_string<char> SymbolCache() noexcept {
		// FIXME: Path to local binary cache.
		// The binaries stored on the server are cached locally. Even over fast Ethernet,
		// opening a dump can take quite long otherwise. Binaries will be stored in subfolders
		// describing their binary uuid
		// ~/symbol_cache/000C/4E9F/E0D9/371D/B304/83BA37460724/library
		return tc::make_str(VERIFY(::getenv("HOME")), "/symbol_cache/");
	}

	std::basic_string<char> SymbolsPath() noexcept {
		// FIXME: Contains files describing your symbols. Files contain two lines.
		// 1. Path to the actual symbol file
		// 2. Path on c_szSourceServer where the source code to this build can be mapped
		// ~/path_to_/program.app.dSYM/Contents/Resources/DWARF/program
		// 201108_my_program_build
		return tc::make_str(VERIFY(::getenv("HOME")), "/symbols/");
	}

	constexpr char c_szSourceServer[] = "http://sourceserver/"; // SVN repos can be mounted so lldb can display source code 
}

SDebugger::SDebugger() noexcept {
	RETURNS_VOID(lldb::SBDebugger::Initialize());
}

SDebugger::SDebugger(tc::ptr_range<unsigned char const> rngbyteDump, bool bMountSource) THROW(ExLoadFailIgnore, ExLoadFail)
	: SDebugger()
{
	auto const vecbyte = CZipFile(rngbyteDump).UnzipFile("minidump.dmp"); // THROW(ExLoadFail)

	SDumpMetaInformation dumpmetainfo;
	// FIXME: Load SDumpMetaInformation from vecbyte

	m_bIgnoreLoadFail = false; // Ignore e.g. early versions known to sent erroneous minidumps
	auto ThrowLoadFail = [&]() THROW(ExLoadFailIgnore, ExLoadFail) {
		if(m_bIgnoreLoadFail) {
			throw ExLoadFailIgnore();
		} else {
			throw ExLoadFail();
		}
	};

	auto const strSymbolsPath = SymbolsPath();
	auto LookupBinaryAndSymbol = [&](tc::ptr_range<char const> strUuid) THROW(ExLoadFail) {
		try {
			// We use the same folder format for our uuid -> binary map that lldb would use for
			// the uuid -> debug symbol map. See https://lldb.llvm.org/symbols.html
			// uuids have the form C4CBD2CF-39D5-3185-851E-85C7DD2F8C7F and the path to the uuid file will be
			// C4CB/D2CF/39D5/3185/851E/85C7DD2F8C7F
			if(tc::size(strUuid)!=36) {
				TRACE("Read invalid uuid ", strUuid);
				ThrowLoadFail();
			}
			auto const strUuidsPath = UuidPath();
			_ASSERT(boost::filesystem::is_directory(strUuidsPath));
			
			auto AppendUuid = [&](auto const& str) noexcept {
				return tc::concat(
					str,
					tc::take_first(strUuid, 4),
					"/",
					tc::drop_first(tc::replace(strUuid, '-', '/'), 4)
				);
			};
			
			// Lookup strUuid in our uuid-to-binary index. The file for strUuid contains
			// a relative path to a binary.

			// FIXME
			auto strBinary = tc::make_str(
				VERIFY(::getenv("HOME")),
				"/mnt/",
				tc::as_typed_range<char>(SFileMapping(tc::as_c_str(tc::make_str(AppendUuid(strUuidsPath)))))
			); // THROW(tc::file_failure)
			
			// We cache the binaries and the symbol files which lets lldb memory-map them.
			// If the file is not yet in the cache, we first download the files to a temp file in the
			// same folder as the cache file and then rename it to the cached file name.
			// Several processes may attempt to cache the same file at the same time.
			auto CacheFile = [](auto&& strSource, auto&& strPathCached) noexcept {
				if(boost::filesystem::exists(strPathCached)) {
					return std::forward<decltype(strPathCached)>(strPathCached);
				} else if(boost::filesystem::exists(strSource)) { // may happen if the uuid-to-binary-index is out-of-date
					NOEXCEPT(boost::filesystem::create_directories( tc::make_str(FilenameWithoutPath<tc::return_take>(strPathCached)) ));
					auto const strPathTemp = tc::make_str(FilenameWithoutPath<tc::return_take>(strPathCached), tc::unique_name<SBase32CodeTable>());
					
					// We call the command line cp command instead of ::copyfile because the latter copied files only partially when copying from
					// a server share using the SMB v2 protocol. It seemed to work fine when using a share with SMB v3 but our current server
					// cannot supply that.
					//
					// ERRNOIGNORE(
					// 		copyfile(tc::as_c_str(strSource), tc::as_c_str(strPathTemp), nullptr, COPYFILE_DATA|COPYFILE_NOFOLLOW|COPYFILE_EXCL|COPYFILE_RECURSIVE), // COPYFILE_RECURSIVE because strSource may be a .dSYM directory
					// 		tc::err::returned(0),
					//		tc::err::returned_less_than(0, as_constexpr(tc::make_array(tc::aggregate_tag, EACCES, ENOENT)))
					//	))
					if(0==CreateAndWaitForProcess(
						"/bin/cp",
						tc::make_array<char const*>(
							tc::aggregate_tag,
							"-R",
							tc::as_c_str(strSource),
							tc::as_c_str(strPathTemp)
						)
					)) {
						// Assert copying succeeds. Otherwise our cache is inconsistent.
						if(boost::filesystem::is_regular_file(strSource)) {
							_ASSERTPRINT(boost::filesystem::file_size(strSource)==boost::filesystem::file_size(strPathTemp), "Copy file failed: ", strSource, " does not have same size as ", strPathTemp);
						} else {
							std::size_t cbSource = 0;
							tc::for_each(tc::filesystem::recursive_file_range(strSource), [&](auto&& direntry) noexcept {
								cbSource += boost::filesystem::file_size(direntry);
							});
							std::size_t cbTemp = 0;
							tc::for_each(tc::filesystem::recursive_file_range(strPathTemp), [&](auto&& direntry) noexcept {
								cbTemp += boost::filesystem::file_size(direntry);
							});
							_ASSERTPRINT(cbSource==cbTemp, "Copy file failed: ", strSource, " does not have same size as ", strPathTemp);
						}
						
						// renamex_np is a POSIX extension that returns EEXIST when the target file already exists
						if(!ERRNOIGNORE(
							renamex_np(tc::as_c_str(strPathTemp), tc::as_c_str(strPathCached), RENAME_EXCL),
							tc::err::returned(0),
							tc::err::returned(-1, EEXIST)
						)) {
							tc::filesystem::remove_all(tc::as_c_str(strPathTemp));
						};
						return std::forward<decltype(strPathCached)>(strPathCached);
					}
					return std::forward<decltype(strSource)>(strSource);
				} else {
					return std::basic_string<char>();
				}
			};
			
			auto const strCacheFolder = tc::concat(AppendUuid(SymbolCache()), "/");
			auto const strBinaryFilename = FilenameWithoutPath<tc::return_drop>(strBinary);
			
			std::basic_string<char> strSymbols;
			try {
				auto const strContents = tc::make_str(
					tc::as_typed_range<char>(SFileMapping(tc::as_c_str(tc::make_str(strSymbolsPath, strUuid))))
				); // THROW(tc::file_failure)
				
				auto const strPath = tc::find_first<tc::return_take_before>(strContents, '\n');
				_ASSERTEQUAL(tc::front(strPath), '~');

				if(bMountSource) {
					CreateAndWaitForProcess(
						"/usr/bin/osascript",
						tc::make_array<char const*>(tc::aggregate_tag, "-s", "o", "-e", tc::make_c_str("mount volume \"", , c_szSourceServer, tc::drop(strContents, modified(tc::end(strPath), ++_)), "\""))
					);
				}

				// We cache the symbol file at the location where lldb will look for it. The API does not allow
				// us to set the symbol file for the executable explicitly.
				
				// FIXME
				// Drop /Contents/Resources/DWARF/lib to get path of lib.dSYM symbol package
				auto const strDSymPath =
					tc::drop_last(FilenameWithoutPath<tc::return_take>(
						tc::drop_last(FilenameWithoutPath<tc::return_take>(
							tc::drop_last(FilenameWithoutPath<tc::return_take>(
								tc::drop_last(FilenameWithoutPath<tc::return_take>(
									strPath
								))
							))
						))
					));
				strSymbols = CacheFile(
					tc::make_str(VERIFY(::getenv("HOME")), tc::drop_first(strDSymPath)), // copy the entire .dSYM folder recursively
					tc::make_str(strCacheFolder, strBinaryFilename, ".dSYM")
				);
				
				// FIXME
				// Append Contents/Resources/DWARF/lib filename again and return it to lldb
				tc::append(strSymbols, tc::drop(strPath, tc::end(strDSymPath)));
			} catch(tc::file_failure const&) {
			}
			return std::make_pair(CacheFile(tc_move(strBinary), tc::make_str(strCacheFolder, strBinaryFilename)), tc_move(strSymbols));
		} catch(tc::file_failure const&) {
		}
		return std::make_pair(std::basic_string<char>(), std::basic_string<char>());
	};

	// The actual binary name is redundant. The binary is always the first loaded module.
	if(!tc::ends_with<tc::return_bool>(tc::front(dumpmetainfo.m_vecmodule).m_strPath, dumpmetainfo.m_strExecutable)) {
		_ASSERTKNOWNFALSEPRINT("Executable is not the first module.\n");
		ThrowLoadFail(); // THROW(ExLoadFail)
	}
	
	auto const strBinary = LookupBinaryAndSymbol(tc::front(dumpmetainfo.m_vecmodule).m_strUuid).first; // THROW(ExLoadFail)
	if(tc::empty(strBinary)) {
		_ASSERTKNOWNFALSEPRINT("No binary found for ", tc::front(dumpmetainfo.m_vecmodule).m_strUuid, " while looking for executable ", tc::front(dumpmetainfo.m_vecmodule).m_strPath, "\n");
		ThrowLoadFail(); // THROW(ExLoadFail)
	}

	TRACE("Debugging dump file with executable ", strBinary, ".\n");

	m_debugger = lldb::SBDebugger::Create();
	_ASSERT(m_debugger.IsValid());

	// Prevent LLDB from indexing the symbol tables for all binaries:
	m_debugger.SetInternalVariable("target.preload-symbols", "false", m_debugger.GetInstanceName());
	m_debugger.SetInternalVariable("symbols.enable-external-lookup", "false", m_debugger.GetInstanceName());

	lldb::SBError error;
	auto target = m_debugger.CreateTarget(tc::as_c_str(strBinary), "x86_64-apple-macosx", "host", /*add_dependent_modules*/ false, error);
	_ASSERT(target.IsValid());

	auto const strFileDump = tc::temporary_file([&](char const* szFile) noexcept {
			auto rngbyteDump = tc::as_pointers(vecbyte);
			tc::drop_inplace(rngbyteDump, tc::search<tc::return_border_after>(rngbyteDump, tc::range_as_blob("</root>")));
			NOEXCEPT(tc::append(tc::appendfile(szFile, tc::create_new_tag), rngbyteDump));
			return true;
		},
		/*bShare*/ false
	);
	scope_exit(tc::delete_file(tc::as_c_str(strFileDump)));

	auto process = target.LoadCore(tc::as_c_str(strFileDump));
	if(!process.IsValid()) {
		_ASSERTKNOWNFALSEPRINT("lldb could not load dump file.\n");
		ThrowLoadFail(); // THROW(ExLoadFail)
	}
	_ASSERTEQUAL(target.GetNumModules(), 1); // only the executable has been loaded

	if(tc::make_size_proxy(process.GetNumThreads())<=dumpmetainfo.m_nThread) {
		_ASSERTKNOWNFALSEPRINT("Number of threads out of bounds");
		ThrowLoadFail(); // THROW(ExLoadFail)
	}
	process.SetSelectedThread(process.GetThreadAtIndex(dumpmetainfo.m_nThread));

	{
		// set start address of executable
		auto const& moduleExecutable = tc::front(dumpmetainfo.m_vecmodule);
		_ASSERTEQUAL(moduleExecutable.m_strUuid, tc::make_str(target.GetModuleAtIndex(0).GetUUIDString()));
		auto const module = target.GetModuleAtIndex(0);
		VERIFY(target.SetModuleLoadAddress(module, moduleExecutable.m_pvStartAddress - module.GetObjectFileHeaderAddress().GetFileAddress()).Success());
	}

	// Lookup and add all other loaded modules
	tc::for_each(tc::drop_first(dumpmetainfo.m_vecmodule), [&](auto const& moduleDump) THROW(ExLoadFail) {
		auto const pairstrstrBinarySymbol = LookupBinaryAndSymbol(moduleDump.m_strUuid); // THROW(ExLoadFail)
		
		if(tc::empty(pairstrstrBinarySymbol.first)) {
			TRACE("No module with uuid ", moduleDump.m_strUuid, " found in binary cache while looking for ", moduleDump.m_strPath, " ", moduleDump.m_modver, "\n");
		} else {
			auto const module = target.AddModule(
				/*path*/ tc::as_c_str(pairstrstrBinarySymbol.first),
				/*triple*/ "x86_64-apple-macosx",
				/*uuid*/ nullptr, // not setting the uuid means LLDB does not make any lookups itself in the global library cache
				/*sym_file*/ tc::as_c_str(pairstrstrBinarySymbol.second)
			);
			if(module.IsValid()) {
				VERIFY(target.SetModuleLoadAddress(module, moduleDump.m_pvStartAddress).Success());
			} else {
				TRACE("lldb could not load module ", pairstrstrBinarySymbol.first, ".\n");
			}
		}
	});
}

SDebugger::~SDebugger() {
	lldb::SBDebugger::Destroy(m_debugger);
	RETURNS_VOID(lldb::SBDebugger::Terminate());
}
