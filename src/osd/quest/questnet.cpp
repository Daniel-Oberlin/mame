// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questnet.cpp - sharing one game with players on the local network
//
//  The host side: accepts guests, streams frames and sound to each on its
//  own thread, and feeds their controller state to SDL virtual gamepads.
//
//============================================================

#include "questnet.h"
#include "questnetlog.h"
#include "questnetproto.h"

#include <SDL2/SDL.h>

#include <zstd.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>


namespace osd::quest::net {

bool pad_state::operator==(pad_state const &that) const
{
	return (buttons == that.buttons) && std::equal(std::begin(axes), std::end(axes), std::begin(that.axes));
}


//============================================================
//  socket helpers
//============================================================

// a guest vanishing mid-send must not kill the process with SIGPIPE
#if defined(MSG_NOSIGNAL)
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0; // sockets get SO_NOSIGPIPE instead
#endif

bool send_all(int socket, void const *data, size_t size)
{
	auto const *bytes = static_cast<uint8_t const *>(data);
	while (size)
	{
		ssize_t const sent = ::send(socket, bytes, size, SEND_FLAGS);
		if (sent < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		bytes += sent;
		size -= size_t(sent);
	}
	return true;
}

bool receive_all(int socket, void *data, size_t size)
{
	auto *bytes = static_cast<uint8_t *>(data);
	while (size)
	{
		ssize_t const received = ::recv(socket, bytes, size, 0);
		if (received <= 0)
		{
			if ((received < 0) && (errno == EINTR))
				continue;
			return false;
		}
		bytes += received;
		size -= size_t(received);
	}
	return true;
}

bool send_message(int socket, message type, void const *body, size_t body_size, void const *extra, size_t extra_size)
{
	message_header const header{ type, uint32_t(body_size + extra_size) };
	return send_all(socket, &header, sizeof(header)) && send_all(socket, body, body_size) && (!extra_size || send_all(socket, extra, extra_size));
}

void close_socket(int &socket)
{
	if (socket >= 0)
	{
		::shutdown(socket, SHUT_RDWR);
		::close(socket);
		socket = -1;
	}
}

std::string address_string(sockaddr_in const &address)
{
	char text[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
	return text;
}


namespace {

//============================================================
//  host
//============================================================

// sound queued for a guest beyond this is dropped rather than letting the
// guest fall further behind
constexpr auto MAX_AUDIO_QUEUE = std::chrono::milliseconds(200);

struct shared_frame
{
	std::vector<uint32_t> pixels;
	uint32_t sequence;
	int width;
	int height;
	float aspect;
	uint8_t flags;
};


// one connected guest, with a thread that sends it frames and sound
class guest_link
{
public:
	guest_link(int socket, sockaddr_in const &address, int player);
	~guest_link();

	int player() const { return m_player; }
	in_addr_t address() const { return m_address.sin_addr.s_addr; }
	bool alive() const { return m_alive; }

	void post_frame(std::shared_ptr<shared_frame const> const &frame);
	void post_audio(int16_t const *samples, int frames, int channels, int rate);

	// newest controller packet seen, so late arrivals are ignored
	uint32_t m_input_sequence = 0;
	bool m_input_seen = false;

private:
	void run();
	bool send_frame(shared_frame const &frame);

	int m_socket;
	sockaddr_in const m_address;
	int const m_player;
	std::atomic<bool> m_alive{ true };
	std::thread m_thread;

	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::shared_ptr<shared_frame const> m_frame;       // latest not yet sent; older ones are skipped
	std::deque<std::vector<uint8_t>> m_audio;          // audio_message + samples, ready to send
	size_t m_audio_bytes = 0;

	// sender thread only
	std::vector<uint32_t> m_sent;     // last frame sent, which the guest XORs against
	int m_sent_width = 0;
	int m_sent_height = 0;
	std::vector<uint32_t> m_delta;
	std::vector<uint8_t> m_compressed;
	ZSTD_CCtx *m_zstd;
};

guest_link::guest_link(int socket, sockaddr_in const &address, int player) :
	m_socket(socket),
	m_address(address),
	m_player(player),
	m_zstd(ZSTD_createCCtx())
{
	m_thread = std::thread([this] () { run(); });
}

guest_link::~guest_link()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_alive = false;
	}
	m_wake.notify_all();
	m_thread.join();
	close_socket(m_socket);
	ZSTD_freeCCtx(m_zstd);
}

void guest_link::post_frame(std::shared_ptr<shared_frame const> const &frame)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_frame = frame;
	}
	m_wake.notify_one();
}

