// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont
//============================================================
//
//  sdlmain.cpp - main file for SDLMAME.
//
//  SDLMAME by Olivier Galibert and R. Belmont
//
//============================================================

// OSD headers
#include "osdsdl.h"
#include "modules/lib/osdlib.h"
#include "modules/diagnostics/diagnostics_module.h"

// MAME headers
#include "emu.h"
#include "emuopts.h"
#include "main.h"
#include "video.h"

#include "corestr.h"

#include "osdepend.h"
#include "strconv.h"

#include <SDL2/SDL.h>

// only for oslog callback
#include <functional>
#include <random>

#ifdef SDLMAME_UNIX
#if (!defined(SDLMAME_MACOSX)) && (!defined(SDLMAME_EMSCRIPTEN)) && (!defined(SDLMAME_ANDROID))
#ifndef SDLMAME_HAIKU
#include <fontconfig/fontconfig.h>
#endif
#endif
#ifdef SDLMAME_MACOSX
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <Carbon/Carbon.h>
#endif
#endif

#include "../quest/questnet.h"

// standard includes
#if !defined(SDLMAME_WIN32)
#include <unistd.h>
#endif


//============================================================
//  Global variables
//============================================================

#if defined(SDLMAME_UNIX) || defined(SDLMAME_WIN32)
int sdl_entered_debugger;
#endif


//============================================================
//  main
//============================================================

// we do some special sauce on Win32...

#if defined(SDLMAME_WIN32)
/* gee */
extern "C" DECLSPEC void SDLCALL SDL_SetModuleHandle(void *hInst);
#endif

int main(int argc, char** argv)
{
	std::vector<std::string> args = osd_get_command_line(argc, argv);
	int res = 0;

#if defined(SDLMAME_ANDROID)
	// on Quest, join a nearby host if one is advertising, otherwise host
	{
		namespace net = osd::quest::net;
		net::guest_options guest;
		guest.address = net::find_host(net::HOST_SEARCH_TIME);
		while (!guest.address.empty())
		{
			int const result = net::guest_main(guest);
			if (result != net::GUEST_HOST_GONE)
				return result;

			// look a little longer for another host, for a random time so that
			// of the players the host left behind, one hosts and the rest join
			std::random_device seed;
			std::uniform_int_distribution<int> extra(0, 2800);
			guest.address = net::find_host(net::HOST_SEARCH_TIME + std::chrono::milliseconds(extra(seed)), guest.address);
			if (guest.address.empty())
				net::host_notice("The host left, so you're hosting now");
		}
		args.emplace_back("-" OSDOPTION_HOST);
	}
#else
	// -join runs a network guest instead of MAME
	{
		osd::quest::net::guest_options guest;
		if (osd::quest::net::parse_guest_options(args, guest))
			return osd::quest::net::guest_main(guest);
	}
#endif

	// disable I/O buffering
	setvbuf(stdout, (char *) nullptr, _IONBF, 0);
	setvbuf(stderr, (char *) nullptr, _IONBF, 0);

	// Initialize crash diagnostics
	diagnostics_module::get_instance()->init_crash_diagnostics();

#if defined(SDLMAME_ANDROID)
	/* Enable standard application logging */
	SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_VERBOSE);
#endif

	// FIXME: this should be done differently

#ifdef SDLMAME_UNIX
	sdl_entered_debugger = 0;
#if (!defined(SDLMAME_MACOSX)) && (!defined(SDLMAME_HAIKU)) && (!defined(SDLMAME_EMSCRIPTEN)) && (!defined(SDLMAME_ANDROID))
	FcInit();
#endif
#endif

	{
		sdl_options options;
		sdl_osd_interface osd(options);
		osd.register_options();
		res = emulator_info::start_frontend(options, osd, args);
	}

#ifdef SDLMAME_UNIX
#if (!defined(SDLMAME_MACOSX)) && (!defined(SDLMAME_HAIKU)) && (!defined(SDLMAME_EMSCRIPTEN)) && (!defined(SDLMAME_ANDROID))
	if (!sdl_entered_debugger)
	{
		FcFini();
	}
#endif
#endif

	exit(res);
}
