#include "EqDialog.h"

#include "ui/EqResponseWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace {
void populateBandTypeCombo(QComboBox* c)
{
  c->addItem(QObject::tr("Peaking"), static_cast<int>(EqBandType::Peaking));
  c->addItem(QObject::tr("Low shelf"), static_cast<int>(EqBandType::LowShelf));
  c->addItem(QObject::tr("High shelf"), static_cast<int>(EqBandType::HighShelf));
  c->addItem(QObject::tr("Low pass"), static_cast<int>(EqBandType::LowPass));
  c->addItem(QObject::tr("High pass"), static_cast<int>(EqBandType::HighPass));
  c->addItem(QObject::tr("Notch"), static_cast<int>(EqBandType::Notch));
  c->addItem(QObject::tr("Band pass"), static_cast<int>(EqBandType::BandPass));
}

int indexForType(QComboBox* c, EqBandType t)
{
  if (!c) {
    return 0;
  }
  const int target = static_cast<int>(t);
  for (int i = 0; i < c->count(); ++i) {
    if (c->itemData(i).toInt() == target) {
      return i;
    }
  }
  return 0;
}

QString presetIdForName(const QString& name)
{
  const QByteArray enc = name.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

struct PresetEntry final {
  QString id;
  QString name;
};

QVector<PresetEntry> listPresetLibrary()
{
  QSettings s;
  s.beginGroup(QStringLiteral("eqPresets"));
  const QStringList ids = s.childGroups();

  QVector<PresetEntry> out;
  out.reserve(ids.size());
  for (const auto& id : ids) {
    s.beginGroup(id);
    QString name = s.value(QStringLiteral("displayName")).toString();
    s.endGroup();
    if (name.isEmpty()) {
      name = id;
    }
    out.push_back(PresetEntry{id, name});
  }
  s.endGroup();

  std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) { return a.name.toLower() < b.name.toLower(); });
  return out;
}

std::optional<EqPreset> parseAutoEqText(const QString& text)
{
  EqPreset preset;
  preset.enabled = true;
  preset.preampDb = 0.0;

  static const QRegularExpression preampRe(QStringLiteral(R"(^\s*Preamp:\s*([-+]?\d+(?:\.\d+)?)\s*dB\s*$)"),
                                           QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression filterRe(
      QStringLiteral(R"(^\s*Filter\s+\d+\s*:\s*(ON|OFF)\s+([A-Za-z]+)\s+Fc\s+(\d+(?:\.\d+)?)\s*Hz\s+Gain\s+([-+]?\d+(?:\.\d+)?)\s*dB\s+Q\s+(\d+(?:\.\d+)?)\s*$)"),
      QRegularExpression::CaseInsensitiveOption);

  const QStringList lines = text.split('\n');
  for (QString line : lines) {
    line = line.trimmed();
    if (line.isEmpty()) {
      continue;
    }

    const auto preampMatch = preampRe.match(line);
    if (preampMatch.hasMatch()) {
      preset.preampDb = preampMatch.captured(1).toDouble();
      continue;
    }

    const auto filterMatch = filterRe.match(line);
    if (!filterMatch.hasMatch()) {
      continue;
    }

    const QString onOff = filterMatch.captured(1).trimmed().toUpper();
    const QString typeStr = filterMatch.captured(2).trimmed().toUpper();
    const double freq = filterMatch.captured(3).toDouble();
    const double gain = filterMatch.captured(4).toDouble();
    const double q = filterMatch.captured(5).toDouble();

    EqBandType type = EqBandType::Peaking;
    if (typeStr == QStringLiteral("PK") || typeStr == QStringLiteral("PEAKING")) {
      type = EqBandType::Peaking;
    } else if (typeStr == QStringLiteral("LS") || typeStr == QStringLiteral("LSC") || typeStr == QStringLiteral("LOWSHELF")) {
      type = EqBandType::LowShelf;
    } else if (typeStr == QStringLiteral("HS") || typeStr == QStringLiteral("HSC") || typeStr == QStringLiteral("HIGHSHELF")) {
      type = EqBandType::HighShelf;
    } else if (typeStr == QStringLiteral("LP") || typeStr == QStringLiteral("LPF") || typeStr == QStringLiteral("LOWPASS")) {
      type = EqBandType::LowPass;
    } else if (typeStr == QStringLiteral("HP") || typeStr == QStringLiteral("HPF") || typeStr == QStringLiteral("HIGHPASS")) {
      type = EqBandType::HighPass;
    } else if (typeStr == QStringLiteral("N") || typeStr == QStringLiteral("NOTCH")) {
      type = EqBandType::Notch;
    } else if (typeStr == QStringLiteral("BP") || typeStr == QStringLiteral("BPF") || typeStr == QStringLiteral("BANDPASS")) {
      type = EqBandType::BandPass;
    } else {
      // Unknown filter type.
      continue;
    }

    EqBand band;
    band.enabled = (onOff == QStringLiteral("ON"));
    band.type = type;
    band.freqHz = freq;
    band.gainDb = gain;
    band.q = q;
    preset.bands.push_back(band);
  }

  if (preset.bands.isEmpty()) {
    return std::nullopt;
  }
  return preset;
}
} // namespace