void guest_link::post_audio(int16_t const *samples, int frames, int channels, int rate)
{
	size_t const sample_bytes = size_t(frames) * size_t(channels) * sizeof(int16_t);
	size_t const limit = size_t(rate) * size_t(channels) * sizeof(int16_t) * MAX_AUDIO_QUEUE.count() / 1000;

	std::vector<uint8_t> packet(sizeof(audio_message) + sample_bytes);
	audio_message const header{ uint32_t(rate), uint16_t(channels), 0 };
	std::memcpy(packet.data(), &header, sizeof(header));
	std::memcpy(packet.data() + sizeof(header), samples, sample_bytes);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		while (!m_audio.empty() && (m_audio_bytes + sample_bytes > limit))
		{
			m_audio_bytes -= m_audio.front().size() - sizeof(audio_message);
			m_audio.pop_front();
		}
		m_audio_bytes += sample_bytes;
		m_audio.emplace_back(std::move(packet));
	}
	m_wake.notify_one();
}

void guest_link::run()
{
	while (true)
	{
		std::shared_ptr<shared_frame const> frame;
		std::deque<std::vector<uint8_t>> audio;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_wake.wait(lock, [this] () { return !m_alive || m_frame || !m_audio.empty(); });
			if (!m_alive)
				return;
			frame = std::move(m_frame);
			m_frame.reset();
			audio.swap(m_audio);
			m_audio_bytes = 0;
		}

		// sound first: it's small, and a gap is more noticeable than a late frame
		bool ok = true;
		for (auto const &packet : audio)
			ok = ok && send_message(m_socket, message::AUDIO, packet.data(), packet.size());
		if (ok && frame)
			ok = send_frame(*frame);
		if (!ok)
		{
			NETLOG("Player %d (%s) disconnected", m_player, address_string(m_address).c_str());
			m_alive = false;
			return;
		}
	}
}

bool guest_link::send_frame(shared_frame const &frame)
{
	size_t const count = frame.pixels.size();
	bool const key = (frame.width != m_sent_width) || (frame.height != m_sent_height);
	uint32_t const *source = frame.pixels.data();
	if (!key)
	{
		m_delta.resize(count);
		for (size_t i = 0; i < count; i++)
			m_delta[i] = frame.pixels[i] ^ m_sent[i];
		source = m_delta.data();
	}

	size_t const raw_size = count * sizeof(uint32_t);
	m_compressed.resize(ZSTD_compressBound(raw_size));
	size_t const compressed = ZSTD_compressCCtx(m_zstd, m_compressed.data(), m_compressed.size(), source, raw_size, 1);
	if (ZSTD_isError(compressed))
		return false;

	frame_message header{ };
	header.sequence = frame.sequence;
	header.width = uint16_t(frame.width);
	header.height = uint16_t(frame.height);
	header.aspect = frame.aspect;
	header.flags = frame.flags | (key ? FRAME_KEY : 0);
	if (!send_message(m_socket, message::FRAME, &header, sizeof(header), m_compressed.data(), compressed))
		return false;

	m_sent = frame.pixels;
	m_sent_width = frame.width;
	m_sent_height = frame.height;
	return true;
}


class host
{
public:
	bool start();

	void post_frame(uint32_t const *pixels, int width, int height, float aspect, bool native, bool vertical);
	void post_audio(int16_t const *samples, int frames, int channels, int rate);

private:
	void attach_pads();
	void accept_guests();
	void receive_input();
	void advertise();

	void welcome(int socket, sockaddr_in const &address);
	void drop_dead_links();
	void release_pad(int seat);
	int free_seats();

	int m_listen = -1;
	int m_input = -1;
	int m_advert = -1;

	// the gamepad each seat's guest drives, in seat order
	SDL_Joystick *m_pads[MAX_GUESTS] = { };

	std::mutex m_mutex;
	std::unique_ptr<guest_link> m_links[MAX_GUESTS];   // by seat, player = seat + 2
	uint32_t m_sequence = 0;
};

host *s_host = nullptr;

