// think-cell minidump library
//
// Copyright (C) 2016-2020 think-cell Software GmbH
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

#include "Minidump.h"
#include "tc/range.h"
#include "tc/append.h"

#include <mach-o/loader.h>
#include <mach-o/dyld_images.h>
#include <mach/vm_param.h>
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <sys/semaphore.h>

template<std::uint32_t nCOMMAND, typename TCommand, typename Func>
tc::break_or_continue ForEachLoadCommand(mach_header_64 const* pmachheader, Func fn) MAYTHROW {
	_ASSERTEQUAL(pmachheader->magic, MH_MAGIC_64);
	auto rngbyteLoadCommand = tc::counted(
		reinterpret_cast<unsigned char const*>(pmachheader) + sizeof(mach_header_64),
		pmachheader->sizeofcmds
	);
	for (auto itbyteLoadCommand = tc::begin(rngbyteLoadCommand);
		itbyteLoadCommand != tc::end(rngbyteLoadCommand);
		itbyteLoadCommand += reinterpret_cast<load_command const*>(itbyteLoadCommand)->cmdsize) // TODO: parallel_for_each segment
	{
		if (nCOMMAND == reinterpret_cast<load_command const*>(itbyteLoadCommand)->cmd) {
			RETURN_IF_BREAK(tc::continue_if_not_break(fn, *reinterpret_cast<TCommand const*>(itbyteLoadCommand))); // MAYTHROW
		}
	}
	return tc::continue_;
}

template<typename Func>
tc::break_or_continue ForEachMemoryRegion(task_t task, mach_vm_address_t pvBegin, Func fn) noexcept {
	mach_vm_size_t cb = 0;

	vm_region_submap_info_64 vmregioninfo;
	natural_t nDepth = 0;

	mach_msg_type_number_t cbVMRegionInfo = VM_REGION_SUBMAP_INFO_COUNT_64;

	while(KERN_SUCCESS == MACHERRIGNORE(
		mach_vm_region_recurse(
			task,
			std::addressof(pvBegin),
			std::addressof(cb),
			std::addressof(nDepth),
			reinterpret_cast<vm_region_info_64_t>(std::addressof(vmregioninfo)),
			std::addressof(cbVMRegionInfo)
		),
		(KERN_INVALID_ADDRESS)
	)) {
		if(vmregioninfo.is_submap) {
			++nDepth;
		} else {
			// See https://opensource.apple.com/source/system_cmds/system_cmds-735.50.6/gcore.tproj/vanilla.c.auto.html
			if(VM_MEMORY_IOKIT != vmregioninfo.user_tag  // skip IO memory segments
			&& VM_PROT_READ==(vmregioninfo.protection&VM_PROT_READ)) { // unreadable segments
				TRACE("vmregion: ",
					tc::as_padded_lc_hex(pvBegin), " ",
					tc::as_dec(vmregioninfo.protection), ", ",
					tc::as_dec(vmregioninfo.user_tag), ", " // see <mach/vm_statistics.h>
					, tc::as_dec(vmregioninfo.share_mode), ", " // see SM_XXX in <mach/vm_region.h>
					, tc::as_dec(vmregioninfo.behavior)); // <mach/vm_behavior.h>
				RETURN_IF_BREAK( tc::continue_if_not_break(fn, pvBegin, cb, vmregioninfo.protection, vmregioninfo.max_protection, vmregioninfo.user_tag) );
			}
			pvBegin += cb;
		}
	}
	return tc::continue_;
}


