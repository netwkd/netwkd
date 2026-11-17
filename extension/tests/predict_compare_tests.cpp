#include "support/netw_cells.h"
#include "support/netw_test.h"
#include "support/predict_rows.h"

#include "godot/variant.hpp"
#include "netw/predict/engine.hpp"
#include "netw/predict/transition_book.hpp"
#include "netw/predict/wiring.hpp"

namespace TestNetwPredictCompare {

using namespace godot;
using netw::NetwPredictionEngine;
using netw::predict::FieldDecl;
using netw::predict::StateRow;
using netw::predict::StateVerdict;
using netw::predict::TransitionBook;
using netw::predict::Wiring;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

constexpr double TAU = 6.2831853071795864769252867666;
constexpr double LOOSE_FALLBACK = 1000.0;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

struct Column {
    Variant::Type type = Variant::NIL;
    double tolerance = 0.0;
    bool angle = false;
    Variant authority;
    Variant inside;
    Variant outside;
};

bool corrects(const Column &p_column, const Variant &p_predicted) {
    NetwPredictionEngine pool;
    const StringName key("value");
    LocalVector<FieldDecl> declared;
    FieldDecl field;
    field.key = key;
    field.type = int(p_column.type);
    field.correction_threshold = p_column.tolerance;
    field.angle = p_column.angle;
    declared.push_back(field);
    const int64_t slot = pool.open(declared);
    REQUIRE(pool.configure(
        slot,
        int(netw::Schedule::TICK),
        int(netw::Role::PREDICT),
        int(netw::CorrectionMode::SNAP),
        int(netw::RestoreMode::EXACT)
    ));
    pool.set_stream_reconstructed(slot, true);
    pool.record_input(slot, 1, 1);
    const netw::predict::DriveRecord drive = pool.open_drive(
        slot,
        Dictionary(),
        1,
        1,
        1.0 / 60.0,
        1,
        true,
        0,
        0,
        0,
        0
    );
    REQUIRE(drive.ran);
    pool.close_drive(slot, drive.transition, 0, 0, 0, 0);

    Dictionary predicted;
    predicted[key] = p_predicted;
    Dictionary authority;
    authority[key] = p_column.authority;
    const StateRow predicted_row = netw_test::state_row(&pool, slot, predicted);
    const StateRow authority_row = netw_test::state_row(&pool, slot, authority);
    const StateVerdict *verdict = pool.compare_rows(
        slot,
        drive.transition,
        predicted_row,
        authority_row,
        int(netw::predict::Domain::IN_DOMAIN),
        LOOSE_FALLBACK
    );
    REQUIRE(verdict != nullptr);
    REQUIRE(verdict->compared);
    return verdict->corrected;
}

LawVerdict judged(const Column &p_column) {
    if (corrects(p_column, p_column.authority)) {
        return law_broken("the server's own value was corrected");
    }
    if (corrects(p_column, p_column.inside)) {
        return law_broken("a value inside the tolerance was corrected");
    }
    if (!corrects(p_column, p_column.outside)) {
        return law_broken("a value outside the tolerance was kept");
    }
    return law_held();
}

LawVerdict float_column(const Stand &) {
    Column column;
    column.type = Variant::FLOAT;
    column.tolerance = 0.5;
    column.authority = 1.0;
    column.inside = 1.3;
    column.outside = 2.0;
    return judged(column);
}

LawVerdict flat_vector(const Stand &) {
    Column column;
    column.type = Variant::VECTOR2;
    column.tolerance = 0.5;
    column.authority = Vector2(1.0, 1.0);
    column.inside = Vector2(1.3, 1.1);
    column.outside = Vector2(3.0, 1.0);
    return judged(column);
}

LawVerdict solid_vector(const Stand &) {
    Column column;
    column.type = Variant::VECTOR3;
    column.tolerance = 0.5;
    column.authority = Vector3(1.0, 2.0, 3.0);
    column.inside = Vector3(1.2, 2.2, 3.0);
    column.outside = Vector3(1.0, 2.0, 4.0);
    return judged(column);
}

LawVerdict quaternion_column(const Stand &) {
    Column column;
    column.type = Variant::QUATERNION;
    column.tolerance = 0.1;
    column.authority = Quaternion();
    column.inside = Quaternion(Vector3(0.0, 1.0, 0.0), 0.05);
    column.outside = Quaternion(Vector3(0.0, 1.0, 0.0), 0.5);
    return judged(column);
}

LawVerdict angle_column(const Stand &) {
    Column column;
    column.type = Variant::FLOAT;
    column.tolerance = 0.5;
    column.angle = true;
    column.authority = 0.1;
    column.inside = TAU - 0.1;
    column.outside = 1.0;
    return judged(column);
}

LawVerdict bool_column(const Stand &) {
    Column column;
    column.type = Variant::BOOL;
    column.tolerance = 0.5;
    column.authority = true;
    column.inside = true;
    column.outside = false;
    return judged(column);
}

LawVerdict int_column(const Stand &) {
    Column column;
    column.type = Variant::INT;
    column.tolerance = 2.0;
    column.authority = int64_t(10);
    column.inside = int64_t(11);
    column.outside = int64_t(20);
    return judged(column);
}

LawVerdict carried_column(const Stand &) {
    Column column;
    column.type = Variant::STRING;
    column.tolerance = 0.5;
    column.authority = String("red");
    column.inside = String("red");
    column.outside = String("blue");
    return judged(column);
}

LawVerdict array_mutated(const Stand &) {
    LocalVector<FieldDecl> declared;
    FieldDecl held;
    held.key = StringName("held");
    held.type = int(Variant::ARRAY);
    declared.push_back(held);
    const Wiring wiring = netw::predict::compile(declared);
    TransitionBook book;
    book.reset(wiring.codec);
    Array live;
    live.push_back(1);
    StateRow written;
    written.resize(1);
    written.set(0, live);
    REQUIRE(book.record_row(wiring.codec, 1, 1, true, written));

    live.push_back(2);
    StateRow read;
    REQUIRE(book.read_row(wiring.codec, 1, read));
    if (Array(read.values[0]).size() != 1) {
        return law_broken("the recorded tick followed the game's Array");
    }
    Array(read.values[0]).push_back(3);
    StateRow again;
    REQUIRE(book.read_row(wiring.codec, 1, again));
    if (Array(again.values[0]).size() != 1) {
        return law_broken("a reader's change reached the recorded tick");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a float", "it is corrected only past its tolerance", &float_column},
    {"a Vector2",
     "it is corrected only when its distance passes the tolerance",
     &flat_vector},
    {"a Vector3",
     "it is corrected only when its distance passes the tolerance",
     &solid_vector},
    {"a Quaternion",
     "it is corrected only when its angle passes the tolerance",
     &quaternion_column},
    {"an angle either side of zero",
     "it is compared the short way round the circle",
     &angle_column},
    {"a bool", "it is corrected only when it differs", &bool_column},
    {"an int", "it is corrected only past its tolerance", &int_column},
    {"a String", "it is corrected only when it differs", &carried_column},
    {"an Array the game changes after its tick was recorded",
     "the recorded tick keeps the value it had",
     &array_mutated},
};

TEST_CASE(
    "[Netw][Predict] a predicted value is corrected only past its "
    "tolerance"
) {
    const Stand stand = {String("a client predicting one property")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwPredictCompare
