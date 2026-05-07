import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../l10n/app_localizations.dart';
import '../utils/media_source.dart';

class OpenNetworkStreamDialog extends StatefulWidget {
  const OpenNetworkStreamDialog({super.key});

  static Future<String?> show(BuildContext context) {
    return showDialog<String>(
      context: context,
      builder: (_) => const OpenNetworkStreamDialog(),
    );
  }

  @override
  State<OpenNetworkStreamDialog> createState() =>
      _OpenNetworkStreamDialogState();
}

class _OpenNetworkStreamDialogState extends State<OpenNetworkStreamDialog> {
  final TextEditingController _controller = TextEditingController();
  final FocusNode _focusNode = FocusNode();
  String? _errorText;

  @override
  void initState() {
    super.initState();
    _prefillFromClipboard();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _focusNode.requestFocus();
    });
  }

  @override
  void dispose() {
    _controller.dispose();
    _focusNode.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    return AlertDialog(
      title: Row(
        children: [
          Icon(Icons.link, size: 20, color: theme.colorScheme.primary),
          const SizedBox(width: 8),
          Text(l.openNetworkStream),
        ],
      ),
      content: SizedBox(
        width: 440,
        child: TextField(
          key: const ValueKey('network-stream-url-field'),
          controller: _controller,
          focusNode: _focusNode,
          keyboardType: TextInputType.url,
          textInputAction: TextInputAction.done,
          decoration: InputDecoration(
            labelText: l.networkStreamUrlLabel,
            hintText: l.networkStreamUrlHint,
            errorText: _errorText,
            prefixIcon: const Icon(Icons.public, size: 18),
          ),
          onChanged: (_) {
            if (_errorText != null) setState(() => _errorText = null);
          },
          onSubmitted: (_) => _submit(),
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

  Future<void> _prefillFromClipboard() async {
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    final text = data?.text?.trim();
    if (!mounted || text == null || text.isEmpty) return;
    if (!isHttpMediaUrl(text)) return;
    _controller.text = text;
    _controller.selection = TextSelection.collapsed(offset: text.length);
  }

  void _submit() {
    final l = AppLocalizations.of(context)!;
    final value = _controller.text.trim();
    if (!isHttpMediaUrl(value)) {
      setState(() => _errorText = l.networkStreamInvalidUrl);
      return;
    }
    Navigator.of(context).pop(value);
  }
}
