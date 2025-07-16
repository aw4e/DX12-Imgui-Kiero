#include "Gui.h"

namespace Gui
{
    bool show_gui = true;
    static ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    void Shutdown()
    {
        show_gui = true;
    }

    void Render()
    {
        if (show_gui)
        {
            ImGui::Begin(GUI_NAME, &show_gui, ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Hi there!");
            ImGui::End();
        }
    }
}
