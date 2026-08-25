// Copyright 2013 The Flutter Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "file_selector_plugin.h"

#include <flutter/method_call.h>
#include <flutter/method_result_functions.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <variant>

#include "file_dialog_controller.h"
#include "string_utils.h"
#include "test/test_file_dialog_controller.h"
#include "test/test_utils.h"

namespace file_selector_windows {
namespace test {

namespace {

using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableValue;

ErrorOr<FileDialogResult> WaitForOpenDialog(
    FileSelectorPlugin& plugin, const SelectionOptions& options,
    const std::string* initial_directory,
    const std::string* confirm_button_text) {
  std::promise<ErrorOr<FileDialogResult>> promise;
  std::future<ErrorOr<FileDialogResult>> future = promise.get_future();
  plugin.ShowOpenDialog(options, initial_directory, confirm_button_text,
                        [&promise](ErrorOr<FileDialogResult> reply) {
                          promise.set_value(std::move(reply));
                        });
  return future.get();
}

ErrorOr<FileDialogResult> WaitForSaveDialog(
    FileSelectorPlugin& plugin, const SelectionOptions& options,
    const std::string* initial_directory, const std::string* suggested_name,
    const std::string* confirm_button_text) {
  std::promise<ErrorOr<FileDialogResult>> promise;
  std::future<ErrorOr<FileDialogResult>> future = promise.get_future();
  plugin.ShowSaveDialog(options, initial_directory, suggested_name,
                        confirm_button_text,
                        [&promise](ErrorOr<FileDialogResult> reply) {
                          promise.set_value(std::move(reply));
                        });
  return future.get();
}

IShellItemPtr CreateShellItem(const std::wstring& path) {
  IShellItemPtr item;
  ::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
  return item;
}

IShellItemArrayPtr CreateShellItemArray(const std::wstring& path) {
  IShellItemPtr item = CreateShellItem(path);
  IShellItemArrayPtr items;
  ::SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items));
  return items;
}

IShellItemArrayPtr CreateShellItemArray(const std::wstring& first_path,
                                        const std::wstring& second_path) {
  PIDLIST_ABSOLUTE first_item = ::ILCreateFromPath(first_path.c_str());
  PIDLIST_ABSOLUTE second_item = ::ILCreateFromPath(second_path.c_str());
  LPCITEMIDLIST item_ids[] = {first_item, second_item};
  IShellItemArrayPtr items;
  ::SHCreateShellItemArrayFromIDLists(2, item_ids, &items);
  ::ILFree(first_item);
  ::ILFree(second_item);
  return items;
}

}  // namespace

