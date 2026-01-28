#include "backend/AudioLevelTap.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "ui/LevelMeterWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHash>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMultiHash>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace mixer {
bool movePlaybackStreamToSink(PipeWireGraph* graph, uint32_t streamId, uint32_t sinkId);
bool moveCaptureStreamToSource(PipeWireGraph* graph, uint32_t streamId, uint32_t sourceId);

namespace {
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
} // namespace

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
} // namespace mixer
