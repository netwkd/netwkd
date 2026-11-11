#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include <memory>

#include "godot/callable.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/group_promise.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"

namespace TestNetwPromise {

using namespace godot;
using netw::NetwGroupPromise;
using netw::NetwPromise;
using netw_test::CallLog;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr const char *UNHANDLED = "nothing handled it";

constexpr const char *SLOW_ANSWER = R"(extends Node

func _init() -> void:
	Netw.configure_entity(self)


@rpc("authority", "call_remote", "reliable")
func slow(frames: int, value: int) -> int:
	for frame in frames:
		await get_tree().process_frame
	return value
)";

constexpr const char *PROBE_ID = "slow_answer";
constexpr int AWAIT_FRAMES = 6;
constexpr int64_t AWAITED_VALUE = 42;
constexpr int LAST_FRAME = 40;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

Ref<NetwPromise> fresh() {
    Ref<NetwPromise> promise;
    promise.instantiate();
    return promise;
}

Script *&probe_script() {
    static Script *held = nullptr;
    return held;
}

Node *build_probe(const Variant &p_name) {
    Node *made = Object::cast_to<Node>(probe_script()->call("new"));
    made->set_name(String(p_name));
    return made;
}

Array named(const char *p_name) {
    Array out;
    out.push_back(String(p_name));
    return out;
}

Array one_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

Array slow_args(int p_frames, int64_t p_value) {
    Array out;
    out.push_back(p_frames);
    out.push_back(p_value);
    return out;
}

int a_probe_the_client_holds(LoopbackRig &p_rig) {
    Node *arena = p_rig.mirror_child("Arena");
    const int route = p_rig.spawn_registered(
        StringName(PROBE_ID),
        callable_mp_static(&build_probe),
        named("Probe"),
        one_type(),
        arena
    );
    p_rig.pump(6);
    return route;
}

LawVerdict settles_once(const Stand &) {
    for (const bool keeps : {true, false}) {
        const Ref<NetwPromise> promise = fresh();
        const CallLog heard;
        promise->wait().connect(heard.callable("waited"));
        promise->connect(StringName("settled"), heard.callable("settled"));
        promise->then(heard.callable("outcome"));
        promise->catch_error(heard.callable("outcome"));
        promise->when_settled(heard.callable("subscribed"));

        if (keeps) {
            promise->resolve(7);
            promise->resolve(9);
            promise->reject(ERR_TIMEOUT, "late");
        } else {
            promise->reject(ERR_UNAUTHORIZED, "refused");
            promise->reject(ERR_TIMEOUT, "later");
            promise->resolve(9);
        }

        const Vector<StringName> order = heard.order();
        if (order.size() != 4 || order[0] != StringName("settled")
            || order[3] != StringName("waited")) {
            return law_broken("keeps %d heard %d calls", keeps, order.size());
        }
        if (keeps
            && (int(promise->get_result()) != 7 || promise->get_is_failed()
                || int(heard.args("outcome")[0]) != 7
                || heard.args("waited")[0] != Variant(7))) {
            return law_broken("a kept promise did not keep its first value");
        }
        if (!keeps
            && (promise->get_code() != ERR_UNAUTHORIZED
                || promise->get_detail() != String("refused")
                || promise->get_is_completed()
                || heard.args("waited")[0] != Variant(int(ERR_UNAUTHORIZED)))) {
            return law_broken("a rejection did not keep its first reason");
        }
        if (!heard.args("subscribed").is_empty()) {
            return law_broken("when_settled was handed arguments");
        }
    }
    return law_held();
}

LawVerdict chains_after_settle(const Stand &) {
    const CallLog log;
    const Ref<NetwPromise> kept = NetwPromise::resolved(Variant(1));
    const Ref<NetwPromise> chained = kept->then(log.callable("then"))
                                         ->catch_error(log.callable("catch"))
                                         ->when_settled(log.callable("done"));

    const Ref<NetwPromise> broken
        = NetwPromise::rejected(ERR_UNAVAILABLE, String());
    broken->then(log.callable("broken then"));
    broken->catch_error(log.callable("broken catch"));
    broken->when_settled(log.callable("broken done"));

    if (chained != kept) {
        return law_broken("a chain answered another promise");
    }
    if (log.count("then") != 1 || log.count("catch") != 0
        || log.count("done") != 1) {
        return law_broken("a kept promise ran the wrong late callbacks");
    }
    if (log.count("broken then") != 0 || log.count("broken catch") != 1
        || log.count("broken done") != 1) {
        return law_broken("a broken promise ran the wrong late callbacks");
    }
    if (kept->answer() != Variant(1)
        || broken->answer() != Variant(int(ERR_UNAVAILABLE))) {
        return law_broken("answer did not carry the result or the code");
    }
    return law_held();
}

