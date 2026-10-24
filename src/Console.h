#pragma once

// On Windows the editor is a GUI program, so it starts without stdout and stderr. This
// connects them to the console of the terminal that started it, for command-line options
// like --version. Output that is redirected (to a file or a pipe) already works and is left
// alone. Does nothing on other systems.
void attachParentConsole();
