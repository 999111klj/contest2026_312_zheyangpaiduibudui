import 'dart:io';

import 'package:dio/dio.dart';
import 'package:path/path.dart' as p;

import 'settings_store.dart';
import 'vlog_store.dart';

/// 设备上的一个 Vlog 条目（来自 GET /list）
class RemoteVlog {
  final String name;
  final int size;
  final String title;
  final String? downloadUrl; // 后端直接给的完整下载 URL

  const RemoteVlog({
    required this.name,
    required this.size,
    this.title = '',
    this.downloadUrl,
  });

  factory RemoteVlog.fromJson(Map<String, dynamic> j) => RemoteVlog(
        name: (j['name'] as String?) ?? '',
        size: (j['size'] as num?)?.toInt() ?? 0,
        title: (j['title'] as String?) ?? '',
        downloadUrl: j['url'] as String?,
      );

  String get sizeText {
    if (size <= 0) return '';
    if (size < 1024 * 1024) return '${(size / 1024).toStringAsFixed(0)} KB';
    return '${(size / 1024 / 1024).toStringAsFixed(1)} MB';
  }

  String get displayName => title.isNotEmpty ? title : name;
}

/// 下载进度回调（0.0~1.0，总长未知时可能为 null）
typedef DownloadProgress = void Function(double? progress);

/// 与开发板（局域网 HTTP 服务器）通信。
///
/// 约定接口：
/// - GET  {base}/list                     → JSON 数组 [{"name":"x.mp4","size":123}]
/// - GET  {base}/download?name=x.mp4       → mp4 文件流（需要 Bearer Token）
class DeviceClient {
  // TODO: 替换为后端部署时生成的 API Token
  // 后端启动时会生成一个随机密钥，将其填入下方 _apiToken 常量中
  static const _apiToken = 'YOUR_BACKEND_API_TOKEN_HERE';

  final Dio _dio;

  DeviceClient({Dio? dio}) : _dio = _buildDio(dio);

  static Dio _buildDio(Dio? override) {
    if (override != null) return override;
    final dio = Dio();
    // 跳过内网自签名证书校验
    (dio.httpClientAdapter as dynamic).onHttpClientCreate =
        (HttpClient client) {
      client.badCertificateCallback = (cert, host, port) => true;
      return client;
    };
    return dio;
  }

  /// 把用户输入规整成 base url，如 "192.168.43.1" -> "https://192.168.43.1"
  static String normalizeBase(String input) {
    var s = input.trim();
    if (s.isEmpty) return '';
    if (!s.startsWith('http://') && !s.startsWith('https://')) {
      s = 'https://$s';
    }
    // 去掉结尾斜杠
    return s.replaceAll(RegExp(r'/+$'), '');
  }

  /// 拉取设备上的 Vlog 列表
  Future<List<RemoteVlog>> fetchList(String baseUrl) async {
    final base = normalizeBase(baseUrl);
    final resp = await _dio.get(
      '$base/list',
      options: Options(
        sendTimeout: const Duration(seconds: 8),
        receiveTimeout: const Duration(seconds: 8),
        responseType: ResponseType.json,
      ),
    );
    final data = resp.data;
    // 兼容两种格式：{"videos":[...]} 或直接 [...]
    final rawList = data is Map ? (data['videos'] ?? data['list'] ?? []) : data;
    final list = <RemoteVlog>[];
    if (rawList is List) {
      for (final e in rawList) {
        if (e is Map) {
          final v = RemoteVlog.fromJson(Map<String, dynamic>.from(e));
          if (v.name.isNotEmpty) list.add(v);
        } else if (e is String && e.isNotEmpty) {
          list.add(RemoteVlog(name: e, size: 0));
        }
      }
    }
    return list;
  }

  /// 下载指定 Vlog 到应用目录，并写入本地库。返回本地文件路径。
  Future<String> download(
    String baseUrl,
    RemoteVlog vlog, {
    DownloadProgress? onProgress,
  }) async {
    // 下载目录由设置决定（自定义/外部/内部，失败自动回退到内部私有目录）
    final outDir = await settingsStore.resolveDownloadDir();

    final safeName = vlog.name.replaceAll(RegExp(r'[^\w\.\-]'), '_');
    final destPath =
        p.join(outDir.path, 'dev_${DateTime.now().millisecondsSinceEpoch}_$safeName');

    // 优先用后端给的完整 URL，否则回退旧协议
    final downloadUrl = vlog.downloadUrl?.isNotEmpty == true
        ? vlog.downloadUrl!
        : '${normalizeBase(baseUrl)}/download?name=${Uri.encodeComponent(vlog.name)}';

    await _dio.download(
      downloadUrl,
      destPath,
      options: Options(
        receiveTimeout: const Duration(minutes: 5),
        headers: {'Authorization': 'Bearer $_apiToken'},
      ),
      onReceiveProgress: (received, total) {
        if (onProgress != null) {
          onProgress(total > 0 ? received / total : null);
        }
      },
    );

    final title = vlog.title.isNotEmpty
        ? vlog.title
        : p.basenameWithoutExtension(vlog.name);
    await vlogStore.add(VlogRecord(
      videoPath: destPath,
      title: title,
      day: '',
      createdAt: DateTime.now().millisecondsSinceEpoch,
    ));
    return destPath;
  }

  /// 友好的错误描述
  static String describeError(Object e) {
    if (e is DioException) {
      switch (e.type) {
        case DioExceptionType.connectionTimeout:
        case DioExceptionType.sendTimeout:
        case DioExceptionType.receiveTimeout:
          return '连接超时：确认设备已连上手机热点、IP 填对了';
        case DioExceptionType.connectionError:
          return '连不上设备：检查 IP/端口，以及是否在同一网络';
        case DioExceptionType.badResponse:
          return '设备返回错误：HTTP ${e.response?.statusCode}';
        default:
          return '请求失败：${e.message ?? e.type.name}';
      }
    }
    return '出错：$e';
  }
}

final deviceClient = DeviceClient();
