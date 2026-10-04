#include "PluginDefinition.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        pluginInit(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        pluginCleanUp();
    }
    return TRUE;
}
