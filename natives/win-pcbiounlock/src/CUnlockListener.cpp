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
  const auto generation = ++m_Generation;
  auto *credential = m_Credential;
  auto workerRunning = std::make_shared<std::atomic<bool>>(true);
  m_WorkerRunning = workerRunning;
  m_ListenThread = std::thread([this, credential, generation, workerRunning] {
    // Stop() detaches instead of joining, so the tile (and with it this listener)
    // may be released before the thread ends. Holding a credential reference keeps
    // both objects alive until the worker is really done.
    if(credential != nullptr)
      credential->AddRef();
    ListenThread(generation, workerRunning);
    if(credential != nullptr)
      credential->Release();
  });
}

void CUnlockListener::Stop() {
  if(!m_IsRunning)
    return;
  // Invalidate the running worker first: it must not call back into LogonUI once
  // the logon screen starts tearing the tile down.
  m_StopRequested = true;
  ++m_Generation;
  if(m_WorkerRunning)
    m_WorkerRunning->store(false);
  m_IsRunning = false;
  m_IgnoreWaitKeyPress = false;
  // Never join here. Stop() runs on LogonUI's thread, and the worker may be blocked
  // inside a callback that only LogonUI's thread can complete - an unbounded join
  // deadlocks the logon screen (it freezes on the welcome screen and the machine
  // has to be powered off). Detach it; the worker exits on its own.
  if(m_ListenThread.joinable()) {
    spdlog::warn("Listen thread detached on stop (non-blocking stop).");
    m_ListenThread.detach();
  }
}

void CUnlockListener::ResetFailures() {
  m_ConsecutiveFailures = 0;
}

bool CUnlockListener::IsCurrent(uint64_t generation) const {
  return !m_StopRequested && m_Generation.load() == generation;
}

// Every message from the listen thread reaches LogonUI through this function.
// While a worker is stale (stopped or superseded by a newer Start) LogonUI's own
// thread may be blocked waiting for us, so touching the credential's UI would
// deadlock the logon screen. Skip the update instead.
void CUnlockListener::PushMessage(uint64_t generation, const std::string &message) {
  if(m_Credential == nullptr || !IsCurrent(generation))
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

void CUnlockListener::ListenThread(uint64_t generation, const std::shared_ptr<std::atomic<bool>> &workerRunning) {
  // Init
  PushMessage(generation, I18n::Get("initializing"));
  const auto userDomainStr = StringUtils::FromWideString(m_UserDomain);
  const auto userSplit = StringUtils::Split(userDomainStr, "\\");
  if(userSplit.size() != 2) {
    PushMessage(generation, I18n::Get("error_invalid_user"));
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
      PushMessage(generation, I18n::Get("wait_network"));
      while(IsCurrent(generation) && workerRunning->load()) {
        auto isAbort = GetAsyncKeyState(VK_LCONTROL) < 0 && GetAsyncKeyState(VK_LMENU) < 0;
        if(NetworkHelper::HasLANConnection() || isAbort) {
          if(isAbort) {
            m_HasResponse = true;
            PushMessage(generation, I18n::Get("unlock_canceled"));
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
        PushMessage(generation, I18n::Get("wait_key_press"));
        byte lastKeys[KEY_RANGE];
        GetAllKeyState(lastKeys, KEY_RANGE);
        while(IsCurrent(generation) && workerRunning->load()) {
          byte keys[KEY_RANGE];
          GetAllKeyState(keys, KEY_RANGE);
          if(memcmp(keys, lastKeys, KEY_RANGE) != 0)
            break;
          Sleep(10);
        }
      } else if(storage.winUnlockBehavior == "foreground_always" || (storage.winUnlockBehavior == "foreground_lock_only" && isUnlock)) {
        // HACK: Might not be 100% reliable
        DWORD currentProcessId = GetCurrentProcessId();
        while(IsCurrent(generation) && workerRunning->load()) {
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

  // A stop or a newer listener supersedes this worker: do not touch the network or
  // the logon screen any further, just exit quietly.
  if(!IsCurrent(generation) || !workerRunning->load()) {
    spdlog::info("Listener superseded or stopped before unlock, logon UI callbacks suppressed.");
    return;
  }

  // Unlock
  std::function<void(const std::string&)> printMessage = [this, generation](const std::string &s) { PushMessage(generation, s); };
  auto handler = UnlockHandler(printMessage);
  const auto result = handler.GetResult(userDomainStr, "Windows-Login", workerRunning.get());

  // External Stop(): LogonUI may be blocked waiting for this thread and must not be
  // re-entered from here, otherwise the logon screen can deadlock.
  if(!IsCurrent(generation)) {
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
    PushMessage(generation, I18n::Get("unlock_retry_limit"));
  m_CredentialProvider->UpdateCredsStatus();
}
