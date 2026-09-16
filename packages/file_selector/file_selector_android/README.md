# file\_selector\_android

The Android implementation of [`file_selector`][1].

## Usage

This package is [endorsed][2], which means you can simply use `file_selector`
normally. This package will be automatically included in your app when you do,
so you do not need to add it to your `pubspec.yaml`.

However, if you `import` this package to use any of its APIs directly, you
should add it to your `pubspec.yaml` as usual.

Selected files are independent copies under the application's `cache/file_selector`
directory. They remain readable after the picker closes. The consumer owns these
temporary copies and should delete each returned file and its empty parent directory
when it has finished using the file (including failed or cancelled operations).
Do not clear other selections while they are still being read. CloudPlayPlus releases
its selections when the corresponding transfer reaches a terminal state.

[1]: https://pub.dev/packages/file_selector
[2]: https://flutter.dev/to/endorsed-federated-plugin
