// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

class QPoint;

namespace PartDesignGui
{

/** Enable direct handling of the radial-menu shortcut while Part Design is active. */
void setRadialMenuShortcutEnabled(bool enabled);

/** Toggle the context-sensitive Part Design radial menu at a global screen position. */
void toggleRadialMenu(const QPoint& globalPosition);

}  // namespace PartDesignGui
