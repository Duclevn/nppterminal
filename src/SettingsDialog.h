#pragma once

#include "Settings.h"

#include <windows.h>

#include <functional>
#include <string>

namespace nppterminal::settings {

using SaveCallback = std::function<bool(const Settings&, std::wstring&)>;

// Shows the native settings dialog.  The caller owns persistence through the
// optional callback, which is invoked only after the form validates.  Cancel
// and dialog creation failure leave updated unchanged.
bool showSettingsDialog(HMODULE module, HWND parent, const Settings& current,
    Settings& updated, SaveCallback saveCallback = {});

} // namespace nppterminal::settings
