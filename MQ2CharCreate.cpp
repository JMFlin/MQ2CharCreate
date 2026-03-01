/*
 * MQ2CharCreate — Plugin lifecycle, slash commands, OnPulse, ImGui overlay
 *
 * This file contains:
 * - Plugin boilerplate (Initialize, Shutdown, SetGameState)
 * - Window helper implementations (HAS_GAMEFACE_UI dual-path, mirrors MQ2AutoLogin)
 * - /charcreate slash command
 * - OnPulse window detection and sensor dispatching
 * - ImGui status overlay
 */

#include <login/Login.h>
#include "MQ2CharCreate.h"

#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <unordered_map>

// Debug log — writes to MQ config dir so it's visible even during PRECHARSELECT
void CC_DebugLog(const char* fmt_str, ...)
{
	static std::string s_logPath;
	if (s_logPath.empty())
	{
		s_logPath = (std::filesystem::path(gPathConfig) / "MQ2CharCreate_debug.log").string();
	}

	std::ofstream ofs(s_logPath, std::ios::app);
	if (!ofs) return;

	// timestamp
	SYSTEMTIME st;
	GetLocalTime(&st);
	char timeBuf[64];
	snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	char msgBuf[2048];
	va_list args;
	va_start(args, fmt_str);
	vsnprintf(msgBuf, sizeof(msgBuf), fmt_str, args);
	va_end(args);

	ofs << "[" << timeBuf << "] " << msgBuf << "\n";
}

PLUGIN_VERSION(1.0);
PreSetup("MQ2CharCreate");

constexpr int STEP_DELAY = 1000; // ms between window detection cycles
static uint64_t s_reenableTime = 0;

// ============================================================================
// Window helper implementations
//
// These mirror MQ2AutoLogin's implementations exactly, including the
// HAS_GAMEFACE_UI branching needed for GAMESTATE_PRECHARSELECT where eqmain
// uses a different set of UI types.
// ============================================================================

void CC_SendWndNotification(CXWnd* pWnd, CXWnd* sender, uint32_t msg, void* data)
{
	if (!pWnd) return;

#if HAS_GAMEFACE_UI
	if (GetGameState() == GAMESTATE_PRECHARSELECT)
		reinterpret_cast<eqlib::eqmain::CXWnd*>(pWnd)->WndNotification(
			reinterpret_cast<eqlib::eqmain::CXWnd*>(sender), msg, data);
	else
#endif
		pWnd->WndNotification(sender, msg, data);
}

CXStr CC_GetWindowText(CXWnd* pWnd)
{
	if (!pWnd) return CXStr();

	CXMLDataManager* pXmlMgr = pSidlMgr ? pSidlMgr->GetParamManager() : nullptr;
	if (!pXmlMgr) return CXStr();
	auto type = pXmlMgr->GetWindowType(pWnd);

#if HAS_GAMEFACE_UI
	if (GetGameState() == GAMESTATE_PRECHARSELECT)
	{
		return type == UI_STMLBox
			? reinterpret_cast<eqlib::eqmain::CStmlWnd*>(pWnd)->STMLText
			: reinterpret_cast<eqlib::eqmain::CXWnd*>(pWnd)->GetWindowText();
	}
#endif

	return type == UI_STMLBox
		? static_cast<CStmlWnd*>(pWnd)->STMLText
		: pWnd->GetWindowText();
}

CXStr CC_GetEditWndText(CEditWnd* pWnd)
{
	if (!pWnd) return CXStr();

#if HAS_GAMEFACE_UI
	if (GetGameState() == GAMESTATE_PRECHARSELECT)
	{
		return reinterpret_cast<eqlib::eqmain::CEditBaseWnd*>(pWnd)->InputText;
	}
#endif

	return pWnd->InputText;
}

void CC_SetEditWndText(CEditWnd* pWnd, std::string_view text)
{
#if HAS_GAMEFACE_UI
	if (GetGameState() == GAMESTATE_PRECHARSELECT)
	{
		reinterpret_cast<eqlib::eqmain::CEditBaseWnd*>(pWnd)->InputText = text;
	}
	else
#endif
	{
		pWnd->InputText = text;
	}
}

