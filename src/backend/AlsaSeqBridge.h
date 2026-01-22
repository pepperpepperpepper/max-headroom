#pragma once

#include <QString>

class AlsaSeqBridge final
{
public:
  static QString configSnippetPath();
  static bool isConfigInstalled();
  static bool installConfig(QString* errorOut = nullptr);
  static bool removeConfig(QString* errorOut = nullptr);
  static bool alsaSequencerDevicePresent();
};

