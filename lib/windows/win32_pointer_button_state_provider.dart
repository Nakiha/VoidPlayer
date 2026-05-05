import '../platform/pointer_button_state_provider.dart';
import 'win32ffi.dart';

class Win32PointerButtonStateProvider implements PointerButtonStateProvider {
  const Win32PointerButtonStateProvider();

  @override
  bool get isPrimaryButtonDown => Win32FFI.isLeftMouseButtonDown();

  @override
  bool get isSecondaryButtonDown => Win32FFI.isRightMouseButtonDown();
}
