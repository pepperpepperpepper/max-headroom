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

namespace mixer {
QGroupBox* makeSection(const QString& title,
                       const QList<PwNodeInfo>& nodes,
                       PipeWireGraph* graph,
                       PipeWireThread* pw,
                       uint32_t defaultNodeId,
                       const QString& filter,
                       std::function<void(const PwNodeInfo&)> onEqForNode,
                       std::function<void(const PwNodeInfo&)> onVisualizeForNode,
                       QList<QPointer<LevelMeterWidget>>& meters,
                       QWidget* parent);

QGroupBox* makeStreamsSection(const QString& title,
                              const QList<PwNodeInfo>& streams,
                              const QList<PwNodeInfo>& devices,
                              PipeWireGraph* graph,
                              PipeWireThread* pw,
                              const QString& filter,
                              std::function<void(const PwNodeInfo&)> onEqForStream,
                              std::function<void(const PwNodeInfo&)> onVisualizeForStream,
                              QList<QPointer<LevelMeterWidget>>& meters,
                              QWidget* parent);
} // namespace mixer

namespace {
bool isInternalNode(const PwNodeInfo& node)
{
  return node.name.startsWith(QStringLiteral("headroom."));
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

  layout->addWidget(mixer::makeStreamsSection(tr("Playback (apps)"), playback, outputs, m_graph, m_pw, filter, onEq, onVisualizeStream, meters, m_container));
  layout->addWidget(mixer::makeStreamsSection(tr("Recording (apps)"), recording, inputs, m_graph, m_pw, filter, onEq, onVisualizeStream, meters, m_container));

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

  layout->addWidget(mixer::makeSection(tr("Output Devices"), outputs, m_graph, m_pw, defaultSinkId, filter, onEq, onVisualizeNode, meters, m_container));
  layout->addWidget(mixer::makeSection(tr("Input Devices"), inputs, m_graph, m_pw, defaultSourceId, filter, onEq, onVisualizeNode, meters, m_container));

  layout->addWidget(mixer::makeSection(tr("Other Nodes"), other, m_graph, m_pw, 0, filter, {}, onVisualizeNode, meters, m_container));

  layout->addStretch(1);

  m_meters = meters;
}
