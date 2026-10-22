// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  drawquest.cpp - Meta Quest renderer
//
//  Rasterises the primitive list in software and hands the frame to a
//  display: on Quest the OpenXR thread, which scales it onto the screen on
//  the GPU; elsewhere the MAME window, so the same frames can be streamed
//  to network guests while developing without a headset. While only
//  the game screen is showing, frames go out at the game's own resolution
//  so the GPU can upscale them (with an optional CRT look). With menus,
//  artwork or vectors on screen they are drawn at an integer multiple of
//  the native size matched to how big the screen looks, keeping text and
//  vector lines sharp.
//
//============================================================

#include "render_module.h"

#include "modules/osdmodule.h"

#if defined(OSD_SDL)

#include "questdisplay.h"
#include "questnet.h"

#include "modules/osdwindow.h"
#include "osdcore.h"

// emu
#include "emucore.h"
#include "render.h"
#include "rendersw.hxx"

#if defined(SDLMAME_ANDROID)
#include "questxr.h"

#include <android/log.h>
#endif

#include <SDL2/SDL.h>

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

	virtual int create() override;
	virtual render_primitive_list *get_primitives() override;
	virtual int draw(const int update) override;

private:
	int pick_scale(int edge);
	void classify(render_primitive_list &primlist);

	std::unique_ptr<quest::frame_display> m_display;

	int m_width = 0;
	int m_height = 0;
	int m_scale = 0;
	int m_edge = 0;
	float m_aspect = 1.0f;

	// whether the last frame held nothing but one raster game screen
	bool m_native = false;
	bool m_vertical = false;

	std::chrono::steady_clock::duration m_draw_time{ };
	int m_draw_count = 0;
};

int renderer_quest::create()
{
#if defined(SDLMAME_ANDROID)
	m_display = quest::make_xr_display();
#else
	m_display = quest::make_window_display(static_cast<osd_window_t<SDL_Window *> &>(window()).platform_window());
#endif
	return 0;
}

int renderer_quest::pick_scale(int edge)
{
	int const pixels = m_display->screen_pixels();
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

void renderer_quest::classify(render_primitive_list &primlist)
{
	int screens = 0;
	bool others = false;
	for (render_primitive const *prim = primlist.first(); prim; prim = prim->next())
	{
		bool const raster_screen = (prim->type == render_primitive::QUAD) && prim->texture.base &&
				(prim->flags & PRIMFLAG_SCREENTEX_MASK) && !(prim->flags & (PRIMFLAG_VECTOR_MASK | PRIMFLAG_VECTORBUF_MASK));
		if (raster_screen)
		{
			++screens;
			m_vertical = (PRIMFLAG_GET_TEXORIENT(prim->flags) & ORIENTATION_SWAP_XY) != 0;
		}
		else
		{
			others = true;
		}
	}
	m_native = (screens == 1) && !others;
}

render_primitive_list *renderer_quest::get_primitives()
{
	render_target &target = *window().target();

	s32 minwidth, minheight;
	target.compute_minimum_size(minwidth, minheight);
	minwidth = std::max(minwidth, 1);
	minheight = std::max(minheight, 1);

	// shape of the layout on screen, which the quad takes
	s32 shape_width, shape_height;
	target.compute_visible_area(4096, 4096, 1.0f, target.orientation(), shape_width, shape_height);
	m_aspect = float(std::max(shape_width, 1)) / float(std::max(shape_height, 1));

	s32 visible_width, visible_height;
	if (m_native)
	{
		// one texel per game pixel: describe pixels as stretched however far
		// it takes for the native size to fill the layout exactly
		float const pixel_aspect = m_aspect * float(minheight) / float(minwidth);
		target.compute_visible_area(minwidth, minheight, pixel_aspect, target.orientation(), visible_width, visible_height);
		m_width = std::max(visible_width, 1);
		m_height = std::max(visible_height, 1);
		target.set_bounds(m_width, m_height, pixel_aspect);
	}
	else
	{
		// scale by a whole number so every source pixel covers the same area,
		// then trim to the area the layout fills
		int const scale = m_scale = pick_scale(std::max(minwidth, minheight));
		target.compute_visible_area(minwidth * scale, minheight * scale, 1.0f, target.orientation(), visible_width, visible_height);
		m_width = std::max(visible_width, 1);
		m_height = std::max(visible_height, 1);
		target.set_bounds(m_width, m_height, 1.0f);
	}
	return &target.get_primitives();
}

int renderer_quest::draw(const int update)
{
	auto const start = std::chrono::steady_clock::now();
	bool const native = m_native;
	uint32_t *const pixels = m_display->begin_frame(m_width, m_height);
	std::fill_n(pixels, size_t(m_width) * size_t(m_height), 0);

	window().m_primlist->acquire_lock();
	software_renderer<uint32_t, 0,0,0, 0,8,16>::draw_primitives(*window().m_primlist, pixels, m_width, m_height, m_width);
	classify(*window().m_primlist);
	window().m_primlist->release_lock();

	quest::net::host_frame(pixels, m_width, m_height, m_aspect, native, m_vertical);
	m_display->end_frame(m_aspect, native, m_vertical);

	m_draw_time += std::chrono::steady_clock::now() - start;
	if (++m_draw_count == TIMING_INTERVAL)
	{
		double const ms = std::chrono::duration<double, std::milli>(m_draw_time).count() / m_draw_count;
#if defined(SDLMAME_ANDROID)
		__android_log_print(ANDROID_LOG_INFO, "MAMEQuest", "Renderer %dx%d (%s), %.2f ms per frame", m_width, m_height, native ? "native" : "scaled", ms);
#else
		osd_printf_verbose("Renderer %dx%d (%s), %.2f ms per frame\n", m_width, m_height, native ? "native" : "scaled", ms);
#endif
		m_draw_time = { };
		m_draw_count = 0;
	}
	return 0;
}


class video_quest : public osd_module, public render_module
{
public:
	video_quest() : osd_module(OSD_RENDERER_PROVIDER, "quest") { }

#if defined(SDLMAME_ANDROID)
	virtual int init(osd_interface &osd, osd_options const &options) override { return quest::xr_start() ? 0 : -1; }
#else
	virtual int init(osd_interface &osd, osd_options const &options) override { return 0; }
#endif
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

#else // defined(OSD_SDL)

namespace osd { namespace { MODULE_NOT_SUPPORTED(video_quest, OSD_RENDERER_PROVIDER, "quest") } }

#endif // defined(OSD_SDL)


MODULE_DEFINITION(RENDERER_QUEST, osd::video_quest)
