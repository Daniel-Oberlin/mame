// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questguest.cpp - joining a game shared by another machine
//
//  The guest doesn't run MAME: it shows the host's frames, plays its
//  sound and sends back the state of the first game controller (on a
//  desktop also the keyboard).
//
//============================================================

#include "questnet.h"
#include "questdisplay.h"
#include "questnetlog.h"
#include "questnetproto.h"

#if defined(SDLMAME_ANDROID)
#include "questxr.h"
#endif

#include <SDL2/SDL.h>

#include <zstd.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>


namespace osd::quest::net {

namespace {

using clock = std::chrono::steady_clock;

// controller state is sent whenever it changes, and at least this often
// so a lost packet is soon replaced
constexpr auto INPUT_REFRESH = std::chrono::milliseconds(50);

// sound buffered before playback starts, and the most allowed to build up
// before incoming sound is dropped to catch up
constexpr int AUDIO_START_MS = 40;
constexpr int AUDIO_MAX_MS = 80;

// how long a scripted press holds its button
constexpr double PRESS_SECONDS = 0.2;

constexpr int SNAPSHOT_INTERVAL = 120;
constexpr auto STATS_INTERVAL = std::chrono::seconds(5);

// how long a notice such as the game's name stays over the picture
constexpr auto NOTICE_TIME = std::chrono::seconds(4);


// a copy of a frame, for saving to disk
struct frame_copy
{
	std::vector<uint32_t> pixels;
	int width = 0;
	int height = 0;
};


//============================================================
//  notices over the picture
//============================================================

// 5x7 capitals, digits and the punctuation in game names; a row per byte,
// leftmost pixel in bit 4
struct glyph
{
	char c;
	uint8_t rows[7];
};

glyph const FONT[] = {
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
	{ 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
	{ 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
	{ 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
	{ '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
	{ ',', { 0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
	{ '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
	{ ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
	{ '\'', { 0x0c, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
	{ ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
	{ '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
	{ '&', { 0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d } },
	{ '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
	{ '?', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
	{ '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } } };

glyph const *find_glyph(char c)
{
	c = char(std::toupper(static_cast<unsigned char>(c)));
	for (glyph const &g : FONT)
		if (g.c == c)
			return &g;
	return nullptr;    // drawn as a space
}

// Writes a line of white text on a darkened band across the lower part of
// the picture, sized to the picture so it reads the same at any resolution.
void draw_notice(uint32_t *pixels, int width, int height, std::string const &text)
{
	int const scale = std::max(1, height / 300);
	int const cell = 6 * scale;
	int const pad = 2 * scale;
	size_t const fits = size_t(std::max(0, (width - 2 * pad) / cell));
	std::string const shown = text.substr(0, fits);
	if (shown.empty() || (height < 7 * scale + 2 * pad))
		return;

	int const band_height = 7 * scale + 2 * pad;
	int const top = std::min(height * 3 / 4, height - band_height);
	for (int y = top; y < top + band_height; y++)
		for (int x = 0; x < width; x++)
			pixels[size_t(y) * width + x] = (pixels[size_t(y) * width + x] >> 2) & 0x3f3f3f3f;

	int const left = (width - int(shown.size()) * cell + scale) / 2;
	for (size_t i = 0; i < shown.size(); i++)
	{
		glyph const *const g = find_glyph(shown[i]);
		if (!g)
			continue;
		int const x0 = left + int(i) * cell;
		for (int row = 0; row < 7; row++)
			for (int col = 0; col < 5; col++)
				if (g->rows[row] & (0x10 >> col))
					for (int dy = 0; dy < scale; dy++)
						std::fill_n(&pixels[size_t(top + pad + row * scale + dy) * width + x0 + col * scale], scale, 0xffffffffU);
	}
}


//============================================================
//  connection to the host
//============================================================

class host_link
{
public:
	~host_link();

	bool connect(std::string const &address, std::string &error);
	int player() const { return m_player; }
	bool alive() const { return m_alive; }

	// copies the newest frame into the display if one arrived since the last
	// call, with the notice, if any, written over it
	bool present(frame_display &display, std::string const &notice, frame_copy *copy);

	// the game's name when the host says it has changed; empty while choosing
	bool take_game(std::string &name);

	// moves received sound into an SDL queue, opening the device on first use
	void play_audio();

	void send_input(pad_state const &state);

	// totals since the last call, for the stats line
	void take_stats(int &frames, size_t &bytes);

	// sound waiting to be played
	int queued_audio_ms() const;

private:
	void receive();
	bool receive_frame(uint32_t size);
	bool receive_audio(uint32_t size);
	bool receive_game(uint32_t size);

	int m_socket = -1;
	int m_input = -1;
	sockaddr_in m_input_address{ };
	uint32_t m_input_sequence = 0;
	int m_player = 0;
	std::atomic<bool> m_alive{ false };
	std::thread m_thread;

	// receive thread only
	std::vector<uint8_t> m_compressed;
	std::vector<uint32_t> m_decoded;
	std::vector<uint32_t> m_current;
	int m_current_width = 0;
	int m_current_height = 0;
	ZSTD_DCtx *m_zstd = nullptr;

	std::mutex m_mutex;
	std::vector<uint32_t> m_latest;
	int m_latest_width = 0;
	int m_latest_height = 0;
	float m_latest_aspect = 1.0f;
	uint8_t m_latest_flags = 0;
	bool m_fresh = false;
	int m_frames_received = 0;
	size_t m_bytes_received = 0;

	std::string m_game;
	bool m_game_changed = false;

	std::deque<std::vector<int16_t>> m_audio;
	int m_audio_rate = 0;
	int m_audio_channels = 0;

	// main thread only
	SDL_AudioDeviceID m_device = 0;
	int m_device_rate = 0;
	int m_device_channels = 0;
	bool m_playing = false;
};

host_link::~host_link()
{
	close_socket(m_socket);
	if (m_thread.joinable())
		m_thread.join();
	close_socket(m_input);
	if (m_device)
		SDL_CloseAudioDevice(m_device);
	if (m_zstd)
		ZSTD_freeDCtx(m_zstd);
}

bool host_link::connect(std::string const &address, std::string &error)
{
	sockaddr_in host{ };
	host.sin_family = AF_INET;
	host.sin_port = htons(STREAM_PORT);
	if (inet_pton(AF_INET, address.c_str(), &host.sin_addr) != 1)
	{
		error = "not an IPv4 address: " + address;
		return false;
	}

	m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
	int const yes = 1;
	setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#if defined(SO_NOSIGPIPE)
	setsockopt(m_socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
	if ((m_socket < 0) || ::connect(m_socket, reinterpret_cast<sockaddr *>(&host), sizeof(host)))
	{
		error = std::string("could not connect: ") + std::strerror(errno);
		close_socket(m_socket);
		return false;
	}

	hello_message const hello{ MAGIC, PROTOCOL_VERSION };
	message_header header;
	if (!send_message(m_socket, message::HELLO, &hello, sizeof(hello)) || !receive_all(m_socket, &header, sizeof(header)))
	{
		error = "the host hung up";
		close_socket(m_socket);
		return false;
	}
	if ((header.type == message::REFUSED) && (header.size == sizeof(refused_message)))
	{
		refused_message refused;
		receive_all(m_socket, &refused, sizeof(refused));
		error = (refused.reason == refusal::FULL) ? "the game is full" : "the host runs a different version";
		close_socket(m_socket);
		return false;
	}
	welcome_message welcome;
	if ((header.type != message::WELCOME) || (header.size != sizeof(welcome)) || !receive_all(m_socket, &welcome, sizeof(welcome)))
	{
		error = "unexpected reply from the host";
		close_socket(m_socket);
		return false;
	}

	m_player = int(welcome.player);
	m_input = ::socket(AF_INET, SOCK_DGRAM, 0);
	m_input_address = host;
	m_zstd = ZSTD_createDCtx();
	m_alive = true;
	m_thread = std::thread([this] () { receive(); });
	return true;
}

void host_link::receive()
{
	while (true)
	{
		message_header header;
		if (!receive_all(m_socket, &header, sizeof(header)))
			break;
		bool ok;
		switch (header.type)
		{
		case message::FRAME:
			ok = receive_frame(header.size);
			break;
		case message::AUDIO:
			ok = receive_audio(header.size);
			break;
		case message::GAME:
			ok = receive_game(header.size);
			break;
		default:
			m_compressed.resize(header.size);
			ok = receive_all(m_socket, m_compressed.data(), header.size);
			break;
		}
		if (!ok)
			break;
	}
	m_alive = false;
}

bool host_link::receive_frame(uint32_t size)
{
	frame_message frame;
	if ((size < sizeof(frame)) || !receive_all(m_socket, &frame, sizeof(frame)))
		return false;
	m_compressed.resize(size - sizeof(frame));
	if (!receive_all(m_socket, m_compressed.data(), m_compressed.size()))
		return false;

	size_t const count = size_t(frame.width) * size_t(frame.height);
	bool const key = frame.flags & FRAME_KEY;
	if (!key && ((frame.width != m_current_width) || (frame.height != m_current_height)))
	{
		NETERR("Frame %u is a change to a picture we don't have", frame.sequence);
		return false;
	}
	m_decoded.resize(count);
	size_t const decoded = ZSTD_decompressDCtx(m_zstd, m_decoded.data(), count * sizeof(uint32_t), m_compressed.data(), m_compressed.size());
	if (ZSTD_isError(decoded) || (decoded != count * sizeof(uint32_t)))
	{
		NETERR("Frame %u could not be decoded", frame.sequence);
		return false;
	}
	if (key)
	{
		m_current.swap(m_decoded);
		m_current_width = frame.width;
		m_current_height = frame.height;
	}
	else
	{
		for (size_t i = 0; i < count; i++)
			m_current[i] ^= m_decoded[i];
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_latest = m_current;
	m_latest_width = m_current_width;
	m_latest_height = m_current_height;
	m_latest_aspect = frame.aspect;
	m_latest_flags = frame.flags;
	m_fresh = true;
	m_frames_received++;
	m_bytes_received += sizeof(message_header) + size;
	return true;
}

bool host_link::receive_audio(uint32_t size)
{
	audio_message audio;
	if ((size < sizeof(audio)) || !receive_all(m_socket, &audio, sizeof(audio)))
		return false;
	std::vector<int16_t> samples((size - sizeof(audio)) / sizeof(int16_t));
	if (!receive_all(m_socket, samples.data(), size - sizeof(audio)))
		return false;

	std::lock_guard<std::mutex> lock(m_mutex);
	m_audio_rate = int(audio.rate);
	m_audio_channels = audio.channels;
	m_audio.emplace_back(std::move(samples));
	m_bytes_received += sizeof(message_header) + size;
	return true;
}

bool host_link::receive_game(uint32_t size)
{
	std::string name(size, '\0');
	if (!receive_all(m_socket, name.data(), size))
		return false;

	std::lock_guard<std::mutex> lock(m_mutex);
	m_game = std::move(name);
	m_game_changed = true;
	return true;
}

bool host_link::take_game(std::string &name)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_game_changed)
		return false;
	m_game_changed = false;
	name = m_game;
	return true;
}

bool host_link::present(frame_display &display, std::string const &notice, frame_copy *copy)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_fresh)
		return false;
	m_fresh = false;
	uint32_t *const pixels = display.begin_frame(m_latest_width, m_latest_height);
	std::copy(m_latest.begin(), m_latest.end(), pixels);
	if (!notice.empty())
		draw_notice(pixels, m_latest_width, m_latest_height, notice);
	if (copy)
	{
		copy->pixels.assign(pixels, pixels + m_latest.size());
		copy->width = m_latest_width;
		copy->height = m_latest_height;
	}
	display.end_frame(m_latest_aspect, m_latest_flags & FRAME_NATIVE, m_latest_flags & FRAME_VERTICAL);
	return true;
}

void host_link::play_audio()
{
	std::deque<std::vector<int16_t>> audio;
	int rate, channels;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		audio.swap(m_audio);
		rate = m_audio_rate;
		channels = m_audio_channels;
	}
	if (audio.empty())
		return;

	if (!m_device || (rate != m_device_rate) || (channels != m_device_channels))
	{
		if (m_device)
			SDL_CloseAudioDevice(m_device);
		SDL_AudioSpec want{ }, have{ };
		want.freq = rate;
		want.format = AUDIO_S16SYS;
		want.channels = uint8_t(channels);
		want.samples = 512;
		m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
		m_device_rate = rate;
		m_device_channels = channels;
		m_playing = false;
		if (!m_device)
		{
			NETERR("Could not open audio: %s", SDL_GetError());
			return;
		}
	}

	size_t const bytes_per_ms = size_t(rate) * size_t(channels) * sizeof(int16_t) / 1000;
	for (auto const &chunk : audio)
	{
		// drop sound rather than let it lag ever further behind the picture
		if (SDL_GetQueuedAudioSize(m_device) > AUDIO_MAX_MS * bytes_per_ms)
			continue;
		SDL_QueueAudio(m_device, chunk.data(), uint32_t(chunk.size() * sizeof(int16_t)));
	}
	if (!m_playing && (SDL_GetQueuedAudioSize(m_device) >= AUDIO_START_MS * bytes_per_ms))
	{
		SDL_PauseAudioDevice(m_device, 0);
		m_playing = true;
	}
}

void host_link::send_input(pad_state const &state)
{
	input_packet packet{ };
	packet.magic = MAGIC;
	packet.version = PROTOCOL_VERSION;
	packet.player = uint32_t(m_player);
	packet.sequence = ++m_input_sequence;
	packet.buttons = state.buttons;
	std::copy(std::begin(state.axes), std::end(state.axes), packet.axes);
	::sendto(m_input, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr const *>(&m_input_address), sizeof(m_input_address));
}

int host_link::queued_audio_ms() const
{
	if (!m_device)
		return 0;
	size_t const bytes_per_ms = size_t(m_device_rate) * size_t(m_device_channels) * sizeof(int16_t) / 1000;
	return int(SDL_GetQueuedAudioSize(m_device) / std::max<size_t>(bytes_per_ms, 1));
}

void host_link::take_stats(int &frames, size_t &bytes)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	frames = m_frames_received;
	bytes = m_bytes_received;
	m_frames_received = 0;
	m_bytes_received = 0;
}


//============================================================
//  input
//============================================================

struct scripted_press
{
	double seconds;
	SDL_GameControllerButton button;
};

std::vector<scripted_press> parse_presses(std::string const &text)
{
	std::vector<scripted_press> presses;
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find(',', start);
		if (end == std::string::npos)
			end = text.size();
		std::string const item = text.substr(start, end - start);
		size_t const colon = item.find(':');
		if (colon != std::string::npos)
		{
			SDL_GameControllerButton const button = SDL_GameControllerGetButtonFromString(item.substr(colon + 1).c_str());
			if (button != SDL_CONTROLLER_BUTTON_INVALID)
				presses.push_back({ std::atof(item.substr(0, colon).c_str()), button });
			else
				NETERR("Unknown button in %s", item.c_str());
		}
		start = end + 1;
	}
	return presses;
}

class input_reader
{
public:
	input_reader(std::string const &presses) : m_presses(parse_presses(presses)) { }
	~input_reader() { if (m_controller) SDL_GameControllerClose(m_controller); }

	void handle_event(SDL_Event const &event);
	pad_state read(double seconds);

private:
	void open_controller();

	SDL_GameController *m_controller = nullptr;
	std::vector<scripted_press> const m_presses;
};

// Opens the first game controller, preferring the Touch controllers' pad
// on Quest. That pad can be attached before we start listening for
// devices, so look rather than wait for an event.
void input_reader::open_controller()
{
	int chosen = -1;
	for (int index = 0; index < SDL_NumJoysticks(); index++)
	{
		if (!SDL_IsGameController(index))
			continue;
		char const *const name = SDL_GameControllerNameForIndex(index);
		if ((chosen < 0) || (name && !std::strcmp(name, "Quest Touch Controllers")))
			chosen = index;
	}
	if (chosen < 0)
		return;
	m_controller = SDL_GameControllerOpen(chosen);
	if (m_controller)
		NETLOG("Using controller %s", SDL_GameControllerName(m_controller));
}

void input_reader::handle_event(SDL_Event const &event)
{
	if ((event.type == SDL_CONTROLLERDEVICEREMOVED) && m_controller && (event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(m_controller))))
	{
		SDL_GameControllerClose(m_controller);
		m_controller = nullptr;
	}
}

pad_state input_reader::read(double seconds)
{
	pad_state state;
	if (!m_controller)
		open_controller();
	if (m_controller)
	{
		for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++)
			if (SDL_GameControllerGetButton(m_controller, SDL_GameControllerButton(button)))
				state.buttons |= 1U << button;
		for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++)
			state.axes[axis] = SDL_GameControllerGetAxis(m_controller, SDL_GameControllerAxis(axis));
	}

#if !defined(SDLMAME_ANDROID)
	// keyboard: arrows, Z X A S for buttons, 1 for start and 5 for coin as in MAME
	static constexpr std::pair<SDL_Scancode, SDL_GameControllerButton> KEYS[] = {
			{ SDL_SCANCODE_UP, SDL_CONTROLLER_BUTTON_DPAD_UP },
			{ SDL_SCANCODE_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
			{ SDL_SCANCODE_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
			{ SDL_SCANCODE_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
			{ SDL_SCANCODE_Z, SDL_CONTROLLER_BUTTON_A },
			{ SDL_SCANCODE_X, SDL_CONTROLLER_BUTTON_B },
			{ SDL_SCANCODE_A, SDL_CONTROLLER_BUTTON_X },
			{ SDL_SCANCODE_S, SDL_CONTROLLER_BUTTON_Y },
			{ SDL_SCANCODE_1, SDL_CONTROLLER_BUTTON_START },
			{ SDL_SCANCODE_5, SDL_CONTROLLER_BUTTON_BACK } };
	Uint8 const *const keys = SDL_GetKeyboardState(nullptr);
	for (auto const &[key, button] : KEYS)
		if (keys[key])
			state.buttons |= 1U << button;
#endif

	for (auto const &press : m_presses)
		if ((seconds >= press.seconds) && (seconds < press.seconds + PRESS_SECONDS))
			state.buttons |= 1U << press.button;
	return state;
}

void save_snapshot(std::string const &dir, int number, frame_copy &frame)
{
	SDL_Surface *const surface = SDL_CreateRGBSurfaceWithFormatFrom(frame.pixels.data(), frame.width, frame.height, 32, frame.width * 4, SDL_PIXELFORMAT_XBGR8888);
	if (!surface)
		return;
	char name[32];
	std::snprintf(name, sizeof(name), "/frame%05d.bmp", number);
	if (SDL_SaveBMP(surface, (dir + name).c_str()))
		NETERR("Could not save %s: %s", (dir + name).c_str(), SDL_GetError());
	SDL_FreeSurface(surface);
}

} // anonymous namespace


//============================================================
//  public interface
//============================================================

std::string find_host(std::chrono::milliseconds wait, std::string const &ignore)
{
	int listener = ::socket(AF_INET, SOCK_DGRAM, 0);
	int const yes = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#if defined(SO_REUSEPORT)
	setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
	sockaddr_in address{ };
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(DISCOVERY_PORT);
	if ((listener < 0) || ::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
	{
		NETERR("Could not listen for hosts: %s", std::strerror(errno));
		close_socket(listener);
		return std::string();
	}

	std::string found;
	auto const deadline = clock::now() + wait;
	while (found.empty() && (clock::now() < deadline))
	{
		auto const left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - clock::now()).count();
		timeval timeout{ time_t(left / 1000000), suseconds_t(std::max<long long>(left % 1000000, 1000)) };
		setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		advert_packet advert;
		sockaddr_in from{ };
		socklen_t length = sizeof(from);
		ssize_t const received = ::recvfrom(listener, &advert, sizeof(advert), 0, reinterpret_cast<sockaddr *>(&from), &length);
		if ((received == sizeof(advert)) && (advert.magic == MAGIC) && (advert.version == PROTOCOL_VERSION) && advert.free_seats && (address_string(from) != ignore))
			found = address_string(from);
	}
	close_socket(listener);
	return found;
}

bool parse_guest_options(std::vector<std::string> const &args, guest_options &options)
{
	auto const join = std::find(args.begin(), args.end(), "-join");
	if (join == args.end())
		return false;
	if ((join + 1 != args.end()) && ((join + 1)->rfind('-', 0) != 0))
		options.address = *(join + 1);
	for (auto arg = args.begin(); arg != args.end(); ++arg)
	{
		if (arg + 1 == args.end())
			break;
		if (*arg == "-snapshots")
			options.snapshot_dir = *(arg + 1);
		else if (*arg == "-press")
			options.presses = *(arg + 1);
		else if (*arg == "-seconds")
			options.seconds = std::atof((arg + 1)->c_str());
	}
	return true;
}

int guest_main(guest_options const &options)
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER))
	{
		NETERR("SDL init failed: %s", SDL_GetError());
		return 1;
	}

	std::string address = options.address;
	if (address.empty())
	{
		NETLOG("Looking for a host...");
		address = find_host(std::chrono::seconds(3));
		if (address.empty())
		{
			NETERR("No host found");
			return GUEST_HOST_GONE;
		}
	}

	host_link link;
	std::string error;
	if (!link.connect(address, error))
	{
		NETERR("Could not join %s: %s", address.c_str(), error.c_str());
		return GUEST_HOST_GONE;
	}
	NETLOG("Joined %s as player %d", address.c_str(), link.player());

#if defined(SDLMAME_ANDROID)
	if (!xr_start())
		return 1;
	std::unique_ptr<frame_display> display = make_xr_display();
#else
	std::string const title = "MAME4Quest - player " + std::to_string(link.player());
	SDL_Window *const window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!window)
	{
		NETERR("Could not open a window: %s", SDL_GetError());
		return 1;
	}
	std::unique_ptr<frame_display> display = make_window_display(window);
#endif

	input_reader input(options.presses);
	pad_state sent;
	auto const start = clock::now();
	auto last_sent = start;
	auto last_stats = start;
	int presented = 0;
	frame_copy snapshot;
	bool quit = false;

	std::string const player = "Player " + std::to_string(link.player());
	std::string notice = "Joined as " + player;
	auto notice_until = start + NOTICE_TIME;

	while (!quit && link.alive())
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			if ((event.type == SDL_QUIT) || ((event.type == SDL_KEYDOWN) && (event.key.keysym.sym == SDLK_ESCAPE)))
				quit = true;
			input.handle_event(event);
		}

		auto const now = clock::now();
		double const seconds = std::chrono::duration<double>(now - start).count();
		if ((options.seconds > 0.0) && (seconds >= options.seconds))
			break;

		pad_state const state = input.read(seconds);

		// holding the Quest's menu button leaves, as it leaves a game when hosting
		if (state.buttons & (1U << SDL_CONTROLLER_BUTTON_MISC1))
			break;
		if ((state != sent) || (now - last_sent >= INPUT_REFRESH))
		{
			link.send_input(state);
			sent = state;
			last_sent = now;
		}

		link.play_audio();

		std::string game;
		if (link.take_game(game))
		{
			notice = player + ": " + (game.empty() ? std::string("host is choosing a game") : game);
			notice_until = now + NOTICE_TIME;
		}
		if (!notice.empty() && (now >= notice_until))
			notice.clear();

		bool const want_snapshot = !options.snapshot_dir.empty() && ((presented % SNAPSHOT_INTERVAL) == 0);
		if (link.present(*display, notice, want_snapshot ? &snapshot : nullptr))
		{
			if (want_snapshot)
				save_snapshot(options.snapshot_dir, presented, snapshot);
			presented++;
		}

		if (now - last_stats >= STATS_INTERVAL)
		{
			int frames;
			size_t bytes;
			link.take_stats(frames, bytes);
			double const elapsed = std::chrono::duration<double>(now - last_stats).count();
			NETLOG("Received %.1f frames/s, %.0f kbit/s, %d ms of sound queued", frames / elapsed, bytes * 8.0 / 1000.0 / elapsed, link.queued_audio_ms());
			last_stats = now;
		}

		SDL_Delay(1);
	}

	bool const host_gone = !link.alive();
	if (host_gone)
		NETLOG("The host has gone");
	display.reset();
#if !defined(SDLMAME_ANDROID)
	SDL_DestroyWindow(window);
	SDL_Quit();
#endif
	// on Quest, SDL and the headset stay up in case MAME runs next
	return host_gone ? GUEST_HOST_GONE : 0;
}

} // namespace osd::quest::net
