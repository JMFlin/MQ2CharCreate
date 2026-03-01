/*
 * MQ2CharCreate — State machine implementations
 *
 * FSM states for the character creation flow:
 *   CCWait → login → server select → char select → create form (DB populate) → enter world
 *
 * Mirrors MQ2AutoLogin's tinyfsm patterns. The hub state (CCWait) dispatches
 * to action states based on detected windows. Action states do their work in
 * entry() and transit back to CCWait.
 */

#include <login/Login.h>
#include "MQ2CharCreate.h"

#include <fmt/format.h>
#include <filesystem>

// State class forward declarations
class CCWait;
class CCSplashScreen;
class CCDoLogin;
class CCLoginConfirm;
class CCDoServerSelect;
class CCServerSelectConfirm;
class CCServerSelectKick;
class CCAtCharSelect;
class CCFillCreateForm;
class CCInGameDone;

// ============================================================================
// Helper: Find server in server list
// Mirrors MQ2AutoLogin's ServerSelect::GetServer logic.
// ============================================================================

static EQLS::EQClientServerData* FindServer(std::string& serverName)
{
	if (GetGameState() != GAMESTATE_PRECHARSELECT)
		return nullptr;
	if (!g_pLoginClient)
		return nullptr;

	// Try direct server ID lookup first
	ServerID serverId = GetServerIDFromServerName(serverName.c_str());

	auto server_it = g_pLoginClient->ServerList.end();
	if (serverId != ServerID::Invalid)
	{
		server_it = std::find_if(g_pLoginClient->ServerList.begin(), g_pLoginClient->ServerList.end(),
			[serverId](EQLS::EQClientServerData* s) { return s->ID == serverId; });
	}

	// Fallback: search by name (short name → long name mappings)
	if (server_it == g_pLoginClient->ServerList.end())
	{
		std::vector server_names = { serverName };
		for (const auto& name : login::db::ReadLongServer(serverName))
			server_names.emplace_back(name);

		server_it = std::find_if(g_pLoginClient->ServerList.begin(), g_pLoginClient->ServerList.end(),
			[&server_names](EQLS::EQClientServerData* s)
			{
				return std::find_if(
					server_names.begin(),
					server_names.end(),
					[&name = s->ServerName](const std::string& long_name)
					{ return ci_equals(name, long_name); }) != server_names.end();
			});
	}

	if (server_it != g_pLoginClient->ServerList.end())
	{
		EQLS::EQClientServerData* serverData = *server_it;

		// Block TrueBox servers (same check as MQ2AutoLogin)
		if (!serverData || serverData->TrueBoxStatus == 1)
			return nullptr;

		return serverData;
	}

	return nullptr;
}

// ============================================================================
// CCWait — Hub state (also the initial state)
//
// When m_active is false, does nothing (effectively idle).
// When m_active is true, dispatches to action states based on detected windows.
// Same pattern as MQ2AutoLogin's Wait state.
// ============================================================================

class CCWait : public CharCreate
{
protected:
	bool transit_condition(const CCStateSensor& e)
	{
		if (GetGameState() == GAMESTATE_PRECHARSELECT && !g_pLoginClient)
			return false; // no offsets yet

		if (e.State == m_lastDetectedState && m_delayTime > MQGetTickCount64())
			return false; // unchanged state + delay still active

		if (e.State == CCDetectedState::SplashScreen)
			return true; // always click through splash screens

		// Must have an active request to proceed
		return m_active && m_request != nullptr;
	}

public:
	void entry() override
	{
		uint64_t new_delay = MQGetTickCount64() + 2000;
		m_delayTime = new_delay > m_delayTime ? new_delay : m_delayTime;
	}

