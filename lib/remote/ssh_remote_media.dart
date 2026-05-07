import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:path/path.dart' as p;

import '../app_paths.dart';

class SshRemoteMediaException implements Exception {
  final String message;
  const SshRemoteMediaException(this.message);

  @override
  String toString() => message;
}

class SshRemoteFile {
  final String original;
  final String host;
  final int? port;
  final String path;

  const SshRemoteFile({
    required this.original,
    required this.host,
    required this.path,
    this.port,
  });

  static SshRemoteFile parse(String value) {
    final trimmed = value.trim();
    if (trimmed.isEmpty) {
      throw const SshRemoteMediaException('Remote path is empty.');
    }

    final uri = Uri.tryParse(trimmed);
    if (uri != null && uri.scheme.toLowerCase() == 'ssh') {
      if (uri.host.isEmpty || uri.path.isEmpty) {
        throw const SshRemoteMediaException(
          'SSH URI must include host and absolute path.',
        );
      }
      final userInfo = uri.userInfo.isEmpty
          ? ''
          : '${Uri.decodeComponent(uri.userInfo)}@';
      return SshRemoteFile(
        original: trimmed,
        host: '$userInfo${uri.host}',
        port: uri.hasPort ? uri.port : null,
        path: Uri.decodeFull(uri.path),
      );
    }
    if (trimmed.contains('://')) {
      throw const SshRemoteMediaException(
        'Only ssh:// URIs or scp-style remote paths are supported.',
      );
    }

    final colon = trimmed.indexOf(':');
    if (colon <= 0 || colon == trimmed.length - 1) {
      throw const SshRemoteMediaException(
        'Use user@host:/absolute/path or ssh://user@host/path.',
      );
    }
    final host = trimmed.substring(0, colon).trim();
    final path = trimmed.substring(colon + 1).trim();
    if (host.isEmpty || path.isEmpty) {
      throw const SshRemoteMediaException(
        'Use user@host:/absolute/path or ssh://user@host/path.',
      );
    }
    return SshRemoteFile(original: trimmed, host: host, path: path);
  }

  String get display => '$host:$path';

  String get scpSource => '$host:$path';

  String get legacyScpSource => '$host:${_remoteShellQuote(path)}';

  String get sftpUrl {
    final endpoint = _SshEndpoint.parse(host);
    final urlPath = path.startsWith('/') ? path : '/$path';
    final portValue = port ?? endpoint.port;
    final portSegment = portValue == null ? '' : ':$portValue';
    final userSegment = endpoint.userInfo == null
        ? ''
        : '${endpoint.userInfo}@';
    final hostSegment =
        endpoint.host.contains(':') && !endpoint.host.startsWith('[')
        ? '[${endpoint.host}]'
        : endpoint.host;
    return 'sftp://$userSegment$hostSegment$portSegment$urlPath';
  }

  String get cacheKey {
    return sha256.convert(utf8.encode(display)).toString().substring(0, 16);
  }

  String get cacheFileName {
    final baseName = p.posix.basename(path).trim();
    final safeBase = _safeFileName(
      baseName.isEmpty ? 'remote_media' : baseName,
    );
    return '$cacheKey-$safeBase';
  }

  List<String> scpArgs(String destinationPath) {
    return [
      '-o',
      'BatchMode=yes',
      '-o',
      'ConnectTimeout=10',
      if (port != null) ...['-P', '$port'],
      scpSource,
      destinationPath,
    ];
  }

  List<String> legacyScpArgs(String destinationPath) {
    return [
      '-O',
      '-o',
      'BatchMode=yes',
      '-o',
      'ConnectTimeout=10',
      if (port != null) ...['-P', '$port'],
      legacyScpSource,
      destinationPath,
    ];
  }

  static String _remoteShellQuote(String value) {
    return "'${value.replaceAll("'", "'\"'\"'")}'";
  }

  static String _safeFileName(String value) {
    final sanitized = value.replaceAll(RegExp(r'[<>:"/\\|?*\x00-\x1F]'), '_');
    return sanitized.trim().isEmpty ? 'remote_media' : sanitized;
  }
}

class SshRemoteSearchResult {
  final String host;
  final String path;

  const SshRemoteSearchResult({required this.host, required this.path});

  String get remoteSpec => '$host:$path';

  String get fileName {
    final name = p.posix.basename(path).trim();
    return name.isEmpty ? path : name;
  }

  String get sftpUrl {
    return SshRemoteFile(original: remoteSpec, host: host, path: path).sftpUrl;
  }
}

class SshRemoteMediaService {
  final Duration downloadTimeout;
  final Duration searchTimeout;
  final String scpExecutable;
  final String sshExecutable;

  const SshRemoteMediaService({
    this.downloadTimeout = const Duration(minutes: 5),
    this.searchTimeout = const Duration(seconds: 20),
    this.scpExecutable = 'scp',
    this.sshExecutable = 'ssh',
  });

  Future<String> download(String input) {
    return downloadFile(SshRemoteFile.parse(input));
  }

  String playableInput(String input) {
    final trimmed = input.trim();
    if (_isSftpUrl(trimmed)) return trimmed;
    return SshRemoteFile.parse(trimmed).sftpUrl;
  }

