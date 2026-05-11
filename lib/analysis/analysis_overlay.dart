enum AnalysisOverlayType { cu, qpHeatmap, cuBitCostHeatmap }

enum AnalysisOverlayLayer { cuGrid, predictionMode, predictionLines }

AnalysisOverlayType analysisOverlayTypeFromName(String value) {
  final normalized = value.trim().toLowerCase().replaceAll(
    RegExp(r'[-_\s]'),
    '',
  );
  return AnalysisOverlayType.values.firstWhere(
    (type) => type.name.toLowerCase() == normalized,
    orElse: () => switch (normalized) {
      'cu' || 'cugrid' || 'partitions' => AnalysisOverlayType.cu,
      'pred' || 'prediction' || 'predictionmode' => AnalysisOverlayType.cu,
      'predlines' ||
      'predictionlines' ||
      'mv' ||
      'mvlines' => AnalysisOverlayType.cu,
      'qp' || 'qpheatmap' => AnalysisOverlayType.qpHeatmap,
      'bits' ||
      'bitrate' ||
      'bitrateheatmap' ||
      'bitcost' ||
      'cubits' ||
      'cubitcost' => AnalysisOverlayType.cuBitCostHeatmap,
      _ => throw ArgumentError.value(value, 'value', 'Unknown overlay type'),
    },
  );
}

AnalysisOverlayLayer analysisOverlayLayerFromName(String value) {
  final normalized = value.trim().toLowerCase().replaceAll(
    RegExp(r'[-_\s]'),
    '',
  );
  return AnalysisOverlayLayer.values.firstWhere(
    (layer) => layer.name.toLowerCase() == normalized,
    orElse: () => switch (normalized) {
      'cu' || 'cugrid' || 'grid' => AnalysisOverlayLayer.cuGrid,
      'pred' ||
      'prediction' ||
      'predictionmode' => AnalysisOverlayLayer.predictionMode,
      'predlines' ||
      'predictionlines' ||
      'mv' ||
      'mvlines' => AnalysisOverlayLayer.predictionLines,
      _ => throw ArgumentError.value(value, 'value', 'Unknown overlay layer'),
    },
  );
}

class AnalysisOverlayConfig {
  final AnalysisOverlayType type;
  final Set<AnalysisOverlayLayer> layers;
  final double opacity;

  const AnalysisOverlayConfig({
    this.type = AnalysisOverlayType.cu,
    this.layers = const {},
    this.opacity = 0.55,
  });

  AnalysisOverlayConfig copyWith({
    AnalysisOverlayType? type,
    Set<AnalysisOverlayLayer>? layers,
    double? opacity,
  }) {
    return AnalysisOverlayConfig(
      type: type ?? this.type,
      layers: layers ?? this.layers,
      opacity: (opacity ?? this.opacity).clamp(0.0, 1.0).toDouble(),
    );
  }

  AnalysisOverlayConfig withTypeDefaults(AnalysisOverlayType nextType) {
    return copyWith(type: nextType, layers: defaultLayersFor(nextType));
  }

  bool get showCuGrid => type == AnalysisOverlayType.cu;
  bool get showPredMode => layers.contains(AnalysisOverlayLayer.predictionMode);
  bool get showPredLines =>
      layers.contains(AnalysisOverlayLayer.predictionLines);
  bool get showQpHeatmap => type == AnalysisOverlayType.qpHeatmap;
  bool get showCuBitCostHeatmap => type == AnalysisOverlayType.cuBitCostHeatmap;

  static Set<AnalysisOverlayLayer> defaultLayersFor(AnalysisOverlayType type) =>
      switch (type) {
        AnalysisOverlayType.cu => {},
        AnalysisOverlayType.qpHeatmap => {},
        AnalysisOverlayType.cuBitCostHeatmap => {},
      };

  static const disabled = AnalysisOverlayConfig(
    type: AnalysisOverlayType.cu,
    layers: {},
    opacity: 0.55,
  );
}
