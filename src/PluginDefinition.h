#pragma once

#include "PluginInterface.h"

#include <windows.h>

constexpr wchar_t NPP_PLUGIN_NAME[] = L"NppTerminal";
constexpr int nbFunc = 4;

void pluginInit(HANDLE module);
void pluginCleanUp();
void commandMenuInit();
void commandMenuCleanUp();
bool setCommand(std::size_t index, const wchar_t* name, PFUNCPLUGINCMD function,
    ShortcutKey* shortcut = nullptr, bool checkOnInit = false);
void toggleTerminal();
void terminalSettings();
void openTerminalHere();
void aboutPlugin();
