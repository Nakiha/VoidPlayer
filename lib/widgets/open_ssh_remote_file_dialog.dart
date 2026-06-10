import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';
import '../remote/ssh_remote_media.dart';
import '../utils/async_guard.dart';

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
  final TextEditingController _hostController = TextEditingController();
  final TextEditingController _directoryController = TextEditingController(
    text: '~/',
  );
  final TextEditingController _patternController = TextEditingController(
    text: '*.mp4',
  );
  final FocusNode _hostFocusNode = FocusNode();
  List<SshRemoteSearchResult> _results = const [];
  SshRemoteSearchResult? _selectedResult;
  String? _messageText;
  bool _messageIsError = false;
  bool _searching = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _hostFocusNode.requestFocus();
    });
  }

  @override
  void dispose() {
    _hostController.dispose();
    _directoryController.dispose();
    _patternController.dispose();
    _hostFocusNode.dispose();
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
                    focusNode: _hostFocusNode,
                    decoration: InputDecoration(
                      labelText: l.sshHostLabel,
                      hintText: l.sshHostHint,
                      isDense: true,
                    ),
                    onChanged: (_) => _clearSearchMessage(),
                    onSubmitted: (_) =>
                        fireAndLog('search SSH remote media', _search()),
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
                    onChanged: (_) => _clearSearchMessage(),
                    onSubmitted: (_) =>
                        fireAndLog('search SSH remote media', _search()),
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
                    onChanged: (_) => _clearSearchMessage(),
                    onSubmitted: (_) =>
                        fireAndLog('search SSH remote media', _search()),
                  ),
                ),
                const SizedBox(width: 8),
                SizedBox(
                  height: 40,
                  child: FilledButton.icon(
                    onPressed: _searching
                        ? null
                        : () =>
                              fireAndLog('search SSH remote media', _search()),
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
            if (_messageText != null) ...[
              const SizedBox(height: 8),
              Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  _messageText!,
                  style: theme.textTheme.bodySmall?.copyWith(
                    color: _messageIsError
                        ? theme.colorScheme.error
                        : theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              ),
            ],
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
                          final selected = result == _selectedResult;
                          return ListTile(
                            key: ValueKey('ssh-search-result-$index'),
                            selected: selected,
                            selectedTileColor: theme.colorScheme.primary
                                .withValues(alpha: 0.10),
                            dense: true,
                            leading: const Icon(Icons.video_file, size: 18),
                            title: Text(
                              result.fileName,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                            ),
                            subtitle: Text(
                              result.path,
                              maxLines: 2,
                              overflow: TextOverflow.ellipsis,
                              style: theme.textTheme.bodySmall?.copyWith(
                                color: theme.colorScheme.onSurfaceVariant,
                              ),
                            ),
                            trailing: selected
                                ? Icon(
                                    Icons.check_circle,
                                    size: 18,
                                    color: theme.colorScheme.primary,
                                  )
                                : null,
                            onTap: () {
                              setState(() {
                                _selectedResult = result;
                                _messageText = null;
                                _messageIsError = false;
                              });
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
        FilledButton(
          onPressed: _selectedResult == null ? null : _submit,
          child: Text(l.open),
        ),
      ],
    );
  }

  Future<void> _search() async {
    final l = AppLocalizations.of(context)!;
    setState(() {
      _searching = true;
      _messageText = null;
      _messageIsError = false;
      _selectedResult = null;
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
        if (results.isEmpty) {
          _messageText = l.sshSearchNoMatches;
          _messageIsError = false;
        }
      });
    } on Object catch (e) {
      if (!mounted) return;
      setState(() {
        _messageText = e.toString();
        _messageIsError = true;
      });
    } finally {
      if (mounted) setState(() => _searching = false);
    }
  }

  void _clearSearchMessage() {
    if (_messageText != null) {
      setState(() {
        _messageText = null;
        _messageIsError = false;
      });
    }
  }

  void _submit() {
    final l = AppLocalizations.of(context)!;
    final selected = _selectedResult;
    if (selected == null) {
      setState(() {
        _messageText = l.sshOpenRequiresSelection;
        _messageIsError = true;
      });
      return;
    }
    Navigator.of(context).pop(selected.sftpUrl);
  }
}
