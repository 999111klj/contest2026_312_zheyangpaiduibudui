import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// 下载存放位置的模式
/// - internal：应用内部私有目录（最稳定，文件管理器不可见，卸载清除）
/// - external：应用外部目录 Android/data/包名/files（文件管理器可见，卸载清除）
/// - custom：用户手动选择的文件夹（受 Android 分区存储限制，部分目录不可写）
enum StorageMode { internal, external, custom }

/// App 设置的持久化（SharedPreferences）。目前管理"下载存放位置"。
class SettingsStore {
  static const _kMode = 'storage_mode';
  static const _kCustomDir = 'storage_custom_dir';

  Future<StorageMode> getMode() async {
    final prefs = await SharedPreferences.getInstance();
    switch (prefs.getString(_kMode)) {
      case 'external':
        return StorageMode.external;
      case 'custom':
        return StorageMode.custom;
      default:
        return StorageMode.internal;
    }
  }

  Future<void> setMode(StorageMode m) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kMode, m.name);
  }

  Future<String?> getCustomDir() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getString(_kCustomDir);
  }

  Future<void> setCustomDir(String path) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kCustomDir, path);
  }

  /// 解析当前应下载到的目录（并确保存在）。任何一步失败都回退到应用内部目录，
  /// 保证下载永远有一个可写的落点。
  Future<Directory> resolveDownloadDir() async {
    final mode = await getMode();
    try {
      if (mode == StorageMode.custom) {
        final c = await getCustomDir();
        if (c != null && c.isNotEmpty) {
          final dir = Directory(c);
          if (!await dir.exists()) await dir.create(recursive: true);
          if (await testWritable(dir.path)) return dir;
        }
      } else if (mode == StorageMode.external) {
        final ext = await getExternalStorageDirectory();
        if (ext != null) {
          final dir = Directory(p.join(ext.path, 'vlogs'));
          if (!await dir.exists()) await dir.create(recursive: true);
          return dir;
        }
      }
    } catch (_) {
      // 落到下面的内部目录回退
    }
    final docs = await getApplicationDocumentsDirectory();
    final dir = Directory(p.join(docs.path, 'vlogs'));
    if (!await dir.exists()) await dir.create(recursive: true);
    return dir;
  }

  /// 测试目录是否真的可写（建目录 + 写一个临时文件再删）。
  Future<bool> testWritable(String dirPath) async {
    try {
      final dir = Directory(dirPath);
      if (!await dir.exists()) await dir.create(recursive: true);
      final probe = File(p.join(dirPath, '.wtest_${DateTime.now().millisecondsSinceEpoch}'));
      await probe.writeAsString('ok');
      await probe.delete();
      return true;
    } catch (_) {
      return false;
    }
  }
}

final settingsStore = SettingsStore();