CXStr CC_GetSTMLText(CStmlWnd* pWnd)
{
	if (!pWnd) return CXStr();

#if HAS_GAMEFACE_UI
	if (GetGameState() == GAMESTATE_PRECHARSELECT)
	{
		return reinterpret_cast<eqlib::eqmain::CStmlWnd*>(pWnd)->STMLText;
	}
#endif

	return pWnd->STMLText;
}

// ============================================================================
// Race/class name resolution
//
// Maps user input (abbreviation or full name, any case) to the proper-cased
// name used in EQ's button names. For example:
//   "shd" → "Shadow Knight"  (abbreviation)
//   "shadow knight" → "Shadow Knight"  (case normalization)
//   "Drakkin" → "Drakkin"  (already correct)
// ============================================================================

std::string CC_ResolveRaceName(const std::string& input)
{
	static const std::unordered_map<std::string, std::string> s_raceMap = {
		// Full names (lowercase key → proper case value)
		{"human", "Human"}, {"barbarian", "Barbarian"}, {"erudite", "Erudite"},
		{"wood elf", "Wood Elf"}, {"high elf", "High Elf"}, {"dark elf", "Dark Elf"},
		{"half elf", "Half Elf"}, {"dwarf", "Dwarf"}, {"troll", "Troll"},
		{"ogre", "Ogre"}, {"halfling", "Halfling"}, {"gnome", "Gnome"},
		{"iksar", "Iksar"}, {"vah shir", "Vah Shir"}, {"froglok", "Froglok"},
		{"drakkin", "Drakkin"},
		// Common abbreviations
		{"hum", "Human"}, {"bar", "Barbarian"}, {"eru", "Erudite"},
		{"welf", "Wood Elf"}, {"helf", "High Elf"}, {"delf", "Dark Elf"},
		{"haf", "Half Elf"}, {"dwf", "Dwarf"}, {"trl", "Troll"},
		{"ogr", "Ogre"}, {"hfl", "Halfling"}, {"gnm", "Gnome"},
		{"iks", "Iksar"}, {"vsh", "Vah Shir"}, {"frg", "Froglok"},
		{"drk", "Drakkin"},
	};

	std::string lower = input;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

	auto it = s_raceMap.find(lower);
	if (it != s_raceMap.end())
		return it->second;

	// Not found in map — return input as-is but warn the user
	WriteChatf("\ay[CharCreate]\ax Warning: Race '%s' not recognized — using as-is. "
		"Button name will be CC_Race_%s_Button.", input.c_str(), input.c_str());
	return input;
}

std::string CC_ResolveClassName(const std::string& input)
{
	static const std::unordered_map<std::string, std::string> s_classMap = {
		// Full names (lowercase key → proper case value)
		{"warrior", "Warrior"}, {"cleric", "Cleric"}, {"paladin", "Paladin"},
		{"ranger", "Ranger"}, {"shadow knight", "Shadow Knight"}, {"druid", "Druid"},
		{"monk", "Monk"}, {"bard", "Bard"}, {"rogue", "Rogue"},
		{"shaman", "Shaman"}, {"necromancer", "Necromancer"}, {"wizard", "Wizard"},
		{"magician", "Magician"}, {"enchanter", "Enchanter"},
		{"beastlord", "Beastlord"}, {"berserker", "Berserker"},
		// Common abbreviations
		{"war", "Warrior"}, {"clr", "Cleric"}, {"pal", "Paladin"},
		{"rng", "Ranger"}, {"shd", "Shadow Knight"}, {"sk", "Shadow Knight"},
		{"dru", "Druid"}, {"mnk", "Monk"}, {"brd", "Bard"},
		{"rog", "Rogue"}, {"shm", "Shaman"}, {"nec", "Necromancer"},
		{"wiz", "Wizard"}, {"mag", "Magician"}, {"enc", "Enchanter"},
		{"bst", "Beastlord"}, {"ber", "Berserker"},
	};

	std::string lower = input;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

	auto it = s_classMap.find(lower);
	if (it != s_classMap.end())
		return it->second;

	WriteChatf("\ay[CharCreate]\ax Warning: Class '%s' not recognized — using as-is. "
		"Button name will be CC_Class_%s_Button.", input.c_str(), input.c_str());
	return input;
}

