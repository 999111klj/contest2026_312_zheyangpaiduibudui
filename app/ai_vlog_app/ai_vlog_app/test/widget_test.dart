import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'package:ai_vlog/main.dart';

void main() {
  testWidgets('App 能正常启动并显示底部导航', (WidgetTester tester) async {
    await tester.pumpWidget(const ProviderScope(child: AiVlogApp()));
    await tester.pump();
    // 底部导航三个 tab
    expect(find.text('Vlog'), findsWidgets);
    expect(find.text('设备'), findsWidgets);
    expect(find.text('设置'), findsWidgets);
  });
}
