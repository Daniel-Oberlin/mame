// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questnetlog.h - logging for questnet, which also runs outside MAME
//
//============================================================
#ifndef MAME_OSD_QUEST_QUESTNETLOG_H
#define MAME_OSD_QUEST_QUESTNETLOG_H

#pragma once

#if defined(SDLMAME_ANDROID)
#include <android/log.h>
#define NETLOG(...) __android_log_print(ANDROID_LOG_INFO, "MAMEQuest", __VA_ARGS__)
#define NETERR(...) __android_log_print(ANDROID_LOG_ERROR, "MAMEQuest", __VA_ARGS__)
#else
#include <cstdio>
#define NETLOG(...) (std::fprintf(stderr, "net: " __VA_ARGS__), std::fputc('\n', stderr))
#define NETERR(...) (std::fprintf(stderr, "net error: " __VA_ARGS__), std::fputc('\n', stderr))
#endif

#endif // MAME_OSD_QUEST_QUESTNETLOG_H
