// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questnet.h - sharing one game with players on the local network
//
//  One machine (the host) runs MAME and streams its picture and sound;
//  guests show the stream and send their controller state back, which
//  the host feeds to MAME as extra gamepads. Guests don't run MAME, so
//  they need no ROMs and can join or leave at any time.
//
//============================================================
#ifndef MAME_OSD_QUEST_QUESTNET_H
#define MAME_OSD_QUEST_QUESTNET_H

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>


namespace osd::quest::net {

// guests play as players 2 to MAX_GUESTS + 1
constexpr int MAX_GUESTS = 3;

// the host's gamepads for guests are named this followed by the player number
constexpr char GUEST_PAD_NAME[] = "Network Player";

// One controller's state, laid out like an SDL game controller: a bit per
// SDL_GameControllerButton and a value per SDL_GameControllerAxis.
struct pad_state
{
	uint32_t buttons = 0;
	int16_t axes[6] = { };

	bool operator==(pad_state const &that) const;
	bool operator!=(pad_state const &that) const { return !(*this == that); }
};


//------------------------------------------------
//  host
//------------------------------------------------

// Starts accepting guests and advertising the game on the local network.
// Attaches one SDL virtual gamepad per guest seat, so call it before MAME's
// input modules start. Later calls are no-ops; the host lives for the rest
// of the process so guests stay connected while games change.
bool host_start();

bool hosting();

// Sends a frame to every guest. The pixels are copied, so this returns
// quickly; arguments are as for frame_display.
void host_frame(uint32_t const *pixels, int width, int height, float aspect, bool native, bool vertical);

// Sends interleaved 16-bit samples to every guest.
void host_audio(int16_t const *samples, int frames, int channels, int rate);

// With shared controls, guests' sticks and buttons drive player 1's controls
// too, as players crowd round one upright cabinet and take turns; only coin
// and start stay their own. Without, each guest has only their own player's
// controls, for games where everyone plays at once.
void host_share_controls(bool share);

// Adds guests' input to the local player's while controls are shared.
void host_merge_shared(pad_state &local);

// Tells guests which game is running; empty while choosing one.
void host_set_game(std::string const &name);

// Short messages for the host's player, such as guests joining and leaving.
// Anything may post one; the host's screen takes them to show.
void host_notice(std::string const &text);
std::vector<std::string> host_take_notices();


//------------------------------------------------
//  guest
//------------------------------------------------

// Address of a host advertising a free seat on the local network, or empty
// if none is heard within wait. A host that has just left may still be
// heard for a moment; pass its address as ignore.
std::string find_host(std::chrono::milliseconds wait, std::string const &ignore = std::string());

// long enough to hear a couple of a host's adverts
constexpr std::chrono::milliseconds HOST_SEARCH_TIME{ 1200 };

struct guest_options
{
	std::string address;        // host to join; empty to look for one
	std::string snapshot_dir;   // save every 120th frame here as a BMP (testing)
	std::string presses;        // scripted input, "seconds:button,..." (testing)
	double seconds = 0.0;       // leave after this long; 0 to stay
};

// guest_main's result when there's no host to play with: it stopped,
// couldn't be joined or wasn't found, rather than the player leaving
constexpr int GUEST_HOST_GONE = 2;

// Joins a host and plays until the host goes away or the player leaves.
// Returns a process exit code, or GUEST_HOST_GONE.
int guest_main(guest_options const &options);

// Recognises "-join [address]" and the guest's testing options in a
// command line. Returns false if the command line doesn't ask to join.
bool parse_guest_options(std::vector<std::string> const &args, guest_options &options);

} // namespace osd::quest::net

#endif // MAME_OSD_QUEST_QUESTNET_H
