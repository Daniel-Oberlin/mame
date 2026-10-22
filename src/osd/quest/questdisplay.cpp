// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questdisplay.cpp - where finished frames are shown
//
//============================================================

#include "questdisplay.h"

#if defined(SDLMAME_ANDROID)
#include "questxr.h"
#else
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>
#endif


namespace osd::quest {

namespace {

#if defined(SDLMAME_ANDROID)

class xr_display : public frame_display
{
public:
	virtual uint32_t *begin_frame(int width, int height) override { return xr_begin_frame(width, height); }
	virtual void end_frame(float aspect, bool native, bool vertical) override { xr_end_frame(aspect, native, vertical); }
	virtual int screen_pixels() override { return xr_screen_pixels(); }
};

#else // defined(SDLMAME_ANDROID)

class window_display : public frame_display
{
public:
	window_display(SDL_Window *window) : m_window(window) { }

	virtual ~window_display() override
	{
		if (m_texture)
			SDL_DestroyTexture(m_texture);
		if (m_renderer)
			SDL_DestroyRenderer(m_renderer);
	}

	virtual uint32_t *begin_frame(int width, int height) override
	{
		m_width = width;
		m_height = height;
		m_pixels.resize(size_t(width) * size_t(height));
		return m_pixels.data();
	}

	virtual void end_frame(float aspect, bool native, bool vertical) override;
	virtual int screen_pixels() override { return m_screen_pixels; }

private:
	SDL_Window *const m_window;
	SDL_Renderer *m_renderer = nullptr;
	SDL_Texture *m_texture = nullptr;
	int m_texture_width = 0;
	int m_texture_height = 0;

	std::vector<uint32_t> m_pixels;
	int m_width = 0;
	int m_height = 0;
	int m_screen_pixels = 0;
};

void window_display::end_frame(float aspect, bool native, bool)
{
	if (!m_renderer)
	{
		m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
		if (!m_renderer)
			return;
	}
	if (!m_texture || (m_texture_width != m_width) || (m_texture_height != m_height))
	{
		if (m_texture)
			SDL_DestroyTexture(m_texture);
		m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
		if (!m_texture)
			return;
		m_texture_width = m_width;
		m_texture_height = m_height;
	}
	SDL_UpdateTexture(m_texture, nullptr, m_pixels.data(), m_width * sizeof(uint32_t));

	// game pixels stay crisp; frames already drawn near window size are smoothed
	SDL_SetTextureScaleMode(m_texture, native ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);

	// largest rectangle of the picture's shape that fits the window
	int output_width, output_height;
	SDL_GetRendererOutputSize(m_renderer, &output_width, &output_height);
	SDL_Rect rect;
	rect.w = std::min(output_width, int(std::lround(output_height * aspect)));
	rect.h = std::min(output_height, int(std::lround(output_width / aspect)));
	rect.x = (output_width - rect.w) / 2;
	rect.y = (output_height - rect.h) / 2;
	m_screen_pixels = std::max(rect.w, rect.h);

	SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
	SDL_RenderClear(m_renderer);
	SDL_RenderCopy(m_renderer, m_texture, nullptr, &rect);
	SDL_RenderPresent(m_renderer);
}

#endif // defined(SDLMAME_ANDROID)

} // anonymous namespace


#if defined(SDLMAME_ANDROID)

std::unique_ptr<frame_display> make_xr_display()
{
	return std::make_unique<xr_display>();
}

#else

std::unique_ptr<frame_display> make_window_display(SDL_Window *window)
{
	return std::make_unique<window_display>(window);
}

#endif

} // namespace osd::quest
