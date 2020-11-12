// think-cell minidump library
//
// Copyright (C) 2016-2020 think-cell Software GmbH
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

#pragma once

#include "tc/range.h"

#include <mach/mach.h>
#include <bootstrap.h>
#include <mach/thread_info.h>

namespace tc {
	using pid = pid_t;
}

struct SDumpInfo final {
private:
	task_t m_task;

	std::uint64_t m_threadid; // as returned by mach thread_info system call

	std::basic_string<char> m_strExecutable;
	std::basic_string<tc::char16> m_strBundleVersion;
	
	struct STcDumpMsg {
	  mach_msg_header_t header;
	  mach_msg_body_t body;
	  mach_msg_port_descriptor_t portdesc;
	};

public:
	std::basic_string<char> WriteDump(bool bBig) const& THROW(tc::file_failure);
	
	template<typename Pipe>
	static void Marshal(Pipe& pipe) MAYTHROW {
		tc::append(pipe,
			/*m_threadid*/ tc::as_blob([]() noexcept {
				auto portThread = mach_thread_self();
				// Surprisingly, portThread must be deallocated as opposed to mach_task_self()
				scope_exit(MACHERR(mach_port_deallocate(mach_task_self(), portThread)));

				thread_identifier_info threadidinfo;
				mach_msg_type_number_t cnInfo = THREAD_IDENTIFIER_INFO_COUNT;
				MACHERR(thread_info(mach_thread_self(), THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(std::addressof(threadidinfo)), std::addressof(cnInfo)));
				return threadidinfo.thread_id;
			}()),
			/*m_strExecutable*/ tc::size_prefixed(ExecutablePath()),
			/*m_strBundleVersion*/ tc::size_prefixed([]() noexcept {
				if(auto const cfbundle = tc::with_get_rule(CFBundleGetMainBundle())) {
					return tc::make_str(tc::derived_cast<NSString>(tc::with_get_rule(CFDictionaryGetValue(VERIFY(CFBundleGetInfoDictionary(cfbundle)), kCFBundleVersionKey))));
				} else {
					return std::basic_string<tc::char16>();
				}
			}())
		); // MAYTHROW
		pipe.flush(); // MAYTHROW
		
		// Wait with timeout for signal that SDumpInfo ctor has setup the mach port.
		// bootstrap_look_up never seems to fail.
		auto const strMachPort = tc::read_container<std::basic_string<char>>(pipe); // MAYTHROW

		// See CFMessagePortCreateRemote in https://opensource.apple.com/source/CF/CF-1153.18/CFMessagePort.c.auto.html
		mach_port_t portBootstrap;
		MACHERR(task_get_bootstrap_port(mach_task_self(), &portBootstrap));

		mach_port_t portChild;
		MACHERR( bootstrap_look_up(portBootstrap, tc::as_c_str(strMachPort), &portChild) );
		scope_exit( MACHERR(mach_port_deallocate(mach_task_self(), portChild)); )

		// This will copy a send right on this process' task port and send it to tcupdate
		STcDumpMsg msg = {
			{ // header
				MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND) | MACH_MSGH_BITS_COMPLEX, // bits
				sizeof(msg), // size
				portChild // remotePort
			},
			{ // body
				1 // msgh_descriptor_count
			},
			{ // portdesc
				mach_task_self(),
				0,
				0,
				// MACH_MSG_TYPE_COPY_SEND copies the send rights to our task to tcupdate.
				// If we specify MACH_MSG_TYPE_PORT_SEND the send rights are moved to tcupdate,
				// i.e., our process can no longer access mach_task_self().
				MACH_MSG_TYPE_COPY_SEND,
				MACH_MSG_PORT_DESCRIPTOR
			}
		};
		MACHERR(mach_msg(std::addressof(msg.header), MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
	}
	
	template<typename In, typename Out>
	SDumpInfo(In&& in, Out&& out) MAYTHROW
	: 	m_threadid(tc::read<std::uint64_t>(in)),
		m_strExecutable(tc::read_container<std::basic_string<char>>(in)),
		m_strBundleVersion(tc::read_container<std::basic_string<tc::char16>>(in))
	{
		// We setup the port the same way CFMessagePortCreateLocal does it.
		// See https://opensource.apple.com/source/CF/CF-1153.18/CFMessagePort.c.auto.html
		mach_port_t portBootstrap;
		MACHERR(task_get_bootstrap_port(mach_task_self(), &portBootstrap));

 		// FIXME: Generate a random port name. Might need to be prefixed with your application group identifier if you are running
		// in a sandboxed environment
		auto const strMachPort = tc::explicit_cast<std::basic_string<char>>("myportname");

		mach_port_t port;
		MACHERR(bootstrap_check_in(portBootstrap, tc::as_c_str(strMachPort), &port));
		scope_exit( MACHERR(mach_port_destroy(mach_task_self(), port)); )
#ifdef _CHECKS
		mach_port_type_t type = 0;
		MACHERR(mach_port_type(mach_task_self(), port, &type));
		_ASSERT(0 != (type & MACH_PORT_TYPE_PORT_RIGHTS));
#endif
		MACHERR(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));

		// signal that we have setup the bootstrap port
		tc::append(out, tc::size_prefixed(strMachPort)); // MAYTHROW
		out.flush(); // MAYTHROW

		struct STcDumpReceivedMsg {
			STcDumpMsg msg;
			mach_msg_trailer_t trailer; // every received msg contains a trailer, this is the empty trailer
		} msgrcv = {
			{{ // header
			  0, // bits
			  sizeof(STcDumpReceivedMsg), // size
			  0, // remotePort
			  port // localPort
			}}
		};

		MACHERR(mach_msg(
			std::addressof(msgrcv.msg.header),
			MACH_RCV_MSG
			| MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)
			| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_NULL),
			0,
			sizeof(STcDumpReceivedMsg),
			port,
			MACH_MSG_TIMEOUT_NONE,
			MACH_PORT_NULL
		));

		m_task = msgrcv.msg.portdesc.name;
	}
	
	tc::pid pid() const& noexcept;
};
