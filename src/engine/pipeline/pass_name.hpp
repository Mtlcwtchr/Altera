#pragma once
// A pass's name for a loading screen and a log - "SceneModels" for
// game::SceneModelsPass - from the compiler's type name, whichever ABI wrote it.
#include <cstdlib>
#include <cstring>
#include <string>
#include <typeinfo>
#if !defined(_MSC_VER)
#include <cxxabi.h>
#endif

namespace engine {

template <class T>
std::string passName(const T& pass) {
    const char* raw = typeid(pass).name();
    std::string name = raw;
#if !defined(_MSC_VER)
    int status = 0;
    if (char* readable = abi::__cxa_demangle(raw, nullptr, nullptr, &status)) {
        if (status == 0) name = readable;
        std::free(readable);
    }
#endif
    for (const char* prefix : {"class ", "struct "})
        if (name.rfind(prefix, 0) == 0) name.erase(0, std::strlen(prefix));
    if (const auto colon = name.rfind("::"); colon != std::string::npos) name.erase(0, colon + 2);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, "Pass") == 0) name.resize(name.size() - 4);
    return name;
}

} // namespace engine