	void react(const StartCharCreate& e) override
	{
		// Store request and mark active. On next pulse, transit_condition will
		// allow dispatching to the appropriate action state.
		CharCreate::react(e);

		WriteChatf("\ag[CharCreate]\ax Starting: \ay%s\ax on \ay%s\ax (%s %s)",
			m_request->characterName.c_str(),
			m_request->serverName.c_str(),
			m_request->raceName.c_str(),
			m_request->className.c_str());
	}

	void react(const CCStateSensor& e) override
	{
		m_currentWindow = e.Window;
		CC_DebugLog("CCWait::react(CCStateSensor) state=%d transit_condition=%s",
			static_cast<int>(e.State), transit_condition(e) ? "true" : "false");

		if (transit_condition(e))
		{
			switch (e.State)
			{
			case CCDetectedState::SplashScreen:
				transit<CCSplashScreen>();
				break;
			case CCDetectedState::Connect:
				transit<CCDoLogin>();
				break;
			case CCDetectedState::ConnectConfirm:
				transit<CCLoginConfirm>();
				break;
			case CCDetectedState::ServerSelect:
				transit<CCDoServerSelect>();
				break;
			case CCDetectedState::ServerSelectConfirm:
				transit<CCServerSelectConfirm>();
				break;
			case CCDetectedState::ServerSelectKick:
				transit<CCServerSelectKick>();
				break;
			case CCDetectedState::CharacterSelect:
				transit<CCAtCharSelect>();
				break;
			case CCDetectedState::CharacterCreate:
				transit<CCFillCreateForm>();
				break;
			}

			m_lastDetectedState = e.State;
		}
	}

	void react(const CCGameStateChanged& e) override
	{
		if (e.GameState == GAMESTATE_INGAME && m_active)
		{
			transit<CCInGameDone>();
		}
	}
};

// ============================================================================
// CCSplashScreen — Click through splash screens
// ============================================================================

class CCSplashScreen : public CharCreate
{
public:
	void entry() override
	{
		m_statusMessage = "Clicking splash screen...";

		CXPoint point(1, 1);
		if (g_pLoginViewManager)
			g_pLoginViewManager->HandleLButtonUp(point);

		transit<CCWait>();
	}
};

// ============================================================================
// CCDoLogin — Fill username/password fields and click Connect
// Mirrors MQ2AutoLogin's Connect state.
// ============================================================================

class CCDoLogin : public CharCreate
{
public:
	void entry() override
	{
		m_statusMessage = "Logging in...";

		if (!m_request)
		{
			dispatch(StopCharCreate());
			transit<CCWait>();
			return;
		}

		if (auto pUsernameEdit = CC_GetChildWindow<CEditWnd>(m_currentWindow, "LOGIN_UsernameEdit"))
		{
			DWORD oldscreenmode = std::exchange(ScreenMode, 3);

			CC_SetEditWndText(pUsernameEdit, m_request->accountName);

			if (auto pPasswordEdit = CC_GetChildWindow<CEditWnd>(m_currentWindow, "LOGIN_PasswordEdit"))
			{
				CC_SetEditWndText(pPasswordEdit, m_request->accountPassword);

				if (auto pConnectButton = CC_GetChildWindow<CButtonWnd>(m_currentWindow, "LOGIN_ConnectButton"))
					CC_SendWndNotification(pConnectButton, pConnectButton, XWM_LCLICK);
			}

			ScreenMode = oldscreenmode;
		}

		transit<CCWait>();
	}
};

// ============================================================================
// CCLoginConfirm — Handle login confirmation/error dialogs
// Mirrors MQ2AutoLogin's ConnectConfirm state.
// ============================================================================

