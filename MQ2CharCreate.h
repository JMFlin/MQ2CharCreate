/*
 * MQ2CharCreate: Automated EverQuest character creation plugin
 *
 * Drives the full character creation flow (login → server select → create → enter world)
 * and populates the MQ2AutoLogin database on success.
 *
 * Coexists with MQ2AutoLogin: AutoLogin self-pauses when no profile is found,
 * and this plugin's FSM takes over to drive the UI.
 */

#pragma once

#include <mq/Plugin.h>

#include <tinyfsm.hpp>
#include <memory>
#include <string>

// Debug log — writes to gPathConfig/MQ2CharCreate_debug.log (visible even during PRECHARSELECT)
void CC_DebugLog(const char* fmt_str, ...);

// ============================================================================
// Configuration: Character creation UI window/control names
//
// Discovered in-game via hover logging. The creation UI is a multi-screen
// wizard with individual buttons per race/class/gender (not list controls).
//
// Flow: CharacterListWnd → CLW_Create_Button → CharacterCreation →
//       CC_RaceScreen → CC_GenderScreen → CC_ClassScreen →
//       CC_OptionsScreen → CC_NameScreen → CC_Create_Button
//
// Button naming patterns:
//   Race:   CC_Race_{RaceName}_Button    (e.g., CC_Race_Drakkin_Button)
//   Gender: CC_{Gender}_Button           (e.g., CC_Male_Button)
//   Class:  CC_Class_{ClassName}_Button  (e.g., CC_Class_Shadow Knight_Button)
// ============================================================================

namespace CCWindowNames
{
	// Character select screen
	constexpr const char* CharSelectWindow  = "CharacterListWnd";
	constexpr const char* CreateButton      = "CLW_Create_Button";

	// Character creation parent window
	constexpr const char* CreateWindow      = "CharacterCreation";

	// Character creation sub-screens (wizard steps)
	constexpr const char* RaceScreen        = "CC_RaceScreen";
	constexpr const char* GenderScreen      = "CC_GenderScreen";
	constexpr const char* ClassScreen       = "CC_ClassScreen";
	constexpr const char* OptionsScreen     = "CC_OptionsScreen";
	constexpr const char* NameScreen        = "CC_NameScreen";

	// Controls within the creation wizard
	constexpr const char* NameEdit          = "CC_Name_Edit";
	constexpr const char* FinalCreateButton = "CC_Create_Button";
	constexpr const char* TutorialButton    = "CC_Tutorial_Button";
}

// ============================================================================
// Character creation request
// ============================================================================

struct CharCreateRequest
{
	std::string serverName;
	std::string accountName;
	std::string accountPassword;
	std::string characterName;
	std::string raceName;
	std::string className;
	std::string genderName = "Male";

	void Reset()
	{
		serverName.clear();
		accountName.clear();
		accountPassword.clear();
		characterName.clear();
		raceName.clear();
		className.clear();
		genderName = "Male";
	}

	[[nodiscard]] bool IsValid() const
	{
		return !serverName.empty()
			&& !accountName.empty()
			&& !accountPassword.empty()
			&& !characterName.empty()
			&& !raceName.empty()
			&& !className.empty();
	}
};

// ============================================================================
// Window helper functions
//
// Non-inline helpers with HAS_GAMEFACE_UI dual-path support.
// Implementations in MQ2CharCreate.cpp, mirroring MQ2AutoLogin's pattern.
// Prefixed with CC_ to avoid symbol collisions if both headers are ever
// included in the same translation unit.
// ============================================================================

void CC_SendWndNotification(CXWnd* pWnd, CXWnd* sender, uint32_t msg, void* data = nullptr);
CXStr CC_GetWindowText(CXWnd* pWnd);
CXStr CC_GetEditWndText(CEditWnd* pWnd);
CXStr CC_GetSTMLText(CStmlWnd* pWnd);
void CC_SetEditWndText(CEditWnd* pWnd, std::string_view text);

// Name resolution: maps user input (abbreviation or full name, any case) to
// the proper-cased name used in EQ button names (e.g., "shd" → "Shadow Knight").
std::string CC_ResolveRaceName(const std::string& input);
std::string CC_ResolveClassName(const std::string& input);

// Inline template helpers (same logic as MQ2AutoLogin.h)

template <typename T = CXWnd>
inline T* CC_GetWindow(const std::string& name)
{
	return static_cast<T*>(FindMQ2Window(name.c_str()));
}

template <typename T = CXWnd>
inline T* CC_GetChildWindow(CXWnd* parentWnd, const std::string& child)
{
	if (!pSidlMgr || !parentWnd) return nullptr;
	CXMLDataManager* pXmlMgr = pSidlMgr->GetParamManager();
	if (pXmlMgr)
		return static_cast<T*>(parentWnd->GetChildItem(pXmlMgr, CXStr{ child }));
	return nullptr;
}

