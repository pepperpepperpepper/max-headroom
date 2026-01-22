#include "SettingsDialog.h"

#include "backend/PipeWireGraph.h"
#include "settings/SettingsKeys.h"
#include "settings/VisualizerSettings.h"
#include "ui/AutoConnectRulesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace {
QString nodeLabel(const PwNodeInfo& node)
{
  return node.description.isEmpty() ? node.name : node.description;
}

template <typename T>
int findComboIndexByData(QComboBox* box, const T& want)
{
  if (!box) {
    return -1;
  }
  for (int i = 0; i < box->count(); ++i) {
    const QVariant v = box->itemData(i);
    if constexpr (std::is_floating_point_v<T>) {
      if (std::abs(v.toDouble() - want) < 1.0e-6) {
        return i;
      }
    } else {
      if (v.value<T>() == want) {
        return i;
      }
    }
  }
  return -1;
}
} // namespace

SettingsDialog::SettingsDialog(PipeWireGraph* graph, QWidget* parent)
    : QDialog(parent)
    , m_graph(graph)
{
  setWindowTitle(tr("Settings"));
  setModal(true);
  resize(680, 520);

  auto* root = new QVBoxLayout(this);

  auto* tabs = new QTabWidget(this);
  root->addWidget(tabs, 1);

  auto* general = new QWidget(tabs);
  {
    auto* v = new QVBoxLayout(general);

    auto* layout = new QGroupBox(tr("Patchbay Layout"), general);
    auto* layoutV = new QVBoxLayout(layout);

    auto* help = new QLabel(tr("Enable layout edit mode to drag nodes in Patchbay. Positions are saved automatically."), layout);
    help->setWordWrap(true);
    layoutV->addWidget(help);

    m_layoutEditMode = new QCheckBox(tr("Enable layout edit mode (drag nodes in Patchbay)"), layout);
    layoutV->addWidget(m_layoutEditMode);

    auto* actions = new QHBoxLayout();
    actions->addStretch(1);
    auto* resetLayoutBtn = new QPushButton(tr("Reset Saved Layout"), layout);
    actions->addWidget(resetLayoutBtn);
    layoutV->addLayout(actions);

    connect(resetLayoutBtn, &QPushButton::clicked, this, &SettingsDialog::resetPatchbayLayout);

    v->addWidget(layout);

    auto* autoConnect = new QGroupBox(tr("Patchbay Auto-Connect"), general);
    auto* autoV = new QVBoxLayout(autoConnect);
    auto* autoHelp = new QLabel(tr("Define regex-based rules to automatically connect ports when nodes appear."), autoConnect);
    autoHelp->setWordWrap(true);
    autoV->addWidget(autoHelp);
    auto* autoActions = new QHBoxLayout();
    autoActions->addStretch(1);
    auto* autoBtn = new QPushButton(tr("Configure Auto-Connect…"), autoConnect);
    autoActions->addWidget(autoBtn);
    autoV->addLayout(autoActions);

    connect(autoBtn, &QPushButton::clicked, this, [this]() {
      AutoConnectRulesDialog dlg(m_graph, this);
      dlg.exec();
    });

    v->addWidget(autoConnect);
    v->addStretch(1);
  }
  tabs->addTab(general, tr("General"));

  auto* devices = new QWidget(tabs);
  {
    auto* v = new QVBoxLayout(devices);

    auto* outputs = new QGroupBox(tr("Output Devices"), devices);
    auto* outV = new QVBoxLayout(outputs);

    auto* help = new QLabel(tr("Drag to reorder Audio/Sink nodes. This affects Mixer and Patchbay layout."), outputs);
    help->setWordWrap(true);
    outV->addWidget(help);

    m_sinksList = new QListWidget(outputs);
    m_sinksList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sinksList->setDragDropMode(QAbstractItemView::InternalMove);
    m_sinksList->setDefaultDropAction(Qt::MoveAction);
    outV->addWidget(m_sinksList, 1);

    auto* actionsRow = new QHBoxLayout();
    actionsRow->addStretch(1);
    auto* resetBtn = new QPushButton(tr("Reset Order"), outputs);
    actionsRow->addWidget(resetBtn);
    outV->addLayout(actionsRow);

    connect(resetBtn, &QPushButton::clicked, this, &SettingsDialog::resetSinksOrder);

    v->addWidget(outputs, 1);
    v->addStretch(1);
  }
  tabs->addTab(devices, tr("Devices"));

  auto* visualizer = new QWidget(tabs);
  {
    auto* v = new QVBoxLayout(visualizer);

    auto* display = new QGroupBox(tr("Display"), visualizer);
    auto* displayForm = new QFormLayout(display);

    m_vizRefresh = new QComboBox(display);
    m_vizRefresh->addItem(tr("60 FPS"), 16);
    m_vizRefresh->addItem(tr("30 FPS"), 33);
    m_vizRefresh->addItem(tr("15 FPS"), 67);
    m_vizRefresh->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    displayForm->addRow(tr("Refresh rate:"), m_vizRefresh);

    m_vizWaveHistory = new QComboBox(display);
    m_vizWaveHistory->addItem(tr("0.25 s"), 0.25);
    m_vizWaveHistory->addItem(tr("0.5 s"), 0.5);
    m_vizWaveHistory->addItem(tr("1 s"), 1.0);
    m_vizWaveHistory->addItem(tr("2 s"), 2.0);
    m_vizWaveHistory->addItem(tr("4 s"), 4.0);
    m_vizWaveHistory->addItem(tr("8 s"), 8.0);
    m_vizWaveHistory->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    displayForm->addRow(tr("Waveform history:"), m_vizWaveHistory);

    v->addWidget(display);

    auto* analysis = new QGroupBox(tr("Analysis"), visualizer);
    auto* analysisForm = new QFormLayout(analysis);

    m_vizFftSize = new QComboBox(analysis);
    m_vizFftSize->addItem(tr("512"), 512);
    m_vizFftSize->addItem(tr("1024"), 1024);
    m_vizFftSize->addItem(tr("2048"), 2048);
    m_vizFftSize->addItem(tr("4096"), 4096);
    m_vizFftSize->addItem(tr("8192"), 8192);
    m_vizFftSize->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    analysisForm->addRow(tr("FFT size:"), m_vizFftSize);

    auto* smoothingRow = new QWidget(analysis);
    auto* smoothingLayout = new QHBoxLayout(smoothingRow);
    smoothingLayout->setContentsMargins(0, 0, 0, 0);
    m_vizSmoothing = new QSlider(Qt::Horizontal, smoothingRow);
    m_vizSmoothing->setRange(0, 99);
    m_vizSmoothing->setSingleStep(1);
    m_vizSmoothingLabel = new QLabel(smoothingRow);
    m_vizSmoothingLabel->setMinimumWidth(52);
    m_vizSmoothingLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    smoothingLayout->addWidget(m_vizSmoothing, 1);
    smoothingLayout->addWidget(m_vizSmoothingLabel);
    analysisForm->addRow(tr("Spectrum smoothing:"), smoothingRow);

    connect(m_vizSmoothing, &QSlider::valueChanged, this, [this](int value) {
      if (m_vizSmoothingLabel) {
        m_vizSmoothingLabel->setText(tr("%1%").arg(value));
      }
    });

    m_vizSpecHistory = new QComboBox(analysis);
    m_vizSpecHistory->addItem(tr("2 s"), 2.0);
    m_vizSpecHistory->addItem(tr("4 s"), 4.0);
    m_vizSpecHistory->addItem(tr("8 s"), 8.0);
    m_vizSpecHistory->addItem(tr("12 s"), 12.0);
    m_vizSpecHistory->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    analysisForm->addRow(tr("Spectrogram history:"), m_vizSpecHistory);

    v->addWidget(analysis);
    v->addStretch(1);
  }
  tabs->addTab(visualizer, tr("Visualizer"));

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
  root->addWidget(buttons);

  loadGeneral();
  loadVisualizer();
  loadSinksOrder();
}