class CCLoginConfirm : public CharCreate
{
public:
	void entry() override
	{
		m_statusMessage = "Handling login response...";

		if (CXWnd* pWnd = CC_GetChildWindow(m_currentWindow, "OK_Display"))
		{
			CXStr str = CC_GetWindowText(pWnd);

			if (ci_find_substr(str, "Logging in to the server") != -1)
			{
				// Success — server select will appear next
				m_statusMessage = "Login successful, waiting for server list...";
			}
			else if (ci_find_substr(str, "password were not valid") != -1
				|| ci_find_substr(str, "Invalid Password") != -1
				|| ci_find_substr(str, "You need to enter a username and password") != -1
				|| ci_find_substr(str, "account be activated") != -1)
			{
				m_errorMessage = fmt::format("Login failed: {}", std::string(str));
				WriteChatf("\ar[CharCreate]\ax Login failed: %s", str.c_str());
				dispatch(StopCharCreate());
			}
			else if (ci_find_substr(str, "OFFLINE TRADER") != -1)
			{
				// Click Yes to boot the offline trader
				if (CXWnd* pButton = CC_GetChildWindow(m_currentWindow, "YESNO_YesButton"))
					CC_SendWndNotification(pButton, pButton, XWM_LCLICK);
			}
			else
			{
				// Unknown dialog — click OK to dismiss and retry
				if (CXWnd* pButton = CC_GetChildWindow(m_currentWindow, "OK_OKButton"))
					CC_SendWndNotification(pButton, pButton, XWM_LCLICK);

				m_delayTime = MQGetTickCount64() + 2000;
			}
		}

		transit<CCWait>();
	}
};

// ============================================================================
// CCDoServerSelect — Find and join the target server
// Mirrors MQ2AutoLogin's ServerSelect state.
// ============================================================================

class CCDoServerSelect : public CharCreate
{
public:
	void entry() override
	{

		if (!m_request || m_request->serverName.empty())
		{
			m_errorMessage = "No server specified";
			WriteChatf("\ar[CharCreate]\ax No server specified.");
			dispatch(StopCharCreate());
			transit<CCWait>();
			return;
		}

		m_statusMessage = fmt::format("Selecting server {}...", m_request->serverName);

		std::string serverName = m_request->serverName;
		auto server = FindServer(serverName);

		if (!server)
		{
			// Server not found in list yet — wait for it to appear
			transit<CCWait>();
			return;
		}

		if (server->StatusFlags & (EQLS::eServerStatus_Down | EQLS::eServerStatus_Locked))
		{
			m_errorMessage = fmt::format("Server {} is down or locked", m_request->serverName);
			WriteChatf("\ar[CharCreate]\ax Server \ay%s\ax is down or locked.", m_request->serverName.c_str());
			dispatch(StopCharCreate());
			transit<CCWait>();
			return;
		}

		// Join the server
		g_pLoginServerAPI->JoinServer((int)server->ID);
		m_statusMessage = fmt::format("Joining server {}...", m_request->serverName);
		transit<CCWait>();
	}
};

// ============================================================================
// CCServerSelectConfirm — Handle server select confirmation dialogs
// Mirrors MQ2AutoLogin's ServerSelectConfirm state.
// ============================================================================

class CCServerSelectConfirm : public CharCreate
{
public:
	void entry() override
	{
		m_statusMessage = "Handling server select response...";

		if (CXWnd* pWnd = CC_GetChildWindow(m_currentWindow, "OK_Display"))
		{
			CXStr str = CC_GetWindowText(pWnd);

			if (str.find("maximum capacity") != CXStr::npos)
			{
				m_errorMessage = "Server at maximum capacity";
				WriteChatf("\ar[CharCreate]\ax Server at maximum capacity. Aborting.");
				dispatch(StopCharCreate());
			}
			else if (str.find("not a free-play server") != CXStr::npos)
			{
				m_errorMessage = "Not a free-play server";
				WriteChatf("\ar[CharCreate]\ax Server requires subscription. Aborting.");
				dispatch(StopCharCreate());
			}
			else
			{
				// Unknown — click OK to dismiss
				if (auto pButton = CC_GetActiveChildWindow<CButtonWnd>(m_currentWindow, "OK_OKButton"))
					CC_SendWndNotification(pButton, pButton, XWM_LCLICK);

				m_delayTime = MQGetTickCount64() + 1000;
			}
		}

		transit<CCWait>();
	}
};

