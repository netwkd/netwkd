#pragma once

#include "godot/callable.hpp"
#include "godot/variant.hpp"
#include "netw/api/quantize.hpp"
#include "netw/schema_core.hpp"

namespace netw {

namespace table {

struct SchemaMigration {
    int from_version = 0;
    godot::Callable step;
};

} // namespace table

using table::SchemaMigration;

class SchemaBook : public SchemaCore {
    godot::HashMap<godot::RID, godot::LocalVector<godot::Ref<NetwQuantize>>>
        quantizers;
    godot::HashMap<godot::RID, godot::LocalVector<SchemaMigration>> migrations;

public:
    void set_column_quantizer(
        const godot::RID &schema,
        int column,
        const godot::Ref<NetwQuantize> &quantizer
    );
    void add_migration(
        const godot::RID &schema,
        int from_version,
        const godot::Callable &step
    );
    godot::Callable migration_from(
        const godot::RID &schema,
        int from_version
    ) const;
    const godot::LocalVector<SchemaMigration> *migrations_of(
        const godot::RID &schema
    ) const;
    godot::Ref<NetwQuantize> column_quantizer(
        const godot::RID &schema,
        int column
    ) const;

    static bool assign_quantizer(
        SchemaRecord *record,
        int column,
        const godot::Ref<NetwQuantize> &quantizer
    );
    static table::QuantSpec spec_of(
        const godot::Ref<NetwQuantize> &quantizer,
        int type
    );
    static NetwQuantize *quantizer_of(const table::QuantSpec &spec);

    static int type_from_variant(int variant_type);
    static godot::Variant make_storage(int type);
    static int storage_type(int type);
    static int element_type(int type);

    static godot::Error validate_value(
        int type,
        int stride,
        const godot::Variant &value
    );
    static godot::Error validate_storable(const godot::Variant &value);
};

} // namespace netw
