import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../services/vlog_store.dart';
import 'player_page.dart';

/// Vlog 库：列出已接收/导入的成片，可播放、保存到相册、删除。
/// 顶部提供"导入视频"入口（手动选本地 mp4 入库，作为接收过渡；正式接收走"设备"页）。
class VlogLibraryPage extends StatefulWidget {
  const VlogLibraryPage({super.key});

  @override
  State<VlogLibraryPage> createState() => _VlogLibraryPageState();
}

class _VlogLibraryPageState extends State<VlogLibraryPage> {
  List<VlogRecord> _records = [];
  bool _loading = true;
  bool _importing = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final list = await vlogStore.loadAll();
    if (!mounted) return;
    setState(() {
      _records = list;
      _loading = false;
    });
  }

  /// 导入本地视频：选文件 → 拷贝到应用目录 → 入库
  Future<void> _import() async {
    setState(() => _importing = true);
    try {
      final result = await FilePicker.platform.pickFiles(
        type: FileType.video,
        allowMultiple: false,
      );
      if (result == null || result.files.single.path == null) {
        setState(() => _importing = false);
        return;
      }
      final srcPath = result.files.single.path!;
      final srcName = result.files.single.name;

      // 拷贝到应用文档目录的 vlogs 下（避免原文件被清理后失效）
      final docs = await getApplicationDocumentsDirectory();
      final outDir = Directory(p.join(docs.path, 'vlogs'));
      if (!await outDir.exists()) await outDir.create(recursive: true);
      final destPath =
          p.join(outDir.path, 'import_${DateTime.now().millisecondsSinceEpoch}_$srcName');
      await File(srcPath).copy(destPath);

      final title = p.basenameWithoutExtension(srcName);
      await vlogStore.add(VlogRecord(
        videoPath: destPath,
        title: title,
        day: '',
        createdAt: DateTime.now().millisecondsSinceEpoch,
      ));
      await _load();
      _snack('已导入：$title', ok: true);
    } catch (e) {
      _snack('导入失败：$e', ok: false);
    } finally {
      if (mounted) setState(() => _importing = false);
    }
  }

  Future<void> _delete(VlogRecord r) async {
    await vlogStore.remove(r.videoPath);
    try {
      final f = File(r.videoPath);
      if (await f.exists()) await f.delete();
    } catch (_) {}
    _load();
  }

  void _snack(String msg, {required bool ok}) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(msg),
      backgroundColor: ok ? Colors.green.shade600 : Colors.red.shade600,
      behavior: SnackBarBehavior.floating,
    ));
  }

  String _fmtTime(int ms) {
    final d = DateTime.fromMillisecondsSinceEpoch(ms);
    String two(int n) => n.toString().padLeft(2, '0');
    return '${d.year}-${two(d.month)}-${two(d.day)} ${two(d.hour)}:${two(d.minute)}';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Vlog 库'),
        actions: [
          IconButton(icon: const Icon(Icons.refresh), onPressed: _load),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _importing ? null : _import,
        icon: _importing
            ? const SizedBox(
                width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
            : const Icon(Icons.file_upload_outlined),
        label: Text(_importing ? '导入中…' : '导入视频'),
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _records.isEmpty
              ? _empty()
              : ListView.separated(
                  padding: const EdgeInsets.fromLTRB(12, 12, 12, 88),
                  itemCount: _records.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 10),
                  itemBuilder: (_, i) => _tile(_records[i]),
                ),
    );
  }

  Widget _tile(VlogRecord r) {
    final exists = File(r.videoPath).existsSync();
    return Card(
      clipBehavior: Clip.antiAlias,
      child: ListTile(
        leading: Container(
          width: 48,
          height: 48,
          decoration: BoxDecoration(
            color: Theme.of(context).colorScheme.primaryContainer,
            borderRadius: BorderRadius.circular(8),
          ),
          child: const Icon(Icons.movie_outlined),
        ),
        title: Text(r.title, maxLines: 1, overflow: TextOverflow.ellipsis),
        subtitle: Text(
            '${_fmtTime(r.createdAt)}${exists ? "" : "（文件已删除）"}'),
        trailing: PopupMenuButton<String>(
          onSelected: (v) {
            if (v == 'delete') _delete(r);
          },
          itemBuilder: (_) => [
            const PopupMenuItem(value: 'delete', child: Text('删除')),
          ],
        ),
        onTap: exists
            ? () {
                Navigator.of(context).push(MaterialPageRoute(
                  builder: (_) => PlayerPage(videoPath: r.videoPath, title: r.title),
                ));
              }
            : null,
      ),
    );
  }

  Widget _empty() {
    return const Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(Icons.video_library_outlined, size: 64, color: Colors.grey),
          SizedBox(height: 12),
          Text('还没有 Vlog', style: TextStyle(color: Colors.grey)),
          SizedBox(height: 4),
          Text('连接设备接收，或点右下角导入本地视频',
              style: TextStyle(fontSize: 12, color: Colors.grey)),
        ],
      ),
    );
  }
}
