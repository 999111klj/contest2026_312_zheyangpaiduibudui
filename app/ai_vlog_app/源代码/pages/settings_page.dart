import 'package:flutter/material.dart';

/// 设置页（精简）：关于 App、说明。Vlog 生成已在设备端完成，App 只负责接收/播放/保存。
class SettingsPage extends StatelessWidget {
  const SettingsPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('设置')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const Card(
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
          ),
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
}
