#include "file_picker_service.h"

#include "utils.h"

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <utility>

namespace {

class ScopedComApartment {
public:
    ScopedComApartment()
        : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
          initialized_(SUCCEEDED(hr_)) {}

    ~ScopedComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    bool ok() const {
        return initialized_;
    }

private:
    HRESULT hr_;
    bool initialized_;
};

HWND DialogOwner(HWND owner_window) {
    if (!owner_window || !IsWindow(owner_window)) {
        return nullptr;
    }
    HWND root = GetAncestor(owner_window, GA_ROOT);
    return root ? root : owner_window;
}

}  // namespace

std::vector<std::string> FilePickerService::PickVideoFiles(
    bool allow_multiple, HWND owner_window) const {
    ScopedComApartment com;
    if (!com.ok()) {
        return {};
    }

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return {};
    }

    FILEOPENDIALOGOPTIONS options =
        FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
    if (allow_multiple) {
        options |= FOS_ALLOWMULTISELECT;
    }
    dialog->SetOptions(options);

    COMDLG_FILTERSPEC filter_spec[] = {
        {L"Video Files",
         L"*.avi;*.flv;*.mkv;*.mov;*.mp4;*.mpeg;*.webm;*.wmv;*.ts;*.m2ts;*.vob;*.mpg;*.m4v;*.3gp"},
        {L"All Files", L"*.*"},
    };
    dialog->SetFileTypes(2, filter_spec);
    dialog->SetFileTypeIndex(1);

    hr = dialog->Show(DialogOwner(owner_window));
    if (FAILED(hr)) {
        return {};
    }

    Microsoft::WRL::ComPtr<IShellItemArray> items;
    hr = dialog->GetResults(&items);
    if (FAILED(hr) || !items) {
        return {};
    }

    DWORD count = 0;
    items->GetCount(&count);
    std::vector<std::string> paths;
    paths.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item)) || !item) {
            continue;
        }

        LPWSTR name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name) {
            continue;
        }
        std::string path = Utf8FromUtf16(name);
        CoTaskMemFree(name);
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}