// ============================================================================
// CCServerSelectKick — Handle "already logged in" prompt
// Mirrors MQ2AutoLogin's ServerSelectKick state.
// Always clicks Yes to kick the existing session (we're creating a new char).
// ============================================================================

class CCServerSelectKick : public CharCreate
{
public:
	void entry() override
	{
		m_statusMessage = "Handling kick prompt...";

		if (CXWnd* pWnd = CC_GetChildWindow(m_currentWindow, "YESNO_Display"))
		{
			CXStr str = CC_GetWindowText(pWnd);

			if (str.find("already have a character logged into") != CXStr::npos
				|| str.find("OFFLINE TRADER") != CXStr::npos)
			{
				// Click Yes to kick and proceed
				if (auto pButton = CC_GetActiveChildWindow<CButtonWnd>(m_currentWindow, "YESNO_YesButton"))
					CC_SendWndNotification(pButton, pButton, XWM_LCLICK);
			}
		}

		transit<CCWait>();
	}
};

// ============================================================================
// CCAtCharSelect — At character select, click the "Create Character" button
//
// The Create button (CLW_Create_Button) is a child of CharacterListWnd.
// ============================================================================

class CCAtCharSelect : public CharCreate
{
	static constexpr int MAX_RETRIES = 15; // ~15 seconds at 1s pulse interval
	static inline int m_retryCount = 0;

public:
	void entry() override
	{
		m_statusMessage = "At character select, clicking Create...";
		CC_DebugLog("CCAtCharSelect::entry()");

		// Enumerate children for debugging
		if (auto pParent = CC_GetActiveWindow<CXWnd>(CCWindowNames::CharSelectWindow))
		{
			CC_DebugLog("  CharacterListWnd found, visible=%d enabled=%d",
				pParent->IsVisible(), pParent->IsEnabled());

			if (pSidlMgr)
			{
				if (CXMLDataManager* pXmlMgr = pSidlMgr->GetParamManager())
				{
					// Try direct child lookup
					auto pBtn = pParent->GetChildItem(pXmlMgr, CXStr{ CCWindowNames::CreateButton });
					CC_DebugLog("  GetChildItem('%s') = %p", CCWindowNames::CreateButton, pBtn);
					if (pBtn)
					{
						CC_DebugLog("    visible=%d enabled=%d",
							pBtn->IsVisible(), pBtn->IsEnabled());
					}

					// Also check for ButtonsScreen
					auto pBtnScreen = pParent->GetChildItem(pXmlMgr, CXStr{ "CLW_ButtonsScreen" });
					CC_DebugLog("  GetChildItem('CLW_ButtonsScreen') = %p", pBtnScreen);
					if (pBtnScreen)
					{
						CC_DebugLog("    visible=%d enabled=%d",
							pBtnScreen->IsVisible(), pBtnScreen->IsEnabled());
						// Try button inside ButtonsScreen
						auto pBtnInner = pBtnScreen->GetChildItem(pXmlMgr, CXStr{ CCWindowNames::CreateButton });
						CC_DebugLog("    GetChildItem('%s') inside ButtonsScreen = %p",
							CCWindowNames::CreateButton, pBtnInner);
						if (pBtnInner)
							CC_DebugLog("      visible=%d enabled=%d", pBtnInner->IsVisible(), pBtnInner->IsEnabled());
					}
				}
			}
		}
		else
		{
			CC_DebugLog("  CharacterListWnd NOT found/active");
		}

		if (auto pCreateBtn = CC_GetActiveChildWindow<CButtonWnd>(
				CCWindowNames::CharSelectWindow, CCWindowNames::CreateButton))
		{
			CC_DebugLog("  Clicking Create button");
			CC_SendWndNotification(pCreateBtn, pCreateBtn, XWM_LCLICK);
			m_statusMessage = "Clicked Create, waiting for creation screen...";
			m_retryCount = 0;
		}
		else
		{
			++m_retryCount;
			CC_DebugLog("  Create button not clickable (retry %d/%d)", m_retryCount, MAX_RETRIES);
			if (m_retryCount >= MAX_RETRIES)
			{
				CC_DebugLog("  Giving up after %d retries", MAX_RETRIES);
				WriteChatf("\ay[CharCreate]\ax Create button '%s' not found/enabled in '%s' after %d retries.",
					CCWindowNames::CreateButton, CCWindowNames::CharSelectWindow, MAX_RETRIES);
				m_errorMessage = "Create button not found";
				m_retryCount = 0;
				dispatch(StopCharCreate());
			}
		}

		transit<CCWait>();
	}
};

