// Copyright 2013 The Flutter Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io';
import 'dart:typed_data';

import 'package:file_selector_android/src/file_selector_android.dart';
import 'package:file_selector_android/src/file_selector_api.g.dart';
import 'package:file_selector_platform_interface/file_selector_platform_interface.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mockito/annotations.dart';
import 'package:mockito/mockito.dart';

import 'file_selector_android_test.mocks.dart';

@GenerateMocks(<Type>[FileSelectorApi])
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late FileSelectorAndroid plugin;
  late MockFileSelectorApi mockApi;
  late Directory cacheDirectory;
  late File textFile;
  late File imageFile;

  setUp(() async {
    mockApi = MockFileSelectorApi();
    plugin = FileSelectorAndroid(api: mockApi);
    cacheDirectory = await Directory.systemTemp.createTemp('file_selector_test_');
    textFile = await File('${cacheDirectory.path}/path.txt').writeAsBytes(List<int>.filled(30, 1));
    imageFile = await File(
      '${cacheDirectory.path}/image.jpg',
    ).writeAsBytes(List<int>.filled(40, 2));
  });

  tearDown(() => cacheDirectory.delete(recursive: true));

  test('registered instance', () {
    FileSelectorAndroid.registerWith();
    expect(FileSelectorPlatform.instance, isA<FileSelectorAndroid>());
  });

  group('openFile', () {
    test('passes the accepted type groups correctly', () async {
      when(
        mockApi.openFile(
          'some/path/',
          argThat(
            isA<FileTypes>()
                .having((FileTypes types) => types.mimeTypes, 'mimeTypes', <String>[
                  'text/plain',
                  'image/jpg',
                ])
                .having((FileTypes types) => types.extensions, 'extensions', <String>[
                  'txt',
                  'jpg',
                ]),
          ),
        ),
      ).thenAnswer(
        (_) => Future<FileResponse?>.value(
          FileResponse(path: textFile.path, size: 30, name: 'name', mimeType: 'text/plain'),
        ),
      );

      const group = XTypeGroup(extensions: <String>['txt'], mimeTypes: <String>['text/plain']);

      const group2 = XTypeGroup(extensions: <String>['jpg'], mimeTypes: <String>['image/jpg']);

      final XFile? file = await plugin.openFile(
        acceptedTypeGroups: <XTypeGroup>[group, group2],
        initialDirectory: 'some/path/',
      );

      expect(file?.path, textFile.path);
      expect(file?.mimeType, 'text/plain');
      expect(await file?.length(), 30);
      expect(await file?.readAsBytes(), List<int>.filled(30, 1));
    });
  });

  group('openFiles', () {
    test('streams a large selected file from its cached path', () async {
      final Directory directory = await Directory.systemTemp.createTemp('file_selector_large_');
      addTearDown(() => directory.delete(recursive: true));
      final cached = File('${directory.path}/large.bin');
      const int fileSize = 200 * 1024 * 1024;
      final RandomAccessFile handle = await cached.open(mode: FileMode.write);
      try {
        await handle.writeFrom(<int>[1, 2, 3, 4]);
        await handle.setPosition(fileSize - 1);
        await handle.writeByte(5);
      } finally {
        await handle.close();
      }
      when(
        mockApi.openFiles(any, any),
      ).thenAnswer((_) async => <FileResponse>[FileResponse(path: cached.path, size: fileSize)]);

      final XFile selected = (await plugin.openFiles()).single;
      expect(await selected.length(), fileSize);
      final Uint8List firstChunk = await selected.openRead().first;
      expect(firstChunk.length, inInclusiveRange(4, 64 * 1024));
      expect(firstChunk.take(4), <int>[1, 2, 3, 4]);
      expect(await selected.openRead(fileSize - 1).expand((chunk) => chunk).toList(), <int>[5]);
    });

    test('passes the accepted type groups correctly', () async {
      when(
        mockApi.openFiles(
          'some/path/',
          argThat(
            isA<FileTypes>()
                .having((FileTypes types) => types.mimeTypes, 'mimeTypes', <String>[
                  'text/plain',
                  'image/jpg',
                ])
                .having((FileTypes types) => types.extensions, 'extensions', <String>[
                  'txt',
                  'jpg',
                ]),
          ),
        ),
      ).thenAnswer(
        (_) => Future<List<FileResponse>>.value(<FileResponse>[
          FileResponse(path: textFile.path, size: 30, name: 'name', mimeType: 'text/plain'),
          FileResponse(path: imageFile.path, size: 40, mimeType: 'image/jpg'),
        ]),
      );

      const group = XTypeGroup(extensions: <String>['txt'], mimeTypes: <String>['text/plain']);

      const group2 = XTypeGroup(extensions: <String>['jpg'], mimeTypes: <String>['image/jpg']);

      final List<XFile> files = await plugin.openFiles(
        acceptedTypeGroups: <XTypeGroup>[group, group2],
        initialDirectory: 'some/path/',
      );

      expect(files[0].path, textFile.path);
      expect(files[0].mimeType, 'text/plain');
      expect(await files[0].length(), 30);
      expect(await files[0].readAsBytes(), List<int>.filled(30, 1));

      expect(files[1].path, imageFile.path);
      expect(files[1].mimeType, 'image/jpg');
      expect(await files[1].length(), 40);
      expect(await files[1].readAsBytes(), List<int>.filled(40, 2));
    });
  });

  test('getDirectoryPath', () async {
    when(
      mockApi.getDirectoryPath('some/path'),
    ).thenAnswer((_) => Future<String?>.value('some/path/chosen/'));

    final String? path = await plugin.getDirectoryPath(initialDirectory: 'some/path');

    expect(path, 'some/path/chosen/');
  });
}
