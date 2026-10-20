// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questanchor.cpp - keeps the virtual screen at a fixed spot in the room
//
//  Uses the XR_FB_spatial_entity family: anchors are created, made
//  storable and saved to the headset's local storage, then found again
//  by UUID on the next launch.
//
//============================================================

#include "questanchor.h"

#include <android/log.h>

#include <algorithm>
#include <cstdio>
#include <iterator>


#define XRLOG(...) __android_log_print(ANDROID_LOG_INFO, "MAMEQuest", __VA_ARGS__)
#define XRERR(...) __android_log_print(ANDROID_LOG_ERROR, "MAMEQuest", __VA_ARGS__)


namespace osd::quest {

namespace {

std::string uuid_to_string(XrUuidEXT const &uuid)
{
	char text[2 * XR_UUID_SIZE_EXT + 1];
	for (int i = 0; i < XR_UUID_SIZE_EXT; ++i)
		std::snprintf(&text[2 * i], 3, "%02x", uuid.data[i]);
	return text;
}

bool uuid_from_string(char const *text, XrUuidEXT &uuid)
{
	for (int i = 0; i < XR_UUID_SIZE_EXT; ++i)
	{
		unsigned byte = 0;
		if (std::sscanf(&text[2 * i], "%2x", &byte) != 1)
			return false;
		uuid.data[i] = uint8_t(byte);
	}
	return true;
}

} // anonymous namespace


std::vector<char const *> const &screen_anchor::extensions()
{
	static std::vector<char const *> const names = {
			XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
			XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,
			XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME };
	return names;
}

bool screen_anchor::init(XrInstance instance, XrSession session, std::string path, float &height)
{
	auto const get = [instance] (char const *name, auto &function)
	{
		xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&function));
		return function != nullptr;
	};
	if (!get("xrCreateSpatialAnchorFB", m_xrCreateSpatialAnchorFB) ||
			!get("xrSetSpaceComponentStatusFB", m_xrSetSpaceComponentStatusFB) ||
			!get("xrSaveSpaceFB", m_xrSaveSpaceFB) ||
			!get("xrEraseSpaceFB", m_xrEraseSpaceFB) ||
			!get("xrQuerySpacesFB", m_xrQuerySpacesFB) ||
			!get("xrRetrieveSpaceQueryResultsFB", m_xrRetrieveSpaceQueryResultsFB))
		return false;
	m_session = session;
	m_path = std::move(path);

	// file holds "<uuid hex> <height>"
	std::FILE *const file = std::fopen(m_path.c_str(), "r");
	if (!file)
		return true;
	char text[2 * XR_UUID_SIZE_EXT + 1] = { };
	float saved_height = 0.0f;
	bool const parsed = (std::fscanf(file, "%32s %f", text, &saved_height) == 2) && uuid_from_string(text, m_saved_uuid);
	std::fclose(file);
	if (!parsed)
		return true;
	height = saved_height;

	XrSpaceStorageLocationFilterInfoFB location{ XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB };
	location.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
	XrSpaceUuidFilterInfoFB filter{ XR_TYPE_SPACE_UUID_FILTER_INFO_FB };
	filter.next = &location;
	filter.uuidCount = 1;
	filter.uuids = &m_saved_uuid;
	XrSpaceQueryInfoFB query{ XR_TYPE_SPACE_QUERY_INFO_FB };
	query.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
	query.maxResultCount = 1;
	query.filter = reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB const *>(&filter);
	XrResult const result = m_xrQuerySpacesFB(m_session, reinterpret_cast<XrSpaceQueryInfoBaseHeaderFB const *>(&query), &m_query);
	if (XR_FAILED(result))
		XRERR("Screen anchor query failed: %d", result);
	else
		XRLOG("Looking for screen anchor %s", text);
	return true;
}

void screen_anchor::shutdown()
{
	if (m_anchor != XR_NULL_HANDLE)
		xrDestroySpace(m_anchor);
	m_anchor = XR_NULL_HANDLE;
	for (XrSpace space : m_retired)
		xrDestroySpace(space);
	for (XrSpace space : m_erasing)
		xrDestroySpace(space);
	m_retired.clear();
	m_erasing.clear();
}

