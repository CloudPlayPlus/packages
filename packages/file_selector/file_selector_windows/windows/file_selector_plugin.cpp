// Copyright 2013 The Flutter Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "file_selector_plugin.h"

#include <comdef.h>
#include <comip.h>
#include <flutter/flutter_view.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <shobjidl.h>
#include <windows.h>

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "file_dialog_controller.h"
#include "string_utils.h"

_COM_SMARTPTR_TYPEDEF(IEnumShellItems, IID_IEnumShellItems);
_COM_SMARTPTR_TYPEDEF(IFileDialog, IID_IFileDialog);
_COM_SMARTPTR_TYPEDEF(IShellItem, IID_IShellItem);
_COM_SMARTPTR_TYPEDEF(IShellItemArray, IID_IShellItemArray);

namespace file_selector_windows {

namespace {

using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableValue;

// The kind of file dialog to show.
enum class DialogMode { open, save };

constexpr wchar_t kMessageWindowClassName[] =
    L"FileSelectorWindowsStaWorkerWindow";
constexpr UINT kShutdownMessage = WM_APP + 0x491;
constexpr UINT_PTR kShutdownTimerId = 1;
constexpr UINT kShutdownRetryMilliseconds = 10;

using DialogActivationCallback =
    std::function<bool(FileDialogController* dialog)>;

// Returns the path for |shell_item| as a UTF-8 string, or an
// empty string on failure.
std::string GetPathForShellItem(IShellItem* shell_item) {
  if (shell_item == nullptr) {
    return "";
  }
  wchar_t* wide_path = nullptr;
  if (!SUCCEEDED(shell_item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
    return "";
  }
  std::string path = Utf8FromUtf16(wide_path);
  ::CoTaskMemFree(wide_path);
  return path;
}

// Implementation of FileDialogControllerFactory that makes standard
// FileDialogController instances.
class DefaultFileDialogControllerFactory : public FileDialogControllerFactory {
 public:
  DefaultFileDialogControllerFactory() {}
  virtual ~DefaultFileDialogControllerFactory() {}

  // Disallow copy and assign.
  DefaultFileDialogControllerFactory(
      const DefaultFileDialogControllerFactory&) = delete;
  DefaultFileDialogControllerFactory& operator=(
      const DefaultFileDialogControllerFactory&) = delete;

  std::unique_ptr<FileDialogController> CreateController(
      IFileDialog* dialog) const override {
    assert(dialog != nullptr);
    return std::make_unique<FileDialogController>(dialog);
  }
};

// Wraps an IFileDialog, managing object lifetime as a scoped object and
// providing a simplified API for interacting with it as needed for the plugin.
class DialogWrapper {
 public:
  explicit DialogWrapper(const FileDialogControllerFactory& dialog_factory,
                         IID type) {
    is_open_dialog_ = type == CLSID_FileOpenDialog;
    IFileDialogPtr dialog = nullptr;
    last_result_ = CoCreateInstance(type, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
    dialog_controller_ = dialog_factory.CreateController(dialog);
  }

  // Attempts to set the default folder for the dialog to |path|,
  // if it exists.
  void SetFolder(std::string_view path) {
    std::wstring wide_path = Utf16FromUtf8(path);
    IShellItemPtr item;
    last_result_ = SHCreateItemFromParsingName(wide_path.c_str(), nullptr,
                                               IID_PPV_ARGS(&item));
    if (!SUCCEEDED(last_result_)) {
      return;
    }
    dialog_controller_->SetFolder(item);
  }

  // Sets the file name that is initially shown in the dialog.
  void SetFileName(std::string_view name) {
    std::wstring wide_name = Utf16FromUtf8(name);
    last_result_ = dialog_controller_->SetFileName(wide_name.c_str());
  }

  // Sets the label of the confirmation button.
  void SetOkButtonLabel(std::string_view label) {
    std::wstring wide_label = Utf16FromUtf8(label);
    last_result_ = dialog_controller_->SetOkButtonLabel(wide_label.c_str());
  }

  // Adds the given options to the dialog's current option set.
  void AddOptions(FILEOPENDIALOGOPTIONS new_options) {
    FILEOPENDIALOGOPTIONS options;
    last_result_ = dialog_controller_->GetOptions(&options);
    if (!SUCCEEDED(last_result_)) {
      return;
    }
    options |= new_options;
    if (options & FOS_PICKFOLDERS) {
      opening_directory_ = true;
    }
    last_result_ = dialog_controller_->SetOptions(options);
  }

  // Sets the filters for allowed file types to select.
  void SetFileTypeFilters(const EncodableList& filters) {
    const std::wstring spec_delimiter = L";";
    const std::wstring file_wildcard = L"*.";
    std::vector<COMDLG_FILTERSPEC> filter_specs;
    // Temporary ownership of the constructed strings whose data is used in
    // filter_specs, so that they live until the call to SetFileTypes is done.
    std::vector<std::wstring> filter_names;
    std::vector<std::wstring> filter_extensions;
    filter_extensions.reserve(filters.size());
    filter_names.reserve(filters.size());

    for (const EncodableValue& filter_info_value : filters) {
      const auto& type_group = std::any_cast<TypeGroup>(
          std::get<CustomEncodableValue>(filter_info_value));
      filter_names.push_back(Utf16FromUtf8(type_group.label()));
      filter_extensions.push_back(L"");
      std::wstring& spec = filter_extensions.back();
      if (type_group.extensions().empty()) {
        spec += L"*.*";
      } else {
        for (const EncodableValue& extension : type_group.extensions()) {
          if (!spec.empty()) {
            spec += spec_delimiter;
          }
          spec +=
              file_wildcard + Utf16FromUtf8(std::get<std::string>(extension));
        }
      }
      filter_specs.push_back({filter_names.back().c_str(), spec.c_str()});
    }
    last_result_ = dialog_controller_->SetFileTypes(
        static_cast<UINT>(filter_specs.size()), filter_specs.data());
  }

  // Displays the dialog, and returns the result, or nullopt on error.
  std::optional<FileDialogResult> Show(
      HWND parent_window, const DialogActivationCallback& activation_callback) {
    assert(dialog_controller_);
    if (!activation_callback(dialog_controller_.get())) {
      activation_callback(nullptr);
      last_result_ = HRESULT_FROM_WIN32(ERROR_CANCELLED);
      return std::nullopt;
    }
    last_result_ = dialog_controller_->Show(parent_window);
    activation_callback(nullptr);
    if (!SUCCEEDED(last_result_)) {
      return std::nullopt;
    }

    EncodableList files;
    if (is_open_dialog_) {
      IShellItemArrayPtr shell_items;
      last_result_ = dialog_controller_->GetResults(&shell_items);
      if (!SUCCEEDED(last_result_)) {
        return std::nullopt;
      }
      IEnumShellItemsPtr item_enumerator;
      last_result_ = shell_items->EnumItems(&item_enumerator);
      if (!SUCCEEDED(last_result_)) {
        return std::nullopt;
      }
      IShellItemPtr shell_item;
      while (item_enumerator->Next(1, &shell_item, nullptr) == S_OK) {
        files.push_back(EncodableValue(GetPathForShellItem(shell_item)));
      }
    } else {
      IShellItemPtr shell_item;
      last_result_ = dialog_controller_->GetResult(&shell_item);
      if (!SUCCEEDED(last_result_)) {
        return std::nullopt;
      }
      files.push_back(EncodableValue(GetPathForShellItem(shell_item)));
    }
    FileDialogResult result(files, nullptr);
    UINT file_type_index;
    if (SUCCEEDED(dialog_controller_->GetFileTypeIndex(&file_type_index)) &&
        file_type_index > 0) {
      // Convert from the one-based index to a Dart index.
      result.set_type_group_index(file_type_index - 1);
    }
    return result;
  }

  // Returns the result of the last Win32 API call related to this object.
  HRESULT last_result() { return last_result_; }

 private:
  // The dialog controller that all interactions are mediated through, to allow
  // for unit testing.
  std::unique_ptr<FileDialogController> dialog_controller_;
  bool is_open_dialog_;
  bool opening_directory_ = false;
  HRESULT last_result_;
};

ErrorOr<FileDialogResult> ShowDialog(
    const FileDialogControllerFactory& dialog_factory, HWND parent_window,
    DialogMode mode, const SelectionOptions& options,
    const std::string* initial_directory, const std::string* suggested_name,
    const std::string* confirm_label,
    const DialogActivationCallback& activation_callback) {
  IID dialog_type =
      mode == DialogMode::save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
  DialogWrapper dialog(dialog_factory, dialog_type);
  if (!SUCCEEDED(dialog.last_result())) {
    return FlutterError(
        "System error", "Could not create dialog",
        EncodableValue(std::in_place_type<int32_t>, dialog.last_result()));
  }

  FILEOPENDIALOGOPTIONS dialog_options = 0;
  if (options.select_folders()) {
    dialog_options |= FOS_PICKFOLDERS;
  }
  if (options.allow_multiple()) {
    dialog_options |= FOS_ALLOWMULTISELECT;
  }
  if (dialog_options != 0) {
    dialog.AddOptions(dialog_options);
  }

  if (initial_directory) {
    dialog.SetFolder(*initial_directory);
  }
  if (suggested_name) {
    dialog.SetFileName(*suggested_name);
  }
  if (confirm_label) {
    dialog.SetOkButtonLabel(*confirm_label);
  }

  if (!options.allowed_types().empty()) {
    dialog.SetFileTypeFilters(options.allowed_types());
  }

  std::optional<FileDialogResult> result =
      dialog.Show(parent_window, activation_callback);
  if (!result) {
    if (dialog.last_result() != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      return FlutterError(
          "System error", "Could not show dialog",
          EncodableValue(std::in_place_type<int32_t>, dialog.last_result()));
    } else {
      return FileDialogResult(EncodableList(), nullptr);
    }
  }
  return std::move(result.value());
}

// Returns the top-level window that owns |view|.
HWND GetRootWindow(flutter::FlutterView* view) {
  return ::GetAncestor(view->GetNativeWindow(), GA_ROOT);
}

}  // namespace

// static
void FileSelectorPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  std::unique_ptr<FileSelectorPlugin> plugin =
      std::make_unique<FileSelectorPlugin>(
          [registrar] { return GetRootWindow(registrar->GetView()); },
          std::make_unique<DefaultFileDialogControllerFactory>());

  FileSelectorApi::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
}

FileSelectorPlugin::FileSelectorPlugin(
    FlutterRootWindowProvider window_provider,
    std::unique_ptr<FileDialogControllerFactory> dialog_controller_factory)
    : get_root_window_(std::move(window_provider)),
      controller_factory_(std::move(dialog_controller_factory)),
      task_event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      worker_ready_event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
  if (task_event_ && worker_ready_event_) {
    worker_ = std::thread(&FileSelectorPlugin::WorkerLoop, this);
    ::WaitForSingleObject(worker_ready_event_, INFINITE);
  }
}

FileSelectorPlugin::~FileSelectorPlugin() {
  shutdown_requested_.store(true);
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    stopping_ = true;
    std::queue<Task> empty;
    tasks_.swap(empty);
  }
  if (HWND window = message_window_.load()) {
    ::PostMessageW(window, kShutdownMessage, 0, 0);
  }
  if (task_event_) {
    ::SetEvent(task_event_);
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (worker_ready_event_) {
    ::CloseHandle(worker_ready_event_);
  }
  if (task_event_) {
    ::CloseHandle(task_event_);
  }
}

void FileSelectorPlugin::ShowOpenDialog(
    const SelectionOptions& options, const std::string* initialDirectory,
    const std::string* confirmButtonText,
    std::function<void(ErrorOr<FileDialogResult> reply)> result) {
  const HWND parent_window = get_root_window_();
  const std::optional<std::string> initial_directory =
      initialDirectory ? std::make_optional(*initialDirectory) : std::nullopt;
  const std::optional<std::string> confirm_button_text =
      confirmButtonText ? std::make_optional(*confirmButtonText) : std::nullopt;
  if (!Enqueue([this, parent_window, options, initial_directory,
                confirm_button_text, result](HRESULT com_result) {
        if (FAILED(com_result)) {
          result(FlutterError(
              "System error", "Could not initialize the file dialog thread",
              EncodableValue(std::in_place_type<int32_t>, com_result)));
          return;
        }
        result(ShowDialog(
            *controller_factory_, parent_window, DialogMode::open, options,
            initial_directory ? &*initial_directory : nullptr, nullptr,
            confirm_button_text ? &*confirm_button_text : nullptr,
            [this](FileDialogController* dialog) {
              active_dialog_ = dialog;
              return !dialog || !shutdown_requested_.load();
            }));
      })) {
    result(FlutterError("System error", "The file selector is shutting down"));
  }
}

void FileSelectorPlugin::ShowSaveDialog(
    const SelectionOptions& options, const std::string* initialDirectory,
    const std::string* suggestedName, const std::string* confirmButtonText,
    std::function<void(ErrorOr<FileDialogResult> reply)> result) {
  const HWND parent_window = get_root_window_();
  const std::optional<std::string> initial_directory =
      initialDirectory ? std::make_optional(*initialDirectory) : std::nullopt;
  const std::optional<std::string> suggested_name =
      suggestedName ? std::make_optional(*suggestedName) : std::nullopt;
  const std::optional<std::string> confirm_button_text =
      confirmButtonText ? std::make_optional(*confirmButtonText) : std::nullopt;
  if (!Enqueue([this, parent_window, options, initial_directory, suggested_name,
                confirm_button_text, result](HRESULT com_result) {
        if (FAILED(com_result)) {
          result(FlutterError(
              "System error", "Could not initialize the file dialog thread",
              EncodableValue(std::in_place_type<int32_t>, com_result)));
          return;
        }
        result(ShowDialog(*controller_factory_, parent_window, DialogMode::save,
                          options,
                          initial_directory ? &*initial_directory : nullptr,
                          suggested_name ? &*suggested_name : nullptr,
                          confirm_button_text ? &*confirm_button_text : nullptr,
                          [this](FileDialogController* dialog) {
                            active_dialog_ = dialog;
                            return !dialog || !shutdown_requested_.load();
                          }));
      })) {
    result(FlutterError("System error", "The file selector is shutting down"));
  }
}

bool FileSelectorPlugin::Enqueue(Task task) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (stopping_ || !worker_.joinable()) {
      return false;
    }
    tasks_.push(std::move(task));
  }
  ::SetEvent(task_event_);
  return true;
}

