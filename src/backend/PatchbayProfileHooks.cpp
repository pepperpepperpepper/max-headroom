#include "PatchbayProfileHooks.h"

#include "settings/SettingsKeys.h"

#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>

namespace {
constexpr const char* kOnLoadKey = "onLoad";
constexpr const char* kOnUnloadKey = "onUnload";

QString profileIdForName(const QString& profileName)
{
  const QString n = profileName.trimmed();
  const QByteArray enc = n.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

QString eventName(PatchbayProfileHookEvent e)
{
  switch (e) {
    case PatchbayProfileHookEvent::Load:
      return QStringLiteral("load");
    case PatchbayProfileHookEvent::Unload:
      return QStringLiteral("unload");
  }
  return QStringLiteral("load");
}

QString shellProgram()
{
  const QString sh = QStandardPaths::findExecutable(QStringLiteral("sh"));
  return sh.isEmpty() ? QStringLiteral("sh") : sh;
}
} // namespace

PatchbayProfileHooks PatchbayProfileHooksStore::load(QSettings& s, const QString& profileName)
{
  PatchbayProfileHooks h;
  const QString id = profileIdForName(profileName);
  if (id.isEmpty()) {
    return h;
  }

  s.beginGroup(SettingsKeys::patchbayProfileHooksGroup());
  s.beginGroup(id);
  h.onLoadCommand = s.value(QString::fromUtf8(kOnLoadKey)).toString();
  h.onUnloadCommand = s.value(QString::fromUtf8(kOnUnloadKey)).toString();
  s.endGroup();
  s.endGroup();
  return h;
}

void PatchbayProfileHooksStore::save(QSettings& s, const QString& profileName, const PatchbayProfileHooks& hooks)
{
  const QString id = profileIdForName(profileName);
  if (id.isEmpty()) {
    return;
  }

  s.beginGroup(SettingsKeys::patchbayProfileHooksGroup());
  s.beginGroup(id);
  s.setValue(QString::fromUtf8(kOnLoadKey), hooks.onLoadCommand.trimmed());
  s.setValue(QString::fromUtf8(kOnUnloadKey), hooks.onUnloadCommand.trimmed());
  s.endGroup();
  s.endGroup();
}

void PatchbayProfileHooksStore::clear(QSettings& s, const QString& profileName)
{
  const QString id = profileIdForName(profileName);
  if (id.isEmpty()) {
    return;
  }
  s.beginGroup(SettingsKeys::patchbayProfileHooksGroup());
  s.remove(id);
  s.endGroup();
}

PatchbayProfileHookStartResult startPatchbayProfileHookDetached(const QString& profileName,
                                                               const QString& previousProfileName,
                                                               const QString& nextProfileName,
                                                               PatchbayProfileHookEvent event,
                                                               const QString& command)
{
  PatchbayProfileHookStartResult r;
  const QString cmd = command.trimmed();
  if (cmd.isEmpty()) {
    return r;
  }

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("HEADROOM_HOOK"), eventName(event));
  env.insert(QStringLiteral("HEADROOM_PROFILE"), profileName);
  env.insert(QStringLiteral("HEADROOM_PROFILE_PREV"), previousProfileName);
  env.insert(QStringLiteral("HEADROOM_PROFILE_NEXT"), nextProfileName);

  QProcess p;
  p.setProgram(shellProgram());
  p.setArguments({QStringLiteral("-lc"), cmd});
  p.setProcessEnvironment(env);
  p.setWorkingDirectory(QDir::homePath());

  qint64 pid = 0;
  const bool ok = p.startDetached(&pid);
  if (!ok) {
    r.error = p.errorString().isEmpty() ? QStringLiteral("failed to start hook process") : p.errorString();
    return r;
  }
  r.started = true;
  r.pid = pid;
  return r;
}

PatchbayProfileHooksTransitionResult runPatchbayProfileTransitionHooksDetached(QSettings& s,
                                                                              const QString& fromProfile,
                                                                              const QString& toProfile)
{
  PatchbayProfileHooksTransitionResult res;

  const QString from = fromProfile.trimmed();
  const QString to = toProfile.trimmed();

  if (!from.isEmpty() && from != to) {
    const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, from);
    res.unload = startPatchbayProfileHookDetached(from, QString{}, to, PatchbayProfileHookEvent::Unload, h.onUnloadCommand);
  }

  if (!to.isEmpty()) {
    const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, to);
    res.load = startPatchbayProfileHookDetached(to, from, QString{}, PatchbayProfileHookEvent::Load, h.onLoadCommand);
  }

  return res;
}