// ============================================================================
// Class name → EQ uppercase short name mapping
//
// Maps proper-cased class names (as used in EQ button names) to the uppercase
// short names used by MQ2AutoLogin's persona database (e.g., "Shadow Knight" → "SHD").
// Falls back to scanning the ClassInfo array if the static map misses.
// ============================================================================

std::string CC_ClassNameToShortName(const std::string& className)
{
	static const std::unordered_map<std::string, std::string> s_nameToShort = {
		{"warrior", "WAR"}, {"cleric", "CLR"}, {"paladin", "PAL"},
		{"ranger", "RNG"}, {"shadow knight", "SHD"}, {"druid", "DRU"},
		{"monk", "MNK"}, {"bard", "BRD"}, {"rogue", "ROG"},
		{"shaman", "SHM"}, {"necromancer", "NEC"}, {"wizard", "WIZ"},
		{"magician", "MAG"}, {"enchanter", "ENC"},
		{"beastlord", "BST"}, {"berserker", "BER"},
	};

	std::string lower = className;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

	auto it = s_nameToShort.find(lower);
	if (it != s_nameToShort.end())
		return it->second;

	// Fallback: scan ClassInfo array for a matching name
	for (int i = 1; i <= MAX_PLAYER_CLASSES; ++i)
	{
		std::string infoName = ClassInfo[i].Name;
		std::transform(infoName.begin(), infoName.end(), infoName.begin(), ::tolower);
		if (infoName == lower)
		{
			std::string shortName = ClassInfo[i].UCShortName;
			to_upper(shortName);
			return shortName;
		}
	}

	return {};
}

// ============================================================================
// Slash command: /charcreate
//
// Usage:
//   /charcreate <server> <account> <password> <charname> <race> <class> [gender]
//   /charcreate stop
//   /charcreate status
//
// Supports abbreviations for race and class (e.g., "drk" for Drakkin,
// "shd" for Shadow Knight). Multi-word names like "Shadow Knight" can be
// passed as an abbreviation or in quotes. Gender defaults to Male.
// ============================================================================

static void Cmd_CharCreate(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);

	if (ci_equals(arg1, "stop"))
	{
		if (CharCreate::is_active())
		{
			CharCreate::dispatch(StopCharCreate());
			WriteChatf("\ag[CharCreate]\ax Character creation stopped.");
		}
		else
		{
			WriteChatf("\ag[CharCreate]\ax No character creation in progress.");
		}
		return;
	}

	if (ci_equals(arg1, "status"))
	{
		WriteChatf("\ag[CharCreate]\ax Status: %s", CharCreate::status_message().c_str());
		if (!CharCreate::error_message().empty())
			WriteChatf("\ar[CharCreate]\ax Last error: %s", CharCreate::error_message().c_str());
		if (CharCreate::is_active())
		{
			const auto* req = CharCreate::request();
			if (req)
			{
				WriteChatf("\ag[CharCreate]\ax   Server: %s  Account: %s", req->serverName.c_str(), req->accountName.c_str());
				WriteChatf("\ag[CharCreate]\ax   Character: %s  Race: %s  Class: %s  Gender: %s",
					req->characterName.c_str(), req->raceName.c_str(),
					req->className.c_str(), req->genderName.c_str());
			}
		}
		return;
	}

	// Parse full creation command
	char arg2[MAX_STRING] = { 0 };
	char arg3[MAX_STRING] = { 0 };
	char arg4[MAX_STRING] = { 0 };
	char arg5[MAX_STRING] = { 0 };
	char arg6[MAX_STRING] = { 0 };
	char arg7[MAX_STRING] = { 0 };

	GetArg(arg2, szLine, 2);
	GetArg(arg3, szLine, 3);
	GetArg(arg4, szLine, 4);
	GetArg(arg5, szLine, 5);
	GetArg(arg6, szLine, 6);
	GetArg(arg7, szLine, 7);  // optional gender

	if (arg1[0] == 0 || arg2[0] == 0 || arg3[0] == 0 || arg4[0] == 0 || arg5[0] == 0 || arg6[0] == 0)
	{
		WriteChatf("\ag[CharCreate]\ax Usage:");
		WriteChatf("  /charcreate <server> <account> <password> <charname> <race> <class> [gender]");
		WriteChatf("  /charcreate stop");
		WriteChatf("  /charcreate status");
		WriteChatf("\ag[CharCreate]\ax Examples:");
		WriteChatf("  /charcreate Rizlona myaccount mypass Newchar Drakkin Bard");
		WriteChatf("  /charcreate Rizlona myaccount mypass Newchar drk shd Female");
		WriteChatf("\ag[CharCreate]\ax Race/class abbreviations: hum bar eru drk trl ogr dwf gnm iks hfl frg");
		WriteChatf("  war clr pal rng shd dru mnk brd rog shm nec wiz mag enc bst ber");
		return;
	}

	if (CharCreate::is_active())
	{
		WriteChatf("\ar[CharCreate]\ax A character creation is already in progress. Use '/charcreate stop' first.");
		return;
	}

	CharCreateRequest request;
	request.serverName = arg1;
	request.accountName = arg2;
	request.accountPassword = arg3;
	request.characterName = arg4;
	request.raceName = CC_ResolveRaceName(arg5);
	request.className = CC_ResolveClassName(arg6);

	if (arg7[0] != 0)
	{
		if (ci_equals(arg7, "female") || ci_equals(arg7, "f"))
			request.genderName = "Female";
		else if (ci_equals(arg7, "male") || ci_equals(arg7, "m"))
			request.genderName = "Male";
		else
		{
			WriteChatf("\ay[CharCreate]\ax Warning: Unrecognized gender '%s' — defaulting to Male. "
				"Use 'male', 'female', 'm', or 'f'.", arg7);
			request.genderName = "Male";
		}
	}

	if (!request.IsValid())
	{
		WriteChatf("\ar[CharCreate]\ax Invalid request — all fields are required.");
		return;
	}

	WriteChatf("\ag[CharCreate]\ax Resolved: Race=\ay%s\ax  Class=\ay%s\ax  Gender=\ay%s\ax",
		request.raceName.c_str(), request.className.c_str(), request.genderName.c_str());

	CharCreate::dispatch(StartCharCreate(std::move(request)));
}

// ============================================================================
// Plugin lifecycle callbacks
// ============================================================================

