import 'dart:async';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/device_client.dart';
import '../services/vlog_store.dart';

/// 设备页：输入视频源（电脑后端）的局域网地址，接收其生成的 Vlog。
///
/// 视频由电脑后端异步生成。App 按其 IP 走 HTTP：GET /list 拉列表，GET /download?name= 下载。
/// 接收方式：
/// - 自动接收（默认开）：连上后定时轮询 /list，发现本地没有的新视频自动下载入库。
/// - 手动下载：也可点列表里的下载按钮单独拉。
class DevicePage extends StatefulWidget {
  const DevicePage({super.key});

  @override
  State<DevicePage> createState() => _DevicePageState();
}

class _DevicePageState extends State<DevicePage> {
  static const _kLastIp = 'last_device_ip';
  static const _kAutoReceive = 'auto_receive';

  final _ipCtrl = TextEditingController();
  bool _loading = false;
  List<RemoteVlog> _remote = [];
  String? _error;
  bool _connected = false;

  // 自动接收
  bool _autoReceive = true;
  Timer? _pollTimer;

  // 正在下载的项 -> 进度
  final Map<String, double?> _downloading = {};
  // 已下载过的原始文件名，用于判断"新视频"
  Set<String> _localNames = {};

  @override
  void initState() {
    super.initState();
    _restore();
    _loadLocalNames();
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _ipCtrl.dispose();
    super.dispose();
  }

  Future<void> _restore() async {
    final prefs = await SharedPreferences.getInstance();
    final last = prefs.getString(_kLastIp);
    final auto = prefs.getBool(_kAutoReceive) ?? true;
    if (!mounted) return;
    setState(() {
      if (last != null && last.isNotEmpty) _ipCtrl.text = last;
      _autoReceive = auto;
    });
  }

  Future<void> _loadLocalNames() async {
    final all = await vlogStore.loadAll();
    final names = <String>{};
    for (final r in all) {
      final base = r.videoPath.split(RegExp(r'[\\/]')).last;
      final m = RegExp(r'^dev_\d+_(.+)$').firstMatch(base);
      if (m != null) names.add(m.group(1)!);
      names.add(r.title);
    }
    if (mounted) setState(() => _localNames = names);
  }

  bool _isNew(RemoteVlog v) {
    final stem = v.name.replaceAll(RegExp(r'\.[^.]+$'), '');
    return !_localNames.contains(v.name) && !_localNames.contains(stem);
  }