// ============================================================================
// CCFillCreateForm — Drive the multi-screen character creation wizard
//
// EQ's creation UI is a wizard with individual buttons per race/class/gender:
//   RaceScreen → GenderScreen → ClassScreen → OptionsScreen → NameScreen
//
// Each entry() call detects which sub-screen is currently visible and clicks
// the appropriate button, then transits back to CCWait. On the next pulse,
// the wizard will have advanced to the next screen and we'll handle that one.
//
// Button naming patterns (discovered via hover logging):
//   CC_Race_{RaceName}_Button       e.g. CC_Race_Drakkin_Button
//   CC_{Gender}_Button              e.g. CC_Male_Button
//   CC_Class_{ClassName}_Button     e.g. CC_Class_Shadow Knight_Button
// ============================================================================

// All sub-screens (Race, Gender, Class, Options, Name) are visible simultaneously.
// We use a step counter to click one button per pulse, advancing through the form.
enum class CCFormStep { Race, Gender, Class, Tutorial, SetName, ClickCreate, Done };

class CCFillCreateForm : public CharCreate
{
	static inline CCFormStep s_step = CCFormStep::Race;

public:
	void entry() override
	{
		if (!m_request)
		{
			dispatch(StopCharCreate());
			transit<CCWait>();
			return;
		}

		CXWnd* pCreateWnd = CC_GetActiveWindow(CCWindowNames::CreateWindow);
		if (!pCreateWnd)
		{
			CC_DebugLog("CCFillCreateForm: CharacterCreation not active, retrying");
			transit<CCWait>();
			return;
		}

		CC_DebugLog("CCFillCreateForm::entry() step=%d", static_cast<int>(s_step));

		switch (s_step)
		{
		case CCFormStep::Race:
			if (ClickButton(pCreateWnd, fmt::format("CC_Race_{}_Button", m_request->raceName)))
			{
				CC_DebugLog("  Clicked race: %s", m_request->raceName.c_str());
				WriteChatf("\ag[CharCreate]\ax Race: \ay%s\ax", m_request->raceName.c_str());
				s_step = CCFormStep::Gender;
			}
			else
			{
				CC_DebugLog("  Race button not found: CC_Race_%s_Button", m_request->raceName.c_str());
				WriteChatf("\ar[CharCreate]\ax Race button not found: CC_Race_%s_Button",
					m_request->raceName.c_str());
				s_step = CCFormStep::Race;
				dispatch(StopCharCreate());
			}
			break;

		case CCFormStep::Gender:
		{
			// Gender buttons are toggles — Male is pressed by default.
			// Only click if the desired gender isn't already selected.
			std::string wantedBtn = fmt::format("CC_{}_Button", m_request->genderName);
			auto pGenderBtn = CC_GetChildWindow<CButtonWnd>(pCreateWnd, wantedBtn);
			if (pGenderBtn)
			{
				bool alreadyChecked = pGenderBtn->IsChecked();
				CC_DebugLog("  Gender '%s' checked=%d", wantedBtn.c_str(), alreadyChecked);
				if (!alreadyChecked)
				{
					SendWndClick2(pGenderBtn, "leftmouseup");
					CC_DebugLog("  Clicked gender: %s", m_request->genderName.c_str());
				}
				else
				{
					CC_DebugLog("  Gender already selected, skipping click");
				}
				WriteChatf("\ag[CharCreate]\ax Gender: \ay%s\ax", m_request->genderName.c_str());
				s_step = CCFormStep::Class;
			}
			else
			{
				CC_DebugLog("  Gender button not found: %s", wantedBtn.c_str());
				WriteChatf("\ar[CharCreate]\ax Gender button not found");
				s_step = CCFormStep::Race;
				dispatch(StopCharCreate());
			}
			break;
		}

		case CCFormStep::Class:
			if (ClickButton(pCreateWnd, fmt::format("CC_Class_{}_Button", m_request->className)))
			{
				CC_DebugLog("  Clicked class: %s", m_request->className.c_str());
				WriteChatf("\ag[CharCreate]\ax Class: \ay%s\ax", m_request->className.c_str());
				s_step = CCFormStep::Tutorial;
			}
			else
			{
				CC_DebugLog("  Class button not found: CC_Class_%s_Button", m_request->className.c_str());
				WriteChatf("\ar[CharCreate]\ax Class button not found: CC_Class_%s_Button (invalid for %s?)",
					m_request->className.c_str(), m_request->raceName.c_str());
				s_step = CCFormStep::Race;
				dispatch(StopCharCreate());
			}
			break;

		case CCFormStep::Tutorial:
		{
			// Tutorial is a toggle button — pressed/down = tutorial enabled.
			// It defaults to ON. Click it to toggle OFF.
			auto pTutBtn = CC_GetChildWindow<CButtonWnd>(pCreateWnd, CCWindowNames::TutorialButton);
			if (pTutBtn)
			{
				bool checked = pTutBtn->IsChecked();
				CC_DebugLog("  Tutorial button found, checked=%d", checked);
				if (checked)
				{
					SendWndClick2(pTutBtn, "leftmouseup");
					CC_DebugLog("  Clicked tutorial to toggle OFF, now checked=%d", pTutBtn->IsChecked());
					WriteChatf("\ag[CharCreate]\ax Tutorial: disabled.");
				}
				else
				{
					CC_DebugLog("  Tutorial already off, skipping");
				}
			}
			else
			{
				CC_DebugLog("  Tutorial button not found, skipping");
			}
			s_step = CCFormStep::SetName;
			break;
		}

		case CCFormStep::SetName:
		{
			auto pNameEdit = CC_GetChildWindow<CEditWnd>(pCreateWnd, CCWindowNames::NameEdit);
			if (pNameEdit)
			{
				CC_SetEditWndText(pNameEdit, m_request->characterName);
				CC_DebugLog("  Set name: %s", m_request->characterName.c_str());
				WriteChatf("\ag[CharCreate]\ax Name: \ay%s\ax", m_request->characterName.c_str());
				s_step = CCFormStep::ClickCreate;
			}
			else
			{
				CC_DebugLog("  Name edit '%s' not found", CCWindowNames::NameEdit);
				WriteChatf("\ar[CharCreate]\ax Name edit '%s' not found.", CCWindowNames::NameEdit);
				s_step = CCFormStep::Race;
				dispatch(StopCharCreate());
			}
			break;
		}

		case CCFormStep::ClickCreate:
			if (ClickButton(pCreateWnd, CCWindowNames::FinalCreateButton))
			{
				CC_DebugLog("  Clicked final Create button");
				WriteChatf("\ag[CharCreate]\ax Clicked Create, entering world...");
				m_statusMessage = "Character created, entering world...";
				PopulateDatabase();
				s_step = CCFormStep::Race; // reset for next time
			}
			else
			{
				CC_DebugLog("  Final Create button '%s' not found", CCWindowNames::FinalCreateButton);
				WriteChatf("\ar[CharCreate]\ax Create button '%s' not found.", CCWindowNames::FinalCreateButton);
				s_step = CCFormStep::Race;
				dispatch(StopCharCreate());
			}
			break;

		default:
			s_step = CCFormStep::Race;
			break;
		}

		transit<CCWait>();
	}

private:
	void PopulateDatabase()
	{
		if (!m_request || m_dbPopulated)
			return;

		ProfileRecord profile;
		profile.accountName = m_request->accountName;
		profile.accountPassword = m_request->accountPassword;
		profile.serverName = GetServerShortName();
		profile.characterName = m_request->characterName;

		// Determine server type from the EQ executable path
		char path[MAX_PATH] = { 0 };
		GetModuleFileName(nullptr, path, MAX_PATH);
		const std::filesystem::path fs_path(path);

		if (const auto server_type = login::db::GetServerTypeFromPath(fs_path.parent_path().string()))
			profile.serverType = *server_type;
		else
			profile.serverType = GetBuildTargetName(static_cast<BuildTarget>(gBuild));

		to_lower(profile.serverType);
		to_lower(profile.accountName);

		// 1. Ensure server short↔long name mapping exists
		login::db::CreateOrUpdateServer(GetServerShortName(), m_request->serverName);

		// 2. Create account entry (idempotent if it already exists)
		login::db::CreateAccount(profile);

		// 3. Create character entry
		to_lower(profile.characterName);
		login::db::CreateCharacter(profile);

		// 4. Create persona (class info) — derive from request since pLocalPlayer
		//    isn't available yet at create-button click time
		std::string shortClass = CC_ClassNameToShortName(m_request->className);
		if (!shortClass.empty())
		{
			profile.characterClass = shortClass;
			profile.characterLevel = 1; // new character is always level 1
			login::db::CreatePersona(profile);
		}
		else
		{
			CC_DebugLog("PopulateDatabase: Could not resolve class '%s' to short name, skipping persona",
				m_request->className.c_str());
		}

		m_dbPopulated = true;
	}

