// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#ifndef _PreComp_
#include <algorithm>
#include <cmath>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#endif

#include <Gui/Action.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/MainWindow.h>
#include <Gui/TaskView/TaskView.h>

#include "RadialMenu.h"

namespace
{

constexpr int buttonSize = 72;
constexpr int buttonSpacing = 10;
constexpr int minimumRadius = 104;
constexpr int menuMargin = 12;
constexpr double pi = 3.14159265358979323846;

using CommandNames = std::vector<QByteArray>;

CommandNames commandsForSelection()
{
    auto* taskPanel = Gui::Control().taskPanel();
    return taskPanel ? taskPanel->matchingWatcherCommands() : CommandNames {};
}

class RadialMenu: public QWidget
{
public:
    explicit RadialMenu(const CommandNames& commandNames)
        : QWidget(
              Gui::getMainWindow(),
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
          )
    {
        setObjectName(QStringLiteral("PartDesignRadialMenu"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::StrongFocus);

        auto& manager = Gui::Application::Instance->commandManager();
        for (const QByteArray& commandName : commandNames) {
            Gui::Command* command = manager.getCommandByName(commandName.constData());
            if (!command || !command->isActive()) {
                continue;
            }

            auto* button = new QToolButton(this);
            button->setObjectName(
                QStringLiteral("RadialMenu_%1").arg(QString::fromLatin1(commandName))
            );
            button->setAccessibleName(Gui::Action::commandMenuText(command));
            button->setText(Gui::Action::commandMenuText(command));
            button->setToolTip(Gui::Action::commandToolTip(command, true));
            button->setIcon(Gui::BitmapFactory().iconFromTheme(command->getPixmap()));
            button->setIconSize(QSize(28, 28));
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->setFixedSize(buttonSize, buttonSize);
            button->setFocusPolicy(Qt::StrongFocus);
            button->setStyleSheet(QStringLiteral(
                "QToolButton {"
                "  background-color: transparent;"
                "  border: 2px solid transparent;"
                "  border-radius: 36px;"
                "  padding: 5px;"
                "}"
                "QToolButton:hover, QToolButton:focus {"
                "  background-color: transparent;"
                "  border: 2px solid transparent;"
                "}"
                "QToolButton:pressed {"
                "  background-color: transparent;"
                "  border: 2px solid transparent;"
                "}"));

            const QByteArray name(commandName);
            connect(button, &QToolButton::clicked, this, [this, name]() {
                close();
                QTimer::singleShot(0, qApp, [name]() {
                    Gui::Application::Instance->commandManager().runCommandByName(name.constData());
                });
            });
            buttons.push_back(button);
        }
    }

    bool isEmpty() const
    {
        return buttons.empty();
    }

    void showAt(const QPoint& globalPosition)
    {
        const int count = static_cast<int>(buttons.size());
        const int radius = std::max(
            minimumRadius,
            static_cast<int>(std::ceil(count * (buttonSize + buttonSpacing) / (2.0 * pi)))
        );
        const int halfExtent = radius + buttonSize / 2 + menuMargin;
        const int extent = 2 * halfExtent;
        resize(extent, extent);

        const QPoint center(halfExtent, halfExtent);
        for (int index = 0; index < count; ++index) {
            const double angle = -pi / 2.0 + (2.0 * pi * index / count);
            const QPoint buttonCenter(
                center.x() + qRound(radius * std::cos(angle)),
                center.y() + qRound(radius * std::sin(angle))
            );
            buttons[index]->move(buttonCenter - QPoint(buttonSize / 2, buttonSize / 2));
        }

        QScreen* screen = QGuiApplication::screenAt(globalPosition);
        if (!screen) {
            screen = QGuiApplication::primaryScreen();
        }

        QPoint topLeft = globalPosition - center;
        if (screen) {
            const QRect available = screen->availableGeometry();
            topLeft.setX(std::clamp(topLeft.x(), available.left(), available.right() - width() + 1));
            topLeft.setY(std::clamp(topLeft.y(), available.top(), available.bottom() - height() + 1));
        }

        move(topLeft);
        show();
        raise();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_S && event->modifiers() == Qt::NoModifier)
            || event->key() == Qt::Key_Escape) {
            if (event->isAutoRepeat()) {
                event->accept();
                return;
            }
            close();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    std::vector<QToolButton*> buttons;
};

QPointer<RadialMenu> activeMenu;

class RadialMenuShortcutFilter: public QObject
{
public:
    RadialMenuShortcutFilter()
        : QObject(qApp)
    {}

    void setEnabled(bool value)
    {
        enabled = value;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        Q_UNUSED(watched);

        if (!enabled
            || (event->type() != QEvent::ShortcutOverride
                && event->type() != QEvent::KeyPress)) {
            return false;
        }

        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (event->type() == QEvent::KeyPress && activeMenu
            && keyEvent->key() == Qt::Key_Escape && !keyEvent->isAutoRepeat()) {
            activeMenu->close();
            keyEvent->accept();
            return true;
        }

        if (keyEvent->key() != Qt::Key_S || keyEvent->modifiers() != Qt::NoModifier) {
            return false;
        }

        if (keyEvent->isAutoRepeat()) {
            if (activeMenu) {
                keyEvent->accept();
                return true;
            }
            return false;
        }

        // Match ShortcutManager's protection for text entry.  A single-letter
        // modeling shortcut must never consume text typed into an editor.
        if (auto* focus = QApplication::focusWidget()) {
            QWidget* focusOrProxy = focus->focusProxy() ? focus->focusProxy() : focus;
            if (focusOrProxy->inherits("QLineEdit") || focusOrProxy->inherits("QTextEdit")
                || focusOrProxy->inherits("QPlainTextEdit")) {
                return false;
            }
        }

        auto& manager = Gui::Application::Instance->commandManager();
        Gui::Command* command = manager.getCommandByName("PartDesign_RadialMenu");
        if (!command || !command->isActive()
            || QKeySequence(command->getShortcut()) != QKeySequence(Qt::Key_S)) {
            return false;
        }

        // S is also the first key of several multi-key shortcuts.  Claim it
        // before Qt's shortcut processing so ShortcutManager does not delay
        // and replay this same key press after the radial menu has opened.
        if (event->type() == QEvent::ShortcutOverride) {
            keyEvent->accept();
            return true;
        }

        PartDesignGui::toggleRadialMenu(QCursor::pos());
        keyEvent->accept();
        return true;
    }

private:
    bool enabled = false;
};

QPointer<RadialMenuShortcutFilter> shortcutFilter;

}  // namespace

void PartDesignGui::setRadialMenuShortcutEnabled(bool enabled)
{
    if (!shortcutFilter) {
        shortcutFilter = new RadialMenuShortcutFilter();
        qApp->installEventFilter(shortcutFilter);
    }
    shortcutFilter->setEnabled(enabled);
}

void PartDesignGui::toggleRadialMenu(const QPoint& globalPosition)
{
    if (activeMenu) {
        activeMenu->close();
        return;
    }

    auto* menu = new RadialMenu(commandsForSelection());
    if (menu->isEmpty()) {
        menu->deleteLater();
        return;
    }

    activeMenu = menu;
    menu->showAt(globalPosition);
}