TEST(FileSelectorPlugin, TestOpenSimple) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  const DWORD caller_thread = ::GetCurrentThreadId();
  DWORD dialog_thread = caller_thread;
  ScopedTestShellItem fake_selected_file;
  const std::wstring selected_path = fake_selected_file.path();

  bool shown = false;
  MockShow show_validator =
      [&shown, &dialog_thread, selected_path, fake_window](
          const TestFileDialogController& dialog, HWND parent) {
        shown = true;
        dialog_thread = ::GetCurrentThreadId();
        EXPECT_EQ(parent, fake_window);

        // Validate options.
        FILEOPENDIALOGOPTIONS options;
        dialog.GetOptions(&options);
        EXPECT_EQ(options & FOS_ALLOWMULTISELECT, 0U);
        EXPECT_EQ(options & FOS_PICKFOLDERS, 0U);

        return MockShowResult(CreateShellItemArray(selected_path));
      };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      nullptr, nullptr);

  EXPECT_TRUE(shown);
  EXPECT_NE(dialog_thread, caller_thread);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file.path()));
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestOpenWithArguments) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  ScopedTestShellItem fake_selected_file;
  const std::wstring selected_path = fake_selected_file.path();

  bool shown = false;
  MockShow show_validator = [&shown, selected_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate arguments.
    EXPECT_EQ(dialog.GetDialogFolderPath(), L"C:\\Program Files");
    // Make sure that the folder was called via SetFolder, not SetDefaultFolder.
    EXPECT_EQ(dialog.GetSetFolderPath(), L"C:\\Program Files");
    EXPECT_EQ(dialog.GetOkButtonLabel(), L"Open it!");

    return MockShowResult(CreateShellItemArray(selected_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  // This directory must exist.
  std::string initial_directory("C:\\Program Files");
  std::string confirm_button("Open it!");
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      &initial_directory, &confirm_button);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file.path()));
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestOpenMultiple) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  ScopedTestFileIdList fake_selected_file_1;
  ScopedTestFileIdList fake_selected_file_2;
  const std::wstring first_path = fake_selected_file_1.path();
  const std::wstring second_path = fake_selected_file_2.path();

  bool shown = false;
  MockShow show_validator = [&shown, first_path, second_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate options.
    FILEOPENDIALOGOPTIONS options;
    dialog.GetOptions(&options);
    EXPECT_NE(options & FOS_ALLOWMULTISELECT, 0U);
    EXPECT_EQ(options & FOS_PICKFOLDERS, 0U);

    return MockShowResult(CreateShellItemArray(first_path, second_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ true,
                       /* select folders = */ false, EncodableList()),
      nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file_1.path()));
  EXPECT_EQ(std::get<std::string>(paths[1]),
            Utf8FromUtf16(fake_selected_file_2.path()));
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestOpenWithFilter) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  ScopedTestShellItem fake_selected_file;
  const std::wstring selected_path = fake_selected_file.path();

  const EncodableValue text_group =
      CustomEncodableValue(TypeGroup("Text", EncodableList({
                                                 EncodableValue("txt"),
                                                 EncodableValue("json"),
                                             })));
  const EncodableValue image_group =
      CustomEncodableValue(TypeGroup("Images", EncodableList({
                                                   EncodableValue("png"),
                                                   EncodableValue("gif"),
                                                   EncodableValue("jpeg"),
                                               })));
  const EncodableValue any_group =
      CustomEncodableValue(TypeGroup("Any", EncodableList()));

  bool shown = false;
  MockShow show_validator = [&shown, selected_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate filter.
    const std::vector<DialogFilter>& filters = dialog.GetFileTypes();
    EXPECT_EQ(filters.size(), 3U);
    if (filters.size() == 3U) {
      EXPECT_EQ(filters[0].name, L"Text");
      EXPECT_EQ(filters[0].spec, L"*.txt;*.json");
      EXPECT_EQ(filters[1].name, L"Images");
      EXPECT_EQ(filters[1].spec, L"*.png;*.gif;*.jpeg");
      EXPECT_EQ(filters[2].name, L"Any");
      EXPECT_EQ(filters[2].spec, L"*.*");
    }

    return MockShowResult(CreateShellItemArray(selected_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result =
      WaitForOpenDialog(plugin,
                        SelectionOptions(/* allow multiple = */ false,
                                         /* select folders = */ false,
                                         EncodableList({
                                             text_group,
                                             image_group,
                                             any_group,
                                         })),
                        nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file.path()));
  // The test dialog controller always reports the last group as
  // selected, so that should be what the plugin returns.
  ASSERT_NE(result.value().type_group_index(), nullptr);
  EXPECT_EQ(*(result.value().type_group_index()), 2);
}

TEST(FileSelectorPlugin, TestOpenCancel) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);

  bool shown = false;
  MockShow show_validator = [&shown, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    return MockShowResult();
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  EXPECT_EQ(paths.size(), 0);
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestShutdownClosesOpenDialog) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  std::promise<void> dialog_shown;
  std::future<void> dialog_shown_future = dialog_shown.get_future();
  bool close_observed = false;

  MockShow show_validator = [&dialog_shown, &close_observed](
                                const TestFileDialogController& dialog,
                                HWND /* parent */) {
    dialog_shown.set_value();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!dialog.was_closed() &&
           std::chrono::steady_clock::now() < deadline) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      const DWORD timeout =
          remaining.count() > 0 ? static_cast<DWORD>(remaining.count()) : 0;
      const DWORD wait_result = ::MsgWaitForMultipleObjectsEx(
          0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      if (wait_result == WAIT_TIMEOUT) {
        break;
      }
      MSG message;
      while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
      }
    }
    close_observed = dialog.was_closed();
    return MockShowResult();
  };

  std::promise<ErrorOr<FileDialogResult>> reply;
  std::future<ErrorOr<FileDialogResult>> reply_future = reply.get_future();
  auto plugin = std::make_unique<FileSelectorPlugin>(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  plugin->ShowOpenDialog(
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      nullptr, nullptr, [&reply](ErrorOr<FileDialogResult> result) {
        reply.set_value(std::move(result));
      });

  ASSERT_EQ(dialog_shown_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  plugin.reset();

  EXPECT_TRUE(close_observed);
  ASSERT_EQ(reply_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  ErrorOr<FileDialogResult> result = reply_future.get();
  ASSERT_FALSE(result.has_error());
  EXPECT_TRUE(result.value().paths().empty());
}

TEST(FileSelectorPlugin, TestSaveSimple) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  ScopedTestShellItem fake_selected_file;
  const std::wstring selected_path = fake_selected_file.path();

  bool shown = false;
  MockShow show_validator = [&shown, selected_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate options.
    FILEOPENDIALOGOPTIONS options;
    dialog.GetOptions(&options);
    EXPECT_EQ(options & FOS_ALLOWMULTISELECT, 0U);
    EXPECT_EQ(options & FOS_PICKFOLDERS, 0U);

    return MockShowResult(CreateShellItem(selected_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForSaveDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      nullptr, nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file.path()));
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestSaveWithArguments) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  ScopedTestShellItem fake_selected_file;
  const std::wstring selected_path = fake_selected_file.path();

  bool shown = false;
  MockShow show_validator = [&shown, selected_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate arguments.
    EXPECT_EQ(dialog.GetDialogFolderPath(), L"C:\\Program Files");
    // Make sure that the folder was called via SetFolder, not
    // SetDefaultFolder.
    EXPECT_EQ(dialog.GetSetFolderPath(), L"C:\\Program Files");
    EXPECT_EQ(dialog.GetFileName(), L"a name");
    EXPECT_EQ(dialog.GetOkButtonLabel(), L"Save it!");

    return MockShowResult(CreateShellItem(selected_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  // This directory must exist.
  std::string initial_directory("C:\\Program Files");
  std::string suggested_name("a name");
  std::string confirm_button("Save it!");
  ErrorOr<FileDialogResult> result = WaitForSaveDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      &initial_directory, &suggested_name, &confirm_button);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file.path()));
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestSaveWithFilter) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  ScopedTestShellItem fake_selected_file;
  const std::wstring selected_path = fake_selected_file.path();

  const EncodableValue text_group =
      CustomEncodableValue(TypeGroup("Text", EncodableList({
                                                 EncodableValue("txt"),
                                                 EncodableValue("json"),
                                             })));
  const EncodableValue image_group =
      CustomEncodableValue(TypeGroup("Images", EncodableList({
                                                   EncodableValue("png"),
                                                   EncodableValue("gif"),
                                                   EncodableValue("jpeg"),
                                               })));

  bool shown = false;
  MockShow show_validator = [&shown, selected_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate filter.
    const std::vector<DialogFilter>& filters = dialog.GetFileTypes();
    EXPECT_EQ(filters.size(), 2U);
    if (filters.size() == 2U) {
      EXPECT_EQ(filters[0].name, L"Text");
      EXPECT_EQ(filters[0].spec, L"*.txt;*.json");
      EXPECT_EQ(filters[1].name, L"Images");
      EXPECT_EQ(filters[1].spec, L"*.png;*.gif;*.jpeg");
    }

    return MockShowResult(CreateShellItem(selected_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result =
      WaitForSaveDialog(plugin,
                        SelectionOptions(/* allow multiple = */ false,
                                         /* select folders = */ false,
                                         EncodableList({
                                             text_group,
                                             image_group,
                                         })),
                        nullptr, nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_file.path()));
  // The test dialog controller always reports the last group as
  // selected, so that should be what the plugin returns.
  ASSERT_NE(result.value().type_group_index(), nullptr);
  EXPECT_EQ(*(result.value().type_group_index()), 1);
}

TEST(FileSelectorPlugin, TestSaveCancel) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);

  bool shown = false;
  MockShow show_validator = [&shown, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    return MockShowResult();
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForSaveDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ false, EncodableList()),
      nullptr, nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  EXPECT_EQ(paths.size(), 0);
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestGetDirectorySimple) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  const std::wstring selected_path = L"C:\\Program Files";

  bool shown = false;
  MockShow show_validator = [&shown, selected_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate options.
    FILEOPENDIALOGOPTIONS options;
    dialog.GetOptions(&options);
    EXPECT_EQ(options & FOS_ALLOWMULTISELECT, 0U);
    EXPECT_NE(options & FOS_PICKFOLDERS, 0U);

    return MockShowResult(CreateShellItemArray(selected_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ true, EncodableList()),
      nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 1);
  EXPECT_EQ(std::get<std::string>(paths[0]), "C:\\Program Files");
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestGetDirectoryMultiple) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);
  // These are actual files, but since the plugin implementation doesn't
  // validate the types of items returned from the system dialog, they are fine
  // to use for unit tests.
  ScopedTestFileIdList fake_selected_dir_1;
  ScopedTestFileIdList fake_selected_dir_2;
  const std::wstring first_path = fake_selected_dir_1.path();
  const std::wstring second_path = fake_selected_dir_2.path();

  bool shown = false;
  MockShow show_validator = [&shown, first_path, second_path, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    EXPECT_EQ(parent, fake_window);

    // Validate options.
    FILEOPENDIALOGOPTIONS options;
    dialog.GetOptions(&options);
    EXPECT_NE(options & FOS_ALLOWMULTISELECT, 0U);
    EXPECT_NE(options & FOS_PICKFOLDERS, 0U);

    return MockShowResult(CreateShellItemArray(first_path, second_path));
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ true, /* select folders = */ true,
                       EncodableList()),
      nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(std::get<std::string>(paths[0]),
            Utf8FromUtf16(fake_selected_dir_1.path()));
  EXPECT_EQ(std::get<std::string>(paths[1]),
            Utf8FromUtf16(fake_selected_dir_2.path()));
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

TEST(FileSelectorPlugin, TestGetDirectoryCancel) {
  const HWND fake_window = reinterpret_cast<HWND>(1337);

  bool shown = false;
  MockShow show_validator = [&shown, fake_window](
                                const TestFileDialogController& dialog,
                                HWND parent) {
    shown = true;
    return MockShowResult();
  };

  FileSelectorPlugin plugin(
      [fake_window] { return fake_window; },
      std::make_unique<TestFileDialogControllerFactory>(show_validator));
  ErrorOr<FileDialogResult> result = WaitForOpenDialog(
      plugin,
      SelectionOptions(/* allow multiple = */ false,
                       /* select folders = */ true, EncodableList()),
      nullptr, nullptr);

  EXPECT_TRUE(shown);
  ASSERT_FALSE(result.has_error());
  const EncodableList& paths = result.value().paths();
  EXPECT_EQ(paths.size(), 0);
  EXPECT_EQ(result.value().type_group_index(), nullptr);
}

}  // namespace test
}  // namespace file_selector_windows