void SettingsDialog::loadGeneral()
{
  QSettings s;
  const bool editMode = s.value(SettingsKeys::patchbayLayoutEditMode()).toBool();
  if (m_layoutEditMode) {
    m_layoutEditMode->setChecked(editMode);
  }
}

void SettingsDialog::loadVisualizer()
{
  QSettings s;
  const VisualizerSettings cfg = VisualizerSettingsStore::load(s);

  if (m_vizRefresh) {
    const int idx = findComboIndexByData<int>(m_vizRefresh, cfg.refreshIntervalMs);
    m_vizRefresh->setCurrentIndex(idx >= 0 ? idx : 0);
  }

  if (m_vizFftSize) {
    const int idx = findComboIndexByData<int>(m_vizFftSize, cfg.fftSize);
    m_vizFftSize->setCurrentIndex(idx >= 0 ? idx : 0);
  }

  if (m_vizSmoothing) {
    const int val = std::clamp(static_cast<int>(std::lround(cfg.spectrumSmoothing * 100.0)), 0, 99);
    m_vizSmoothing->setValue(val);
  }
  if (m_vizSmoothingLabel && m_vizSmoothing) {
    m_vizSmoothingLabel->setText(tr("%1%").arg(m_vizSmoothing->value()));
  }

  if (m_vizWaveHistory) {
    const int idx = findComboIndexByData<double>(m_vizWaveHistory, cfg.waveformHistorySeconds);
    m_vizWaveHistory->setCurrentIndex(idx >= 0 ? idx : 0);
  }

  if (m_vizSpecHistory) {
    const int idx = findComboIndexByData<double>(m_vizSpecHistory, cfg.spectrogramHistorySeconds);
    m_vizSpecHistory->setCurrentIndex(idx >= 0 ? idx : 0);
  }
}

