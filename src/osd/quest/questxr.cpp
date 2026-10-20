// license:BSD-3-Clause
// copyright-holders:Daniel Oberlin
//============================================================
//
//  questxr.cpp - OpenXR presentation and controller input for Meta Quest
//
//  The emulated screen is shown as a compositor quad layer floating in
//  front of the user, over passthrough when available. Touch controller
//  state is read through OpenXR actions and fed to an SDL virtual game
//  controller, so MAME sees an ordinary gamepad.
//
//============================================================

#include "questxr.h"
#include "questanchor.h"

#include <SDL2/SDL.h>

#include <android/log.h>
#include <jni.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>


#define XRLOG(...) __android_log_print(ANDROID_LOG_INFO, "MAMEQuest", __VA_ARGS__)
#define XRERR(...) __android_log_print(ANDROID_LOG_ERROR, "MAMEQuest", __VA_ARGS__)


namespace osd::quest {

namespace {

// initial screen placement in the LOCAL reference space (metres, origin at
// the user's head)
constexpr float SCREEN_DISTANCE = 2.0f;
constexpr float SCREEN_HEIGHT = 1.2f;

// limits and rates for adjusting a grabbed screen with the stick
constexpr float MIN_GRAB_DISTANCE = 0.3f;
constexpr float MAX_GRAB_DISTANCE = 20.0f;
constexpr float MIN_SCREEN_HEIGHT = 0.2f;
constexpr float MAX_SCREEN_HEIGHT = 10.0f;
constexpr float STICK_DEADZONE = 0.2f;
constexpr float ADJUST_RATE = 1.0f; // e-folds per second at full deflection

// grip travel that grabs the screen, and the lower travel that lets it go
constexpr float GRAB_THRESHOLD = 0.5f;
constexpr float RELEASE_THRESHOLD = 0.3f;

// the menu button opens MAME's menu when tapped and exits (the game, or MAME
// from the system list) when held
constexpr auto MENU_HOLD_TIME = std::chrono::milliseconds(800);
constexpr auto BUTTON_PULSE_TIME = std::chrono::milliseconds(100);

struct button_binding
{
	char const *action;
	char const *path;
	SDL_GameControllerButton button;
};

// Touch controller -> standard gamepad layout, right hand leading: the right
// stick drives the gamepad's primary (left) stick. MAME's game controller
// support maps A/B to buttons 1-2 (and menu select/back), Back to coin, Start
// to start and Guide to its menu. The menu button is handled separately so it
// can tell a tap from a hold.
button_binding const BUTTONS[] = {
	{ "a",     "/user/hand/right/input/a/click",   SDL_CONTROLLER_BUTTON_A },
	{ "b",     "/user/hand/right/input/b/click",   SDL_CONTROLLER_BUTTON_B },
	{ "coin",  "/user/hand/left/input/x/click",    SDL_CONTROLLER_BUTTON_BACK },
	{ "start", "/user/hand/left/input/y/click",    SDL_CONTROLLER_BUTTON_START },
	{ "l1",    "/user/hand/left/input/thumbstick/click",  SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
	{ "r1",    "/user/hand/right/input/thumbstick/click", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
};

struct frame
{
	std::vector<uint32_t> pixels;
	int width = 0;
	int height = 0;
};

Sint16 to_axis(float value)
{
	return Sint16(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
}

XrVector3f operator+(XrVector3f const &a, XrVector3f const &b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
XrVector3f operator-(XrVector3f const &a, XrVector3f const &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
XrVector3f operator*(XrVector3f const &v, float s) { return { v.x * s, v.y * s, v.z * s }; }
XrVector3f cross(XrVector3f const &a, XrVector3f const &b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float length(XrVector3f const &v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

XrVector3f rotate(XrQuaternionf const &q, XrVector3f const &v)
{
	XrVector3f const u{ q.x, q.y, q.z };
	XrVector3f const t = cross(u, v) * 2.0f;
	return v + t * q.w + cross(u, t);
}

XrQuaternionf conjugate(XrQuaternionf const &q) { return { -q.x, -q.y, -q.z, q.w }; }

// orientation whose +Z (the visible side of a quad) points along forward,
// kept upright: yaw about world Y, then pitch, never roll
XrQuaternionf facing(XrVector3f const &forward)
{
	float const yaw = std::atan2(forward.x, forward.z);
	float const pitch = -std::atan2(forward.y, std::hypot(forward.x, forward.z));
	float const cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
	float const cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
	// rotation about Y by yaw composed with rotation about X by pitch
	return { cy * sp, sy * cp, -sy * sp, cy * cp };
}

float deadzone(float value)
{
	if (std::abs(value) < STICK_DEADZONE)
		return 0.0f;
	return (value - std::copysign(STICK_DEADZONE, value)) / (1.0f - STICK_DEADZONE);
}


class quest_xr
{
public:
	bool start();
	uint32_t *begin_frame(int width, int height);
	void end_frame();
	bool has_focus() const { return m_focused; }

private:
	void attach_gamepad();
	void run();
	bool init_egl();
	bool init_instance();
	bool init_session();
	bool init_actions();
	void init_passthrough();
	bool ensure_swapchain(int width, int height);
	void destroy_swapchain();
	void upload_frame();
	void poll_events();
	void sync_input();
	void update_placement(XrTime time, XrDuration period);
	void reset_placement();
	void render_frame();
	void shutdown();

	XrAction create_action(char const *name, XrActionType type);
	void suggest(std::vector<XrActionSuggestedBinding> &bindings, XrAction action, char const *path);
	bool get_bool(XrAction action);
	float get_float(XrAction action);
	XrVector2f get_vector(XrAction action);

	bool m_started = false;
	JavaVM *m_vm = nullptr;
	jobject m_activity = nullptr;
	SDL_Joystick *m_pad = nullptr;
	std::thread m_thread;

	// frames move back (renderer) -> ready (shared) -> front (XR thread)
	std::mutex m_frame_lock;
	frame m_frames[3];
	frame *m_back = &m_frames[0];
	frame *m_ready = &m_frames[1];
	frame *m_front = &m_frames[2];
	bool m_ready_fresh = false;
	bool m_front_fresh = false;

	EGLDisplay m_egl_display = EGL_NO_DISPLAY;
	EGLConfig m_egl_config = nullptr;
	EGLContext m_egl_context = EGL_NO_CONTEXT;
	EGLSurface m_egl_surface = EGL_NO_SURFACE;

	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_system = XR_NULL_SYSTEM_ID;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_space = XR_NULL_HANDLE;
	bool m_running = false;
	bool m_exit = false;
	std::atomic<bool> m_focused{ false };

	int64_t m_format = 0;
	XrSwapchain m_swapchain = XR_NULL_HANDLE;
	int m_sc_width = 0;
	int m_sc_height = 0;
	bool m_sc_has_content = false;
	std::vector<XrSwapchainImageOpenGLESKHR> m_sc_images;

	bool m_has_passthrough_ext = false;
	XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
	XrPassthroughLayerFB m_passthrough_layer = XR_NULL_HANDLE;
	PFN_xrDestroyPassthroughFB m_xrDestroyPassthroughFB = nullptr;
	PFN_xrDestroyPassthroughLayerFB m_xrDestroyPassthroughLayerFB = nullptr;

	XrActionSet m_action_set = XR_NULL_HANDLE;
	XrAction m_buttons[std::size(BUTTONS)] = { };
	XrAction m_stick[2] = { };
	XrAction m_trigger[2] = { };
	XrAction m_grip[2] = { };
	XrAction m_aim[2] = { };
	XrSpace m_aim_space[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrSpace m_view_space = XR_NULL_HANDLE;
	XrAction m_menu = XR_NULL_HANDLE;
	uint32_t m_pressed = 0;

	using clock = std::chrono::steady_clock;
	bool m_menu_down = false;
	bool m_menu_held = false;
	clock::time_point m_menu_since;
	clock::time_point m_guide_until;   // tap: pulse Guide (MAME menu)
	clock::time_point m_cancel_until;  // hold: pulse Misc1 (MAME UI cancel)

	std::string m_files_dir;
	bool m_has_anchor_ext = false;
	bool m_anchor_ok = false;
	bool m_reanchor = false;
	screen_anchor m_anchor;

	// screen placement in m_space; only the XR thread touches these
	XrPosef m_screen_pose{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -SCREEN_DISTANCE } };
	float m_screen_height = SCREEN_HEIGHT;
	int m_grab_hand = -1;
	XrVector3f m_grab_offset{ }; // screen centre in the grabbing hand's aim space
	XrVector2f m_grab_stick{ };  // the grabbing hand's stick, kept away from MAME
};

quest_xr s_xr;


//============================================================
//  renderer side
//============================================================

bool quest_xr::start()
{
	if (m_started)
		return true;

	auto *const env = reinterpret_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	auto const activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
	if (!env || !activity)
	{
		XRERR("No JNI environment or activity: %s", SDL_GetError());
		return false;
	}
	env->GetJavaVM(&m_vm);
	if (char const *const dir = SDL_AndroidGetExternalStoragePath())
		m_files_dir = dir;
	m_activity = env->NewGlobalRef(activity);
	env->DeleteLocalRef(activity);

	attach_gamepad();

	m_thread = std::thread([this] () { run(); });
	m_thread.detach();
	m_started = true;
	return true;
}

uint32_t *quest_xr::begin_frame(int width, int height)
{
	m_back->width = width;
	m_back->height = height;
	m_back->pixels.resize(size_t(width) * size_t(height));
	return m_back->pixels.data();
}

void quest_xr::end_frame()
{
	// GL textures are stored bottom row first
	int const width = m_back->width;
	for (int top = 0, bottom = m_back->height - 1; top < bottom; ++top, --bottom)
	{
		uint32_t *const toprow = &m_back->pixels[size_t(top) * width];
		std::swap_ranges(toprow, toprow + width, &m_back->pixels[size_t(bottom) * width]);
	}

	std::lock_guard<std::mutex> lock(m_frame_lock);
	std::swap(m_back, m_ready);
	m_ready_fresh = true;
}

void quest_xr::attach_gamepad()
{
	// the headset's accelerometer would otherwise show up as a joystick
	SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");

	// hold our own reference so the device survives MAME shutting down
	// joystick support between machines
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK))
	{
		XRERR("SDL joystick init failed: %s", SDL_GetError());
		return;
	}

	SDL_VirtualJoystickDesc desc;
	std::memset(&desc, 0, sizeof(desc));
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	desc.name = "Quest Touch Controllers";

	int const index = SDL_JoystickAttachVirtualEx(&desc);
	if (index < 0)
	{
		XRERR("Could not attach virtual gamepad: %s", SDL_GetError());
		return;
	}
	m_pad = SDL_JoystickOpen(index);
	if (!m_pad)
		XRERR("Could not open virtual gamepad: %s", SDL_GetError());
}


//============================================================
//  XR thread
//============================================================

void quest_xr::run()
{
	JNIEnv *env = nullptr;
	m_vm->AttachCurrentThread(&env, nullptr);

	if (init_egl() && init_instance() && init_session() && init_actions())
	{
		init_passthrough();
		if (m_has_anchor_ext && !m_files_dir.empty())
			m_anchor_ok = m_anchor.init(m_instance, m_session, m_files_dir + "/screen_anchor.txt", m_screen_height);
		XRLOG("OpenXR ready%s", m_anchor_ok ? "" : " (screen placement won't be saved)");
		while (!m_exit)
		{
			poll_events();
			if (m_running)
			{
				sync_input();
				render_frame();
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}
	else
	{
		XRERR("OpenXR initialisation failed");
	}

	shutdown();
	m_vm->DetachCurrentThread();

	// let MAME wind down; the activity kills the process when it is destroyed
	SDL_Event quit;
	std::memset(&quit, 0, sizeof(quit));
	quit.type = SDL_QUIT;
	SDL_PushEvent(&quit);
}

bool quest_xr::init_egl()
{
	m_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(m_egl_display, nullptr, nullptr))
		return false;

	EGLint const config_attribs[] = {
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_NONE };
	EGLint count = 0;
	if (!eglChooseConfig(m_egl_display, config_attribs, &m_egl_config, 1, &count) || !count)
		return false;

	EGLint const context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	m_egl_context = eglCreateContext(m_egl_display, m_egl_config, EGL_NO_CONTEXT, context_attribs);
	if (m_egl_context == EGL_NO_CONTEXT)
		return false;

	EGLint const surface_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	m_egl_surface = eglCreatePbufferSurface(m_egl_display, m_egl_config, surface_attribs);
	return eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context);
}

bool quest_xr::init_instance()
{
	PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>(&initialize_loader));
	if (!initialize_loader)
		return false;
	XrLoaderInitInfoAndroidKHR loader_info{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loader_info.applicationVM = m_vm;
	loader_info.applicationContext = m_activity;
	if (XR_FAILED(initialize_loader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR const *>(&loader_info))))
		return false;

	uint32_t ext_count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
	std::vector<XrExtensionProperties> available(ext_count, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, available.data());
	auto const has_extension = [&available] (char const *name)
	{
		return std::any_of(available.begin(), available.end(), [name] (auto const &ext) { return !std::strcmp(ext.extensionName, name); });
	};
	m_has_passthrough_ext = has_extension(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	auto const &anchor_extensions = screen_anchor::extensions();
	m_has_anchor_ext = std::all_of(anchor_extensions.begin(), anchor_extensions.end(), has_extension);

	std::vector<char const *> extensions = {
			XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
			XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
	if (m_has_passthrough_ext)
		extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	if (m_has_anchor_ext)
		extensions.insert(extensions.end(), anchor_extensions.begin(), anchor_extensions.end());

	XrInstanceCreateInfoAndroidKHR android_info{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	android_info.applicationVM = m_vm;
	android_info.applicationActivity = m_activity;

	XrInstanceCreateInfo create_info{ XR_TYPE_INSTANCE_CREATE_INFO };
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName, "MAMEQuest", XR_MAX_APPLICATION_NAME_SIZE - 1);
	std::strncpy(create_info.applicationInfo.engineName, "MAME", XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	create_info.enabledExtensionCount = uint32_t(extensions.size());
	create_info.enabledExtensionNames = extensions.data();
	XrResult const result = xrCreateInstance(&create_info, &m_instance);
	if (XR_FAILED(result))
	{
		XRERR("xrCreateInstance failed: %d", result);
		return false;
	}

	XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	return XR_SUCCEEDED(xrGetSystem(m_instance, &system_info, &m_system));
}

bool quest_xr::init_session()
{
	// the runtime requires this query before a session is created
	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
	xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&get_requirements));
	if (!get_requirements)
		return false;
	XrGraphicsRequirementsOpenGLESKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	get_requirements(m_instance, m_system, &requirements);

	XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = m_egl_display;
	binding.config = m_egl_config;
	binding.context = m_egl_context;

	XrSessionCreateInfo session_info{ XR_TYPE_SESSION_CREATE_INFO };
	session_info.next = &binding;
	session_info.systemId = m_system;
	XrResult const result = xrCreateSession(m_instance, &session_info, &m_session);
	if (XR_FAILED(result))
	{
		XRERR("xrCreateSession failed: %d", result);
		return false;
	}

	XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	if (XR_FAILED(xrCreateReferenceSpace(m_session, &space_info, &m_space)))
		return false;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (XR_FAILED(xrCreateReferenceSpace(m_session, &space_info, &m_view_space)))
		return false;

	// prefer an sRGB format: MAME's output is already gamma encoded
	uint32_t format_count = 0;
	xrEnumerateSwapchainFormats(m_session, 0, &format_count, nullptr);
	std::vector<int64_t> formats(format_count);
	xrEnumerateSwapchainFormats(m_session, format_count, &format_count, formats.data());
	m_format = GL_RGBA8;
	if (std::find(formats.begin(), formats.end(), GL_SRGB8_ALPHA8) != formats.end())
		m_format = GL_SRGB8_ALPHA8;
	return true;
}

XrAction quest_xr::create_action(char const *name, XrActionType type)
{
	XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	XrAction action = XR_NULL_HANDLE;
	xrCreateAction(m_action_set, &info, &action);
	return action;
}

void quest_xr::suggest(std::vector<XrActionSuggestedBinding> &bindings, XrAction action, char const *path)
{
	XrPath xrpath = XR_NULL_PATH;
	xrStringToPath(m_instance, path, &xrpath);
	bindings.push_back({ action, xrpath });
}

bool quest_xr::init_actions()
{
	XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
	std::strcpy(set_info.actionSetName, "gamepad");
	std::strcpy(set_info.localizedActionSetName, "Gamepad");
	if (XR_FAILED(xrCreateActionSet(m_instance, &set_info, &m_action_set)))
		return false;

	std::vector<XrActionSuggestedBinding> bindings;
	for (size_t i = 0; i < std::size(BUTTONS); ++i)
	{
		m_buttons[i] = create_action(BUTTONS[i].action, XR_ACTION_TYPE_BOOLEAN_INPUT);
		suggest(bindings, m_buttons[i], BUTTONS[i].path);
	}

	m_menu = create_action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
	suggest(bindings, m_menu, "/user/hand/left/input/menu/click");

	char const *const hands[2] = { "left", "right" };
	for (int hand = 0; hand < 2; ++hand)
	{
		std::string const prefix = std::string("/user/hand/") + hands[hand] + "/input/";
		m_stick[hand] = create_action((std::string("stick_") + hands[hand]).c_str(), XR_ACTION_TYPE_VECTOR2F_INPUT);
		m_trigger[hand] = create_action((std::string("trigger_") + hands[hand]).c_str(), XR_ACTION_TYPE_FLOAT_INPUT);
		m_grip[hand] = create_action((std::string("grip_") + hands[hand]).c_str(), XR_ACTION_TYPE_FLOAT_INPUT);
		m_aim[hand] = create_action((std::string("aim_") + hands[hand]).c_str(), XR_ACTION_TYPE_POSE_INPUT);
		suggest(bindings, m_stick[hand], (prefix + "thumbstick").c_str());
		suggest(bindings, m_trigger[hand], (prefix + "trigger/value").c_str());
		suggest(bindings, m_grip[hand], (prefix + "squeeze/value").c_str());
		suggest(bindings, m_aim[hand], (prefix + "aim/pose").c_str());
	}

	XrPath profile = XR_NULL_PATH;
	xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &profile);
	XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = profile;
	suggested.countSuggestedBindings = uint32_t(bindings.size());
	suggested.suggestedBindings = bindings.data();
	XrResult result = xrSuggestInteractionProfileBindings(m_instance, &suggested);
	if (XR_FAILED(result))
	{
		XRERR("xrSuggestInteractionProfileBindings failed: %d", result);
		return false;
	}

	XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &m_action_set;
	if (XR_FAILED(xrAttachSessionActionSets(m_session, &attach)))
		return false;

	for (int hand = 0; hand < 2; ++hand)
	{
		XrActionSpaceCreateInfo space_info{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		space_info.action = m_aim[hand];
		space_info.poseInActionSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateActionSpace(m_session, &space_info, &m_aim_space[hand])))
			return false;
	}
	return true;
}

void quest_xr::init_passthrough()
{
	if (!m_has_passthrough_ext)
		return;

	PFN_xrCreatePassthroughFB create_passthrough = nullptr;
	PFN_xrCreatePassthroughLayerFB create_layer = nullptr;
	xrGetInstanceProcAddr(m_instance, "xrCreatePassthroughFB", reinterpret_cast<PFN_xrVoidFunction *>(&create_passthrough));
	xrGetInstanceProcAddr(m_instance, "xrCreatePassthroughLayerFB", reinterpret_cast<PFN_xrVoidFunction *>(&create_layer));
	xrGetInstanceProcAddr(m_instance, "xrDestroyPassthroughFB", reinterpret_cast<PFN_xrVoidFunction *>(&m_xrDestroyPassthroughFB));
	xrGetInstanceProcAddr(m_instance, "xrDestroyPassthroughLayerFB", reinterpret_cast<PFN_xrVoidFunction *>(&m_xrDestroyPassthroughLayerFB));
	if (!create_passthrough || !create_layer)
		return;

	XrPassthroughCreateInfoFB info{ XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
	info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
	if (XR_FAILED(create_passthrough(m_session, &info, &m_passthrough)))
	{
		XRERR("Passthrough unavailable");
		return;
	}

	XrPassthroughLayerCreateInfoFB layer_info{ XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
	layer_info.passthrough = m_passthrough;
	layer_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
	layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	if (XR_FAILED(create_layer(m_session, &layer_info, &m_passthrough_layer)))
		m_passthrough_layer = XR_NULL_HANDLE;
}

bool quest_xr::ensure_swapchain(int width, int height)
{
	if (m_swapchain != XR_NULL_HANDLE && m_sc_width == width && m_sc_height == height)
		return true;
	destroy_swapchain();

	XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	info.format = m_format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	XrResult const result = xrCreateSwapchain(m_session, &info, &m_swapchain);
	if (XR_FAILED(result))
	{
		XRERR("xrCreateSwapchain %dx%d failed: %d", width, height, result);
		m_swapchain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t count = 0;
	xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr);
	m_sc_images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
	xrEnumerateSwapchainImages(m_swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(m_sc_images.data()));
	m_sc_width = width;
	m_sc_height = height;
	m_sc_has_content = false;
	XRLOG("Screen swapchain %dx%d", width, height);
	return true;
}

void quest_xr::destroy_swapchain()
{
	if (m_swapchain != XR_NULL_HANDLE)
		xrDestroySwapchain(m_swapchain);
	m_swapchain = XR_NULL_HANDLE;
	m_sc_images.clear();
	m_sc_has_content = false;
}

void quest_xr::upload_frame()
{
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (XR_FAILED(xrAcquireSwapchainImage(m_swapchain, &acquire, &index)))
		return;
	XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(m_swapchain, &wait);

	glBindTexture(GL_TEXTURE_2D, m_sc_images[index].image);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_front->width, m_front->height, GL_RGBA, GL_UNSIGNED_BYTE, m_front->pixels.data());
	glBindTexture(GL_TEXTURE_2D, 0);
	glFlush();

	XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_swapchain, &release);
	m_sc_has_content = true;
}

void quest_xr::poll_events()
{
	XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				auto const &change = reinterpret_cast<XrEventDataSessionStateChanged const &>(event);
				XRLOG("Session state %d", change.state);
				m_focused = (change.state == XR_SESSION_STATE_FOCUSED);
				switch (change.state)
				{
				case XR_SESSION_STATE_READY:
					{
						XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
						begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						m_running = XR_SUCCEEDED(xrBeginSession(m_session, &begin));
					}
					break;
				case XR_SESSION_STATE_STOPPING:
					xrEndSession(m_session);
					m_running = false;
					break;
				case XR_SESSION_STATE_EXITING:
				case XR_SESSION_STATE_LOSS_PENDING:
					m_exit = true;
					break;
				default:
					break;
				}
			}
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			m_exit = true;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			// recentring the view also brings the screen back in front
			if (reinterpret_cast<XrEventDataReferenceSpaceChangePending const &>(event).referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
				reset_placement();
			break;
		default:
			if (m_anchor_ok)
				m_anchor.handle_event(event);
			break;
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

bool quest_xr::get_bool(XrAction action)
{
	XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &info, &state)) && state.isActive && state.currentState;
}

float quest_xr::get_float(XrAction action)
{
	XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
	if (XR_FAILED(xrGetActionStateFloat(m_session, &info, &state)) || !state.isActive)
		return 0.0f;
	return state.currentState;
}

XrVector2f quest_xr::get_vector(XrAction action)
{
	XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
	if (XR_FAILED(xrGetActionStateVector2f(m_session, &info, &state)) || !state.isActive)
		return { 0.0f, 0.0f };
	return state.currentState;
}

void quest_xr::sync_input()
{
	XrActiveActionSet active{ m_action_set, XR_NULL_PATH };
	XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	// while unfocused this succeeds with every action inactive, releasing all inputs
	if (XR_FAILED(xrSyncActions(m_session, &sync)) || !m_pad)
		return;

	uint32_t pressed = 0;
	for (size_t i = 0; i < std::size(BUTTONS); ++i)
	{
		bool const down = get_bool(m_buttons[i]);
		SDL_JoystickSetVirtualButton(m_pad, BUTTONS[i].button, down ? SDL_PRESSED : SDL_RELEASED);
		if (down)
			pressed |= 1U << i;
	}
	if (pressed != m_pressed)
	{
		XRLOG("Buttons 0x%02x", pressed);
		m_pressed = pressed;
	}

	auto const now = clock::now();
	bool const menu = get_bool(m_menu);
	if (menu && !m_menu_down)
	{
		m_menu_since = now;
		m_menu_held = false;
	}
	else if (menu && !m_menu_held && (now - m_menu_since) >= MENU_HOLD_TIME)
	{
		m_menu_held = true;
		m_cancel_until = now + BUTTON_PULSE_TIME;
	}
	else if (!menu && m_menu_down && !m_menu_held)
	{
		m_guide_until = now + BUTTON_PULSE_TIME;
	}
	m_menu_down = menu;
	SDL_JoystickSetVirtualButton(m_pad, SDL_CONTROLLER_BUTTON_GUIDE, (now < m_guide_until) ? SDL_PRESSED : SDL_RELEASED);
	SDL_JoystickSetVirtualButton(m_pad, SDL_CONTROLLER_BUTTON_MISC1, (now < m_cancel_until) ? SDL_PRESSED : SDL_RELEASED);

	// the hand holding the screen uses its stick to adjust it instead
	XrVector2f sticks[2] = { get_vector(m_stick[0]), get_vector(m_stick[1]) };
	if (m_grab_hand >= 0)
	{
		m_grab_stick = sticks[m_grab_hand];
		sticks[m_grab_hand] = { 0.0f, 0.0f };
	}

	// right stick is primary; OpenXR stick Y is up-positive, SDL's is down-positive
	XrVector2f const primary = sticks[1];
	XrVector2f const secondary = sticks[0];
	SDL_JoystickSetVirtualAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTX, to_axis(primary.x));
	SDL_JoystickSetVirtualAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTY, to_axis(-primary.y));
	SDL_JoystickSetVirtualAxis(m_pad, SDL_CONTROLLER_AXIS_RIGHTX, to_axis(secondary.x));
	SDL_JoystickSetVirtualAxis(m_pad, SDL_CONTROLLER_AXIS_RIGHTY, to_axis(-secondary.y));
	SDL_JoystickSetVirtualAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, to_axis(get_float(m_trigger[0])));
	SDL_JoystickSetVirtualAxis(m_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, to_axis(get_float(m_trigger[1])));
}

void quest_xr::update_placement(XrTime time, XrDuration period)
{
	if (m_grab_hand >= 0 && get_float(m_grip[m_grab_hand]) < RELEASE_THRESHOLD)
	{
		XrVector3f const &p = m_screen_pose.position;
		XRLOG("Screen placed at (%.2f, %.2f, %.2f), height %.2f m", p.x, p.y, p.z, m_screen_height);
		m_grab_hand = -1;
		if (m_anchor_ok)
			m_anchor.place(m_space, m_screen_pose, time, m_screen_height);
	}

	XrSpaceLocation hand{ XR_TYPE_SPACE_LOCATION };
	auto const located = [&hand, time, this] (int which)
	{
		XrSpaceLocationFlags const needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		return XR_SUCCEEDED(xrLocateSpace(m_aim_space[which], m_space, time, &hand)) && ((hand.locationFlags & needed) == needed);
	};

	if (m_grab_hand < 0)
	{
		if (m_reanchor)
		{
			m_reanchor = false;
			m_anchor.place(m_space, m_screen_pose, time, m_screen_height);
		}
		else
		{
			// follow the anchor so the screen stays put in the room
			m_anchor.locate(m_space, time, m_screen_pose);
		}

		for (int which = 1; which >= 0; --which)
		{
			if (get_float(m_grip[which]) > GRAB_THRESHOLD && located(which))
			{
				m_grab_hand = which;
				m_grab_offset = rotate(conjugate(hand.pose.orientation), m_screen_pose.position - hand.pose.position);
				m_grab_stick = { 0.0f, 0.0f };
				break;
			}
		}
		return;
	}

	if (!located(m_grab_hand))
		return;

	// stick up/down pushes and pulls along the hand's reach, left/right resizes
	float const seconds = float(period) * 1e-9f;
	float const reach = length(m_grab_offset);
	if (reach > 0.0f)
	{
		float const target = std::clamp(reach * std::exp(deadzone(m_grab_stick.y) * ADJUST_RATE * seconds), MIN_GRAB_DISTANCE, MAX_GRAB_DISTANCE);
		m_grab_offset = m_grab_offset * (target / reach);
	}
	m_screen_height = std::clamp(m_screen_height * std::exp(deadzone(m_grab_stick.x) * ADJUST_RATE * seconds), MIN_SCREEN_HEIGHT, MAX_SCREEN_HEIGHT);

	m_screen_pose.position = hand.pose.position + rotate(hand.pose.orientation, m_grab_offset);

	// turn the screen to face the viewer
	XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
	if (XR_SUCCEEDED(xrLocateSpace(m_view_space, m_space, time, &head)) && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
	{
		XrVector3f const forward = head.pose.position - m_screen_pose.position;
		if (length(forward) > 0.01f)
			m_screen_pose.orientation = facing(forward);
	}
}

void quest_xr::reset_placement()
{
	m_screen_pose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -SCREEN_DISTANCE } };
	m_screen_height = SCREEN_HEIGHT;
	m_grab_hand = -1;
	m_reanchor = m_anchor_ok;
}

