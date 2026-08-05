// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#ifndef _PreComp_
#include <algorithm>
#include <cmath>
#include <initializer_list>
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

#include <App/DocumentObject.h>
#include <Gui/Action.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/FeatureSketchBased.h>
#include <Mod/PartDesign/App/ShapeBinder.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "RadialMenu.h"

namespace
{

constexpr int buttonSize = 72;
constexpr int buttonSpacing = 10;
constexpr int minimumRadius = 104;
constexpr int menuMargin = 12;
constexpr double pi = 3.14159265358979323846;

using CommandNames = std::vector<const char*>;

void appendUnique(CommandNames& result, std::initializer_list<const char*> commands)
{
    for (const char* command : commands) {
        if (std::find(result.begin(), result.end(), command) == result.end()) {
            result.push_back(command);
        }
    }
}

CommandNames commandsForSelection()
{
    const auto selection = Gui::Selection().getSelectionEx();
    if (selection.empty()) {
        return {"PartDesign_NewSketch"};
    }

    int faces = 0;
    int edges = 0;
    int vertices = 0;
    int sketches = 0;
    int bodies = 0;
    int shapeBinders = 0;
    int sketchBasedFeatures = 0;
    int otherObjects = 0;

    for (const auto& selected : selection) {
        const App::DocumentObject* object = selected.getObject();
        if (!object) {
            continue;
        }

        const auto& subNames = selected.getSubNames();
        if (!subNames.empty()) {
            for (const auto& subName : subNames) {
                if (subName.rfind("Face", 0) == 0) {
                    ++faces;
                }
                else if (subName.rfind("Edge", 0) == 0) {
                    ++edges;
                }
                else if (subName.rfind("Vertex", 0) == 0) {
                    ++vertices;
                }
                else {
                    ++otherObjects;
                }
            }
            continue;
        }

        if (object->isDerivedFrom<Sketcher::SketchObject>()) {
            ++sketches;
        }
        else if (object->isDerivedFrom<PartDesign::Body>()) {
            ++bodies;
        }
        else if (object->isDerivedFrom<PartDesign::ShapeBinder>()
                 || object->isDerivedFrom<PartDesign::SubShapeBinder>()) {
            ++shapeBinders;
        }
        else if (object->isDerivedFrom<PartDesign::ProfileBased>()) {
            ++sketchBasedFeatures;
        }
        else {
            ++otherObjects;
        }
    }

    CommandNames result;

    // A selected face is a valid direct profile for Pad and Pocket.  Keep those
    // high-frequency operations nearest the beginning of the clockwise ring.
    if (faces == 1 && edges == 0 && vertices == 0 && sketches == 0 && otherObjects == 0) {
        appendUnique(result,
                     {"PartDesign_NewSketch",
                      "PartDesign_Pad",
                      "PartDesign_Pocket",
                      "PartDesign_Fillet",
                      "PartDesign_Chamfer",
                      "PartDesign_Draft",
                      "PartDesign_Thickness"});
    }
    else if (faces > 0) {
        appendUnique(result,
                     {"PartDesign_Fillet",
                      "PartDesign_Chamfer",
                      "PartDesign_Draft",
                      "PartDesign_Thickness"});
    }

    if (edges > 0) {
        appendUnique(result, {"PartDesign_Fillet", "PartDesign_Chamfer"});
    }

    if (vertices > 0 && faces == 0 && edges == 0) {
        appendUnique(result,
                     {"Part_DatumPoint",
                      "Part_DatumLine",
                      "Part_DatumPlane",
                      "Part_CoordinateSystem"});
    }

    if (sketches == 1 && selection.size() == 1) {
        appendUnique(result,
                     {"PartDesign_Pad",
                      "PartDesign_Pocket",
                      "PartDesign_Hole",
                      "PartDesign_Revolution",
                      "PartDesign_Groove",
                      "PartDesign_AdditiveLoft",
                      "PartDesign_SubtractiveLoft",
                      "PartDesign_AdditivePipe",
                      "PartDesign_SubtractivePipe",
                      "PartDesign_AdditiveHelix",
                      "PartDesign_SubtractiveHelix"});
    }
    else if (sketches > 1 && sketches == static_cast<int>(selection.size())) {
        appendUnique(result,
                     {"PartDesign_AdditiveLoft",
                      "PartDesign_SubtractiveLoft",
                      "PartDesign_AdditivePipe",
                      "PartDesign_SubtractivePipe"});
    }

    if (shapeBinders == 1 && selection.size() == 1) {
        appendUnique(result,
                     {"PartDesign_Pad",
                      "PartDesign_Pocket",
                      "PartDesign_Revolution",
                      "PartDesign_Groove",
                      "PartDesign_AdditiveLoft",
                      "PartDesign_SubtractiveLoft",
                      "PartDesign_AdditivePipe",
                      "PartDesign_SubtractivePipe"});
    }

    if (sketchBasedFeatures == 1 && selection.size() == 1) {
        appendUnique(result,
                     {"PartDesign_Mirrored",
                      "PartDesign_LinearPattern",
                      "PartDesign_PolarPattern",
                      "PartDesign_MultiTransform"});
    }

    if (bodies == 1 && selection.size() == 1) {
        appendUnique(result, {"PartDesign_NewSketch"});
    }
    else if (bodies > 1 && bodies == static_cast<int>(selection.size())) {
        appendUnique(result, {"PartDesign_Boolean"});
    }

    return result;
}

class RadialMenu: public QWidget
{
public:
    explicit RadialMenu(const CommandNames& commandNames)
        : QWidget(Gui::getMainWindow(), Qt::Popup | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("PartDesignRadialMenu"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::StrongFocus);

        auto& manager = Gui::Application::Instance->commandManager();
        for (const char* commandName : commandNames) {
            Gui::Command* command = manager.getCommandByName(commandName);
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
                "  border: 1px solid transparent;"
                "  border-radius: 36px;"
                "  padding: 5px;"
                "}"
                "QToolButton:hover, QToolButton:focus {"
                "  background-color: transparent;"
                "  border: 2px solid palette(highlight);"
                "}"
                "QToolButton:pressed {"
                "  background-color: transparent;"
                "  border: 2px solid palette(dark);"
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
        activateWindow();
        setFocus(Qt::ShortcutFocusReason);
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_S && event->modifiers() == Qt::NoModifier)
            || event->key() == Qt::Key_Escape) {
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

        if (!enabled || event->type() != QEvent::KeyPress) {
            return false;
        }

        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() != Qt::Key_S || keyEvent->modifiers() != Qt::NoModifier
            || keyEvent->isAutoRepeat()) {
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
