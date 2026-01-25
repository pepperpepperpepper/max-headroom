#include "MixerPage.h"

#include "backend/AudioLevelTap.h"
#include "backend/EqManager.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "settings/SettingsKeys.h"
#include "ui/EqDialog.h"
#include "ui/LevelMeterWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QPushButton>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
bool isInternalNode(const PwNodeInfo& node)
{
  return node.name.startsWith(QStringLiteral("headroom."));
}

bool matchesFilter(const QString& haystack, const QString& needle)
{
  if (needle.trimmed().isEmpty()) {
    return true;
  }
  return haystack.contains(needle, Qt::CaseInsensitive);
}

QWidget* makeNodeRow(PipeWireGraph* graph,
                     PipeWireThread* pw,
                     const PwNodeInfo& node,
                     const PwNodeControls& controls,
                     bool isDefault,
                     std::function<void()> onEq,
                     std::function<void()> onVisualize,
                     QList<QPointer<LevelMeterWidget>>& meters,
                     QWidget* parent)
{
  auto* row = new QWidget(parent);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(8, 6, 8, 6);

  const QString label = node.description.isEmpty() ? node.name : node.description;
  auto* name = new QLabel(isDefault ? QStringLiteral("%1  (default)").arg(label) : label, row);
  name->setMinimumWidth(260);
  name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  name->setTextInteractionFlags(Qt::TextSelectableByMouse);
  if (isDefault) {
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
  }
  h->addWidget(name, 2);

  AudioLevelTap* tap = nullptr;
  if (pw && node.mediaClass.contains(QStringLiteral("Audio"))) {
    tap = new AudioLevelTap(pw, row);
    tap->setCaptureSink(node.mediaClass == QStringLiteral("Audio/Sink"));
    tap->setTargetObject(node.name);
  }
  auto* meter = new LevelMeterWidget(row);
  meter->setTap(tap);
  if (tap) {
    meters.push_back(meter);
  }
  h->addWidget(meter, 0);

  auto* slider = new QSlider(Qt::Horizontal, row);
  slider->setRange(0, 150);
  slider->setEnabled(controls.hasVolume);
  slider->setTracking(true);
  slider->setValue(std::clamp(static_cast<int>(std::lround(controls.volume * 100.0f)), 0, 150));
  h->addWidget(slider, 4);

  auto* pct = new QLabel(QStringLiteral("%1%").arg(slider->value()), row);
  pct->setMinimumWidth(50);
  pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  h->addWidget(pct, 0);

  auto* mute = new QCheckBox(QObject::tr("Mute"), row);
  mute->setEnabled(controls.hasMute);
  mute->setChecked(controls.mute);
  h->addWidget(mute, 0);

  if (onVisualize) {
    auto* vizBtn = new QPushButton(QObject::tr("Viz"), row);
    vizBtn->setToolTip(QObject::tr("Show in Visualizer"));
    QObject::connect(vizBtn, &QPushButton::clicked, row, [onVisualize = std::move(onVisualize)]() { onVisualize(); });
    h->addWidget(vizBtn, 0);
  }

  if (onEq) {
    auto* eqBtn = new QPushButton(QObject::tr("EQ…"), row);
    QObject::connect(eqBtn, &QPushButton::clicked, row, [onEq = std::move(onEq)]() { onEq(); });
    h->addWidget(eqBtn, 0);
  }

  QObject::connect(slider, &QSlider::valueChanged, row, [pct](int v) { pct->setText(QStringLiteral("%1%").arg(v)); });
  QObject::connect(slider, &QSlider::valueChanged, row, [graph, nodeId = node.id, slider](int v) {
    if (!graph || slider->isSliderDown()) {
      return;
    }
    graph->setNodeVolume(nodeId, static_cast<float>(v) / 100.0f);
  });
  QObject::connect(slider, &QSlider::sliderReleased, row, [graph, nodeId = node.id, slider]() {
    if (!graph) {
      return;
    }
    graph->setNodeVolume(nodeId, static_cast<float>(slider->value()) / 100.0f);
  });
  QObject::connect(mute, &QCheckBox::toggled, row, [graph, nodeId = node.id](bool checked) {
    if (!graph) {
      return;
    }
    graph->setNodeMute(nodeId, checked);
  });

  return row;
}