bool host::start()
{
	attach_pads();

	int const yes = 1;
	sockaddr_in address{ };
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(STREAM_PORT);

	m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if ((m_listen < 0) || ::bind(m_listen, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || ::listen(m_listen, 4))
	{
		NETERR("Could not listen on port %d: %s", STREAM_PORT, std::strerror(errno));
		close_socket(m_listen);
		return false;
	}

	m_input = ::socket(AF_INET, SOCK_DGRAM, 0);
	if ((m_input < 0) || ::bind(m_input, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
	{
		NETERR("Could not receive controller state on port %d: %s", STREAM_PORT, std::strerror(errno));
		close_socket(m_listen);
		close_socket(m_input);
		return false;
	}

	m_advert = ::socket(AF_INET, SOCK_DGRAM, 0);
	setsockopt(m_advert, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

	std::thread([this] () { accept_guests(); }).detach();
	std::thread([this] () { receive_input(); }).detach();
	std::thread([this] () { advertise(); }).detach();
	NETLOG("Hosting on port %d", STREAM_PORT);
	return true;
}

void host::attach_pads()
{
	// hold our own reference so the pads survive MAME shutting down joystick
	// support between machines
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK))
	{
		NETERR("SDL joystick init failed: %s", SDL_GetError());
		return;
	}

#if !defined(SDLMAME_ANDROID)
	// on Quest the Touch controllers are joystick 1; elsewhere hold that
	// place so guests still become players 2 and up
	static char const *const LOCAL_NAME = "Local Player";
#endif

	auto const attach = [] (char const *name) -> SDL_Joystick *
	{
		SDL_VirtualJoystickDesc desc;
		std::memset(&desc, 0, sizeof(desc));
		desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
		desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
		desc.naxes = SDL_CONTROLLER_AXIS_MAX;
		desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
		desc.name = name;
		int const index = SDL_JoystickAttachVirtualEx(&desc);
		SDL_Joystick *const pad = (index >= 0) ? SDL_JoystickOpen(index) : nullptr;
		if (!pad)
			NETERR("Could not attach virtual gamepad %s: %s", name, SDL_GetError());
		return pad;
	};

#if !defined(SDLMAME_ANDROID)
	attach(LOCAL_NAME);
#endif
	for (int seat = 0; seat < MAX_GUESTS; seat++)
		m_pads[seat] = attach((std::string(GUEST_PAD_NAME) + ' ' + std::to_string(seat + 2)).c_str());
}

void host::accept_guests()
{
	while (true)
	{
		sockaddr_in address{ };
		socklen_t length = sizeof(address);
		int const socket = ::accept(m_listen, reinterpret_cast<sockaddr *>(&address), &length);
		if (socket < 0)
		{
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			NETERR("Stopped accepting guests: %s", std::strerror(errno));
			return;
		}
		welcome(socket, address);
	}
}

void host::welcome(int socket, sockaddr_in const &address)
{
	int const yes = 1;
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#if defined(SO_NOSIGPIPE)
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
	timeval timeout{ 2, 0 };
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	message_header header;
	hello_message hello;
	int fd = socket;
	if (!receive_all(fd, &header, sizeof(header)) || (header.type != message::HELLO) || (header.size != sizeof(hello)) || !receive_all(fd, &hello, sizeof(hello)) || (hello.magic != MAGIC))
	{
		NETLOG("Ignored a connection from %s that isn't a guest", address_string(address).c_str());
		close_socket(fd);
		return;
	}
	if (hello.version != PROTOCOL_VERSION)
	{
		NETLOG("Refused %s: protocol version %u, ours is %u", address_string(address).c_str(), hello.version, PROTOCOL_VERSION);
		refused_message const refused{ refusal::VERSION };
		send_message(fd, message::REFUSED, &refused, sizeof(refused));
		close_socket(fd);
		return;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	drop_dead_links();
	int seat = 0;
	while ((seat < MAX_GUESTS) && m_links[seat])
		seat++;
	if (seat == MAX_GUESTS)
	{
		NETLOG("Refused %s: no free seats", address_string(address).c_str());
		refused_message const refused{ refusal::FULL };
		send_message(fd, message::REFUSED, &refused, sizeof(refused));
		close_socket(fd);
		return;
	}

	welcome_message const welcome{ uint32_t(seat + 2) };
	if (!send_message(fd, message::WELCOME, &welcome, sizeof(welcome)))
	{
		close_socket(fd);
		return;
	}
	m_links[seat] = std::make_unique<guest_link>(fd, address, seat + 2);
	NETLOG("Player %d joined from %s", seat + 2, address_string(address).c_str());
}

void host::drop_dead_links()
{
	// call with m_mutex held
	for (int seat = 0; seat < MAX_GUESTS; seat++)
	{
		if (m_links[seat] && !m_links[seat]->alive())
		{
			m_links[seat].reset();
			release_pad(seat);
		}
	}
}

void host::release_pad(int seat)
{
	if (SDL_Joystick *const pad = m_pads[seat])
	{
		for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++)
			SDL_JoystickSetVirtualButton(pad, button, SDL_RELEASED);
		for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++)
			SDL_JoystickSetVirtualAxis(pad, axis, 0);
	}
}

int host::free_seats()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	drop_dead_links();
	return int(std::count(std::begin(m_links), std::end(m_links), nullptr));
}

