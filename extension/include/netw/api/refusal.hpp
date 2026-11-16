#pragma once

#include "godot/ref_counted.hpp"

namespace netw {

template <typename T>
godot::Ref<T> refused() {
    godot::Ref<T> config;
    config.instantiate();
    config->refuse();
    return config;
}

} // namespace netw
