#pragma once

#include <windows.h>

#include <string>
#include <vector>

class FilePickerService {
public:
    std::vector<std::string> PickVideoFiles(bool allow_multiple, HWND owner_window) const;
};
