#include "VisualizerPage.h"

#include "backend/AudioTap.h"
#include "backend/PipeWireGraph.h"
#include "ui/VisualizerWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace {
struct TapSpec final {
  QString targetObject;
  bool captureSink = false;
};

QVariant makeTapData(const QString& targetObject, bool captureSink)
{
  QVariantMap m;
  m.insert(QStringLiteral("targetObject"), targetObject);
  m.insert(QStringLiteral("captureSink"), captureSink);
  return m;
}

std::optional<TapSpec> tapSpecFromVariant(const QVariant& v)
{
  if (!v.isValid()) {
    return std::nullopt;
  }

  // Backwards compat: older builds stored a plain QString target.
  if (v.canConvert<QString>() && v.userType() != QMetaType::QVariantMap) {
    const QString target = v.toString();
    return TapSpec{target, false};
  }

  const QVariantMap m = v.toMap();
  if (!m.contains(QStringLiteral("targetObject"))) {
    return std::nullopt;
  }

  TapSpec spec;
  spec.targetObject = m.value(QStringLiteral("targetObject")).toString();
  spec.captureSink = m.value(QStringLiteral("captureSink")).toBool();
  return spec;
}

int findTapIndex(QComboBox* box, const QString& targetObject, bool captureSink)
{
  if (!box) {
    return -1;
  }

  for (int i = 0; i < box->count(); ++i) {
    const auto specOpt = tapSpecFromVariant(box->itemData(i));
    if (!specOpt.has_value()) {
      continue;
    }
    if (specOpt->targetObject == targetObject && specOpt->captureSink == captureSink) {
      return i;
    }
  }
  return -1;
}

QString nodeLabel(const PwNodeInfo& node)
{
  return node.description.isEmpty() ? node.name : node.description;
}

QString portLabel(const PwPortInfo& port)
{
  QString label = port.name;
  if (!port.audioChannel.isEmpty() && !label.contains(port.audioChannel, Qt::CaseInsensitive)) {
    label = QStringLiteral("%1 (%2)").arg(label, port.audioChannel);
  }
  return label;
}
} // namespace

VisualizerPage::VisualizerPage(PipeWireGraph* graph, AudioTap* tap, QWidget* parent)
    : QWidget(parent)
    , m_graph(graph)
    , m_tap(tap)
{
  auto* root = new QVBoxLayout(this);

  auto* form = new QFormLayout();
  m_sources = new QComboBox(this);
  m_sources->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  form->addRow(tr("Tap:"), m_sources);

  m_state = new QLabel(tr("unconnected"), this);
  m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(tr("Stream:"), m_state);

  root->addLayout(form);

  m_widget = new VisualizerWidget(m_tap, this);
  root->addWidget(m_widget, 1);

  if (m_graph) {
    connect(m_graph, &PipeWireGraph::topologyChanged, this, &VisualizerPage::repopulateSources);
  }
  if (m_tap) {
    connect(m_tap, &AudioTap::streamStateChanged, this, [this](const QString& s) { m_state->setText(s); });
  }

  connect(m_sources, QOverload<int>::of(&QComboBox::activated), this, [this](int) { m_userHasChosenTarget = true; });

  connect(m_sources, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (!m_tap) {
      return;
    }
    const auto specOpt = tapSpecFromVariant(m_sources->itemData(index));
    if (!specOpt.has_value()) {
      return;
    }
    m_tap->setTarget(specOpt->captureSink, specOpt->targetObject);
  });

  repopulateSources();
}

void VisualizerPage::applySettings(const VisualizerSettings& settings)
{
  if (m_tap) {
    m_tap->applySettings(settings);
  }
  if (m_widget) {
    m_widget->applySettings(settings);
  }
}

void VisualizerPage::setTapTarget(const QString& targetObject, bool captureSink)
{
  m_userHasChosenTarget = true;
  m_pendingTargetObject = targetObject;
  m_pendingCaptureSink = captureSink;
  m_hasPendingTarget = true;

  if (m_tap) {
    m_tap->setTarget(captureSink, targetObject);
  }

  const int idx = findTapIndex(m_sources, targetObject, captureSink);
  if (idx >= 0 && m_sources) {
    const QSignalBlocker blocker(m_sources);
    m_sources->setCurrentIndex(idx);
    m_hasPendingTarget = false;
  }
}

