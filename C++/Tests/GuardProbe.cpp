// Loaded by test_module_guard so a same-name producer library has a bound counterpart.
#ifdef _WIN32
#define GUARD_EXPORT __declspec(dllexport)
#else
#define GUARD_EXPORT __attribute__((visibility("default")))
#endif

extern "C" GUARD_EXPORT int onegen_guard_probe() { return 42; }