struct StreamRoute final {
  uint32_t deviceId = 0;
  QString deviceName;
  bool isPlayback = true;
};

StreamRoute routeForStream(PipeWireGraph* graph, const PwNodeInfo& stream)
{
  if (!graph) {
    return {};
  }

  const bool isPlayback = stream.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));

  const QList<PwNodeInfo> nodes = graph->nodes();
  const QList<PwLinkInfo> links = graph->links();

  QHash<uint32_t, PwNodeInfo> nodesById;
  nodesById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodesById.insert(n.id, n);
  }

  QMultiHash<uint32_t, uint32_t> forward;
  QMultiHash<uint32_t, uint32_t> backward;
  forward.reserve(links.size());
  backward.reserve(links.size());
  for (const auto& l : links) {
    if (l.outputNodeId != 0 && l.inputNodeId != 0) {
      forward.insert(l.outputNodeId, l.inputNodeId);
      backward.insert(l.inputNodeId, l.outputNodeId);
    }
  }

  const QString wanted = isPlayback ? QStringLiteral("Audio/Sink") : QStringLiteral("Audio/Source");

  struct QueueItem {
    uint32_t nodeId = 0;
    int depth = 0;
  };

  constexpr int kMaxDepth = 6;

  QList<QueueItem> queue;
  queue.reserve(64);
  QSet<uint32_t> visited;
  visited.reserve(64);

  visited.insert(stream.id);
  queue.push_back({stream.id, 0});

  while (!queue.isEmpty()) {
    const QueueItem item = queue.takeFirst();
    if (item.depth > kMaxDepth) {
      continue;
    }

    if (item.nodeId != stream.id) {
      const auto it = nodesById.constFind(item.nodeId);
      if (it != nodesById.constEnd() && it->mediaClass == wanted) {
        const QString label = it->description.isEmpty() ? it->name : it->description;
        return StreamRoute{it->id, label, isPlayback};
      }
    }

    if (item.depth == kMaxDepth) {
      continue;
    }

    const auto nexts = isPlayback ? forward.values(item.nodeId) : backward.values(item.nodeId);
    for (uint32_t next : nexts) {
      if (visited.contains(next)) {
        continue;
      }
      visited.insert(next);
      queue.push_back({next, item.depth + 1});
    }
  }

  return StreamRoute{0, {}, isPlayback};
}

QList<PwPortInfo> portsForNode(const QList<PwPortInfo>& ports, uint32_t nodeId, const QString& direction)
{
  QList<PwPortInfo> out;
  for (const auto& p : ports) {
    if (p.nodeId == nodeId && p.direction == direction) {
      out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwPortInfo& a, const PwPortInfo& b) { return a.id < b.id; });
  return out;
}

