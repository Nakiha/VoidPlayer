import 'package:flutter/material.dart';

class AxTreeRegion extends StatelessWidget {
  final String label;
  final String? value;
  final String? hint;
  final bool explicitChildNodes;
  final bool image;
  final Widget child;

  const AxTreeRegion({
    super.key,
    required this.label,
    this.value,
    this.hint,
    this.explicitChildNodes = true,
    this.image = false,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return Semantics(
      container: true,
      explicitChildNodes: explicitChildNodes,
      label: label,
      value: value,
      hint: hint,
      image: image,
      child: child,
    );
  }
}

class AxTreeVisualRegion extends StatelessWidget {
  final String label;
  final String? value;
  final String? hint;
  final bool image;
  final Widget child;

  const AxTreeVisualRegion({
    super.key,
    required this.label,
    this.value,
    this.hint,
    this.image = true,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return Semantics(
      container: true,
      label: label,
      value: value,
      hint: hint,
      image: image,
      child: ExcludeSemantics(child: child),
    );
  }
}

String axPercent(double value) => '${(value.clamp(0.0, 1.0) * 100).round()}%';
