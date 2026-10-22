// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questnetproto.h - wire format and socket helpers for questnet
//
//  Guests connect to the host over TCP and receive frames and sound on
//  that connection. Controller state goes back over UDP: each packet
//  carries the whole state, so a lost packet is simply replaced by the
//  next. Hosts advertise themselves with UDP broadcasts. Everything is
//  little-endian, as on every machine this runs on.
//
//  Frames are sent as the XOR of the previous frame sent to that guest,
//  compressed with zstd: arcade pictures change little between frames,
//  so this is small without needing a video codec.
//
//============================================================
#ifndef MAME_OSD_QUEST_QUESTNETPROTO_H
#define MAME_OSD_QUEST_QUESTNETPROTO_H

#pragma once

#include "questnet.h"

#include <netinet/in.h>

#include <chrono>
#include <cstdint>
#include <string>


namespace osd::quest::net {

constexpr uint32_t MAGIC = 0x4e51344d; // "M4QN"

// bump on any change to the messages below; peers refuse a mismatch
constexpr uint32_t PROTOCOL_VERSION = 1;

constexpr uint16_t STREAM_PORT = 47047;    // TCP stream, and UDP controller state to the host
constexpr uint16_t DISCOVERY_PORT = 47048; // UDP host adverts

constexpr auto ADVERT_INTERVAL = std::chrono::milliseconds(500);


enum class message : uint32_t
{
	HELLO = 1,      // guest -> host: hello_message
	WELCOME,        // host -> guest: welcome_message
	REFUSED,        // host -> guest: refused_message, then the host hangs up
	FRAME,          // host -> guest: frame_message + compressed pixels
	AUDIO,          // host -> guest: audio_message + samples
	GAME            // host -> guest: the game's name in UTF-8, empty while choosing one
};

enum class refusal : uint32_t
{
	VERSION = 1,    // different protocol version
	FULL            // every seat is taken
};

#pragma pack(push, 1)

struct message_header
{
	message type;
	uint32_t size;  // bytes following this header
};

struct hello_message
{
	uint32_t magic;
	uint32_t version;
};

struct welcome_message
{
	uint32_t player;    // 2 to MAX_GUESTS + 1
};

struct refused_message
{
	refusal reason;
};

enum : uint8_t
{
	FRAME_NATIVE = 1,
	FRAME_VERTICAL = 2,
	FRAME_KEY = 4   // whole frame rather than the XOR of the previous one
};

struct frame_message
{
	uint32_t sequence;
	uint16_t width;
	uint16_t height;
	float aspect;
	uint8_t flags;
	uint8_t reserved[3];
};

struct audio_message
{
	uint32_t rate;
	uint16_t channels;
	uint16_t reserved;
};

struct input_packet
{
	uint32_t magic;
	uint32_t version;
	uint32_t player;
	uint32_t sequence;  // newer packets win; older ones arriving late are dropped
	uint32_t buttons;
	int16_t axes[6];
};

struct advert_packet
{
	uint32_t magic;
	uint32_t version;
	uint16_t stream_port;
	uint8_t free_seats;
	uint8_t reserved;
};

#pragma pack(pop)


// socket helpers; all return false (or -1) on failure

bool send_all(int socket, void const *data, size_t size);
bool receive_all(int socket, void *data, size_t size);
bool send_message(int socket, message type, void const *body, size_t body_size, void const *extra = nullptr, size_t extra_size = 0);
void close_socket(int &socket);
std::string address_string(sockaddr_in const &address);

} // namespace osd::quest::net

#endif // MAME_OSD_QUEST_QUESTNETPROTO_H