LRESULT CALLBACK FileSelectorPlugin::MessageWindowProc(HWND window,
                                                       UINT message,
                                                       WPARAM wparam,
                                                       LPARAM lparam) {
  FileSelectorPlugin* plugin = reinterpret_cast<FileSelectorPlugin*>(
      ::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    plugin = static_cast<FileSelectorPlugin*>(create->lpCreateParams);
    ::SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(plugin));
  } else if (plugin && (message == kShutdownMessage ||
                        (message == WM_TIMER && wparam == kShutdownTimerId))) {
    plugin->shutdown_requested_.store(true);
    if (plugin->active_dialog_) {
      const HRESULT close_result =
          plugin->active_dialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
      if (FAILED(close_result)) {
        ::SetTimer(window, kShutdownTimerId, kShutdownRetryMilliseconds,
                   nullptr);
      } else {
        ::KillTimer(window, kShutdownTimerId);
      }
    } else {
      ::KillTimer(window, kShutdownTimerId);
    }
    return 0;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

void FileSelectorPlugin::WorkerLoop() {
  const HRESULT com_result =
      ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HRESULT initialization_result = com_result;
  if (SUCCEEDED(initialization_result)) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &FileSelectorPlugin::MessageWindowProc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = kMessageWindowClassName;
    if (!::RegisterClassW(&window_class) &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      initialization_result = HRESULT_FROM_WIN32(::GetLastError());
    } else {
      message_window_.store(::CreateWindowExW(
          0, kMessageWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
          window_class.hInstance, this));
      if (!message_window_.load()) {
        initialization_result = HRESULT_FROM_WIN32(::GetLastError());
      }
    }
  }
  ::SetEvent(worker_ready_event_);

  for (;;) {
    const DWORD wait_result = ::MsgWaitForMultipleObjectsEx(
        1, &task_event_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait_result == WAIT_FAILED) {
      break;
    }
    if (wait_result == WAIT_OBJECT_0 + 1) {
      MSG message;
      while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
      }
      continue;
    }

    Task task;
    {
      std::lock_guard<std::mutex> lock(task_mutex_);
      if (stopping_) {
        break;
      }
      if (tasks_.empty()) {
        ::ResetEvent(task_event_);
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
      if (tasks_.empty()) {
        ::ResetEvent(task_event_);
      }
    }
    task(initialization_result);
  }
  if (HWND window = message_window_.exchange(nullptr)) {
    ::DestroyWindow(window);
  }
  if (SUCCEEDED(com_result)) {
    ::CoUninitialize();
  }
}

}  // namespace file_selector_windows