bool screen_anchor::handle_event(XrEventDataBuffer const &event)
{
	switch (event.type)
	{
	case XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB:
		{
			auto const &available = reinterpret_cast<XrEventDataSpaceQueryResultsAvailableFB const &>(event);
			if (available.requestId != m_query)
				return false;
			XrSpaceQueryResultFB found[1];
			XrSpaceQueryResultsFB results{ XR_TYPE_SPACE_QUERY_RESULTS_FB };
			results.resultCapacityInput = uint32_t(std::size(found));
			results.results = found;
			if (XR_FAILED(m_xrRetrieveSpaceQueryResultsFB(m_session, m_query, &results)) || !results.resultCountOutput)
				return true;
			if (m_placed)
			{
				// the screen was placed again before the old anchor turned up
				m_retired.push_back(found[0].space);
				return true;
			}
			m_anchor = found[0].space;
			enable_component(m_anchor, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB);
			XRLOG("Screen anchor %s loaded", uuid_to_string(found[0].uuid).c_str());
		}
		return true;

	case XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB:
		{
			auto const &complete = reinterpret_cast<XrEventDataSpaceQueryCompleteFB const &>(event);
			if (complete.requestId != m_query)
				return false;
			if (m_anchor == XR_NULL_HANDLE)
				XRLOG("Saved screen anchor not found (%d)", complete.result);
		}
		return true;

	case XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB:
		{
			auto const &created = reinterpret_cast<XrEventDataSpatialAnchorCreateCompleteFB const &>(event);
			if (XR_FAILED(created.result))
			{
				XRERR("Screen anchor creation failed: %d", created.result);
				return true;
			}
			if (m_anchor != XR_NULL_HANDLE)
				m_retired.push_back(m_anchor);
			m_anchor = created.space;
			XRLOG("Screen anchor created");
			m_height = m_pending_height;
			if (enable_component(m_anchor, XR_SPACE_COMPONENT_TYPE_STORABLE_FB) == status::READY)
				save();
		}
		return true;

	case XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB:
		{
			auto const &set = reinterpret_cast<XrEventDataSpaceSetStatusCompleteFB const &>(event);
			if (set.space == m_anchor && set.componentType == XR_SPACE_COMPONENT_TYPE_STORABLE_FB)
			{
				if (XR_SUCCEEDED(set.result))
					save();
				else
					XRERR("Screen anchor not storable: %d", set.result);
			}
		}
		return true;

	case XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB:
		{
			auto const &saved = reinterpret_cast<XrEventDataSpaceSaveCompleteFB const &>(event);
			if (saved.space != m_anchor)
				return true;
			if (XR_FAILED(saved.result))
			{
				XRERR("Screen anchor save failed: %d", saved.result);
				return true;
			}
			write_file(saved.uuid);
			XRLOG("Screen anchor %s saved", uuid_to_string(saved.uuid).c_str());
			for (XrSpace space : m_retired)
				retire(space);
			m_retired.clear();
		}
		return true;

	case XR_TYPE_EVENT_DATA_SPACE_ERASE_COMPLETE_FB:
		{
			auto const &erased = reinterpret_cast<XrEventDataSpaceEraseCompleteFB const &>(event);
			auto const it = std::find(m_erasing.begin(), m_erasing.end(), erased.space);
			if (it != m_erasing.end())
			{
				xrDestroySpace(*it);
				m_erasing.erase(it);
			}
		}
		return true;

	default:
		return false;
	}
}

void screen_anchor::place(XrSpace base, XrPosef const &pose, XrTime time, float height)
{
	// stop following the old anchor right away so the screen stays where it
	// was put while the new one is created
	if (m_anchor != XR_NULL_HANDLE)
		m_retired.push_back(m_anchor);
	m_anchor = XR_NULL_HANDLE;
	m_placed = true;

	XrSpatialAnchorCreateInfoFB info{ XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB };
	info.space = base;
	info.poseInSpace = pose;
	info.time = time;
	XrAsyncRequestIdFB request = 0;
	XrResult const result = m_xrCreateSpatialAnchorFB(m_session, &info, &request);
	if (XR_FAILED(result))
		XRERR("xrCreateSpatialAnchorFB failed: %d", result);
	else
		m_pending_height = height;
}

bool screen_anchor::locate(XrSpace base, XrTime time, XrPosef &pose) const
{
	if (m_anchor == XR_NULL_HANDLE)
		return false;
	XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
	XrSpaceLocationFlags const needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(xrLocateSpace(m_anchor, base, time, &location)) || ((location.locationFlags & needed) != needed))
		return false;
	pose = location.pose;
	return true;
}

screen_anchor::status screen_anchor::enable_component(XrSpace space, XrSpaceComponentTypeFB component)
{
	XrSpaceComponentStatusSetInfoFB info{ XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB };
	info.componentType = component;
	info.enabled = XR_TRUE;
	XrAsyncRequestIdFB request = 0;
	XrResult const result = m_xrSetSpaceComponentStatusFB(space, &info, &request);
	if (result == XR_ERROR_SPACE_COMPONENT_STATUS_ALREADY_SET_FB)
		return status::READY;
	if (XR_FAILED(result))
	{
		XRERR("xrSetSpaceComponentStatusFB(%d) failed: %d", component, result);
		return status::FAILED;
	}
	return status::PENDING;
}

void screen_anchor::save()
{
	XrSpaceSaveInfoFB info{ XR_TYPE_SPACE_SAVE_INFO_FB };
	info.space = m_anchor;
	info.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
	info.persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB;
	XrAsyncRequestIdFB request = 0;
	XrResult const result = m_xrSaveSpaceFB(m_session, &info, &request);
	if (XR_FAILED(result))
		XRERR("xrSaveSpaceFB failed: %d", result);
}

void screen_anchor::retire(XrSpace space)
{
	XrSpaceEraseInfoFB info{ XR_TYPE_SPACE_ERASE_INFO_FB };
	info.space = space;
	info.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
	XrAsyncRequestIdFB request = 0;
	if (XR_SUCCEEDED(m_xrEraseSpaceFB(m_session, &info, &request)))
		m_erasing.push_back(space);
	else
		xrDestroySpace(space);
}

void screen_anchor::write_file(XrUuidEXT const &uuid) const
{
	std::FILE *const file = std::fopen(m_path.c_str(), "w");
	if (!file)
	{
		XRERR("Can't write %s", m_path.c_str());
		return;
	}
	std::fprintf(file, "%s %.3f\n", uuid_to_string(uuid).c_str(), m_height);
	std::fclose(file);
}

} // namespace osd::quest