void VisualizerPage::repopulateSources()
{
  if (!m_sources) {
    return;
  }

  const auto previousSpec = tapSpecFromVariant(m_sources->currentData());
  const TapSpec desiredSpec = m_hasPendingTarget
      ? TapSpec{m_pendingTargetObject, m_pendingCaptureSink}
      : previousSpec.value_or(TapSpec{});
  m_sources->blockSignals(true);
  m_sources->clear();

  m_sources->addItem(tr("Auto (default)"), makeTapData(QString{}, false));

  if (m_graph) {
    const auto nodes = m_graph->nodes();
    const auto ports = m_graph->ports();

    QHash<uint32_t, QList<PwPortInfo>> outPortsByNode;
    outPortsByNode.reserve(ports.size());
    for (const auto& p : ports) {
      if (p.nodeId == 0 || p.direction != QStringLiteral("out") || p.objectSerial.isEmpty()) {
        continue;
      }
      outPortsByNode[p.nodeId].push_back(p);
    }
    for (auto it = outPortsByNode.begin(); it != outPortsByNode.end(); ++it) {
      std::sort(it->begin(), it->end(), [](const PwPortInfo& a, const PwPortInfo& b) { return a.id < b.id; });
    }

    QList<PwNodeInfo> sources;
    QList<PwNodeInfo> sinks;
    QList<PwNodeInfo> playback;
    QList<PwNodeInfo> recording;

    for (const auto& n : nodes) {
      if (n.name.startsWith(QStringLiteral("headroom."))) {
        continue;
      }
      if (n.mediaClass == QStringLiteral("Audio/Source")) {
        sources.push_back(n);
      } else if (n.mediaClass == QStringLiteral("Audio/Sink")) {
        sinks.push_back(n);
      } else if (n.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
        playback.push_back(n);
      } else if (n.mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
        recording.push_back(n);
      }
    }

    auto byLabel = [](const PwNodeInfo& a, const PwNodeInfo& b) {
      return nodeLabel(a).toLower() < nodeLabel(b).toLower();
    };
    std::sort(sources.begin(), sources.end(), byLabel);
    std::sort(sinks.begin(), sinks.end(), byLabel);
    std::sort(playback.begin(), playback.end(), byLabel);
    std::sort(recording.begin(), recording.end(), byLabel);

    auto addGroup = [&](const QString& title, const QList<PwNodeInfo>& group) {
      if (group.isEmpty()) {
        return;
      }
      m_sources->addItem(title);
      if (auto* model = qobject_cast<QStandardItemModel*>(m_sources->model())) {
        const int row = m_sources->count() - 1;
        if (auto* item = model->item(row)) {
          item->setEnabled(false);
        }
      }
      for (const auto& node : group) {
        const bool captureSink = node.mediaClass == QStringLiteral("Audio/Sink");
        m_sources->addItem(QStringLiteral("  %1").arg(nodeLabel(node)), makeTapData(node.name, captureSink));

        const auto outPorts = outPortsByNode.value(node.id);
        for (const auto& port : outPorts) {
          m_sources->addItem(QStringLiteral("    %1").arg(portLabel(port)), makeTapData(port.objectSerial, false));
        }
      }
    };

    addGroup(tr("Output Devices"), sinks);
    addGroup(tr("Sources"), sources);
    addGroup(tr("Playback (streams)"), playback);
    addGroup(tr("Recording (streams)"), recording);
  }

  int idx = findTapIndex(m_sources, desiredSpec.targetObject, desiredSpec.captureSink);
  if (idx < 0) {
    idx = 0;
  }
  m_sources->setCurrentIndex(idx);
  m_sources->blockSignals(false);
  if (idx >= 0 && idx < m_sources->count()) {
    const auto currentSpec = tapSpecFromVariant(m_sources->itemData(idx));
    if (m_hasPendingTarget && currentSpec.has_value()
        && currentSpec->targetObject == m_pendingTargetObject
        && currentSpec->captureSink == m_pendingCaptureSink) {
      m_hasPendingTarget = false;
    }
  }

  // If we were on Auto and now have stream targets, pick the first available target.
  if (!m_userHasChosenTarget && m_tap && !m_tap->captureSink() && m_tap->targetObject().isEmpty() && m_sources->count() > 1) {
    int bestIndex = -1;
    int bestScore = -1;
    for (int i = 0; i < m_sources->count(); ++i) {
      const auto specOpt = tapSpecFromVariant(m_sources->itemData(i));
      if (!specOpt.has_value()) {
        continue;
      }
      if (specOpt->targetObject.isEmpty()) {
        continue;
      }

      // Heuristics: prefer port-object serials (numeric ids) and prefer non-sink capture.
      int score = 0;
      bool isNumber = false;
      specOpt->targetObject.toUInt(&isNumber);
      if (isNumber) {
        score += 100;
      }
      if (!specOpt->captureSink) {
        score += 10;
      }

      if (score > bestScore) {
        bestScore = score;
        bestIndex = i;
      }
    }

    if (bestIndex >= 0) {
      const auto specOpt = tapSpecFromVariant(m_sources->itemData(bestIndex));
      if (specOpt.has_value()) {
        m_sources->setCurrentIndex(bestIndex);
        m_tap->setTarget(specOpt->captureSink, specOpt->targetObject);
      }
    }
  }
}
