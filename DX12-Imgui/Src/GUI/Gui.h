#pragma once
#include <imgui.h>

#define GUI_NAME "Testing"

namespace Gui
{
    extern bool show_gui;

    void Render();
    void Shutdown();
}