void SettingsDialog::loadSinksOrder()
{
  if (!m_sinksList) {
    return;
  }

  m_sinksList->clear();

  if (!m_graph) {
    return;
  }

  const QList<PwNodeInfo> sinks = m_graph->audioSinks();

  QSettings s;
  const QStringList saved = s.value(SettingsKeys::sinksOrder()).toStringList();

  QHash<QString, PwNodeInfo> byName;
  byName.reserve(sinks.size());
  for (const auto& node : sinks) {
    byName.insert(node.name, node);
  }

  QStringList used;
  used.reserve(saved.size());
  for (const auto& name : saved) {
    if (!byName.contains(name)) {
      continue;
    }
    const PwNodeInfo node = byName.value(name);
    auto* item = new QListWidgetItem(nodeLabel(node), m_sinksList);
    item->setData(Qt::UserRole, node.name);
    item->setToolTip(node.name);
    used.push_back(node.name);
  }

  QList<PwNodeInfo> remaining;
  remaining.reserve(sinks.size());
  for (const auto& node : sinks) {
    if (!used.contains(node.name)) {
      remaining.push_back(node);
    }
  }
  std::sort(remaining.begin(), remaining.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return nodeLabel(a) < nodeLabel(b); });
  for (const auto& node : remaining) {
    auto* item = new QListWidgetItem(nodeLabel(node), m_sinksList);
    item->setData(Qt::UserRole, node.name);
    item->setToolTip(node.name);
  }
}

QStringList SettingsDialog::currentSinksOrder() const
{
  QStringList order;
  if (!m_sinksList) {
    return order;
  }

  order.reserve(m_sinksList->count());
  for (int i = 0; i < m_sinksList->count(); ++i) {
    const QString name = m_sinksList->item(i)->data(Qt::UserRole).toString();
    if (!name.isEmpty()) {
      order.push_back(name);
    }
  }
  return order;
}

