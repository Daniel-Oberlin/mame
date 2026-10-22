// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questdisplay.h - where finished frames are shown
//
//  The Quest renderer and the network guest both produce whole frames in
//  memory and hand them to a display: the headset's virtual screen on
//  Quest, or a desktop window elsewhere (used to develop multiplayer
//  without a headset).
//
//============================================================
#ifndef MAME_OSD_QUEST_QUESTDISPLAY_H
#define MAME_OSD_QUEST_QUESTDISPLAY_H

#pragma once

#include <cstdint>
#include <memory>

struct SDL_Window;


namespace osd::quest {

class frame_display
{
public:
	virtual ~frame_display() = default;

	// Returns a width x height buffer (pitch == width, bytes R,G,B,X, top
	// row first) for the next frame.
	virtual uint32_t *begin_frame(int width, int height) = 0;

	// Shows the frame drawn since begin_frame. aspect is the shape of the
	// picture on screen (width / height), which its pixels need not share.
	// native says the frame is a raster game screen at its own resolution;
	// vertical says that game's monitor was mounted sideways.
	virtual void end_frame(float aspect, bool native, bool vertical) = 0;

	// How many display pixels the picture's longest edge spans, or 0 if not
	// yet known, so frames can be drawn at a matching resolution.
	virtual int screen_pixels() = 0;
};

#if defined(SDLMAME_ANDROID)
// The headset's virtual screen (see questxr.h); xr_start() must have run.
std::unique_ptr<frame_display> make_xr_display();
#else
// Draws into an SDL window with an SDL renderer. The window must outlive
// the display, and both must be used from the thread that created the window.
std::unique_ptr<frame_display> make_window_display(SDL_Window *window);
#endif

} // namespace osd::quest

#endif // MAME_OSD_QUEST_QUESTDISPLAY_H
