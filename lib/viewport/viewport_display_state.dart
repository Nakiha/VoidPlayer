enum ViewportDisplayStatus { loading, empty, active, error }

class ViewportDisplayState {
  final ViewportDisplayStatus status;
  final String? errorText;

  const ViewportDisplayState._(this.status, {this.errorText});

  const ViewportDisplayState.loading() : this._(ViewportDisplayStatus.loading);

  const ViewportDisplayState.empty() : this._(ViewportDisplayStatus.empty);

  const ViewportDisplayState.active() : this._(ViewportDisplayStatus.active);

  const ViewportDisplayState.error(String message)
    : this._(ViewportDisplayStatus.error, errorText: message);

  int get stackIndex => switch (status) {
    ViewportDisplayStatus.loading => 0,
    ViewportDisplayStatus.empty => 1,
    ViewportDisplayStatus.active => 2,
    ViewportDisplayStatus.error => 3,
  };

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ViewportDisplayState &&
          other.status == status &&
          other.errorText == errorText;

  @override
  int get hashCode => Object.hash(status, errorText);
}