EqDialog::EqDialog(const QString& deviceLabel, const EqPreset& initial, QWidget* parent)
    : QDialog(parent)
{
  setWindowTitle(tr("Parametric EQ — %1").arg(deviceLabel));
  setModal(true);
  resize(760, 520);

  m_defaults = defaultEqPreset(10);

  auto* root = new QVBoxLayout(this);

  auto* form = new QFormLayout();
  m_enabled = new QCheckBox(tr("Enable EQ"), this);
  form->addRow(QString{}, m_enabled);

  m_preamp = new QDoubleSpinBox(this);
  m_preamp->setDecimals(1);
  m_preamp->setRange(-24.0, 24.0);
  m_preamp->setSuffix(tr(" dB"));
  form->addRow(tr("Preamp:"), m_preamp);
  root->addLayout(form);

  auto* responseBox = new QGroupBox(tr("Response"), this);
  auto* responseLayout = new QVBoxLayout(responseBox);
  m_response = new EqResponseWidget(responseBox);
  responseLayout->addWidget(m_response);
  root->addWidget(responseBox);

  auto* bandsBox = new QGroupBox(tr("Bands"), this);
  auto* grid = new QGridLayout(bandsBox);
  grid->setColumnStretch(2, 1);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(6);

  grid->addWidget(new QLabel(tr("On"), bandsBox), 0, 0);
  grid->addWidget(new QLabel(tr("Type"), bandsBox), 0, 1);
  grid->addWidget(new QLabel(tr("Freq (Hz)"), bandsBox), 0, 2);
  grid->addWidget(new QLabel(tr("Gain (dB)"), bandsBox), 0, 3);
  grid->addWidget(new QLabel(tr("Q"), bandsBox), 0, 4);

  const int n = std::max(1, static_cast<int>(std::max(initial.bands.size(), m_defaults.bands.size())));
  m_bandEnabled.reserve(n);
  m_bandType.reserve(n);
  m_bandFreq.reserve(n);
  m_bandGain.reserve(n);
  m_bandQ.reserve(n);

  for (int i = 0; i < n; ++i) {
    auto* en = new QCheckBox(bandsBox);
    auto* type = new QComboBox(bandsBox);
    populateBandTypeCombo(type);

    auto* freq = new QDoubleSpinBox(bandsBox);
    freq->setDecimals(1);
    freq->setRange(20.0, 20000.0);
    freq->setSingleStep(10.0);

    auto* gain = new QDoubleSpinBox(bandsBox);
    gain->setDecimals(1);
    gain->setRange(-24.0, 24.0);
    gain->setSingleStep(0.5);

    auto* q = new QDoubleSpinBox(bandsBox);
    q->setDecimals(2);
    q->setRange(0.1, 24.0);
    q->setSingleStep(0.1);

    grid->addWidget(en, i + 1, 0);
    grid->addWidget(type, i + 1, 1);
    grid->addWidget(freq, i + 1, 2);
    grid->addWidget(gain, i + 1, 3);
    grid->addWidget(q, i + 1, 4);

    m_bandEnabled.push_back(en);
    m_bandType.push_back(type);
    m_bandFreq.push_back(freq);
    m_bandGain.push_back(gain);
    m_bandQ.push_back(q);
  }

  root->addWidget(bandsBox, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  auto* loadBtn = buttons->addButton(tr("Load…"), QDialogButtonBox::ActionRole);
  auto* saveBtn = buttons->addButton(tr("Save…"), QDialogButtonBox::ActionRole);
  auto* importBtn = buttons->addButton(tr("Import…"), QDialogButtonBox::ActionRole);
  auto* resetBtn = buttons->addButton(tr("Reset"), QDialogButtonBox::ResetRole);
  connect(loadBtn, &QPushButton::clicked, this, &EqDialog::loadPresetFromLibrary);
  connect(saveBtn, &QPushButton::clicked, this, &EqDialog::savePresetToLibrary);
  connect(importBtn, &QPushButton::clicked, this, &EqDialog::importPresetFromTextFile);
  connect(resetBtn, &QPushButton::clicked, this, &EqDialog::resetToDefaults);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);

  auto onChange = [this]() { refreshResponse(); };
  connect(m_enabled, &QCheckBox::toggled, this, onChange);
  connect(m_preamp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
  for (int i = 0; i < n; ++i) {
    connect(m_bandEnabled[i], &QCheckBox::toggled, this, onChange);
    connect(m_bandType[i], QOverload<int>::of(&QComboBox::currentIndexChanged), this, onChange);
    connect(m_bandFreq[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
    connect(m_bandGain[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
    connect(m_bandQ[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onChange);
  }

  loadFromPreset(initial);
}

EqPreset EqDialog::preset() const
{
  return collectPreset();
}

void EqDialog::resetToDefaults()
{
  loadFromPreset(m_defaults);
}

void EqDialog::loadFromPreset(const EqPreset& p)
{
  if (m_enabled) {
    m_enabled->setChecked(p.enabled);
  }
  if (m_preamp) {
    m_preamp->setValue(p.preampDb);
  }

  const int n = static_cast<int>(std::min(p.bands.size(), m_bandEnabled.size()));
  for (int i = 0; i < n; ++i) {
    const EqBand b = p.bands[i];
    m_bandEnabled[i]->setChecked(b.enabled);
    m_bandType[i]->setCurrentIndex(indexForType(m_bandType[i], b.type));
    m_bandFreq[i]->setValue(b.freqHz);
    m_bandGain[i]->setValue(b.gainDb);
    m_bandQ[i]->setValue(b.q);
  }

  // Any remaining UI bands (if preset has fewer) stay as-is but are disabled by default.
  for (int i = n; i < m_bandEnabled.size(); ++i) {
    m_bandEnabled[i]->setChecked(false);
  }

  refreshResponse();
}

EqPreset EqDialog::collectPreset() const
{
  EqPreset p;
  p.enabled = m_enabled ? m_enabled->isChecked() : false;
  p.preampDb = m_preamp ? m_preamp->value() : 0.0;

  const int n = static_cast<int>(std::min({m_bandEnabled.size(), m_bandType.size(), m_bandFreq.size(), m_bandGain.size(), m_bandQ.size()}));
  p.bands.reserve(n);
  for (int i = 0; i < n; ++i) {
    EqBand b;
    b.enabled = m_bandEnabled[i]->isChecked();
    b.type = static_cast<EqBandType>(m_bandType[i]->currentData().toInt());
    b.freqHz = m_bandFreq[i]->value();
    b.gainDb = m_bandGain[i]->value();
    b.q = m_bandQ[i]->value();
    p.bands.push_back(b);
  }

  return p;
}

void EqDialog::refreshResponse()
{
  if (!m_response) {
    return;
  }
  m_response->setPreset(collectPreset());
}

void EqDialog::loadPresetFromLibrary()
{
  const auto entries = listPresetLibrary();
  if (entries.isEmpty()) {
    QMessageBox::information(this, tr("Load Preset"), tr("No saved presets yet."));
    return;
  }

  QStringList names;
  names.reserve(entries.size());
  for (const auto& e : entries) {
    names.push_back(e.name);
  }

  bool ok = false;
  const QString chosen = QInputDialog::getItem(this, tr("Load Preset"), tr("Preset:"), names, 0, false, &ok);
  if (!ok || chosen.trimmed().isEmpty()) {
    return;
  }

  QString id;
  for (const auto& e : entries) {
    if (e.name == chosen) {
      id = e.id;
      break;
    }
  }
  if (id.isEmpty()) {
    return;
  }

  QSettings s;
  s.beginGroup(QStringLiteral("eqPresets/%1").arg(id));
  const QString json = s.value(QStringLiteral("presetJson")).toString();
  s.endGroup();
  if (json.trimmed().isEmpty()) {
    return;
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    QMessageBox::warning(this, tr("Load Preset"), tr("Preset data is invalid JSON."));
    return;
  }

  loadFromPreset(eqPresetFromJson(doc.object()));
}

void EqDialog::savePresetToLibrary()
{
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("Save Preset"), tr("Preset name:"), QLineEdit::Normal, QString{}, &ok).trimmed();
  if (!ok || name.isEmpty()) {
    return;
  }

  auto entries = listPresetLibrary();
  QString id;
  for (const auto& e : entries) {
    if (e.name == name) {
      id = e.id;
      break;
    }
  }
  if (!id.isEmpty()) {
    const auto answer = QMessageBox::question(this,
                                              tr("Overwrite Preset"),
                                              tr("A preset named “%1” already exists. Overwrite it?").arg(name),
                                              QMessageBox::Yes | QMessageBox::No,
                                              QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      return;
    }
  } else {
    id = presetIdForName(name);
  }

  const EqPreset p = collectPreset();
  const QJsonDocument doc(eqPresetToJson(p));

  QSettings s;
  s.beginGroup(QStringLiteral("eqPresets/%1").arg(id));
  s.setValue(QStringLiteral("displayName"), name);
  s.setValue(QStringLiteral("presetJson"), QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
  s.endGroup();
}

void EqDialog::importPresetFromTextFile()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Import EQ Preset"), QString{}, tr("Text files (*.txt);;All files (*)"));
  if (path.trimmed().isEmpty()) {
    return;
  }

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QMessageBox::warning(this, tr("Import EQ Preset"), tr("Failed to open file."));
    return;
  }

  const QString text = QString::fromUtf8(f.readAll());
  const auto parsed = parseAutoEqText(text);
  if (!parsed.has_value()) {
    QMessageBox::warning(this, tr("Import EQ Preset"), tr("Could not find any filters to import."));
    return;
  }

  EqPreset p = parsed.value();
  if (p.bands.size() > m_bandEnabled.size()) {
    p.bands.resize(m_bandEnabled.size());
    QMessageBox::information(this, tr("Import EQ Preset"), tr("Imported preset has more bands than this dialog supports; extra bands were dropped."));
  }
  loadFromPreset(p);
}
