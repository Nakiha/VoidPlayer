#pragma once

#include <string>
#include <vector>

class FilePickerService {
public:
    std::vector<std::string> PickVideoFiles(bool allow_multiple) const;
};