LawVerdict late_wait_is_quiet(const Stand &) {
    const CallLog heard;
    const Ref<NetwPromise> promise = fresh();
    promise->connect(StringName("settled"), heard.callable("settled"));
    promise->then(heard.callable("then"));
    promise->resolve(Variant(7));

    promise->wait().connect(heard.callable("late"));

    if (heard.count("settled") != 1 || heard.count("then") != 1
        || heard.count("late") != 0) {
        return law_broken("a wait on a settled promise settled it again");
    }
    return law_held();
}

const CallLog *reentry_log = nullptr;
NetwPromise *reentry_promise = nullptr;

void chain_while_settling() {
    reentry_promise->then(reentry_log->callable("reentered"));
}

LawVerdict chains_while_settling(const Stand &) {
    const CallLog heard;
    const Ref<NetwPromise> promise = fresh();
    reentry_log = &heard;
    reentry_promise = promise.ptr();
    promise->connect(
        StringName("settled"),
        callable_mp_static(&chain_while_settling)
    );

    promise->resolve(Variant(7));
    promise->resolve(Variant(9));
    reentry_log = nullptr;
    reentry_promise = nullptr;

    if (heard.count("reentered") != 1
        || heard.args("reentered")[0] != Variant(7)) {
        return law_broken("a callback chained while settling missed the value");
    }
    return law_held();
}

Ref<NetwPromise> &sole_holder() {
    static Ref<NetwPromise> held;
    return held;
}

bool &alive_after_release() {
    static bool alive = false;
    return alive;
}

void release_holder(int64_t p_promise) {
    sole_holder().unref();
    alive_after_release()
        = netw::gd::instance_from_id(ObjectID(uint64_t(p_promise))) != nullptr;
}

void release_on_resolve(const Variant &, int64_t p_promise) {
    release_holder(p_promise);
}

void release_on_reject(int64_t, const String &, int64_t p_promise) {
    release_holder(p_promise);
}

LawVerdict released_while_settling(const Stand &) {
    for (const bool rejects : {false, true}) {
        Ref<NetwPromise> made = fresh();
        const int64_t id = int64_t(uint64_t(netw::gd::instance_id(made.ptr())));
        NetwPromise *settling = made.ptr();
        if (rejects) {
            made->catch_error(callable_mp_static(&release_on_reject).bind(id));
        } else {
            made->then(callable_mp_static(&release_on_resolve).bind(id));
        }
        sole_holder() = made;
        made.unref();
        alive_after_release() = false;

        if (rejects) {
            settling->reject(ERR_CANT_CREATE, String("refused"));
        } else {
            settling->resolve(Variant(7));
        }

        if (!alive_after_release()) {
            return law_broken("rejects %d freed it mid-settle", rejects);
        }
        if (netw::gd::instance_from_id(ObjectID(uint64_t(id))) != nullptr) {
            return law_broken("rejects %d kept the promise alive", rejects);
        }
    }
    return law_held();
}

int64_t warnings_while(void (*p_act)()) {
    const FaultWatch faults;
    p_act();
    return faults.warnings_containing(UNHANDLED);
}

LawVerdict unread_failure_warns(const Stand &) {
    const int64_t freed = warnings_while([] {
        fresh()->reject(ERR_TIMEOUT, "the host did not answer");
    });
    const int64_t made_failed = warnings_while([] {
        NetwPromise::rejected(ERR_CANT_CONNECT, "no route");
    });
    if (freed != 1 || made_failed != 1) {
        return law_broken(
            "unread failures warned %d and %d times",
            int(freed),
            int(made_failed)
        );
    }

    const int64_t handled = warnings_while([] {
        const Ref<NetwPromise> waited = fresh();
        waited->reject(ERR_TIMEOUT, "the host did not answer");
        waited->wait();
        (void)NetwPromise::rejected(ERR_CANT_CONNECT, "no route")->get_code();
        (void)NetwPromise::rejected(ERR_CANT_CONNECT, "")->get_detail();
        (void)NetwPromise::rejected(ERR_CANT_CONNECT, "")->get_is_failed();
        fresh()->reject(ERR_SKIP, "cancelled");
    });
    if (handled != 0) {
        return law_broken("a handled failure warned %d times", int(handled));
    }
    return law_held();
}