PLUGIN_API void InitializePlugin()
{
	// Initialize the login database for profile population after creation.
	// Each DLL gets its own copy of the login static library's globals,
	// so this is independent of MQ2AutoLogin's database connection.
	if (!login::db::InitDatabase((std::filesystem::path(gPathConfig) / "login.db").string()))
	{
		WriteChatf("\ar[CharCreate]\ax Could not load login database. DB population after creation will not work.");
	}

	CharCreate::set_initial_state();

	AddCommand("/charcreate", Cmd_CharCreate);

	s_reenableTime = MQGetTickCount64() + STEP_DELAY;

	// Check for dashboard-triggered creation via MQ2CC_* environment variables.
	// The dashboard sets these before spawning eqgame.exe so the plugin can
	// automatically start character creation without a /charcreate command.
	{
		char envBuf[512] = { 0 };
		CC_DebugLog("=== MQ2CharCreate Initialize ===");
		CC_DebugLog("gPathConfig = %s", gPathConfig);
		if (GetEnvironmentVariableA("MQ2CC_SERVER", envBuf, sizeof(envBuf)))
		{
			CC_DebugLog("MQ2CC_SERVER = '%s'", envBuf);
			CharCreateRequest envRequest;
			envRequest.serverName = envBuf;

			if (GetEnvironmentVariableA("MQ2CC_ACCOUNT", envBuf, sizeof(envBuf)))
			{
				CC_DebugLog("MQ2CC_ACCOUNT = '%s'", envBuf);
				envRequest.accountName = envBuf;
			}
			else
				CC_DebugLog("MQ2CC_ACCOUNT not set!");

			if (GetEnvironmentVariableA("MQ2CC_CHARNAME", envBuf, sizeof(envBuf)))
			{
				CC_DebugLog("MQ2CC_CHARNAME = '%s'", envBuf);
				envRequest.characterName = envBuf;
			}
			if (GetEnvironmentVariableA("MQ2CC_RACE", envBuf, sizeof(envBuf)))
			{
				CC_DebugLog("MQ2CC_RACE = '%s'", envBuf);
				envRequest.raceName = CC_ResolveRaceName(envBuf);
				CC_DebugLog("  resolved race = '%s'", envRequest.raceName.c_str());
			}
			if (GetEnvironmentVariableA("MQ2CC_CLASS", envBuf, sizeof(envBuf)))
			{
				CC_DebugLog("MQ2CC_CLASS = '%s'", envBuf);
				envRequest.className = CC_ResolveClassName(envBuf);
				CC_DebugLog("  resolved class = '%s'", envRequest.className.c_str());
			}
			if (GetEnvironmentVariableA("MQ2CC_GENDER", envBuf, sizeof(envBuf)))
			{
				CC_DebugLog("MQ2CC_GENDER = '%s'", envBuf);
				if (ci_equals(envBuf, "female") || ci_equals(envBuf, "f"))
					envRequest.genderName = "Female";
				else
					envRequest.genderName = "Male";
			}

			// Password: from env var (new account) or from login.db (existing account)
			if (GetEnvironmentVariableA("MQ2CC_PASSWORD", envBuf, sizeof(envBuf)))
			{
				CC_DebugLog("MQ2CC_PASSWORD set (from env var), len=%zu", strlen(envBuf));
				envRequest.accountPassword = envBuf;
			}
			else
			{
				CC_DebugLog("MQ2CC_PASSWORD not set, looking up from login.db...");
				char exePath[MAX_PATH] = { 0 };
				GetModuleFileName(nullptr, exePath, MAX_PATH);
				CC_DebugLog("  exePath = '%s'", exePath);
				const std::filesystem::path fsPath(exePath);
				std::string parentPath = fsPath.parent_path().string();
				CC_DebugLog("  parentPath = '%s'", parentPath.c_str());
				if (const auto serverType = login::db::GetServerTypeFromPath(parentPath))
				{
					CC_DebugLog("  serverType = '%s'", serverType->c_str());
					if (auto pass = login::db::ReadPassword(envRequest.accountName, *serverType))
					{
						CC_DebugLog("  password found, len=%zu", pass->length());
						envRequest.accountPassword = *pass;
					}
					else
						CC_DebugLog("  ReadPassword returned nullopt for account='%s' serverType='%s'",
							envRequest.accountName.c_str(), serverType->c_str());
				}
				else
					CC_DebugLog("  GetServerTypeFromPath returned nullopt!");
			}

			CC_DebugLog("Final request: server='%s' account='%s' char='%s' race='%s' class='%s' gender='%s' hasPassword=%s",
				envRequest.serverName.c_str(), envRequest.accountName.c_str(),
				envRequest.characterName.c_str(), envRequest.raceName.c_str(),
				envRequest.className.c_str(), envRequest.genderName.c_str(),
				envRequest.accountPassword.empty() ? "NO" : "YES");
			CC_DebugLog("IsValid = %s", envRequest.IsValid() ? "true" : "false");

			// Clear all MQ2CC_* env vars to prevent re-triggering
			for (const char* var : { "MQ2CC_SERVER", "MQ2CC_ACCOUNT", "MQ2CC_PASSWORD",
				"MQ2CC_CHARNAME", "MQ2CC_RACE", "MQ2CC_CLASS", "MQ2CC_GENDER" })
			{
				SetEnvironmentVariableA(var, nullptr);
			}

			if (envRequest.IsValid())
			{
				WriteChatf("\ag[CharCreate]\ax Dashboard-triggered creation: %s %s %s on %s",
					envRequest.genderName.c_str(), envRequest.raceName.c_str(),
					envRequest.className.c_str(), envRequest.serverName.c_str());
				CharCreate::dispatch(StartCharCreate(std::move(envRequest)));
			}
			else
			{
				WriteChatf("\ar[CharCreate]\ax Dashboard env vars present but request is incomplete.");
			}
		}
	}

	WriteChatf("\ag[CharCreate]\ax Plugin loaded. Type /charcreate for usage.");
}

