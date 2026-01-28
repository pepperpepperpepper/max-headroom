#include "tui/TuiInternal.h"

#include <algorithm>
#include <cstring>

#include <curses.h>

namespace headroomtui {

void promptScrollLines(const char* title, const QStringList& lines, int height, int width)
{
  timeout(-1);
  noecho();
  curs_set(0);

  int top = 0;
  while (true) {
    erase();
    mvprintw(0, 0, "%s", title);
    mvaddnstr(1, 0, "Up/Down scroll  PgUp/PgDn page  q to close", std::max(0, width - 1));

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 1);
    const int totalLines = static_cast<int>(lines.size());
    const int maxTop = std::max(0, totalLines - listHeight);
    top = std::clamp(top, 0, maxTop);

    for (int row = 0; row < listHeight; ++row) {
      const int idx = top + row;
      if (idx >= totalLines) {
        break;
      }
      const QByteArray utf8 = lines.at(idx).toUtf8();
      mvaddnstr(listTop + row, 0, utf8.constData(), std::max(0, width - 1));
    }

    refresh();
    const int ch = getch();
    if (ch == 'q' || ch == 'Q' || ch == 27) {
      break;
    }
    if (ch == KEY_UP) {
      top = std::max(0, top - 1);
    } else if (ch == KEY_DOWN) {
      top = std::min(maxTop, top + 1);
    } else if (ch == KEY_PPAGE) {
      top = std::max(0, top - listHeight);
    } else if (ch == KEY_NPAGE) {
      top = std::min(maxTop, top + listHeight);
    }
  }

  timeout(kMainLoopTimeoutMs);
}

QString promptInputLine(const char* title, const char* prompt, const QString& currentValue, int height, int width)
{
  timeout(-1);
  echo();
  curs_set(1);

  erase();
  mvprintw(0, 0, "%s", title);
  mvaddnstr(2, 0, prompt, std::max(0, width - 1));

  if (!currentValue.isEmpty()) {
    const QByteArray curUtf8 = QStringLiteral("Current: %1").arg(currentValue).toUtf8();
    mvaddnstr(3, 0, curUtf8.constData(), std::max(0, width - 1));
  }

  mvaddnstr(std::max(0, height - 2), 0, "Enter to accept, blank to cancel.", std::max(0, width - 1));
  mvprintw(5, 0, "> ");

  char buf[1024];
  std::memset(buf, 0, sizeof(buf));
  getnstr(buf, static_cast<int>(sizeof(buf) - 1));

  noecho();
  curs_set(0);
  timeout(kMainLoopTimeoutMs);

  return QString::fromLocal8Bit(buf).trimmed();
}

uint32_t promptSelectNodeId(const char* title, const QList<PwNodeInfo>& nodes, uint32_t currentId, int height, int width)
{
  const int nodeCount = static_cast<int>(nodes.size());
  if (!title || nodeCount <= 0 || height <= 0 || width <= 0) {
    return 0;
  }

  int selected = 0;
  if (currentId != 0) {
    for (int i = 0; i < nodeCount; ++i) {
      if (nodes[i].id == currentId) {
        selected = i;
        break;
      }
    }
  }

  timeout(-1);

  while (true) {
    erase();
    mvprintw(0, 0, "%s", title);
    mvprintw(1, 0, "Up/Down select  Enter confirm  Esc cancel");

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 2);

    selected = clampIndex(selected, nodeCount);

    int start = 0;
    if (selected >= listHeight) {
      start = selected - listHeight + 1;
    }
    start = std::clamp(start, 0, std::max(0, nodeCount - listHeight));

    for (int row = 0; row < listHeight; ++row) {
      const int idx = start + row;
      if (idx >= nodeCount) {
        break;
      }
      const bool isSel = (idx == selected);
      if (isSel) {
        attron(A_REVERSE);
      }

      const auto& n = nodes[idx];
      const QString name = displayNameForNode(n);
      const QByteArray nameUtf8 = name.toUtf8();
      mvprintw(listTop + row, 0, "%c %4u  ", isSel ? '>' : ' ', n.id);
      mvaddnstr(listTop + row, 8, nameUtf8.constData(), std::max(0, width - 10));

      if (isSel) {
        attroff(A_REVERSE);
      }
    }

    refresh();

    const int ch = getch();
    switch (ch) {
    case 27: // Esc
    case 'q':
    case 'Q':
      timeout(kMainLoopTimeoutMs);
      return 0;
    case KEY_UP:
    case 'k':
    case 'K':
      selected = clampIndex(selected - 1, nodeCount);
      break;
    case KEY_DOWN:
    case 'j':
    case 'J':
      selected = clampIndex(selected + 1, nodeCount);
      break;
    case '\n':
    case KEY_ENTER:
      timeout(kMainLoopTimeoutMs);
      return nodes[selected].id;
    default:
      break;
    }
  }
}

} // namespace headroomtui