QStringList SettingsDialog::defaultSinksOrder() const
{
  QStringList order;
  if (!m_graph) {
    return order;
  }

  QList<PwNodeInfo> sinks = m_graph->audioSinks();
  std::sort(sinks.begin(), sinks.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return nodeLabel(a).toLower() < nodeLabel(b).toLower(); });
  order.reserve(sinks.size());
  for (const auto& node : sinks) {
    order.push_back(node.name);
  }
  return order;
}

void SettingsDialog::accept()
{
  QSettings s;
  const bool prevLayoutEdit = s.value(SettingsKeys::patchbayLayoutEditMode()).toBool();
  const QStringList previous = s.value(SettingsKeys::sinksOrder()).toStringList();
  const VisualizerSettings prevViz = VisualizerSettingsStore::load(s);

  bool layoutChanged = false;
  const bool nextLayoutEdit = m_layoutEditMode && m_layoutEditMode->isChecked();
  if (nextLayoutEdit != prevLayoutEdit) {
    if (nextLayoutEdit) {
      s.setValue(SettingsKeys::patchbayLayoutEditMode(), true);
    } else {
      s.remove(SettingsKeys::patchbayLayoutEditMode());
    }
    layoutChanged = true;
  }

  if (m_resetLayoutRequested) {
    s.remove(SettingsKeys::patchbayLayoutPositionsGroup());
    layoutChanged = true;
  }

  const QStringList next = currentSinksOrder();
  const QStringList defaultOrder = defaultSinksOrder();

  const bool storeCustom = !defaultOrder.isEmpty() && next != defaultOrder;
  const QStringList newStored = storeCustom ? next : QStringList{};

  if (newStored.isEmpty()) {
    s.remove(SettingsKeys::sinksOrder());
  } else {
    s.setValue(SettingsKeys::sinksOrder(), newStored);
  }

  if (newStored != previous) {
    emit sinksOrderChanged();
  }

  if (layoutChanged) {
    emit layoutSettingsChanged();
  }

  VisualizerSettings nextViz = VisualizerSettingsStore::defaults();
  if (m_vizRefresh) {
    nextViz.refreshIntervalMs = m_vizRefresh->currentData().toInt();
  }
  if (m_vizFftSize) {
    nextViz.fftSize = m_vizFftSize->currentData().toInt();
  }
  if (m_vizSmoothing) {
    nextViz.spectrumSmoothing = static_cast<double>(m_vizSmoothing->value()) / 100.0;
  }
  if (m_vizWaveHistory) {
    nextViz.waveformHistorySeconds = m_vizWaveHistory->currentData().toDouble();
  }
  if (m_vizSpecHistory) {
    nextViz.spectrogramHistorySeconds = m_vizSpecHistory->currentData().toDouble();
  }
  VisualizerSettingsStore::save(s, nextViz);

  if (!VisualizerSettingsStore::approxEqual(prevViz, nextViz)) {
    emit visualizerSettingsChanged();
  }

  QDialog::accept();
}

void SettingsDialog::resetSinksOrder()
{
  if (!m_sinksList) {
    return;
  }

  m_sinksList->clear();
  if (!m_graph) {
    return;
  }

  QList<PwNodeInfo> sinks = m_graph->audioSinks();
  std::sort(sinks.begin(), sinks.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return nodeLabel(a).toLower() < nodeLabel(b).toLower(); });
  for (const auto& node : sinks) {
    auto* item = new QListWidgetItem(nodeLabel(node), m_sinksList);
    item->setData(Qt::UserRole, node.name);
    item->setToolTip(node.name);
  }
}

void SettingsDialog::resetPatchbayLayout()
{
  if (m_resetLayoutRequested) {
    return;
  }

  const auto answer = QMessageBox::question(this,
                                            tr("Reset Patchbay Layout"),
                                            tr("This will clear all saved Patchbay node positions. Continue?"),
                                            QMessageBox::Yes | QMessageBox::No,
                                            QMessageBox::No);
  if (answer != QMessageBox::Yes) {
    return;
  }

  m_resetLayoutRequested = true;
}