  Future<String> downloadFile(SshRemoteFile remote) async {
    final cacheDir = Directory(AppPaths.current.remoteCacheDir);
    await cacheDir.create(recursive: true);
    final destination = p.join(cacheDir.path, remote.cacheFileName);
    final partial = '$destination.part';
    final partialFile = File(partial);
    if (await partialFile.exists()) {
      await partialFile.delete();
    }

    var result = await _runProcess(
      scpExecutable,
      remote.scpArgs(partial),
      timeout: downloadTimeout,
      timeoutMessage: 'SSH download timed out.',
    );
    if (result.exitCode != 0) {
      result = await _runProcess(
        scpExecutable,
        remote.legacyScpArgs(partial),
        timeout: downloadTimeout,
        timeoutMessage: 'SSH download timed out.',
      );
    }
    if (result.exitCode != 0) {
      await _deleteIfExists(partialFile);
      throw SshRemoteMediaException(
        _processFailure('SSH download failed', result),
      );
    }

    final downloaded = File(partial);
    if (!await downloaded.exists() || await downloaded.length() == 0) {
      await _deleteIfExists(downloaded);
      throw const SshRemoteMediaException('SSH download produced no file.');
    }
    final destinationFile = File(destination);
    if (await destinationFile.exists()) {
      await destinationFile.delete();
    }
    await downloaded.rename(destination);
    return destination;
  }

  Future<List<SshRemoteSearchResult>> search({
    required String host,
    required String directory,
    required String pattern,
    int limit = 100,
  }) async {
    final cleanHost = host.trim();
    final cleanDirectory = directory.trim();
    final cleanPattern = pattern.trim().isEmpty ? '*' : pattern.trim();
    if (cleanHost.isEmpty || cleanDirectory.isEmpty) {
      throw const SshRemoteMediaException('Host and directory are required.');
    }

    final command = buildFindCommand(
      directory: cleanDirectory,
      pattern: cleanPattern,
      limit: limit,
    );
    final result = await _runProcess(
      sshExecutable,
      ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', cleanHost, command],
      timeout: searchTimeout,
      timeoutMessage: 'SSH search timed out.',
    );
    if (result.exitCode != 0) {
      throw SshRemoteMediaException(
        _processFailure('SSH search failed', result),
      );
    }
    return const LineSplitter()
        .convert(result.stdout)
        .map((line) => line.trim())
        .where((line) => line.isNotEmpty)
        .map((path) => SshRemoteSearchResult(host: cleanHost, path: path))
        .toList();
  }

  String buildFindCommand({
    required String directory,
    required String pattern,
    required int limit,
  }) {
    final cleanDirectory = directory.trim();
    final cleanPattern = pattern.trim().isEmpty ? '*' : pattern.trim();
    return 'find ${_remotePathExpression(cleanDirectory)} -type f '
        '-iname ${_remoteShellQuote(cleanPattern)} | head -n $limit';
  }

  Future<_ProcessResult> _runProcess(
    String executable,
    List<String> arguments, {
    required Duration timeout,
    required String timeoutMessage,
  }) async {
    late final Process process;
    try {
      process = await Process.start(executable, arguments, runInShell: false);
    } on Object catch (e) {
      throw SshRemoteMediaException('Could not start $executable: $e');
    }
    final stdoutFuture = process.stdout.transform(utf8.decoder).join();
    final stderrFuture = process.stderr.transform(utf8.decoder).join();
    late final int exitCode;
    try {
      exitCode = await process.exitCode.timeout(timeout);
    } on TimeoutException {
      process.kill(ProcessSignal.sigkill);
      throw SshRemoteMediaException(timeoutMessage);
    }
    return _ProcessResult(
      exitCode: exitCode,
      stdout: await stdoutFuture,
      stderr: await stderrFuture,
    );
  }

  String _processFailure(String prefix, _ProcessResult result) {
    final detail = result.stderr.trim().isNotEmpty
        ? result.stderr.trim()
        : result.stdout.trim();
    return detail.isEmpty ? '$prefix.' : '$prefix: $detail';
  }

  Future<void> _deleteIfExists(File file) async {
    if (await file.exists()) await file.delete();
  }

  static String _remoteShellQuote(String value) {
    return "'${value.replaceAll("'", "'\"'\"'")}'";
  }

  static String _remotePathExpression(String value) {
    final trimmed = value.trim();
    if (trimmed == '~') return r'$HOME';
    if (trimmed.startsWith('~/')) {
      return r'$HOME' + _remoteShellQuote(trimmed.substring(1));
    }
    return _remoteShellQuote(trimmed);
  }

  static bool _isSftpUrl(String value) {
    final uri = Uri.tryParse(value);
    return uri != null &&
        uri.scheme.toLowerCase() == 'sftp' &&
        uri.host.isNotEmpty;
  }
}

class _SshEndpoint {
  final String? userInfo;
  final String host;
  final int? port;

  const _SshEndpoint({required this.host, this.userInfo, this.port});

  static _SshEndpoint parse(String value) {
    final trimmed = value.trim();
    final at = trimmed.lastIndexOf('@');
    final userInfo = at > 0 ? trimmed.substring(0, at) : null;
    var hostPort = at > 0 ? trimmed.substring(at + 1) : trimmed;
    int? port;

    if (hostPort.startsWith('[')) {
      final close = hostPort.indexOf(']');
      if (close > 0) {
        final parsedPort = _parsePortSuffix(hostPort.substring(close + 1));
        port = parsedPort;
        hostPort = hostPort.substring(1, close);
      }
    } else if (':'.allMatches(hostPort).length == 1) {
      final colon = hostPort.lastIndexOf(':');
      final parsedPort = int.tryParse(hostPort.substring(colon + 1));
      if (parsedPort != null) {
        port = parsedPort;
        hostPort = hostPort.substring(0, colon);
      }
    }

    return _SshEndpoint(userInfo: userInfo, host: hostPort, port: port);
  }

  static int? _parsePortSuffix(String value) {
    if (!value.startsWith(':')) return null;
    return int.tryParse(value.substring(1));
  }
}

class _ProcessResult {
  final int exitCode;
  final String stdout;
  final String stderr;

  const _ProcessResult({
    required this.exitCode,
    required this.stdout,
    required this.stderr,
  });
}
