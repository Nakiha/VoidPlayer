enum AnalysisOverlayType {
  cu,
  prediction,
  predictionLines,
  qpHeatmap,
  cuBitCostHeatmap,
}

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
      'pred' ||
      'prediction' ||
      'predictionmode' => AnalysisOverlayType.prediction,
      'predlines' ||
      'predictionlines' ||
      'mv' ||
      'mvlines' => AnalysisOverlayType.predictionLines,
      'qp' || 'qpheatmap' => AnalysisOverlayType.qpHeatmap,
      'bits' ||
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
    this.layers = const {AnalysisOverlayLayer.cuGrid},
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
      opacity: (opacity ?? this.opacity).clamp(0.1, 1.0).toDouble(),
    );
  }

  AnalysisOverlayConfig withTypeDefaults(AnalysisOverlayType nextType) {
    return copyWith(type: nextType, layers: defaultLayersFor(nextType));
  }

  bool get showCuGrid => layers.contains(AnalysisOverlayLayer.cuGrid);
  bool get showPredMode => layers.contains(AnalysisOverlayLayer.predictionMode);
  bool get showPredLines =>
      layers.contains(AnalysisOverlayLayer.predictionLines);
  bool get showQpHeatmap => type == AnalysisOverlayType.qpHeatmap;
  bool get showCuBitCostHeatmap => type == AnalysisOverlayType.cuBitCostHeatmap;

  static Set<AnalysisOverlayLayer> defaultLayersFor(AnalysisOverlayType type) =>
      switch (type) {
        AnalysisOverlayType.cu => {AnalysisOverlayLayer.cuGrid},
        AnalysisOverlayType.prediction => {
          AnalysisOverlayLayer.cuGrid,
          AnalysisOverlayLayer.predictionMode,
        },
        AnalysisOverlayType.predictionLines => {
          AnalysisOverlayLayer.cuGrid,
          AnalysisOverlayLayer.predictionMode,
          AnalysisOverlayLayer.predictionLines,
        },
        AnalysisOverlayType.qpHeatmap => {AnalysisOverlayLayer.cuGrid},
        AnalysisOverlayType.cuBitCostHeatmap => {AnalysisOverlayLayer.cuGrid},
      };

  static const disabled = AnalysisOverlayConfig(
    type: AnalysisOverlayType.cu,
    layers: {},
    opacity: 0.55,
  );
}
