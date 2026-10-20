// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  drawquest.cpp - Meta Quest renderer
//
//  Rasterises the primitive list in software at an integer multiple
//  of the game's native resolution and hands the frame to the OpenXR
//  thread, which shows it on a quad layer. The multiple follows how
//  many display pixels the screen covers, so a big or close screen gets
//  more detail (mostly visible in vector games).
//
//============================================================

#include "render_module.h"

#include "modules/osdmodule.h"

#if defined(SDLMAME_ANDROID)

#include "questxr.h"

#include "modules/osdwindow.h"

// emu
#include "emucore.h"
#include "render.h"
#include "rendersw.hxx"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <memory>


namespace osd {

namespace {

// longest edge to aim for until the XR thread knows the screen's size, and
// limits on it: software rendering cost grows with the pixel count
constexpr int DEFAULT_EDGE = 1024;
constexpr int MIN_EDGE = 512;
constexpr int MAX_EDGE = 2048;

// how far past the halfway point between scales the target must move before
// the scale changes, so small head movements don't flip it back and forth
constexpr float SCALE_HYSTERESIS = 0.15f;

// frames between render timing reports in the log
constexpr int TIMING_INTERVAL = 600;


class renderer_quest : public osd_renderer
{
public:
	renderer_quest(osd_window &window) : osd_renderer(window) { }

	virtual int create() override { return 0; }
	virtual render_primitive_list *get_primitives() override;
	virtual int draw(const int update) override;

private:
	int pick_scale(int edge);

	int m_width = 0;
	int m_height = 0;
	int m_scale = 0;
	int m_edge = 0;

	std::chrono::steady_clock::duration m_draw_time{ };
	int m_draw_count = 0;
};

int renderer_quest::pick_scale(int edge)
{
	int const pixels = quest::xr_screen_pixels();
	int const target = std::clamp(pixels ? pixels : DEFAULT_EDGE, MIN_EDGE, MAX_EDGE);
	float const wanted = float(target) / float(edge);
	int const max_scale = std::max(1, MAX_EDGE / edge);
	if (edge != m_edge)
	{
		m_edge = edge;
		m_scale = 0;
	}
	if (m_scale > 0 && std::abs(wanted - float(m_scale)) < 0.5f + SCALE_HYSTERESIS)
		return std::min(m_scale, max_scale);
	return std::clamp(int(wanted + 0.5f), 1, max_scale);
}

render_primitive_list *renderer_quest::get_primitives()
{
	render_target &target = *window().target();

	// scale by a whole number so every source pixel covers the same area
	s32 minwidth, minheight;
	target.compute_minimum_size(minwidth, minheight);
	int const edge = std::max(std::max(minwidth, minheight), 1);
	int const scale = m_scale = pick_scale(edge);

	// trim to the area the layout fills, so the quad takes the game's aspect ratio
	s32 visible_width, visible_height;
	target.compute_visible_area(minwidth * scale, minheight * scale, 1.0f, target.orientation(), visible_width, visible_height);
	m_width = std::max(visible_width, 1);
	m_height = std::max(visible_height, 1);

	target.set_bounds(m_width, m_height, 1.0f);
	return &target.get_primitives();
}

int renderer_quest::draw(const int update)
{
	auto const start = std::chrono::steady_clock::now();
	uint32_t *const pixels = quest::xr_begin_frame(m_width, m_height);
	std::fill_n(pixels, size_t(m_width) * size_t(m_height), 0);

	window().m_primlist->acquire_lock();
	software_renderer<uint32_t, 0,0,0, 0,8,16>::draw_primitives(*window().m_primlist, pixels, m_width, m_height, m_width);
	window().m_primlist->release_lock();

	quest::xr_end_frame();

	m_draw_time += std::chrono::steady_clock::now() - start;
	if (++m_draw_count == TIMING_INTERVAL)
	{
		__android_log_print(ANDROID_LOG_INFO, "MAMEQuest", "Renderer %dx%d (x%d), %.2f ms per frame", m_width, m_height, m_scale,
				std::chrono::duration<double, std::milli>(m_draw_time).count() / m_draw_count);
		m_draw_time = { };
		m_draw_count = 0;
	}
	return 0;
}


class video_quest : public osd_module, public render_module
{
public:
	video_quest() : osd_module(OSD_RENDERER_PROVIDER, "quest") { }

	virtual int init(osd_interface &osd, osd_options const &options) override { return quest::xr_start() ? 0 : -1; }
	virtual void exit() override { }

	virtual std::unique_ptr<osd_renderer> create(osd_window &window) override
	{
		return std::make_unique<renderer_quest>(window);
	}

protected:
	virtual unsigned flags() const override { return FLAG_INTERACTIVE; }
};

} // anonymous namespace

} // namespace osd

#else // defined(SDLMAME_ANDROID)

namespace osd { namespace { MODULE_NOT_SUPPORTED(video_quest, OSD_RENDERER_PROVIDER, "quest") } }

#endif // defined(SDLMAME_ANDROID)


MODULE_DEFINITION(RENDERER_QUEST, osd::video_quest)