PLUGIN_API void ShutdownPlugin()
{
	// Cancel any in-progress creation
	if (CharCreate::is_active())
		CharCreate::dispatch(StopCharCreate());

	RemoveCommand("/charcreate");

	// Each DLL has its own copy of the login static library's database
	// connection, so shutting down ours does not affect MQ2AutoLogin.
	login::db::ShutdownDatabase();
}

PLUGIN_API void SetGameState(int GameState)
{
	CharCreate::dispatch(CCGameStateChanged(GameState));
}

// ============================================================================
// OnPulse — Window detection and FSM sensor dispatching
//
// Detects which login/server-select/charselect UI is visible and dispatches
// the appropriate CCStateSensor event to the FSM. Also clicks through splash
// screens (in case MQ2AutoLogin is not loaded).
//
// Mirrors MQ2AutoLogin's OnPulse detection logic.
// ============================================================================

PLUGIN_API void OnPulse()
{
	// Only run if we have an active creation request
	if (!CharCreate::is_active())
		return;

	uint64_t now = MQGetTickCount64();
	if (now < s_reenableTime)
		return;

	int gameState = GetGameState();
	static int s_lastLoggedGameState = -1;
	if (gameState != s_lastLoggedGameState)
	{
		CC_DebugLog("OnPulse: gameState changed to %d", gameState);
		s_lastLoggedGameState = gameState;
	}

	if (gameState == GAMESTATE_CHARSELECT || gameState == GAMESTATE_CHARCREATE)
	{
		// Handle confirmation dialogs at character select
		// (e.g., TLP server rules acceptance, character loading messages)
		CSidlScreenWnd* pConfirmationWnd = CC_GetWindow<CSidlScreenWnd>("ConfirmationDialogBox");
		if (pConfirmationWnd != nullptr && pConfirmationWnd->IsVisible())
		{
			if (CStmlWnd* pStmlWnd = CC_GetChildWindow<CStmlWnd>(pConfirmationWnd, "CD_TextOutput"))
			{
				CXStr messageText = CC_GetSTMLText(pStmlWnd);

				static const std::vector<std::pair<const char*, const char*>> PromptWindows = {
					{ "Do you accept these rules?", "CD_Yes_Button" },
					{ "Please contact Customer Service if one of your characters is missing.", "CD_OK_Button" },
				};

				for (auto [message, buttonName] : PromptWindows)
				{
					if (ci_find_substr(messageText, message) != -1)
					{
						if (CButtonWnd* pButton = CC_GetChildWindow<CButtonWnd>(pConfirmationWnd, buttonName))
							CC_SendWndNotification(pButton, pButton, XWM_LCLICK);
					}
				}
			}
		}
		else
		{
			// Check for character creation window first
			CXWnd* pCreateWnd = CC_GetActiveWindow(CCWindowNames::CreateWindow);
			CXWnd* pCharsWnd = CC_GetActiveWindow(CCWindowNames::CharSelectWindow);

			static bool s_lastCreateVisible = false;
			static bool s_lastCharsVisible = false;
			bool createVisible = pCreateWnd != nullptr;
			bool charsVisible = pCharsWnd != nullptr;
			if (createVisible != s_lastCreateVisible || charsVisible != s_lastCharsVisible)
			{
				CC_DebugLog("OnPulse: CharacterCreation=%s CharacterListWnd=%s",
					createVisible ? "visible" : "hidden",
					charsVisible ? "visible" : "hidden");

				// Extra debug: check raw window state even if not "active"
				if (!createVisible)
				{
					auto pRaw = CC_GetWindow<CXWnd>(CCWindowNames::CreateWindow);
					if (pRaw)
						CC_DebugLog("  CharacterCreation exists but not active: visible=%d enabled=%d",
							pRaw->IsVisible(), pRaw->IsEnabled());
					else
						CC_DebugLog("  CharacterCreation window does not exist at all");
				}

				s_lastCreateVisible = createVisible;
				s_lastCharsVisible = charsVisible;
			}

			if (pCreateWnd)
			{
				CharCreate::dispatch(CCStateSensor(CCDetectedState::CharacterCreate, pCreateWnd));
			}
			else if (pCharsWnd)
			{
				CharCreate::dispatch(CCStateSensor(CCDetectedState::CharacterSelect, pCharsWnd));
			}
		}

		s_reenableTime = now + STEP_DELAY;
	}
	else if (gameState == GAMESTATE_PRECHARSELECT && g_pLoginClient)
	{
		// Click through splash/EULA/order screens.
		// MQ2AutoLogin does this unconditionally too, but we handle it ourselves
		// in case MQ2AutoLogin is not loaded.
		static const std::vector<std::pair<const char*, const char*>> PromptWindows = {
			{ "OrderWindow",          "Order_DeclineButton" },
			{ "EulaWindow",           "EULA_AcceptButton" },
			{ "seizurewarning",       "HELP_OKButton" },
			{ "OrderExpansionWindow", "OrderExp_DeclineButton" },
			{ "main",                 "MAIN_ConnectButton" },
			{ "news",                 "NEWS_OKButton" }
		};

		for (const auto& [windowName, buttonName] : PromptWindows)
		{
			if (CButtonWnd* pButton = CC_GetActiveChildWindow<CButtonWnd>(windowName, buttonName))
			{
				CC_SendWndNotification(pButton, pButton, XWM_LCLICK);
				s_reenableTime = now + STEP_DELAY;
				return;
			}
		}

		// Detect current login/server-select screen and dispatch sensor
		if (CC_IsWindowActive("dbgsplash") || CC_IsWindowActive("soesplash"))
		{
			CharCreate::dispatch(CCStateSensor(CCDetectedState::SplashScreen, nullptr));
		}
		else if (CXWnd* pConnectWnd = CC_GetActiveWindow("connect"))
		{
			if (CXWnd* pOkDialog = CC_GetActiveWindow("okdialog"))
				CharCreate::dispatch(CCStateSensor(CCDetectedState::ConnectConfirm, pOkDialog));
			else
				CharCreate::dispatch(CCStateSensor(CCDetectedState::Connect, pConnectWnd));
		}
		else if (CXWnd* pServerWnd = CC_GetActiveWindow("serverselect"))
		{
			if (CXWnd* pOkDialog = CC_GetActiveWindow("okdialog"))
				CharCreate::dispatch(CCStateSensor(CCDetectedState::ServerSelectConfirm, pOkDialog));
			else if (CXWnd* pYesNoDialog = CC_GetActiveWindow("yesnodialog"))
				CharCreate::dispatch(CCStateSensor(CCDetectedState::ServerSelectKick, pYesNoDialog));
			else
				CharCreate::dispatch(CCStateSensor(CCDetectedState::ServerSelect, pServerWnd));
		}

		s_reenableTime = now + STEP_DELAY;
	}
}

// ============================================================================
// ImGui overlay — shows creation status during the flow
// ============================================================================

PLUGIN_API void OnUpdateImGui()
{
	if (GetGameState() != GAMESTATE_CHARSELECT
		&& GetGameState() != GAMESTATE_PRECHARSELECT
		&& GetGameState() != GAMESTATE_INGAME)
		return;

	if (!CharCreate::is_active())
		return;

	ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MQ2CharCreate", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const auto* req = CharCreate::request();
		if (req)
		{
			ImGui::Text("Server:    %s", req->serverName.c_str());
			ImGui::Text("Account:   %s", req->accountName.c_str());
			ImGui::Text("Character: %s", req->characterName.c_str());
			ImGui::Text("Race/Class: %s %s %s", req->genderName.c_str(), req->raceName.c_str(), req->className.c_str());
			ImGui::Separator();
		}

		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Status: %s",
			CharCreate::status_message().c_str());

		if (!CharCreate::error_message().empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s",
				CharCreate::error_message().c_str());
		}

		ImGui::Spacing();
		if (ImGui::Button("Stop Creation"))
		{
			CharCreate::dispatch(StopCharCreate());
		}
	}
	ImGui::End();
}