  Future<void> _connect() async {
    final ip = _ipCtrl.text.trim();
    if (ip.isEmpty) {
      _snack('请输入视频源地址', ok: false);
      return;
    }
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final list = await deviceClient.fetchList(ip);
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString(_kLastIp, ip);
      if (!mounted) return;
      setState(() {
        _remote = list;
        _connected = true;
      });
      _startPollingIfNeeded();
      if (_autoReceive) _autoFetchNew();
      if (list.isEmpty) _snack('已连接，但暂无 Vlog', ok: true);
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = DeviceClient.describeError(e);
        _connected = false;
      });
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  void _startPollingIfNeeded() {
    _pollTimer?.cancel();
    if (!_autoReceive || !_connected) return;
    _pollTimer = Timer.periodic(const Duration(seconds: 10), (_) => _refreshAndAutoFetch());
  }

  Future<void> _refreshAndAutoFetch() async {
    if (!_connected) return;
    try {
      final list = await deviceClient.fetchList(_ipCtrl.text.trim());
      if (!mounted) return;
      setState(() => _remote = list);
      if (_autoReceive) _autoFetchNew();
    } catch (_) {
      // 轮询失败静默
    }
  }

  Future<void> _autoFetchNew() async {
    for (final v in _remote) {
      if (_isNew(v) && !_downloading.containsKey(v.name)) {
        await _download(v, auto: true);
      }
    }
  }

  Future<void> _download(RemoteVlog v, {bool auto = false}) async {
    setState(() => _downloading[v.name] = null);
    try {
      await deviceClient.download(
        _ipCtrl.text.trim(),
        v,
        onProgress: (prog) {
          if (mounted) setState(() => _downloading[v.name] = prog);
        },
      );
      if (!mounted) return;
      setState(() => _downloading.remove(v.name));
      await _loadLocalNames();
      _snack('${auto ? "自动接收" : "已下载"}「${v.displayName}」→ 去 Vlog 库播放', ok: true);
    } catch (e) {
      if (!mounted) return;
      setState(() => _downloading.remove(v.name));
      if (!auto) _snack(DeviceClient.describeError(e), ok: false);
    }
  }

  Future<void> _toggleAuto(bool v) async {
    setState(() => _autoReceive = v);
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_kAutoReceive, v);
    if (v) {
      _startPollingIfNeeded();
      if (_connected) _autoFetchNew();
    } else {
      _pollTimer?.cancel();
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
      appBar: AppBar(title: const Text('设备')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _connectCard(),
          const SizedBox(height: 12),
          if (_error != null)
            Card(
              color: Colors.red.shade50,
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Text(_error!, style: TextStyle(color: Colors.red.shade700, fontSize: 13)),
              ),
            ),
          if (_connected) ...[
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('自动接收新视频'),
              subtitle: const Text('检测到有新 Vlog 时自动下载到本地'),
              value: _autoReceive,
              onChanged: _toggleAuto,
            ),
            Row(
              children: [
                const Text('可接收的 Vlog', style: TextStyle(fontWeight: FontWeight.bold)),
                const Spacer(),
                Text('${_remote.length} 个',
                    style: const TextStyle(color: Colors.grey, fontSize: 12)),
              ],
            ),
            const SizedBox(height: 6),
            if (_remote.isEmpty)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 16),
                child: Center(child: Text('暂无 Vlog', style: TextStyle(color: Colors.grey))),
              ),
            ..._remote.map(_remoteTile),
          ],
        ],
      ),
    );
  }

  Widget _connectCard() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('连接视频源', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
            const SizedBox(height: 8),
            const Text('手机与电脑同一网络后，输入电脑后端的地址：',
                style: TextStyle(fontSize: 12, color: Colors.grey)),
            const SizedBox(height: 12),
            TextField(
              controller: _ipCtrl,
              keyboardType: TextInputType.url,
              decoration: const InputDecoration(
                labelText: '地址',
                hintText: '如 192.168.43.100:8080',
                border: OutlineInputBorder(),
                prefixIcon: Icon(Icons.computer_outlined),
              ),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                onPressed: _loading ? null : _connect,
                icon: _loading
                    ? const SizedBox(
                        width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.link),
                label: Text(_loading ? '连接中…' : (_connected ? '刷新列表' : '连接')),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _remoteTile(RemoteVlog v) {
    final downloading = _downloading.containsKey(v.name);
    final prog = _downloading[v.name];
    final isNew = _isNew(v);
    return Card(
      child: ListTile(
        leading: Icon(Icons.movie_outlined,
            color: isNew ? Theme.of(context).colorScheme.primary : Colors.grey),
        title: Row(
          children: [
            Flexible(child: Text(v.displayName, maxLines: 1, overflow: TextOverflow.ellipsis)),
            if (!isNew)
              const Padding(
                padding: EdgeInsets.only(left: 6),
                child: Icon(Icons.check_circle, size: 14, color: Colors.green),
              ),
          ],
        ),
        subtitle: downloading
            ? Row(
                children: [
                  Expanded(child: LinearProgressIndicator(value: prog)),
                  const SizedBox(width: 8),
                  Text(prog != null ? '${(prog * 100).round()}%' : '…',
                      style: const TextStyle(fontSize: 12)),
                ],
              )
            : Text(isNew ? v.sizeText : '已在本地'),
        trailing: downloading
            ? null
            : IconButton(
                icon: Icon(isNew ? Icons.download : Icons.refresh),
                tooltip: isNew ? '下载' : '重新下载',
                onPressed: () => _download(v),
              ),
      ),
    );
  }
}