LawVerdict departed_peer_settles(const Stand &) {
    const Ref<Script> shape = netw_test::minted_script(SLOW_ANSWER);
    REQUIRE(shape.is_valid());
    probe_script() = shape.ptr();
    LoopbackRig rig(1);
    rig.mount();
    const int route = a_probe_the_client_holds(rig);
    Node *probe = rig.route_node(route);
    if (probe == nullptr || rig.route_node(route, 0) == nullptr) {
        return law_broken("the client never received the probe");
    }

    rig.hold(0);
    const Ref<NetwGroupPromise> group = rig.server()->rpc_request_call_group(
        Callable(probe, StringName("slow")),
        slow_args(1000, 1),
        30.0
    );
    if (group.is_null() || group->get_is_settled()) {
        return law_broken("the request settled before the peer left");
    }
    rig.drop_client(0);
    rig.pump(4);

    const bool settled = group->get_is_settled();
    probe_script() = nullptr;
    if (!settled) {
        return law_broken("the request still waits on the departed peer");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a promise resolved or rejected, then answered again",
     "it keeps the first outcome, and settled, its own callback, "
     "when_settled and a waiter each hear it once, in that order",
     &settles_once},
    {"callbacks chained on a settled promise",
     "each runs now on its own side only, and the chain answers the same "
     "promise",
     &chains_after_settle},
    {"a wait on a promise that already settled",
     "nobody that heard the settle is told again during the call",
     &late_wait_is_quiet},
    {"a callback chained from inside the settled signal",
     "it runs once with the stored result",
     &chains_while_settling},
    {"a promise whose last holder lets go inside its own callback",
     "it lives until the settle returns, then it is freed",
     &released_while_settling},
    {"a failed promise freed with nobody reading its failure",
     "it warns once, and a waited, read or cancelled one warns nothing",
     &unread_failure_warns},
    {"a request whose only peer leaves before it answers",
     "the request settles without waiting out its deadline",
     &departed_peer_settles},
};

