import 'dart:io';

import 'package:flutter/material.dart';
import 'package:gal/gal.dart';
import 'package:video_player/video_player.dart';

/// 成片播放页：播放 mp4 + 保存到相册。
class PlayerPage extends StatefulWidget {
  final String videoPath;
  final String title;

  const PlayerPage({super.key, required this.videoPath, this.title = '我的 Vlog'});

  @override
  State<PlayerPage> createState() => _PlayerPageState();
}

class _PlayerPageState extends State<PlayerPage> {
  VideoPlayerController? _controller;
  bool _saving = false;
  bool _ready = false;

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    final c = VideoPlayerController.file(File(widget.videoPath));
    _controller = c;
    try {
      await c.initialize();
      await c.setLooping(true);
      await c.play();
      if (mounted) setState(() => _ready = true);
    } catch (_) {
      if (mounted) setState(() => _ready = false);
    }
  }

  @override
  void dispose() {
    _controller?.dispose();
    super.dispose();
  }

  Future<void> _saveToGallery() async {
    setState(() => _saving = true);
    try {
      final has = await Gal.hasAccess();
      if (!has) await Gal.requestAccess();
      await Gal.putVideo(widget.videoPath, album: 'AI Vlog');
      _snack('已保存到相册', ok: true);
    } catch (e) {
      _snack('保存失败：$e', ok: false);
    } finally {
      if (mounted) setState(() => _saving = false);
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
    final c = _controller;
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.black,
        foregroundColor: Colors.white,
        title: Text(widget.title),
      ),
      body: Center(
        child: (_ready && c != null)
            ? AspectRatio(
                aspectRatio: c.value.aspectRatio == 0 ? 9 / 16 : c.value.aspectRatio,
                child: GestureDetector(
                  onTap: () => setState(() {
                    c.value.isPlaying ? c.pause() : c.play();
                  }),
                  child: Stack(
                    alignment: Alignment.center,
                    children: [
                      VideoPlayer(c),
                      if (!c.value.isPlaying)
                        const Icon(Icons.play_circle_fill, size: 72, color: Colors.white70),
                    ],
                  ),
                ),
              )
            : const CircularProgressIndicator(color: Colors.white),
      ),
      bottomNavigationBar: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: FilledButton.icon(
            onPressed: _saving ? null : _saveToGallery,
            icon: _saving
                ? const SizedBox(
                    width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.download),
            label: Text(_saving ? '保存中…' : '保存到相册'),
          ),
        ),
      ),
    );
  }
}