bool movePlaybackStreamToSink(PipeWireGraph* graph, uint32_t streamId, uint32_t sinkId)
{
  if (!graph || streamId == 0 || sinkId == 0) {
    return false;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QList<PwLinkInfo> links = graph->links();

  const QList<PwPortInfo> streamOutPorts = portsForNode(ports, streamId, QStringLiteral("out"));
  const QList<PwPortInfo> sinkInPorts = portsForNode(ports, sinkId, QStringLiteral("in"));
  if (streamOutPorts.isEmpty() || sinkInPorts.isEmpty()) {
    return false;
  }

  QSet<uint32_t> streamOutPortIds;
  streamOutPortIds.reserve(streamOutPorts.size());
  for (const auto& p : streamOutPorts) {
    streamOutPortIds.insert(p.id);
  }

  for (const auto& l : links) {
    if (l.outputNodeId == streamId && streamOutPortIds.contains(l.outputPortId)) {
      graph->destroyLink(l.id);
    }
  }

  QHash<QString, uint32_t> sinkByChannel;
  sinkByChannel.reserve(sinkInPorts.size());
  for (const auto& p : sinkInPorts) {
    if (!p.audioChannel.isEmpty()) {
      sinkByChannel.insert(p.audioChannel, p.id);
    }
  }

  bool anyOk = false;
  for (int i = 0; i < streamOutPorts.size(); ++i) {
    const auto& sp = streamOutPorts[i];
    uint32_t targetPortId = 0;

    if (!sp.audioChannel.isEmpty() && sinkByChannel.contains(sp.audioChannel)) {
      targetPortId = sinkByChannel.value(sp.audioChannel);
    } else if (i < sinkInPorts.size()) {
      targetPortId = sinkInPorts[i].id;
    }

    if (targetPortId != 0) {
      anyOk = graph->createLink(streamId, sp.id, sinkId, targetPortId) || anyOk;
    }
  }

  return anyOk;
}

bool moveCaptureStreamToSource(PipeWireGraph* graph, uint32_t streamId, uint32_t sourceId)
{
  if (!graph || streamId == 0 || sourceId == 0) {
    return false;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QList<PwLinkInfo> links = graph->links();

  const QList<PwPortInfo> streamInPorts = portsForNode(ports, streamId, QStringLiteral("in"));
  const QList<PwPortInfo> sourceOutPorts = portsForNode(ports, sourceId, QStringLiteral("out"));
  if (streamInPorts.isEmpty() || sourceOutPorts.isEmpty()) {
    return false;
  }

  QSet<uint32_t> streamInPortIds;
  streamInPortIds.reserve(streamInPorts.size());
  for (const auto& p : streamInPorts) {
    streamInPortIds.insert(p.id);
  }

  for (const auto& l : links) {
    if (l.inputNodeId == streamId && streamInPortIds.contains(l.inputPortId)) {
      graph->destroyLink(l.id);
    }
  }

  QHash<QString, uint32_t> sourceByChannel;
  sourceByChannel.reserve(sourceOutPorts.size());
  for (const auto& p : sourceOutPorts) {
    if (!p.audioChannel.isEmpty()) {
      sourceByChannel.insert(p.audioChannel, p.id);
    }
  }

  bool anyOk = false;
  for (int i = 0; i < streamInPorts.size(); ++i) {
    const auto& sp = streamInPorts[i];
    uint32_t sourcePortId = 0;

    if (!sp.audioChannel.isEmpty() && sourceByChannel.contains(sp.audioChannel)) {
      sourcePortId = sourceByChannel.value(sp.audioChannel);
    } else if (i < sourceOutPorts.size()) {
      sourcePortId = sourceOutPorts[i].id;
    }

    if (sourcePortId != 0) {
      anyOk = graph->createLink(sourceId, sourcePortId, streamId, sp.id) || anyOk;
    }
  }

  return anyOk;
}

QWidget* makeStreamRow(PipeWireGraph* graph,
                       PipeWireThread* pw,
                       const PwNodeInfo& stream,
                       const PwNodeControls& controls,
                       const QList<PwNodeInfo>& devices,
                       std::function<void()> onEq,
                       std::function<void()> onVisualize,
                       QList<QPointer<LevelMeterWidget>>& meters,
                       QWidget* parent)
{
  auto* row = new QWidget(parent);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(8, 6, 8, 6);

  const StreamRoute route = routeForStream(graph, stream);

  const QString nameText = stream.description.isEmpty() ? stream.name : stream.description;
  auto* name = new QLabel(nameText, row);
  name->setMinimumWidth(260);
  name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  name->setTextInteractionFlags(Qt::TextSelectableByMouse);
  name->setToolTip(route.deviceName.isEmpty() ? stream.name : QStringLiteral("%1\n%2").arg(stream.name, route.deviceName));
  h->addWidget(name, 2);

  AudioLevelTap* tap = nullptr;
  if (pw) {
    tap = new AudioLevelTap(pw, row);
    tap->setTargetObject(stream.name);
  }
  auto* meter = new LevelMeterWidget(row);
  meter->setTap(tap);
  if (tap) {
    meters.push_back(meter);
  }
  h->addWidget(meter, 0);

  auto* slider = new QSlider(Qt::Horizontal, row);
  slider->setRange(0, 150);
  slider->setEnabled(controls.hasVolume);
  slider->setTracking(true);
  slider->setValue(std::clamp(static_cast<int>(std::lround(controls.volume * 100.0f)), 0, 150));
  h->addWidget(slider, 4);

  auto* pct = new QLabel(QStringLiteral("%1%").arg(slider->value()), row);
  pct->setMinimumWidth(50);
  pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  h->addWidget(pct, 0);

  auto* mute = new QCheckBox(QObject::tr("Mute"), row);
  mute->setEnabled(controls.hasMute);
  mute->setChecked(controls.mute);
  h->addWidget(mute, 0);

  if (onVisualize) {
    auto* vizBtn = new QPushButton(QObject::tr("Viz"), row);
    vizBtn->setToolTip(QObject::tr("Show in Visualizer"));
    QObject::connect(vizBtn, &QPushButton::clicked, row, [onVisualize = std::move(onVisualize)]() { onVisualize(); });
    h->addWidget(vizBtn, 0);
  }

  if (onEq) {
    auto* eqBtn = new QPushButton(QObject::tr("EQ…"), row);
    QObject::connect(eqBtn, &QPushButton::clicked, row, [onEq = std::move(onEq)]() { onEq(); });
    h->addWidget(eqBtn, 0);
  }

  auto* deviceBox = new QComboBox(row);
  deviceBox->setMinimumWidth(220);

  if (devices.isEmpty()) {
    deviceBox->addItem(QObject::tr("(no devices)"), QVariant{0});
    deviceBox->setEnabled(false);
  } else {
    for (const auto& d : devices) {
      const QString label = d.description.isEmpty() ? d.name : d.description;
      deviceBox->addItem(label, QVariant::fromValue<quint32>(d.id));
    }

    int idx = -1;
    for (int i = 0; i < deviceBox->count(); ++i) {
      if (deviceBox->itemData(i).toUInt() == route.deviceId && route.deviceId != 0) {
        idx = i;
        break;
      }
    }

    if (idx >= 0) {
      const QSignalBlocker blocker(deviceBox);
      deviceBox->setCurrentIndex(idx);
    } else {
      deviceBox->insertItem(0, QObject::tr("(unknown route)"), QVariant{0});
      const QSignalBlocker blocker(deviceBox);
      deviceBox->setCurrentIndex(0);
    }

    QObject::connect(deviceBox, &QComboBox::currentIndexChanged, row, [graph, streamId = stream.id, deviceBox, isPlayback = route.isPlayback](int) {
      if (!graph) {
        return;
      }
      const uint32_t deviceId = deviceBox->currentData().toUInt();
      if (deviceId == 0 || streamId == 0) {
        return;
      }
      if (isPlayback) {
        movePlaybackStreamToSink(graph, streamId, deviceId);
      } else {
        moveCaptureStreamToSource(graph, streamId, deviceId);
      }
    });
  }

  h->addWidget(deviceBox, 0);

  QObject::connect(slider, &QSlider::valueChanged, row, [pct](int v) { pct->setText(QStringLiteral("%1%").arg(v)); });
  QObject::connect(slider, &QSlider::valueChanged, row, [graph, nodeId = stream.id, slider](int v) {
    if (!graph || slider->isSliderDown()) {
      return;
    }
    graph->setNodeVolume(nodeId, static_cast<float>(v) / 100.0f);
  });
  QObject::connect(slider, &QSlider::sliderReleased, row, [graph, nodeId = stream.id, slider]() {
    if (!graph) {
      return;
    }
    graph->setNodeVolume(nodeId, static_cast<float>(slider->value()) / 100.0f);
  });
  QObject::connect(mute, &QCheckBox::toggled, row, [graph, nodeId = stream.id](bool checked) {
    if (!graph) {
      return;
    }
    graph->setNodeMute(nodeId, checked);
  });

  return row;
}

QGroupBox* makeSection(const QString& title,
                       const QList<PwNodeInfo>& nodes,
                       PipeWireGraph* graph,
                       PipeWireThread* pw,
                       uint32_t defaultNodeId,
                       const QString& filter,
                       std::function<void(const PwNodeInfo&)> onEqForNode,
                       std::function<void(const PwNodeInfo&)> onVisualizeForNode,
                       QList<QPointer<LevelMeterWidget>>& meters,
                       QWidget* parent)
{
  auto* box = new QGroupBox(title, parent);
  auto* v = new QVBoxLayout(box);
  v->setContentsMargins(8, 8, 8, 8);

  int count = 0;
  for (const auto& node : nodes) {
    const QString label = node.description.isEmpty() ? node.name : node.description;
    const QString hay = QStringLiteral("%1 %2 %3").arg(label, node.name, node.mediaClass);
    if (!matchesFilter(hay, filter)) {
      continue;
    }

    const auto controlsOpt = graph ? graph->nodeControls(node.id) : std::nullopt;
    const PwNodeControls controls = controlsOpt.value_or(PwNodeControls{});

    std::function<void()> onEq;
    if (onEqForNode) {
      const PwNodeInfo nodeCopy = node;
      onEq = [onEqForNode, nodeCopy]() { onEqForNode(nodeCopy); };
    }

    std::function<void()> onVisualize;
    if (onVisualizeForNode) {
      const PwNodeInfo nodeCopy = node;
      onVisualize = [onVisualizeForNode, nodeCopy]() { onVisualizeForNode(nodeCopy); };
    }

    const bool isDefault = defaultNodeId != 0 && node.id == defaultNodeId;
    v->addWidget(makeNodeRow(graph, pw, node, controls, isDefault, std::move(onEq), std::move(onVisualize), meters, box));
    ++count;
  }

  if (count == 0) {
    auto* empty = new QLabel(QObject::tr("No matching nodes"), box);
    empty->setStyleSheet(QStringLiteral("color: #94a3b8;"));
    v->addWidget(empty);
  }

  return box;
}

QGroupBox* makeStreamsSection(const QString& title,
                              const QList<PwNodeInfo>& streams,
                              const QList<PwNodeInfo>& devices,
                              PipeWireGraph* graph,
                              PipeWireThread* pw,
                              const QString& filter,
                              std::function<void(const PwNodeInfo&)> onEqForStream,
                              std::function<void(const PwNodeInfo&)> onVisualizeForStream,
                              QList<QPointer<LevelMeterWidget>>& meters,
                              QWidget* parent)
{
  auto* box = new QGroupBox(title, parent);
  auto* v = new QVBoxLayout(box);
  v->setContentsMargins(8, 8, 8, 8);

  int count = 0;
  for (const auto& stream : streams) {
    const QString label = stream.description.isEmpty() ? stream.name : stream.description;
    const StreamRoute route = routeForStream(graph, stream);
    const QString hay = QStringLiteral("%1 %2 %3 %4").arg(label, stream.name, stream.mediaClass, route.deviceName);
    if (!matchesFilter(hay, filter)) {
      continue;
    }

    const auto controlsOpt = graph ? graph->nodeControls(stream.id) : std::nullopt;
    const PwNodeControls controls = controlsOpt.value_or(PwNodeControls{});

    std::function<void()> onEq;
    if (onEqForStream) {
      const PwNodeInfo streamCopy = stream;
      onEq = [onEqForStream, streamCopy]() { onEqForStream(streamCopy); };
    }

    std::function<void()> onVisualize;
    if (onVisualizeForStream) {
      const PwNodeInfo streamCopy = stream;
      onVisualize = [onVisualizeForStream, streamCopy]() { onVisualizeForStream(streamCopy); };
    }

    v->addWidget(makeStreamRow(graph, pw, stream, controls, devices, std::move(onEq), std::move(onVisualize), meters, box));
    ++count;
  }

  if (count == 0) {
    auto* empty = new QLabel(QObject::tr("No matching streams"), box);
    empty->setStyleSheet(QStringLiteral("color: #94a3b8;"));
    v->addWidget(empty);
  }

  return box;
}
} // namespace

MixerPage::MixerPage(PipeWireThread* pw, PipeWireGraph* graph, EqManager* eq, QWidget* parent)
    : QWidget(parent)
    , m_pw(pw)
    , m_graph(graph)
    , m_eq(eq)
{
  auto* root = new QVBoxLayout(this);

  auto* form = new QFormLayout();

  auto makeDefaultPickerRow = [this](QComboBox*& boxOut, QPushButton*& btnOut, QWidget* parentRow) {
    auto* row = new QWidget(parentRow);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);

    boxOut = new QComboBox(row);
    boxOut->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    btnOut = new QPushButton(tr("Set"), row);
    btnOut->setMinimumWidth(80);

    h->addWidget(boxOut, 1);
    h->addWidget(btnOut, 0);
    return row;
  };

  form->addRow(tr("Default output:"), makeDefaultPickerRow(m_defaultOutput, m_setDefaultOutput, this));
  form->addRow(tr("Default input:"), makeDefaultPickerRow(m_defaultInput, m_setDefaultInput, this));

  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Filter streams/devices…"));
  form->addRow(tr("Filter:"), m_filter);
  root->addLayout(form);

  m_scroll = new QScrollArea(this);
  m_scroll->setWidgetResizable(true);
  m_scroll->setFrameShape(QFrame::NoFrame);
  root->addWidget(m_scroll, 1);

  m_container = new QWidget(m_scroll);
  m_scroll->setWidget(m_container);

  m_rebuildTimer = new QTimer(this);
  m_rebuildTimer->setSingleShot(true);
  m_rebuildTimer->setInterval(50);
  connect(m_rebuildTimer, &QTimer::timeout, this, &MixerPage::rebuild);

  m_meterTimer = new QTimer(this);
  m_meterTimer->setInterval(33);
  connect(m_meterTimer, &QTimer::timeout, this, &MixerPage::tickMeters);
  m_meterTimer->start();

  connect(m_filter, &QLineEdit::textChanged, this, &MixerPage::scheduleRebuild);
  if (m_graph) {
    connect(m_graph, &PipeWireGraph::graphChanged, this, &MixerPage::scheduleRebuild);
  }

  connect(m_setDefaultOutput, &QPushButton::clicked, this, [this]() {
    if (!m_graph || !m_defaultOutput) {
      return;
    }
    const uint32_t nodeId = m_defaultOutput->currentData().toUInt();
    if (nodeId != 0) {
      m_graph->setDefaultAudioSink(nodeId);
    }
  });

  connect(m_setDefaultInput, &QPushButton::clicked, this, [this]() {
    if (!m_graph || !m_defaultInput) {
      return;
    }
    const uint32_t nodeId = m_defaultInput->currentData().toUInt();
    if (nodeId != 0) {
      m_graph->setDefaultAudioSource(nodeId);
    }
  });

  rebuild();
}

