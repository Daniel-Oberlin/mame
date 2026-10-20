// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questanchor.h - keeps the virtual screen at a fixed spot in the room
//
//============================================================
#ifndef MAME_OSD_QUEST_QUESTANCHOR_H
#define MAME_OSD_QUEST_QUESTANCHOR_H

#pragma once

#include <openxr/openxr.h>

#include <string>
#include <vector>


namespace osd::quest {

// Holds the screen's placement as a spatial anchor saved on the headset, so it
// stays put across launches. Each new placement creates and saves a fresh
// anchor; the previous one is erased once the new one is safely stored. The
// anchor's UUID and the screen height are kept in a small text file.
class screen_anchor
{
public:
	// instance extensions the anchor support needs
	static std::vector<char const *> const &extensions();

	// Starts loading the saved anchor, if any. Returns false when the runtime
	// lacks anchor support. height is set when a saved height is found.
	bool init(XrInstance instance, XrSession session, std::string path, float &height);
	void shutdown();

	// Returns true if the event was an anchor event and has been handled.
	bool handle_event(XrEventDataBuffer const &event);

	// Anchors the screen at pose (in base) and saves it with the given height.
	void place(XrSpace base, XrPosef const &pose, XrTime time, float height);

	// Gets the anchored screen pose in base; false if there is no anchor or it
	// can't currently be located.
	bool locate(XrSpace base, XrTime time, XrPosef &pose) const;

private:
	enum class status { READY, PENDING, FAILED };

	status enable_component(XrSpace space, XrSpaceComponentTypeFB component);
	void save();
	void retire(XrSpace space);
	void write_file(XrUuidEXT const &uuid) const;

	XrSession m_session = XR_NULL_HANDLE;
	std::string m_path;

	XrSpace m_anchor = XR_NULL_HANDLE;
	float m_height = 0.0f;           // height saved with m_anchor
	float m_pending_height = 0.0f;   // height for the anchor being created
	std::vector<XrSpace> m_retired;  // previous anchors, erased once m_anchor is saved
	std::vector<XrSpace> m_erasing;  // destroyed when their erase completes
	XrUuidEXT m_saved_uuid{ };
	XrAsyncRequestIdFB m_query = 0;
	bool m_placed = false;           // placed this session, so ignore a late-loading saved anchor

	PFN_xrCreateSpatialAnchorFB m_xrCreateSpatialAnchorFB = nullptr;
	PFN_xrSetSpaceComponentStatusFB m_xrSetSpaceComponentStatusFB = nullptr;
	PFN_xrSaveSpaceFB m_xrSaveSpaceFB = nullptr;
	PFN_xrEraseSpaceFB m_xrEraseSpaceFB = nullptr;
	PFN_xrQuerySpacesFB m_xrQuerySpacesFB = nullptr;
	PFN_xrRetrieveSpaceQueryResultsFB m_xrRetrieveSpaceQueryResultsFB = nullptr;
};

} // namespace osd::quest

#endif // MAME_OSD_QUEST_QUESTANCHOR_H
