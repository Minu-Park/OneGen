// Stands in for a CTI whose vendor runtime is delay-loaded on first use.
extern "C" __declspec(dllimport) int onegen_guard_dependency();
extern "C" __declspec(dllexport) int onegen_guard_delayed() { return onegen_guard_dependency(); }
