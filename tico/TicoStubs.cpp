/// @file TicoStubs.cpp
/// @brief Stubs for POSIX functions not available on Switch

extern "C" {

// POSIX stubs needed by imgui (Platform_OpenInShellFn_DefaultImpl)
int fork(void) { return -1; }
int execvp(const char *file, char *const argv[]) { return -1; }
int waitpid(int pid, int *status, int options) { return -1; }

} // extern "C"
