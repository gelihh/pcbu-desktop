#include "CUnlockListener.h"

#include "CSampleProvider.h"
#include "handler/UnlockHandler.h"
#include "helpers.h"
#include "platform/NetworkHelper.h"
#include "storage/AppSettings.h"
#include "utils/StringUtils.h"
#include <spdlog/spdlog.h>

void CUnlockListener::Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, CSampleProvider *pCredentialProvider, CUnlockCredential *pCredential,
                                 const std::wstring &userDomain) {
  m_ProviderUsage = cpus;
  m_CredentialProvider = pCredentialProvider;
  m_Credential = pCredential;
  m_UserDomain = userDomain;
}

void CUnlockListener::Release() {
  Stop();
}

// After this many failed unlock attempts in a row, stop restarting the listener
// automatically (e.g. when LogonUI re-selects the tile) and require the user to
// press "Retry" or type their password instead.
static constexpr int MAX_CONSECUTIVE_FAILURES = 3;

void CUnlockListener::Start(bool ignoreWaitKeyPress) {
  if(m_IsRunning)
    return;
  if(!ignoreWaitKeyPress && m_ConsecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
    // No UI update here: Start() runs on LogonUI's own thread (SetSelected), and
    // pushing a field change from inside that call re-enters LogonUI.
    spdlog::warn("Unlock failed {} times in a row, automatic retry paused. Use Retry or password.", MAX_CONSECUTIVE_FAILURES);
    return;
  }
  Stop();
  m_StopRequested = false;
  m_IsRunning = true;
  m_IgnoreWaitKeyPress = ignoreWaitKeyPress;
  m_ListenThread = std::thread(&CUnlockListener::ListenThread, this);
}

void CUnlockListener::Stop() {
  if(!m_IsRunning)
    return;
  // Mark an external stop so the listen thread does not call back into LogonUI
  // (SetUnlockData/UpdateCredsStatus) while Stop() is joining it - those calls
  // re-enter LogonUI from a worker thread and can deadlock the logon screen.
  m_StopRequested = true;
  m_IsRunning = false;
  m_IgnoreWaitKeyPress = false;
  if(m_ListenThread.joinable())
    m_ListenThread.join();
}

void CUnlockListener::ResetFailures() {
  m_ConsecutiveFailures = 0;
}

// Every message from the listen thread reaches LogonUI through this function. Once
// an external Stop() is in flight, LogonUI's UI thread is blocked joining this
// thread, so any callback would deadlock the logon screen (it freezes on the
// welcome screen and the machine has to be powered off). Skip them instead.
void CUnlockListener::PushMessage(const std::string &message) {
  if(m_StopRequested || m_Credential == nullptr)
    return;
  m_Credential->UpdateMessage(message);
}

bool CUnlockListener::HasResponse() const {
  return m_HasResponse;
}

#define KEY_RANGE 0xA6
void GetAllKeyState(byte *keys, size_t len) {
  for(int i = 0; i < len; i++) {
    if(GetAsyncKeyState(i) < 0)
      keys[i] = 1;
    else
      keys[i] = 0;
  }
}

void CUnlockListener::ListenThread() {
  // Init
  PushMessage(I18n::Get("initializing"));
  const auto userDomainStr = StringUtils::FromWideString(m_UserDomain);
  const auto userSplit = StringUtils::Split(userDomainStr, "\\");
  if(userSplit.size() != 2) {
    PushMessage(I18n::Get("error_invalid_user"));
    return;
  }

  // Wait
  Sleep(500);
  auto storage = AppSettings::Get();
  auto devices = PairedDevicesStorage::GetDevices();
  const auto waitForNetwork = std::ranges::any_of(devices, [](const PairedDevice &device) {
    return device.pairingMethod == PairingMethod::TCP || device.pairingMethod == PairingMethod::UDP || device.pairingMethod == PairingMethod::MANUAL_UDP;
  });
  if(m_ProviderUsage == CPUS_LOGON || m_ProviderUsage == CPUS_UNLOCK_WORKSTATION) {
    const bool isUserLoggedOn = IsUserLoggedOn(m_UserDomain, 15);

    // Network
    if(waitForNetwork) {
      PushMessage(I18n::Get("wait_network"));
      while(m_IsRunning) {
        auto isAbort = GetAsyncKeyState(VK_LCONTROL) < 0 && GetAsyncKeyState(VK_LMENU) < 0;
        if(NetworkHelper::HasLANConnection() || isAbort) {
          if(isAbort) {
            m_HasResponse = true;
            PushMessage(I18n::Get("unlock_canceled"));
            return;
          }
          break;
        }
        Sleep(10);
      }
    }

    // Unlock behavior
    if(!m_IgnoreWaitKeyPress) {
      const bool isUnlock = m_ProviderUsage == CPUS_UNLOCK_WORKSTATION || (m_ProviderUsage == CPUS_LOGON && isUserLoggedOn);
      if(storage.winUnlockBehavior == "key_press"  || (storage.winUnlockBehavior == "key_press_lock_only" && isUnlock)) {
        Sleep(500);
        PushMessage(I18n::Get("wait_key_press"));
        byte lastKeys[KEY_RANGE];
        GetAllKeyState(lastKeys, KEY_RANGE);
        while(m_IsRunning) {
          byte keys[KEY_RANGE];
          GetAllKeyState(keys, KEY_RANGE);
          if(memcmp(keys, lastKeys, KEY_RANGE) != 0)
            break;
          Sleep(10);
        }
      } else if(storage.winUnlockBehavior == "foreground_always" || (storage.winUnlockBehavior == "foreground_lock_only" && isUnlock)) {
        // HACK: Might not be 100% reliable
        DWORD currentProcessId = GetCurrentProcessId();
        while(m_IsRunning) {
          if(HWND hwndForeground = GetForegroundWindow()) {
            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(hwndForeground, &foregroundProcessId);
            if(foregroundProcessId == currentProcessId) {
              break;
            }
          }
          Sleep(100);
        }
      }
    }
  }

  // Unlock
  std::function<void(const std::string&)> printMessage = [this](const std::string &s) { PushMessage(s); };
  auto handler = UnlockHandler(printMessage);
  const auto result = handler.GetResult(userDomainStr, "Windows-Login", &m_IsRunning);

  // External Stop(): LogonUI is waiting on this thread and must not be re-entered
  // from here, otherwise the logon screen can deadlock.
  if(m_StopRequested) {
    spdlog::info("Listener stopped externally, logon UI callbacks suppressed.");
    return;
  }

  m_HasResponse = true;
  if(result.state == UnlockState::SUCCESS)
    m_ConsecutiveFailures = 0;
  else
    m_ConsecutiveFailures++;
  m_Credential->SetUnlockData(result);
  if(m_ConsecutiveFailures >= MAX_CONSECUTIVE_FAILURES)
    PushMessage(I18n::Get("unlock_retry_limit"));
  m_CredentialProvider->UpdateCredsStatus();
}
