#include "RecorderDialog.h"

#include <QComboBox>
#include <QVariant>

#include "backend/PipeWireGraph.h"

namespace RecorderDialogInternal {

QString nodeDisplayName(const PwNodeInfo& n)
{
  if (!n.description.isEmpty()) {
    return n.description;
  }
  if (!n.name.isEmpty()) {
    return n.name;
  }
  return QStringLiteral("(unnamed)");
}

} // namespace RecorderDialogInternal

namespace {

QString streamDisplayName(const PwNodeInfo& n)
{
  const QString base = RecorderDialogInternal::nodeDisplayName(n);
  const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
  return (!app.isEmpty() && app != base) ? QStringLiteral("%1 — %2").arg(app, base) : base;
}

QVariantMap targetData(const QString& targetObject, bool captureSink)
{
  return QVariantMap{
      {QStringLiteral("targetObject"), targetObject},
      {QStringLiteral("captureSink"), captureSink},
  };
}

} // namespace

void RecorderDialog::rebuildTargets()
{
  QString prevObj;
  bool prevSink = true;
  bool havePrev = false;
  {
    const QVariant v = m_targetCombo->currentData();
    if (v.isValid()) {
      const QVariantMap prev = v.toMap();
      if (prev.contains(QStringLiteral("targetObject")) && prev.contains(QStringLiteral("captureSink"))) {
        prevObj = prev.value(QStringLiteral("targetObject")).toString();
        prevSink = prev.value(QStringLiteral("captureSink")).toBool();
        havePrev = true;
      }
    }
  }

  m_targetCombo->clear();

  m_targetCombo->addItem(tr("System mix (default output monitor)"), targetData(QString{}, true));

  if (m_graph) {
    const QList<PwNodeInfo> sinks = m_graph->audioSinks();
    if (!sinks.isEmpty()) {
      m_targetCombo->insertSeparator(m_targetCombo->count());
      for (const auto& n : sinks) {
        m_targetCombo->addItem(tr("Output: %1").arg(RecorderDialogInternal::nodeDisplayName(n)), targetData(n.name, true));
      }
    }

    const QList<PwNodeInfo> playback = m_graph->audioPlaybackStreams();
    if (!playback.isEmpty()) {
      m_targetCombo->insertSeparator(m_targetCombo->count());
      for (const auto& n : playback) {
        m_targetCombo->addItem(tr("App playback: %1").arg(streamDisplayName(n)), targetData(n.name, false));
      }
    }

    m_targetCombo->insertSeparator(m_targetCombo->count());
    m_targetCombo->addItem(tr("Default input (mic)"), targetData(QString{}, false));

    const QList<PwNodeInfo> sources = m_graph->audioSources();
    if (!sources.isEmpty()) {
      m_targetCombo->insertSeparator(m_targetCombo->count());
      for (const auto& n : sources) {
        m_targetCombo->addItem(tr("Input: %1").arg(RecorderDialogInternal::nodeDisplayName(n)), targetData(n.name, false));
      }
    }

    const QList<PwNodeInfo> capture = m_graph->audioCaptureStreams();
    if (!capture.isEmpty()) {
      m_targetCombo->insertSeparator(m_targetCombo->count());
      for (const auto& n : capture) {
        m_targetCombo->addItem(tr("App recording: %1").arg(streamDisplayName(n)), targetData(n.name, true));
      }
    }
  }

  if (havePrev) {
    for (int i = 0; i < m_targetCombo->count(); ++i) {
      const QVariant v = m_targetCombo->itemData(i);
      if (!v.isValid()) {
        continue;
      }
      const QVariantMap d = v.toMap();
      if (!d.contains(QStringLiteral("targetObject")) || !d.contains(QStringLiteral("captureSink"))) {
        continue;
      }
      if (d.value(QStringLiteral("targetObject")).toString() == prevObj && d.value(QStringLiteral("captureSink")).toBool() == prevSink) {
        m_targetCombo->setCurrentIndex(i);
        break;
      }
    }
  }
}

