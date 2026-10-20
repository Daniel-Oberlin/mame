// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questxr.h - OpenXR presentation and controller input for Meta Quest
//
//============================================================
#ifndef MAME_OSD_QUEST_QUESTXR_H
#define MAME_OSD_QUEST_QUESTXR_H

#pragma once

#include <cstdint>


namespace osd::quest {

// Starts the OpenXR thread and attaches the virtual gamepad that Touch
// controller input is fed into. Call from the SDL main thread before input
// modules initialise; later calls are no-ops. The XR thread lives for the rest
// of the process so it survives MAME restarting the machine.
bool xr_start();

// Returns a width x height buffer (pitch == width, bytes R,G,B,X, top row
// first) for the next frame. Only the renderer thread may call this.
uint32_t *xr_begin_frame(int width, int height);

// Publishes the frame drawn since xr_begin_frame. aspect is the shape of the
// picture on screen (width / height), which its pixels need not share.
// native says the frame is a raster game screen at its own resolution, which
// may get the CRT treatment; vertical says that game's monitor was mounted
// sideways, so its scanlines run vertically.
void xr_end_frame(float aspect, bool native, bool vertical);

// How many display pixels the screen's longest edge currently spans in the
// user's view, or 0 if not yet known. Lets the renderer match its resolution
// to how big the screen looks.
int xr_screen_pixels();

// True while the XR session has input focus. The Android window never gets
// focus in immersive mode, so this stands in for it.
bool xr_has_focus();

} // namespace osd::quest

#endif // MAME_OSD_QUEST_QUESTXR_H