void host::receive_input()
{
	// guests may not open MAME's menus or quit the host's game
	constexpr uint32_t HOST_ONLY = (1U << SDL_CONTROLLER_BUTTON_GUIDE) | (1U << SDL_CONTROLLER_BUTTON_MISC1);

	while (true)
	{
		input_packet packet;
		sockaddr_in from{ };
		socklen_t length = sizeof(from);
		ssize_t const received = ::recvfrom(m_input, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &length);
		if (received < 0)
		{
			if (errno == EINTR)
				continue;
			NETERR("Stopped receiving controller state: %s", std::strerror(errno));
			return;
		}
		if ((received != sizeof(packet)) || (packet.magic != MAGIC) || (packet.version != PROTOCOL_VERSION))
			continue;
		int const seat = int(packet.player) - 2;
		if ((seat < 0) || (seat >= MAX_GUESTS))
			continue;

		std::lock_guard<std::mutex> lock(m_mutex);
		guest_link *const link = m_links[seat].get();
		SDL_Joystick *const pad = m_pads[seat];
		if (!link || !link->alive() || !pad || (link->address() != from.sin_addr.s_addr))
			continue;
		if (link->m_input_seen && (int32_t(packet.sequence - link->m_input_sequence) <= 0))
			continue;
		link->m_input_seen = true;
		link->m_input_sequence = packet.sequence;

		uint32_t const buttons = packet.buttons & ~HOST_ONLY;
		for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++)
			SDL_JoystickSetVirtualButton(pad, button, ((buttons >> button) & 1) ? SDL_PRESSED : SDL_RELEASED);
		for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++)
			SDL_JoystickSetVirtualAxis(pad, axis, packet.axes[axis]);
	}
}

void host::advertise()
{
	sockaddr_in broadcast{ };
	broadcast.sin_family = AF_INET;
	broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	broadcast.sin_port = htons(DISCOVERY_PORT);

	// broadcasts don't loop back on every system; tell this machine directly
	// too, so guests on it can find us
	sockaddr_in loopback = broadcast;
	loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	while (true)
	{
		advert_packet const advert{ MAGIC, PROTOCOL_VERSION, STREAM_PORT, uint8_t(free_seats()), 0 };
		::sendto(m_advert, &advert, sizeof(advert), 0, reinterpret_cast<sockaddr const *>(&broadcast), sizeof(broadcast));
		::sendto(m_advert, &advert, sizeof(advert), 0, reinterpret_cast<sockaddr const *>(&loopback), sizeof(loopback));
		std::this_thread::sleep_for(ADVERT_INTERVAL);
	}
}

void host::post_frame(uint32_t const *pixels, int width, int height, float aspect, bool native, bool vertical)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	drop_dead_links();
	if (std::none_of(std::begin(m_links), std::end(m_links), [] (auto const &link) { return bool(link); }))
		return;

	auto frame = std::make_shared<shared_frame>();
	frame->pixels.assign(pixels, pixels + size_t(width) * size_t(height));
	frame->sequence = m_sequence++;
	frame->width = width;
	frame->height = height;
	frame->aspect = aspect;
	frame->flags = (native ? FRAME_NATIVE : 0) | (vertical ? FRAME_VERTICAL : 0);
	std::shared_ptr<shared_frame const> const shared = std::move(frame);
	for (auto const &link : m_links)
		if (link)
			link->post_frame(shared);
}

void host::post_audio(int16_t const *samples, int frames, int channels, int rate)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto const &link : m_links)
		if (link && link->alive())
			link->post_audio(samples, frames, channels, rate);
}

} // anonymous namespace


bool host_start()
{
	if (!s_host)
	{
		// lives for the rest of the process, like its threads
		auto *const created = new host;
		if (!created->start())
		{
			delete created;
			return false;
		}
		s_host = created;
	}
	return true;
}

bool hosting()
{
	return s_host != nullptr;
}

void host_frame(uint32_t const *pixels, int width, int height, float aspect, bool native, bool vertical)
{
	if (s_host)
		s_host->post_frame(pixels, width, height, aspect, native, vertical);
}

void host_audio(int16_t const *samples, int frames, int channels, int rate)
{
	if (s_host && frames)
		s_host->post_audio(samples, frames, channels, rate);
}

} // namespace osd::quest::net
