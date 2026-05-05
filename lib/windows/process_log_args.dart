const String redactedProcessArgValue = '<redacted>';

const Set<String> _sensitiveArgNames = {'--analysis-ipc-token'};

List<String> redactProcessArgsForLog(Iterable<String> args) {
  final redacted = <String>[];
  var redactNext = false;

  for (final arg in args) {
    if (redactNext) {
      redacted.add(redactedProcessArgValue);
      redactNext = false;
      continue;
    }

    final equalsIndex = arg.indexOf('=');
    final name = equalsIndex >= 0 ? arg.substring(0, equalsIndex) : arg;
    if (!_sensitiveArgNames.contains(name)) {
      redacted.add(arg);
      continue;
    }

    if (equalsIndex >= 0) {
      redacted.add('$name=$redactedProcessArgValue');
    } else {
      redacted.add(arg);
      redactNext = true;
    }
  }

  return redacted;
}
