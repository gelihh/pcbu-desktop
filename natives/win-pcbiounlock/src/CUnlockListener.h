#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <credentialprovider.h>

class CSampleProvider;
class CUnlockCredential;
class CUnlockListener {
public:
  CUnlockListener() = default;
  void Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, CSampleProvider *pCredentialProvider, CUnlockCredential *pCredential,
                  const std::wstring &userDomain);
  void Release();

  void Start(bool ignoreWaitKeyPress = false);
  void Stop();
  void ResetFailures();

  bool HasResponse() const;

private:
  void ListenThread(uint64_t generation, const std::shared_ptr<std::atomic<bool>> &workerRunning);
  void PushMessage(uint64_t generation, const std::string &message);
  bool IsCurrent(uint64_t generation) const;

  std::thread m_ListenThread{};
  std::atomic<bool> m_IsRunning{};
  // Per-worker abort flag. A new worker gets a fresh flag, so stopping and starting
  // again (Retry) cannot re-arm the previous worker's network waits.
  std::shared_ptr<std::atomic<bool>> m_WorkerRunning{};
  // Set while an external Stop() invalidates the running worker. The worker must
  // never call back into LogonUI once stopping began - doing so can leave the logon
  // screen waiting on this thread (freeze on the welcome screen) - see PushMessage.
  std::atomic<bool> m_StopRequested{false};
  // Incremented on every Start()/Stop(). A worker only talks to LogonUI while its
  // own generation is still the current one.
  std::atomic<uint64_t> m_Generation{0};
  std::atomic<int> m_ConsecutiveFailures{0};
  bool m_HasResponse{};
  bool m_IgnoreWaitKeyPress{};

  CREDENTIAL_PROVIDER_USAGE_SCENARIO m_ProviderUsage{};
  CSampleProvider *m_CredentialProvider{};
  CUnlockCredential *m_Credential{};
  std::wstring m_UserDomain{};
};