void quest_xr::render_frame()
{
	XrFrameWaitInfo wait_info{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
	if (XR_FAILED(xrWaitFrame(m_session, &wait_info, &frame_state)))
		return;
	XrFrameBeginInfo begin_info{ XR_TYPE_FRAME_BEGIN_INFO };
	if (XR_FAILED(xrBeginFrame(m_session, &begin_info)))
		return;

	update_placement(frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod);

	{
		std::lock_guard<std::mutex> lock(m_frame_lock);
		if (m_ready_fresh)
		{
			std::swap(m_ready, m_front);
			m_ready_fresh = false;
			m_front_fresh = true;
		}
	}

	XrCompositionLayerBaseHeader const *layers[2];
	uint32_t layer_count = 0;

	XrCompositionLayerPassthroughFB passthrough{ XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
	if (m_passthrough_layer != XR_NULL_HANDLE)
	{
		passthrough.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		passthrough.layerHandle = m_passthrough_layer;
		layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader const *>(&passthrough);
	}

	XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	if (frame_state.shouldRender && m_front->width > 0 && m_front->height > 0 && ensure_swapchain(m_front->width, m_front->height))
	{
		if (m_front_fresh || !m_sc_has_content)
		{
			upload_frame();
			m_front_fresh = false;
		}
		if (m_sc_has_content)
		{
			float const aspect = float(m_sc_width) / float(m_sc_height);
			quad.space = m_space;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = m_swapchain;
			quad.subImage.imageRect = { { 0, 0 }, { m_sc_width, m_sc_height } };
			quad.pose = m_screen_pose;
			quad.size = { m_screen_height * aspect, m_screen_height };
			layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader const *>(&quad);
		}
	}

	XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = frame_state.shouldRender ? layer_count : 0;
	end_info.layers = layers;
	xrEndFrame(m_session, &end_info);
}

void quest_xr::shutdown()
{
	destroy_swapchain();
	m_anchor.shutdown();
	if (m_passthrough_layer != XR_NULL_HANDLE && m_xrDestroyPassthroughLayerFB)
		m_xrDestroyPassthroughLayerFB(m_passthrough_layer);
	if (m_passthrough != XR_NULL_HANDLE && m_xrDestroyPassthroughFB)
		m_xrDestroyPassthroughFB(m_passthrough);
	for (XrSpace &space : m_aim_space)
	{
		if (space != XR_NULL_HANDLE)
			xrDestroySpace(space);
		space = XR_NULL_HANDLE;
	}
	if (m_view_space != XR_NULL_HANDLE)
		xrDestroySpace(m_view_space);
	m_view_space = XR_NULL_HANDLE;
	if (m_action_set != XR_NULL_HANDLE)
		xrDestroyActionSet(m_action_set);
	if (m_space != XR_NULL_HANDLE)
		xrDestroySpace(m_space);
	if (m_session != XR_NULL_HANDLE)
		xrDestroySession(m_session);
	if (m_instance != XR_NULL_HANDLE)
		xrDestroyInstance(m_instance);
	m_passthrough_layer = XR_NULL_HANDLE;
	m_passthrough = XR_NULL_HANDLE;
	m_action_set = XR_NULL_HANDLE;
	m_space = XR_NULL_HANDLE;
	m_session = XR_NULL_HANDLE;
	m_instance = XR_NULL_HANDLE;

	if (m_egl_display != EGL_NO_DISPLAY)
	{
		eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (m_egl_surface != EGL_NO_SURFACE)
			eglDestroySurface(m_egl_display, m_egl_surface);
		if (m_egl_context != EGL_NO_CONTEXT)
			eglDestroyContext(m_egl_display, m_egl_context);
	}
	m_egl_surface = EGL_NO_SURFACE;
	m_egl_context = EGL_NO_CONTEXT;
}

} // anonymous namespace


bool xr_start()
{
	return s_xr.start();
}

uint32_t *xr_begin_frame(int width, int height)
{
	return s_xr.begin_frame(width, height);
}

void xr_end_frame()
{
	s_xr.end_frame();
}

bool xr_has_focus()
{
	return s_xr.has_focus();
}

} // namespace osd::quest
