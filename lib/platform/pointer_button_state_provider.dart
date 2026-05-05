abstract interface class PointerButtonStateProvider {
  bool get isPrimaryButtonDown;
  bool get isSecondaryButtonDown;
}

class EmptyPointerButtonStateProvider implements PointerButtonStateProvider {
  const EmptyPointerButtonStateProvider();

  @override
  bool get isPrimaryButtonDown => false;

  @override
  bool get isSecondaryButtonDown => false;
}

const PointerButtonStateProvider emptyPointerButtonStateProvider =
    EmptyPointerButtonStateProvider();