template <typename T = CXWnd>
inline T* CC_GetChildWindow(const std::string& parent, const std::string& child)
{
	return CC_GetChildWindow<T>(CC_GetWindow<CXWnd>(parent), child);
}

inline bool CC_IsWindowActive(const std::string& name)
{
	const CXWnd* pWnd = CC_GetWindow(name);
	return pWnd != nullptr && pWnd->IsVisible() && pWnd->IsEnabled();
}

template <typename T = CXWnd>
inline T* CC_GetActiveWindow(const std::string& name)
{
	auto pWindow = CC_GetWindow<T>(name);
	if (pWindow != nullptr && pWindow->IsVisible() && pWindow->IsEnabled())
		return pWindow;
	return nullptr;
}

template <typename T = CXWnd>
inline T* CC_GetActiveChildWindow(CXWnd* parentWnd, const std::string& child)
{
	if (!pSidlMgr || !parentWnd) return nullptr;
	CXMLDataManager* pXmlMgr = pSidlMgr->GetParamManager();
	if (pXmlMgr)
	{
		T* pChild = static_cast<T*>(parentWnd->GetChildItem(pXmlMgr, CXStr{ child }));
		if (pChild && pChild->IsVisible() && pChild->IsEnabled())
			return pChild;
	}
	return nullptr;
}

template <typename T = CXWnd>
inline T* CC_GetActiveChildWindow(const std::string& parent, const std::string& child)
{
	return CC_GetActiveChildWindow<T>(CC_GetActiveWindow<CXWnd>(parent), child);
}

// ============================================================================
// FSM detected states (what window is currently visible)
// ============================================================================

enum class CCDetectedState
{
	SplashScreen,
	Connect,
	ConnectConfirm,
	ServerSelect,
	ServerSelectConfirm,
	ServerSelectKick,
	CharacterSelect,
	CharacterCreate,    // CharacterCreation window visible
};

// ============================================================================
// FSM events
// ============================================================================

struct CCStateSensor : tinyfsm::Event
{
	CCDetectedState State;
	CXWnd* Window;

	CCStateSensor(CCDetectedState state, CXWnd* window) : State(state), Window(window) {}

	CCStateSensor(const CCStateSensor&) = delete;
	CCStateSensor& operator=(const CCStateSensor&) = delete;
};

struct StartCharCreate : tinyfsm::Event
{
	CharCreateRequest Request;

	StartCharCreate(CharCreateRequest request) : Request(std::move(request)) {}

	StartCharCreate(const StartCharCreate&) = delete;
	StartCharCreate& operator=(const StartCharCreate&) = delete;
};

struct StopCharCreate : tinyfsm::Event {};

struct CCGameStateChanged : tinyfsm::Event
{
	int GameState;

	CCGameStateChanged(int gameState) : GameState(gameState) {}

	CCGameStateChanged(const CCGameStateChanged&) = delete;
	CCGameStateChanged& operator=(const CCGameStateChanged&) = delete;
};

// ============================================================================
// FSM base class
//
// Follows the same tinyfsm pattern as MQ2AutoLogin's Login class.
// Static members are shared across all states (tinyfsm design).
// ============================================================================

class CharCreate : public tinyfsm::Fsm<CharCreate>
{
protected:
	// The current creation request (null when idle)
	static inline std::unique_ptr<CharCreateRequest> m_request;

	// Current detected UI window
	static inline CXWnd* m_currentWindow = nullptr;

	// Whether a creation is in progress
	static inline bool m_active = false;

	// Delay timer for throttling transitions
	static inline uint64_t m_delayTime = 0;

	// Last detected state (for change detection in transit_condition)
	static inline CCDetectedState m_lastDetectedState = CCDetectedState::Connect;

	// Status strings for ImGui overlay and /charcreate status
	static inline std::string m_statusMessage = "Idle";
	static inline std::string m_errorMessage;

	// Whether we've already populated the DB for this creation
	static inline bool m_dbPopulated = false;

public:
	virtual ~CharCreate() {}

	// Default event handlers — states override the ones they care about.

	virtual void react(const CCStateSensor&) {}

	virtual void react(const StartCharCreate& e)
	{
		m_request = std::make_unique<CharCreateRequest>(e.Request);
		m_active = true;
		m_errorMessage.clear();
		m_dbPopulated = false;
		m_statusMessage = "Starting character creation...";
	}

	virtual void react(const StopCharCreate&)
	{
		m_active = false;
		m_request.reset();
		m_statusMessage = "Stopped";
	}

	virtual void react(const CCGameStateChanged&) {}

	virtual void entry() {}

	virtual void exit() {}

	// Public getters for ImGui overlay and slash command
	static bool is_active() { return m_active && m_request != nullptr; }
	static const std::string& status_message() { return m_statusMessage; }
	static const std::string& error_message() { return m_errorMessage; }
	static const CharCreateRequest* request() { return m_request.get(); }
};