void MixerPage::refresh()
{
  scheduleRebuild();
}

void MixerPage::scheduleRebuild()
{
  if (!m_rebuildTimer->isActive()) {
    m_rebuildTimer->start();
  }
}

void MixerPage::tickMeters()
{
  QList<QPointer<LevelMeterWidget>> alive;
  alive.reserve(m_meters.size());
  for (const auto& w : m_meters) {
    if (!w) {
      continue;
    }
    w->tick();
    alive.push_back(w);
  }
  m_meters.swap(alive);
}

void MixerPage::rebuild()
{
  if (!m_container) {
    return;
  }

  m_meters.clear();

  const QString filter = m_filter ? m_filter->text() : QString{};

  if (auto* old = m_container->layout()) {
    QLayoutItem* item = nullptr;
    while ((item = old->takeAt(0)) != nullptr) {
      if (auto* w = item->widget()) {
        w->deleteLater();
      }
      delete item;
    }
    delete old;
  }

  auto* layout = new QVBoxLayout();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);
  m_container->setLayout(layout);

  const QList<PwNodeInfo> nodes = m_graph ? m_graph->nodes() : QList<PwNodeInfo>{};

  QList<QPointer<LevelMeterWidget>> meters;

  QList<PwNodeInfo> playback;
  QList<PwNodeInfo> recording;
  QList<PwNodeInfo> outputs;
  QList<PwNodeInfo> inputs;
  QList<PwNodeInfo> other;

  for (const auto& n : nodes) {
    if (isInternalNode(n)) {
      continue;
    }
    if (n.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
      playback.push_back(n);
    } else if (n.mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
      recording.push_back(n);
    } else if (n.mediaClass == QStringLiteral("Audio/Sink")) {
      outputs.push_back(n);
    } else if (n.mediaClass == QStringLiteral("Audio/Source")) {
      inputs.push_back(n);
    } else {
      other.push_back(n);
    }
  }

  auto sortByLabel = [](const PwNodeInfo& a, const PwNodeInfo& b) {
    const QString la = a.description.isEmpty() ? a.name : a.description;
    const QString lb = b.description.isEmpty() ? b.name : b.description;
    return la.toLower() < lb.toLower();
  };
  std::sort(playback.begin(), playback.end(), sortByLabel);
  std::sort(recording.begin(), recording.end(), sortByLabel);
  {
    QSettings s;
    const QStringList order = s.value(SettingsKeys::sinksOrder()).toStringList();
    QHash<QString, int> indexByName;
    indexByName.reserve(order.size());
    for (int i = 0; i < order.size(); ++i) {
      indexByName.insert(order[i], i);
    }

    std::sort(outputs.begin(), outputs.end(), [&](const PwNodeInfo& a, const PwNodeInfo& b) {
      const int ia = indexByName.value(a.name, 1'000'000);
      const int ib = indexByName.value(b.name, 1'000'000);
      if (ia != ib) {
        return ia < ib;
      }
      return sortByLabel(a, b);
    });
  }
  std::sort(inputs.begin(), inputs.end(), sortByLabel);
  std::sort(other.begin(), other.end(), sortByLabel);

  auto onVisualizeNode = [this](const PwNodeInfo& node) {
    emit visualizerTapRequested(node.name, node.mediaClass == QStringLiteral("Audio/Sink"));
  };
  auto onVisualizeStream = [this](const PwNodeInfo& stream) {
    emit visualizerTapRequested(stream.name, false);
  };

  auto onEq = [this](const PwNodeInfo& node) {
    if (!m_eq) {
      return;
    }

    const QString label = node.description.isEmpty() ? node.name : node.description;
    const EqPreset initial = m_eq->presetForNodeName(node.name);

    EqDialog dlg(label, initial, this);
    if (dlg.exec() != QDialog::Accepted) {
      return;
    }

    m_eq->setPresetForNodeName(node.name, dlg.preset());
  };

  layout->addWidget(makeStreamsSection(tr("Playback (apps)"), playback, outputs, m_graph, m_pw, filter, onEq, onVisualizeStream, meters, m_container));
  layout->addWidget(makeStreamsSection(tr("Recording (apps)"), recording, inputs, m_graph, m_pw, filter, onEq, onVisualizeStream, meters, m_container));

  const uint32_t defaultSinkId = m_graph ? m_graph->defaultAudioSinkId().value_or(0) : 0;
  const uint32_t defaultSourceId = m_graph ? m_graph->defaultAudioSourceId().value_or(0) : 0;

  auto repopulateDefaultBox = [this](QComboBox* box, QPushButton* button, const QList<PwNodeInfo>& devices, uint32_t currentDefaultId) {
    if (!box || !button) {
      return;
    }

    const uint32_t previousSelected = box->currentData().toUInt();
    const bool supported = m_graph && m_graph->hasDefaultDeviceSupport();

    QSignalBlocker blocker(box);
    box->clear();

    if (!supported) {
      box->addItem(tr("(PipeWire metadata unavailable)"), QVariant{0});
      box->setEnabled(false);
      button->setEnabled(false);
      return;
    }

    if (devices.isEmpty()) {
      box->addItem(tr("(no devices)"), QVariant{0});
      box->setEnabled(false);
      button->setEnabled(false);
      return;
    }

    for (const auto& d : devices) {
      const QString label = d.description.isEmpty() ? d.name : d.description;
      box->addItem(label, QVariant::fromValue<quint32>(d.id));
    }

    uint32_t want = previousSelected;
    auto containsId = [&](uint32_t id) {
      for (int i = 0; i < box->count(); ++i) {
        if (box->itemData(i).toUInt() == id) {
          return i;
        }
      }
      return -1;
    };

    if (want == 0 || containsId(want) < 0) {
      want = currentDefaultId;
    }
    int idx = containsId(want);
    if (idx < 0) {
      idx = 0;
    }
    box->setCurrentIndex(idx);

    box->setEnabled(true);
    button->setEnabled(true);
  };

  repopulateDefaultBox(m_defaultOutput, m_setDefaultOutput, outputs, defaultSinkId);
  repopulateDefaultBox(m_defaultInput, m_setDefaultInput, inputs, defaultSourceId);

  layout->addWidget(makeSection(tr("Output Devices"), outputs, m_graph, m_pw, defaultSinkId, filter, onEq, onVisualizeNode, meters, m_container));
  layout->addWidget(makeSection(tr("Input Devices"), inputs, m_graph, m_pw, defaultSourceId, filter, onEq, onVisualizeNode, meters, m_container));

  layout->addWidget(makeSection(tr("Other Nodes"), other, m_graph, m_pw, 0, filter, {}, onVisualizeNode, meters, m_container));

  layout->addStretch(1);

  m_meters = meters;
}