std::basic_string<char> MiniDumpWriteDump(task_t task, std::uint64_t threadid, bool bBig, tc::ptr_range<char const> strExecutable, tc::ptr_range<tc::char16 const> strBundleVersion) THROW(tc::file_failure) {
	MACHERR(task_suspend(task));
	scope_exit(MACHERR(task_resume(task)));

	struct SThreadCommand {
		thread_command m_header;
		x86_thread_state m_threadstate;
		x86_float_state m_floatstate;
		x86_exception_state m_exceptionstate;
	};

	int iCurrentThread;
	tc::vector<SThreadCommand> const vecthreadcmd = [&]() noexcept {
		mach_msg_type_number_t cThreads;
		thread_array_t athread;

		MACHERR(task_threads(task, &athread, &cThreads));
		scope_exit(
			tc::for_each(tc::iota(0u, cThreads), [&](int iThread) noexcept {
				MACHERR(mach_port_deallocate(mach_task_self(), athread[iThread]));
			});

			MACHERR(mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(athread), cThreads * sizeof(thread_act_t)));
		);
		
		return tc::make_vector(
			tc::transform(
				tc::iota(0u, cThreads),
				[&](int iThread) noexcept {
					thread_identifier_info threadidinfo;
					mach_msg_type_number_t cnInfo = THREAD_IDENTIFIER_INFO_COUNT;
					MACHERR(thread_info(athread[iThread], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(std::addressof(threadidinfo)), std::addressof(cnInfo)));
					if(threadidinfo.thread_id==threadid) {
						iCurrentThread = iThread;
					}

					SThreadCommand threadcmd = {
						{LC_THREAD, sizeof(SThreadCommand) },
						{{x86_THREAD_STATE64, x86_THREAD_STATE64_COUNT}},
						{{x86_FLOAT_STATE64, x86_FLOAT_STATE64_COUNT}},
						{{x86_EXCEPTION_STATE64, x86_EXCEPTION_STATE64_COUNT}}
					};
					
					auto GetThreadState = [&](x86_state_hdr_t const& hdr, auto& threadstate) noexcept {
						mach_msg_type_number_t cbThreadState = hdr.count;
						MACHERR(thread_get_state(athread[iThread], hdr.flavor, reinterpret_cast<thread_state_t>(std::addressof(threadstate)), std::addressof(cbThreadState)));
						_ASSERTEQUAL(cbThreadState, hdr.count);
					};
					
					GetThreadState(threadcmd.m_threadstate.tsh, threadcmd.m_threadstate.uts);
					GetThreadState(threadcmd.m_floatstate.fsh, threadcmd.m_floatstate.ufs);
					GetThreadState(threadcmd.m_exceptionstate.esh, threadcmd.m_exceptionstate.ues);
			
					return threadcmd;
				}
			)
		);
	}();
	_ASSERTINITIALIZED(iCurrentThread);

	std::basic_string<char> strFileDump;
	scope_exit( tc::delete_file(tc::as_c_str(strFileDump)) );
	{
		tc::readwritefile fileDump;
		tc::tie(fileDump, strFileDump) = tc::readwritefile::create_temporary(); // THROW(tc::file_failure)

		// Write XML header
		tc::append(tc::make_typed_stream<XMLCHAR>(fileDump),
			"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
			"<root>"
			"<version val=\"" BOOST_PP_STRINGIZE(c_nBuild) "\"/>"
			"<PersistentType>"
			"<m_strExecutable>", SXmlStringEscaper::Escape(strExecutable), "</m_strExecutable>"
			"<m_strBundleVersion>", SXmlStringEscaper::Escape(strBundleVersion), "</m_strBundleVersion>"
			"<m_nThread val=\"", tc::as_dec(iCurrentThread), "\"/>"); // THROW(tc::file_failure)

		// Write list of loaded modules, their file path and start address
		task_dyld_info dyldinfo;
		mach_msg_type_number_t cnDyldInfo = TASK_DYLD_INFO_COUNT;
		MACHERR(task_info(task, TASK_DYLD_INFO, reinterpret_cast<task_info_t>(std::addressof(dyldinfo)), &cnDyldInfo));
		_ASSERTEQUAL(dyldinfo.all_image_info_format, TASK_DYLD_ALL_IMAGE_INFO_64);

		auto ReadTaskMemory = [&](mach_vm_address_t pv, tc::ptr_range<unsigned char> rngbyte) noexcept {
			mach_vm_size_t cbActual = 0;
			MACHERR(mach_vm_read_overwrite(task, pv, tc::size(rngbyte), reinterpret_cast<mach_vm_address_t>(tc::ptr_begin(rngbyte)), std::addressof(cbActual)));
			_ASSERTEQUAL(cbActual, tc::size(rngbyte));
		};

		// Subset of dyld_all_image_infos. dyld_all_image_infos grows with macOS version updates. Extract only what we need.
		struct dyld_all_image_infos_subset {
			std::uint32_t version;
			std::uint32_t infoArrayCount;
			const struct dyld_image_info* infoArray;
		};

		dyld_all_image_infos_subset dyldallimginfos;
		_ASSERT(sizeof(dyld_all_image_infos_subset) <= dyldinfo.all_image_info_size);
		ReadTaskMemory(dyldinfo.all_image_info_addr, tc::as_blob(dyldallimginfos));

		tc::vector<dyld_image_info> vecdyldimginfo;
		vecdyldimginfo.resize(dyldallimginfos.infoArrayCount);
		ReadTaskMemory(reinterpret_cast<mach_vm_address_t>(dyldallimginfos.infoArray), tc::range_as_blob(vecdyldimginfo));

		tc::append(tc::make_typed_stream<XMLCHAR>(fileDump),
			"<m_vecmodule length=\"", tc::as_dec(dyldallimginfos.infoArrayCount), "\">"); // THROW(tc::file_failure)
		tc::for_each(
			vecdyldimginfo,
			[&](dyld_image_info const& dyldimginfo) noexcept {

				tc::append(tc::make_typed_stream<XMLCHAR>(fileDump),
					"<elem>"
					"<m_pvStartAddress val=\"", tc::as_dec(reinterpret_cast<std::uint64_t>(dyldimginfo.imageLoadAddress)), "\"/>"); // THROW(tc::file_failure)

				{	// Map part of task's memory so we can read and print the zero-terminated file path
					vm_region_basic_info_64 regionbasicinfo;
					mach_vm_address_t pvRegion = reinterpret_cast<mach_vm_address_t>(dyldimginfo.imageFilePath);
					mach_vm_size_t cb = 0;
					mach_msg_type_number_t cnInfo = VM_REGION_BASIC_INFO_COUNT_64;
					mach_port_t portObject = 0;
					MACHERR(mach_vm_region(task, std::addressof(pvRegion), std::addressof(cb), VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(std::addressof(regionbasicinfo)), std::addressof(cnInfo), std::addressof(portObject)));

					mach_vm_address_t pvRegionNew = 0;
					vm_prot_t protCur = VM_PROT_NONE;
					vm_prot_t protMax = VM_PROT_NONE;
					if(KERN_SUCCESS==MACHERRIGNORE(mach_vm_remap(mach_task_self(), std::addressof(pvRegionNew), cb, 0, VM_FLAGS_ANYWHERE, task, pvRegion, false, std::addressof(protCur), std::addressof(protMax), VM_INHERIT_NONE), (KERN_NO_SPACE))) {

						scope_exit(MACHERR(mach_vm_deallocate(mach_task_self(), pvRegionNew, cb)));
						tc::append(tc::make_typed_stream<XMLCHAR>(fileDump),
							"<m_strPath>", SXmlStringEscaper::Escape(dyldimginfo.imageFilePath - pvRegion + pvRegionNew), "</m_strPath>"); // THROW(tc::file_failure)
					}
				}

				tc::vector<unsigned char> vecbyteModule(sizeof(mach_header_64));
				ReadTaskMemory(reinterpret_cast<mach_vm_address_t>(dyldimginfo.imageLoadAddress), tc::range_as_blob(vecbyteModule));
				vecbyteModule.resize(tc::size(vecbyteModule)+reinterpret_cast<mach_header_64 const*>(tc::ptr_begin(vecbyteModule))->sizeofcmds);

				ReadTaskMemory(reinterpret_cast<mach_vm_address_t>(dyldimginfo.imageLoadAddress), tc::range_as_blob(vecbyteModule));

				ForEachLoadCommand<LC_ID_DYLIB, dylib_command>(
					reinterpret_cast<mach_header_64 const*>(tc::ptr_begin(vecbyteModule)),
					[&](auto const& dylibcmd) noexcept {
						tc::append(tc::make_typed_stream<XMLCHAR>(fileDump),
							"<m_modver val=\"", tc::as_dec(dylibcmd.dylib.current_version), "\"/>"); // THROW(tc::file_failure)
						return INTEGRAL_CONSTANT(tc::break_)();
					}
				);

				ForEachLoadCommand<LC_UUID, uuid_command>(
					reinterpret_cast<mach_header_64 const*>(tc::ptr_begin(vecbyteModule)),
					[&](auto const& uuidcmd) noexcept {
						boost::uuids::uuid uuid;
						STATICASSERTEQUAL(sizeof(uuid.data), sizeof(uuidcmd.uuid));
						tc::cont_assign(uuid.data,uuidcmd.uuid);
						tc::append(tc::make_typed_stream<XMLCHAR>(fileDump), "<m_uuid val=\"", tc::as_lc_hex(uuid), "\"/>"); // THROW(tc::file_failure)
						return INTEGRAL_CONSTANT(tc::break_)();
					}
				);
				tc::append(tc::make_typed_stream<XMLCHAR>(fileDump), "</elem>"); // THROW(tc::file_failure)
			}
		);
		tc::append(tc::make_typed_stream<XMLCHAR>(fileDump),
			"</m_vecmodule>"
			"</PersistentType>"
			"</root>"); // THROW(tc::file_failure)

		tc::vector<segment_command_64> vecsegmentMapped; // memory content will be sent with dump
		tc::vector<segment_command_64> vecsegmentUnmapped; // memory will not be sent
		ForEachMemoryRegion(task, MACH_VM_MIN_ADDRESS, [&](mach_vm_address_t pvBegin, mach_vm_size_t cb, vm_prot_t prot, vm_prot_t protMax, unsigned int nUserTag) noexcept {
			auto const bMapped = bBig
				|| VM_MEMORY_STACK==nUserTag
				|| tc::any_of(vecthreadcmd, [&](SThreadCommand const& threadcmd) noexcept {
					auto const intvl = tc::make_interval(pvBegin, cb, tc::lo);
					return intvl.contains(threadcmd.m_threadstate.uts.ts64.__rbp) || intvl.contains(threadcmd.m_threadstate.uts.ts64.__rsp);
				});
			tc::cont_emplace_back(
				bMapped
				? vecsegmentMapped
				: vecsegmentUnmapped,
				segment_command_64 {
					LC_SEGMENT_64,
					sizeof(segment_command_64),
					{0}, // segname[16]
					pvBegin,
					cb,
					0, // file offset needs to be set once number of segments has been determined
					bMapped ? cb : 0,
					protMax,
					prot,
					0, // nsects
					0 // flags
				}
			);
		});

		mach_header_64 const header = {
			MH_MAGIC_64,
			CPU_TYPE_X86_64,
			CPU_SUBTYPE_X86_64_ALL,
			MH_CORE,
			tc::size(vecsegmentMapped) + tc::size(vecsegmentUnmapped) + tc::size(vecthreadcmd),
			tc::size(tc::range_as_blob(vecsegmentMapped)) + tc::size(tc::range_as_blob(vecsegmentUnmapped)) + tc::size(tc::range_as_blob(vecthreadcmd))
		};

		auto const cbDumpFileHeader = fileDump.size(); // THROW(tc::file_failure)
		auto cbFileOffset = round_page(sizeof(mach_header_64) + header.sizeofcmds);
		tc::for_each(vecsegmentMapped, [&](segment_command_64& segcmd) noexcept {
			segcmd.fileoff = cbFileOffset;
			cbFileOffset += segcmd.filesize;
		});

		tc::append(fileDump, tc::as_blob(header), tc::range_as_blob(vecsegmentMapped), tc::range_as_blob(vecsegmentUnmapped), tc::range_as_blob(vecthreadcmd));  // THROW(tc::file_failure)

		{
			tc::for_each(vecsegmentMapped, [&](segment_command_64 const& segcmd) noexcept {
				fileDump.seek(cbDumpFileHeader + segcmd.fileoff);  // THROW(tc::file_failure)

				mach_vm_address_t pvRegionNew = 0;
				vm_prot_t protCur = VM_PROT_NONE;
				vm_prot_t protMax = VM_PROT_NONE;
				MACHERR(mach_vm_remap(mach_task_self(), std::addressof(pvRegionNew), segcmd.vmsize, 0, VM_FLAGS_ANYWHERE, task, segcmd.vmaddr, false, std::addressof(protCur), std::addressof(protMax), VM_INHERIT_NONE));
				scope_exit(mach_vm_deallocate(mach_task_self(), pvRegionNew, segcmd.vmsize));

				tc::append(fileDump, tc::counted(reinterpret_cast<unsigned char const*>(pvRegionNew), segcmd.vmsize));
			});
		}
	} // closes fileDump

	return tc::temporary_file([&](char const* szPath) noexcept {
			tc::cont_emplace_back(strFileDump, '\0');
			ZipFile( tc::as_c_str(strFileDump), "minidump.dmp\0", szPath);
			return true;
		},
		/*bShare*/ false
	);
}
