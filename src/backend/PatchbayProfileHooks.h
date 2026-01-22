#pragma once

#include <QString>

#include <cstdint>

class QSettings;

struct PatchbayProfileHooks final {
  QString onLoadCommand;
  QString onUnloadCommand;
};

class PatchbayProfileHooksStore final
{
public:
  static PatchbayProfileHooks load(QSettings& s, const QString& profileName);
  static void save(QSettings& s, const QString& profileName, const PatchbayProfileHooks& hooks);
  static void clear(QSettings& s, const QString& profileName);
};

enum class PatchbayProfileHookEvent : uint8_t {
  Load = 0,
  Unload = 1,
};

struct PatchbayProfileHookStartResult final {
  bool started = false;
  qint64 pid = 0;
  QString error;
};

PatchbayProfileHookStartResult startPatchbayProfileHookDetached(const QString& profileName,
                                                               const QString& previousProfileName,
                                                               const QString& nextProfileName,
                                                               PatchbayProfileHookEvent event,
                                                               const QString& command);

struct PatchbayProfileHooksTransitionResult final {
  PatchbayProfileHookStartResult unload;
  PatchbayProfileHookStartResult load;
};

PatchbayProfileHooksTransitionResult runPatchbayProfileTransitionHooksDetached(QSettings& s,
                                                                              const QString& fromProfile,
                                                                              const QString& toProfile);

