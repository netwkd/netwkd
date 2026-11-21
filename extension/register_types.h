#pragma once

#if defined(NETW_GDEXTENSION)
#include "godot/class_db.hpp"

void initialize_netwkd_module(godot::ModuleInitializationLevel level);
void uninitialize_netwkd_module(godot::ModuleInitializationLevel level);
#else
#include "modules/register_module_types.h"

void initialize_netwkd_module(ModuleInitializationLevel level);
void uninitialize_netwkd_module(ModuleInitializationLevel level);
#endif
