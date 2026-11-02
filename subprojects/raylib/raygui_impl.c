// Single translation unit that compiles the raygui implementation.
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

// Editor extension: raygui keeps the text box cursor in a file-static variable
// and only moves it on mouse clicks, so expose a setter for programmatic focus.
void GuiSetTextBoxCursor(int index)
{
    textBoxCursorIndex = index;
    autoCursorCounter = 0;
}
