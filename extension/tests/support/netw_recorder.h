#pragma once

#include "internals.h"
#include "netw_test.h"

#include <memory>

#include "godot/callable.hpp"
#include "godot/object.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/predict/decl.hpp"

namespace netw_test {

struct RecorderLog {
    struct Emission {
        godot::StringName signal;
        godot::Array args;
    };

    godot::Vector<Emission> emissions;
};

class RecorderSink final : public godot::CallableCustom {
    std::shared_ptr<RecorderLog> log;
    godot::StringName signal;
    godot::ObjectID source;

    static bool same(
        const godot::CallableCustom *a,
        const godot::CallableCustom *b
    ) {
        return a == b;
    }

    static bool before(
        const godot::CallableCustom *a,
        const godot::CallableCustom *b
    ) {
        return a < b;
    }

public:
    RecorderSink(
        const std::shared_ptr<RecorderLog> &p_log,
        const godot::StringName &p_signal,
        const godot::Object *p_source
    )
        : log(p_log), signal(p_signal),
          source(netw::gd::instance_id(p_source)) {
    }

    uint32_t hash() const override {
        return uint32_t(uintptr_t(this));
    }

    godot::String get_as_text() const override {
        return godot::String("NetwRecorder::") + godot::String(signal);
    }

    CompareEqualFunc get_compare_equal_func() const override {
        return &RecorderSink::same;
    }

    CompareLessFunc get_compare_less_func() const override {
        return &RecorderSink::before;
    }

    godot::ObjectID get_object() const override {
        return source;
    }

    void call(
        const godot::Variant **p_arguments,
        int p_argcount,
        godot::Variant &r_return_value,
        netw::gd::CallError &r_call_error
    ) const override {
        godot::Array args;
        for (int index = 0; index < p_argcount; ++index) {
            args.push_back(*p_arguments[index]);
        }
        log->emissions.push_back({signal, args});
        r_return_value = godot::Variant();
        netw::gd::call_ok(r_call_error);
    }
};

class Recorder {
    godot::Object *source = nullptr;

protected:
    std::shared_ptr<RecorderLog> log;

private:
    godot::Vector<godot::StringName> signals;
    godot::Vector<godot::Callable> callables;

public:
    Recorder(
        godot::Object *p_object,
        const godot::Vector<godot::StringName> &p_signals
    )
        : source(p_object), log(std::make_shared<RecorderLog>()) {
        REQUIRE(p_object != nullptr);
        for (const godot::StringName &name : p_signals) {
            REQUIRE_MESSAGE(
                p_object->has_signal(name),
                "no such signal on the recorded object"
            );
            if (!p_object->has_signal(name)) {
                continue;
            }
            const godot::Callable callable(
                memnew(RecorderSink(log, name, p_object))
            );
            p_object->connect(name, callable);
            signals.push_back(name);
            callables.push_back(callable);
        }
    }

    ~Recorder() {
        if (source == nullptr) {
            return;
        }
        for (int index = 0; index < signals.size(); ++index) {
            if (source->is_connected(signals[index], callables[index])) {
                source->disconnect(signals[index], callables[index]);
            }
        }
    }

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    int count(const godot::StringName &p_signal) const {
        int total = 0;
        for (const RecorderLog::Emission &emission : log->emissions) {
            total += emission.signal == p_signal ? 1 : 0;
        }
        return total;
    }

    godot::Array args(const godot::StringName &p_signal, int p_nth = 0) const {
        int seen = 0;
        for (const RecorderLog::Emission &emission : log->emissions) {
            if (emission.signal != p_signal) {
                continue;
            }
            if (seen == p_nth) {
                return emission.args;
            }
            ++seen;
        }
        return godot::Array();
    }

    godot::Vector<godot::StringName> order() const {
        godot::Vector<godot::StringName> names;
        for (const RecorderLog::Emission &emission : log->emissions) {
            names.push_back(emission.signal);
        }
        return names;
    }

    void clear() {
        log->emissions.clear();
    }
};

class PredictionReports : public Recorder {
    godot::ObjectID reported;

public:
    PredictionReports(
        netw::NetwPrediction *p_handle,
        const godot::Vector<godot::StringName> &p_reports
    )
        : Recorder(p_handle, godot::Vector<godot::StringName>()) {
        const godot::Ref<netw::NetwEntity> entity = p_handle->get_entity();
        REQUIRE(entity.is_valid());
        reported = entity->get_instance_id();
        const std::shared_ptr<RecorderLog> into = log;
        Internals::record(entity.ptr())->predict_declaration().test_report
            = [into, p_reports](
                  const godot::StringName &p_name,
                  const godot::Array &p_facts
              ) {
                  if (p_reports.has(p_name)) {
                      into->emissions.push_back({p_name, p_facts});
                  }
              };
    }

    ~PredictionReports() {
        const netw::NetwEntity *entity
            = godot::Object::cast_to<netw::NetwEntity>(
                netw::gd::object_of(reported)
            );
        netw::predict::Decl *decl = entity != nullptr
            ? Internals::record(entity)->find_predict_declaration()
            : nullptr;
        if (decl != nullptr) {
            decl->test_report = nullptr;
        }
    }
};

} // namespace netw_test