TEST_CASE(
    "[Networked][Promise] a promise settles once, tells every caller on its "
    "own side, and never fails silently"
) {
    const Stand stand = {String("a promise and a loopback session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

struct Evidence {
    String label;
    bool driven = false;
    int resolved_before = 0;
    int resolved_after = 0;
    Variant resolved_answer;
    int rejected_after = 0;
    Variant rejected_answer;
    int batch_after = 0;
    int pending_after = 0;
    int early_after = 0;
    int late_after = 0;
    int later_after = 0;
    bool seated = false;
    bool completed = false;
    Variant result;
    int asked_frame = -1;
    int settled_frame = -1;
};

Evidence &evidence() {
    static Evidence seen;
    return seen;
}

class SettleScenario final : public netw_test::FrameScenario {
    int frame = 0;
    bool done = false;
    std::unique_ptr<CallLog> answers;
    Ref<NetwPromise> resolved;
    Ref<NetwPromise> rejected;
    Ref<NetwPromise> pending;
    Ref<NetwPromise> shared;
    Ref<NetwGroupPromise> batch;
    Ref<Script> shape;
    std::unique_ptr<LoopbackRig> rig;
    Ref<NetwPromise> asked;

    void open_waits() {
        answers = std::make_unique<CallLog>();
        resolved = NetwPromise::resolved(Variant(7));
        rejected = NetwPromise::rejected(ERR_TIMEOUT, String("gone"));
        PackedInt32Array peers;
        peers.push_back(1);
        batch = NetwGroupPromise::create(peers);
        batch->resolve_peer(1, Variant(7));
        pending.instantiate();
        shared.instantiate();

        resolved->wait().connect(answers->callable("resolved"));
        rejected->wait().connect(answers->callable("rejected"));
        batch->wait().connect(answers->callable("batch"));
        pending->wait().connect(answers->callable("pending"));
        shared->wait().connect(answers->callable("early"));
        shared->resolve(Variant(5));
        shared->wait().connect(answers->callable("late"));
        shared->wait().connect(answers->callable("later"));
        evidence().resolved_before = answers->count("resolved");
    }

    void read_waits() {
        Evidence &seen = evidence();
        seen.resolved_after = answers->count("resolved");
        seen.rejected_after = answers->count("rejected");
        seen.batch_after = answers->count("batch");
        seen.pending_after = answers->count("pending");
        seen.early_after = answers->count("early");
        seen.late_after = answers->count("late");
        seen.later_after = answers->count("later");
        if (seen.resolved_after > 0) {
            seen.resolved_answer = answers->args("resolved")[0];
        }
        if (seen.rejected_after > 0) {
            seen.rejected_answer = answers->args("rejected")[0];
        }
        resolved.unref();
        rejected.unref();
        pending.unref();
        shared.unref();
        batch.unref();
        answers.reset();
    }

    void open_request() {
        shape = netw_test::minted_script(SLOW_ANSWER);
        if (shape.is_null()) {
            return;
        }
        probe_script() = shape.ptr();
        rig = std::make_unique<LoopbackRig>(1);
        rig->mount();
        const int route = a_probe_the_client_holds(*rig);
        Node *probe = rig->route_node(route);
        if (probe == nullptr || rig->route_node(route, 0) == nullptr) {
            return;
        }
        evidence().seated = true;
        asked = rig->server()->rpc_request_call(
            rig->peer_id(0),
            Callable(probe, StringName("slow")),
            slow_args(AWAIT_FRAMES, AWAITED_VALUE)
        );
        evidence().asked_frame = frame;
    }

    void close() {
        Evidence &seen = evidence();
        seen.completed = asked.is_valid() && asked->get_is_completed();
        seen.result = asked.is_valid() ? asked->get_result() : Variant();
        seen.driven = true;
        asked.unref();
        rig.reset();
        probe_script() = nullptr;
        shape.unref();
    }

public:
    bool advance() override {
        if (netw::gd::scene_root() == nullptr || done) {
            return false;
        }
        if (frame == 0) {
            open_waits();
            open_request();
        } else if (frame == 1) {
            read_waits();
        }
        Evidence &seen = evidence();
        if (seen.settled_frame < 0 && asked.is_valid()
            && asked->get_is_settled()) {
            seen.settled_frame = frame;
        }
        if (frame >= LAST_FRAME || (frame > 0 && !seen.seated)) {
            close();
            done = true;
            return false;
        }
        if (rig != nullptr) {
            rig->pump(1);
        }
        ++frame;
        return true;
    }
};

NETW_FRAME_SCENARIO(SettleScenario, promise_settle_scenario);

typedef netw_test::LawRowFor<Evidence> FrameRow;

LawVerdict late_waiter_answered(const Evidence &p_seen) {
    if (p_seen.resolved_before != 0) {
        return law_broken("a late waiter was answered inside wait()");
    }
    if (p_seen.resolved_after != 1 || p_seen.resolved_answer != Variant(7)) {
        return law_broken(
            "a resolved promise answered %d late waiters",
            p_seen.resolved_after
        );
    }
    if (p_seen.rejected_after != 1
        || p_seen.rejected_answer != Variant(int(ERR_TIMEOUT))) {
        return law_broken(
            "a rejected promise answered %d late waiters",
            p_seen.rejected_after
        );
    }
    if (p_seen.batch_after != 1) {
        return law_broken(
            "a settled batch answered %d late waiters",
            p_seen.batch_after
        );
    }
    if (p_seen.pending_after != 0) {
        return law_broken("a pending promise answered its waiter");
    }
    if (p_seen.early_after != 1 || p_seen.late_after != 1
        || p_seen.later_after != 1) {
        return law_broken(
            "waiters heard %d, %d and %d answers",
            p_seen.early_after,
            p_seen.late_after,
            p_seen.later_after
        );
    }
    return law_held();
}

LawVerdict awaiting_request_replies(const Evidence &p_seen) {
    if (!p_seen.seated) {
        return law_broken("the client never received the probe");
    }
    if (!p_seen.completed || p_seen.result.get_type() != Variant::INT
        || int64_t(p_seen.result) != AWAITED_VALUE) {
        return law_broken(
            "the request completed %d with type %d",
            p_seen.completed,
            int(p_seen.result.get_type())
        );
    }
    if (p_seen.settled_frame < p_seen.asked_frame + AWAIT_FRAMES) {
        return law_broken(
            "the request settled at frame %d, asked at %d",
            p_seen.settled_frame,
            p_seen.asked_frame
        );
    }
    return law_held();
}

const FrameRow FRAME_ROWS[] = {
    {"a waiter on a promise or batch that already settled",
     "it is answered once on the next frame with the result or the code, "
     "and a waiter on a pending promise is not",
     &late_waiter_answered},
    {"a request to a method that awaits frames before it returns",
     "the request completes with the returned value once the method "
     "returns",
     &awaiting_request_replies},
};

TEST_CASE(
    "[Networked][Promise][Frame] a promise that settled before anyone "
    "waited, and a request answered frames later, both reach the waiter"
) {
    Evidence &seen = evidence();
    REQUIRE(seen.driven);
    seen.label = String("one frame of waits and a loopback request");
    for (const FrameRow &row : FRAME_ROWS) {
        NETW_CELL(row, seen);
        NETW_LAW_HOLDS(row, seen);
    }
}

} // namespace TestNetwPromise

#endif
