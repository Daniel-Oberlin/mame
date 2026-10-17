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

// Returns a width x height buffer (pitch == width, bytes R,G,B,X) for the
// next frame. Only the renderer thread may call this.
uint32_t *xr_begin_frame(int width, int height);

// Publishes the frame drawn since xr_begin_frame to the compositor.
void xr_end_frame();

} // namespace osd::quest

#endif // MAME_OSD_QUEST_QUESTXR_H
