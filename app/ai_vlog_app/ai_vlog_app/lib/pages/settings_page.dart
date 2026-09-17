import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../services/settings_store.dart';

/// 设置页：关于 App + 下载存放位置设置。
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  StorageMode _mode = StorageMode.internal;
  String? _customDir;
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final mode = await settingsStore.getMode();
    final custom = await settingsStore.getCustomDir();
    if (!mounted) return;
    setState(() {
      _mode = mode;
      _customDir = custom;
      _loading = false;
    });
  }

  Future<void> _pickMode(StorageMode? m) async {
    if (m == null) return;
    // 选"自定义"时立即弹文件夹选择器；用户没选或不可写就不切换
    if (m == StorageMode.custom) {
      final ok = await _pickCustomDir();
      if (!ok) return;
    }
    await settingsStore.setMode(m);
    if (mounted) setState(() => _mode = m);
  }

  /// 弹系统文件夹选择器，测试可写后保存。返回是否成功。
  Future<bool> _pickCustomDir() async {
    try {
      final dir = await FilePicker.platform.getDirectoryPath();
      if (dir == null) return false; // 用户取消
      final writable = await settingsStore.testWritable(dir);
      if (!writable) {
        _snack('该文件夹无法写入（Android 存储限制），请换一个，或选应用外部目录', ok: false);
        return false;
      }
      await settingsStore.setCustomDir(dir);
      if (mounted) setState(() => _customDir = dir);
      return true;
    } catch (e) {
      _snack('选择文件夹失败：$e', ok: false);
      return false;
    }
  }

  void _snack(String msg, {required bool ok}) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(msg),
      backgroundColor: ok ? Colors.green.shade600 : Colors.red.shade600,
      behavior: SnackBarBehavior.floating,
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('设置')),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.all(16),
              children: [
                _storageCard(),
                const SizedBox(height: 12),
                _aboutCard(),
                const SizedBox(height: 12),
                const Card(
                  child: ListTile(
                    leading: Icon(Icons.info_outline),
                    title: Text('版本'),
                    trailing: Text('1.0.0'),
                  ),
                ),
              ],
            ),
    );
  }

  Widget _storageCard() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('下载存放位置',
                style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            const SizedBox(height: 4),
            const Text('接收/下载的 Vlog 保存到哪里',
                style: TextStyle(fontSize: 12, color: Colors.grey)),
            const SizedBox(height: 8),
            RadioGroup<StorageMode>(
              groupValue: _mode,
              onChanged: _pickMode,
              child: Column(
                children: [
                  const RadioListTile<StorageMode>(
                    contentPadding: EdgeInsets.zero,
                    value: StorageMode.internal,
                    title: Text('应用内部目录（推荐）'),
                    subtitle: Text('最稳定，文件管理器不可见，卸载 App 后清除'),
                  ),
                  const RadioListTile<StorageMode>(
                    contentPadding: EdgeInsets.zero,
                    value: StorageMode.external,
                    title: Text('应用外部目录'),
                    subtitle: Text('文件管理器可见（Android/data/包名/files），卸载后清除'),
                  ),
                  RadioListTile<StorageMode>(
                    contentPadding: EdgeInsets.zero,
                    value: StorageMode.custom,
                    title: const Text('自定义文件夹'),
                    subtitle: Text(
                      _mode == StorageMode.custom && _customDir != null
                          ? _customDir!
                          : '手动选择一个文件夹（部分系统目录受限不可写）',
                    ),
                  ),
                ],
              ),
            ),
            if (_mode == StorageMode.custom)
              Align(
                alignment: Alignment.centerRight,
                child: TextButton.icon(
                  onPressed: _pickCustomDir,
                  icon: const Icon(Icons.folder_open, size: 18),
                  label: const Text('更换文件夹'),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _aboutCard() {
    return const Card(
      child: Padding(
        padding: EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('关于 AI Vlog',
                style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            SizedBox(height: 8),
            Text(
              '设备端已完成 Vlog 的采集与生成，本 App 负责：\n'
              '· 从设备接收生成好的 Vlog\n'
              '· 管理与播放\n'
              '· 保存到手机相册',
              style: TextStyle(fontSize: 13, height: 1.6),
            ),
          ],
        ),
      ),
    );
  }
}
