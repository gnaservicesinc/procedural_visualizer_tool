// Driver-recognized hints must be exported by the executable, not the renderer
// DLL. Windows and explicit driver/user graphics preferences remain in control.
// NVIDIA: Optimus Rendering Policies (NvOptimusEnablement).
// AMD GPUOpen: Selecting the Best Graphics Device to Run a 3D Intensive Application.
#if defined(_WIN32)
static_assert(sizeof(unsigned long) == 4, "Windows GPU hints require a DWORD");

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1UL;
__declspec(dllexport) unsigned long AmdPowerXpressRequestHighPerformance = 1UL;
}
#endif
