import 'dart:async';

import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';
import '../remote/ssh_remote_media.dart';

class OpenSshRemoteFileDialog extends StatefulWidget {
  final SshRemoteMediaService service;

  const OpenSshRemoteFileDialog({
    super.key,
    this.service = const SshRemoteMediaService(),
  });

  static Future<String?> show(BuildContext context) {
    return showDialog<String>(
      context: context,
      builder: (_) => const OpenSshRemoteFileDialog(),
    );
  }

  @override
  State<OpenSshRemoteFileDialog> createState() =>
      _OpenSshRemoteFileDialogState();
}

class _OpenSshRemoteFileDialogState extends State<OpenSshRemoteFileDialog> {
  final TextEditingController _remoteController = TextEditingController();
  final TextEditingController _hostController = TextEditingController();
  final TextEditingController _directoryController = TextEditingController(
    text: '~/',
  );
  final TextEditingController _patternController = TextEditingController(
    text: '*.mp4',
  );
  final FocusNode _remoteFocusNode = FocusNode();
  List<SshRemoteSearchResult> _results = const [];
  String? _errorText;
  bool _searching = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _remoteFocusNode.requestFocus();
    });
  }

  @override
  void dispose() {
    _remoteController.dispose();
    _hostController.dispose();
    _directoryController.dispose();
    _patternController.dispose();
    _remoteFocusNode.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    return AlertDialog(
      title: Row(
        children: [
          Icon(Icons.dns_outlined, size: 20, color: theme.colorScheme.primary),
          const SizedBox(width: 8),
          Text(l.openSshRemoteFile),
        ],
      ),
      content: SizedBox(
        width: 520,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            TextField(
              key: const ValueKey('ssh-remote-file-field'),
              controller: _remoteController,
              focusNode: _remoteFocusNode,
              decoration: InputDecoration(
                labelText: l.sshRemotePathLabel,
                hintText: l.sshRemotePathHint,
                errorText: _errorText,
                prefixIcon: const Icon(Icons.terminal, size: 18),
              ),
              textInputAction: TextInputAction.done,
              onChanged: (_) {
                if (_errorText != null) setState(() => _errorText = null);
              },
              onSubmitted: (_) => _submit(),
            ),
            const SizedBox(height: 16),
            Align(
              alignment: Alignment.centerLeft,
              child: Text(l.sshSearchTitle, style: theme.textTheme.labelLarge),
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                Expanded(
                  flex: 5,
                  child: TextField(
                    controller: _hostController,
                    decoration: InputDecoration(
                      labelText: l.sshHostLabel,
                      hintText: l.sshHostHint,
                      isDense: true,
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  flex: 4,
                  child: TextField(
                    controller: _patternController,
                    decoration: InputDecoration(
                      labelText: l.sshPatternLabel,
                      isDense: true,
                    ),
                    onSubmitted: (_) => unawaited(_search()),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _directoryController,
                    decoration: InputDecoration(
                      labelText: l.sshDirectoryLabel,
                      hintText: l.sshDirectoryHint,
                      isDense: true,
                    ),
                    onSubmitted: (_) => unawaited(_search()),
                  ),
                ),
                const SizedBox(width: 8),
                SizedBox(
                  height: 40,
                  child: FilledButton.icon(
                    onPressed: _searching ? null : () => unawaited(_search()),
                    icon: _searching
                        ? const SizedBox(
                            width: 16,
                            height: 16,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.search, size: 18),
                    label: Text(l.search),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 10),
            ConstrainedBox(
              constraints: const BoxConstraints(maxHeight: 180),
              child: DecoratedBox(
                decoration: BoxDecoration(
                  border: Border.all(color: theme.colorScheme.outlineVariant),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: _results.isEmpty
                    ? Padding(
                        padding: const EdgeInsets.all(12),
                        child: Align(
                          alignment: Alignment.centerLeft,
                          child: Text(
                            l.sshSearchEmpty,
                            style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                            ),
                          ),
                        ),
                      )
                    : ListView.separated(
                        shrinkWrap: true,
                        itemCount: _results.length,
                        separatorBuilder: (_, _) => const Divider(height: 1),
                        itemBuilder: (context, index) {
                          final result = _results[index];
                          return ListTile(
                            dense: true,
                            leading: const Icon(Icons.video_file, size: 18),
                            title: Text(
                              result.path,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                            ),
                            onTap: () {
                              _remoteController.text = result.remoteSpec;
                              _remoteController.selection =
                                  TextSelection.collapsed(
                                    offset: result.remoteSpec.length,
                                  );
                            },
                          );
                        },
                      ),
              ),
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(l.cancel),
        ),
        FilledButton(onPressed: _submit, child: Text(l.open)),
      ],
    );
  }

  Future<void> _search() async {
    final l = AppLocalizations.of(context)!;
    setState(() {
      _searching = true;
      _errorText = null;
    });
    try {
      final results = await widget.service.search(
        host: _hostController.text,
        directory: _directoryController.text,
        pattern: _patternController.text,
      );
      if (!mounted) return;
      setState(() {
        _results = results;
        if (results.isEmpty) _errorText = l.sshSearchNoMatches;
      });
    } on Object catch (e) {
      if (!mounted) return;
      setState(() => _errorText = e.toString());
    } finally {
      if (mounted) setState(() => _searching = false);
    }
  }

  void _submit() {
    final l = AppLocalizations.of(context)!;
    final value = _remoteController.text.trim();
    try {
      SshRemoteFile.parse(value);
    } on Object catch (_) {
      setState(() => _errorText = l.sshRemoteInvalidPath);
      return;
    }
    Navigator.of(context).pop(value);
  }
}
