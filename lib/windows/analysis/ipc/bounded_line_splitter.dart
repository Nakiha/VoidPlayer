import 'dart:async';

const int analysisIpcMaxLineLength = 64 * 1024;

class BoundedLineSplitter extends StreamTransformerBase<String, String> {
  final int maxLineLength;

  const BoundedLineSplitter({this.maxLineLength = analysisIpcMaxLineLength});

  @override
  Stream<String> bind(Stream<String> stream) {
    late StreamSubscription<String> subscription;
    late StreamController<String> controller;
    final buffer = StringBuffer();

    void emitLine(String line) {
      if (line.endsWith('\r')) {
        controller.add(line.substring(0, line.length - 1));
      } else {
        controller.add(line);
      }
    }

    void failLineTooLong() {
      controller.addError(
        FormatException('line exceeds $maxLineLength characters'),
      );
      unawaited(subscription.cancel());
    }

    controller = StreamController<String>(
      sync: true,
      onListen: () {
        subscription = stream.listen(
          (chunk) {
            for (var i = 0; i < chunk.length; i++) {
              final codeUnit = chunk.codeUnitAt(i);
              if (codeUnit == 0x0A) {
                emitLine(buffer.toString());
                buffer.clear();
                continue;
              }
              buffer.writeCharCode(codeUnit);
              if (buffer.length > maxLineLength) {
                failLineTooLong();
                return;
              }
            }
          },
          onError: controller.addError,
          onDone: () {
            if (buffer.isNotEmpty) {
              emitLine(buffer.toString());
            }
            controller.close();
          },
          cancelOnError: true,
        );
      },
      onPause: () => subscription.pause(),
      onResume: () => subscription.resume(),
      onCancel: () => subscription.cancel(),
    );

    return controller.stream;
  }
}
