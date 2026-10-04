#pragma once

#include <memory>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace kairo {
namespace gpu {

class LoadedLibraryLease {
public:
    LoadedLibraryLease() = default;

    bool valid() const {
        return static_cast<bool>(library_);
    }

private:
    struct Library {
#ifdef _WIN32
        explicit Library(HMODULE native_handle) : handle(native_handle) {}
        HMODULE handle;
#else
        explicit Library(void* native_handle) : handle(native_handle) {}
        void* handle;
#endif

        ~Library() {
#ifdef _WIN32
            if (handle != nullptr) {
                FreeLibrary(handle);
            }
#else
            if (handle != nullptr) {
                dlclose(handle);
            }
#endif
        }
    };

    explicit LoadedLibraryLease(std::shared_ptr<Library> library)
        : library_(std::move(library)) {}

    std::shared_ptr<Library> library_;

    friend class CudaLoader;
    friend class OpenCLLoader;
};

} // namespace gpu
} // namespace kairo