	// Find and click a button anywhere inside the creation window hierarchy.
	// Uses SendWndClick2 (LButtonDown + LButtonUp) which works for toggle
	// buttons and buttons that may be scrolled out of view.
	bool ClickButton(CXWnd* pParent, const std::string& buttonName)
	{
		auto pButton = CC_GetChildWindow<CButtonWnd>(pParent, buttonName);
		if (!pButton)
		{
			CC_DebugLog("  ClickButton: '%s' not found", buttonName.c_str());
			return false;
		}
		CC_DebugLog("  ClickButton: '%s' found, visible=%d enabled=%d checked=%d",
			buttonName.c_str(), pButton->IsVisible(), pButton->IsEnabled(), pButton->IsChecked());
		SendWndClick2(pButton, "leftmouseup");
		return true;
	}
};

// ============================================================================
// CCInGameDone — Character is in-game. Show success message and clean up.
//
// DB population already happened at CC_Create_Button click time (in
// CCFillCreateForm::PopulateDatabase). This state just provides the
// user-facing success message and resets m_active / m_request.
// ============================================================================

class CCInGameDone : public CharCreate
{
public:
	void entry() override
	{
		if (!m_request)
		{
			m_active = false;
			transit<CCWait>();
			return;
		}

		WriteChatf("\ag[CharCreate]\ax Character \ay%s\ax created successfully on \ay%s\ax!",
			m_request->characterName.c_str(), m_request->serverName.c_str());
		WriteChatf("\ag[CharCreate]\ax Character added to MQ2AutoLogin database.");

		m_statusMessage = "Done!";
		m_active = false;
		m_request.reset();
		transit<CCWait>();
	}
};

// ============================================================================
// Initial state
// ============================================================================

FSM_INITIAL_STATE(CharCreate, CCWait)
