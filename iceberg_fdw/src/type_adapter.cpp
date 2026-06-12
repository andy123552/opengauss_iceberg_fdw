#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"

#include "iceberg_fdw.h"

#define ICEBERG_FDW_VECTOR_MAX_DIM 16000

static const char *
icebergLogicalTypeName(IcebergFdwLogicalType type)
{
    switch (type) {
        case ICEBERG_FDW_TYPE_INT16:
            return "int2";
        case ICEBERG_FDW_TYPE_INT32:
            return "int4";
        case ICEBERG_FDW_TYPE_INT64:
            return "int8";
        case ICEBERG_FDW_TYPE_STRING:
            return "string";
        case ICEBERG_FDW_TYPE_VECTOR_FLOAT32:
            return "vector";
        default:
            return "unknown";
    }
}

static int
icebergVectorDimFromTypmod(int32 typmod, const char *attname)
{
    if (typmod < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg("iceberg_fdw column \"%s\" uses vector without fixed dimension", attname),
                errdetail("Managed Iceberg vector columns must be declared as vector(n).")));
    }

    if (typmod < 1 || typmod > ICEBERG_FDW_VECTOR_MAX_DIM) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg("iceberg_fdw column \"%s\" has invalid vector dimension %d", attname, typmod)));
    }

    return typmod;
}

static IcebergFdwColumnMapping *
icebergBuildColumnMappingValues(AttrNumber attnum, const char *attname, Oid pg_type, int32 pg_typmod,
    Oid pg_collation, bool nullable, int field_id)
{
    IcebergFdwColumnMapping *mapping = (IcebergFdwColumnMapping *)palloc0(sizeof(IcebergFdwColumnMapping));

    mapping->attnum = attnum;
    mapping->field_id = field_id;
    mapping->field_name = pstrdup(attname);
    mapping->pg_type = pg_type;
    mapping->pg_typmod = pg_typmod;
    mapping->pg_collation = pg_collation;
    mapping->nullable = nullable;
    mapping->vector_dim = -1;

    switch (pg_type) {
        case INT2OID:
            mapping->logical_type = ICEBERG_FDW_TYPE_INT16;
            mapping->iceberg_type = pstrdup("int");
            break;
        case INT4OID:
            mapping->logical_type = ICEBERG_FDW_TYPE_INT32;
            mapping->iceberg_type = pstrdup("int");
            break;
        case INT8OID:
            mapping->logical_type = ICEBERG_FDW_TYPE_INT64;
            mapping->iceberg_type = pstrdup("long");
            break;
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
            mapping->logical_type = ICEBERG_FDW_TYPE_STRING;
            mapping->iceberg_type = pstrdup("string");
            break;
        case VECTOROID:
            mapping->logical_type = ICEBERG_FDW_TYPE_VECTOR_FLOAT32;
            mapping->vector_dim = icebergVectorDimFromTypmod(pg_typmod, attname);
            mapping->iceberg_type = psprintf("list<float>;logical=vector;dimension=%d", mapping->vector_dim);
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                    errmsg("iceberg_fdw column \"%s\" type %s is not supported for managed Iceberg tables",
                        attname, format_type_be(pg_type)),
                    errdetail("Supported DDL types are int2, int4, int8, text, varchar, bpchar, and vector(n).")));
    }

    ereport(DEBUG1,
        (errmsg("iceberg_fdw mapped column \"%s\" to Iceberg %s",
            attname, icebergLogicalTypeName(mapping->logical_type))));

    return mapping;
}

static IcebergFdwColumnMapping *
icebergBuildColumnMapping(Form_pg_attribute attr, int field_id)
{
    return icebergBuildColumnMappingValues(attr->attnum, NameStr(attr->attname), attr->atttypid, attr->atttypmod,
        attr->attcollation, !attr->attnotnull, field_id);
}

List *
iceberg_type_build_column_mappings(TupleDesc tuple_desc)
{
    List *mappings = NIL;
    int field_id = 1;

    for (int i = 0; i < tuple_desc->natts; i++) {
        Form_pg_attribute attr = TupleDescAttr(tuple_desc, i);

        if (attr->attisdropped) {
            continue;
        }

        if (attr->attnum <= 0) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                    errmsg("iceberg_fdw cannot map system column \"%s\"", NameStr(attr->attname))));
        }

        mappings = lappend(mappings, icebergBuildColumnMapping(attr, field_id++));
    }

    if (mappings == NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                errmsg("iceberg_fdw managed Iceberg table must contain at least one user column")));
    }

    return mappings;
}

List *
iceberg_type_build_column_mappings_from_column_defs(List *column_defs)
{
    List *mappings = NIL;
    ListCell *lc;
    int field_id = 1;
    AttrNumber attnum = 1;

    foreach (lc, column_defs) {
        Node *node = (Node *)lfirst(lc);
        ColumnDef *col_def;
        Oid type_id;
        int32 typmod;

        if (!IsA(node, ColumnDef)) {
            continue;
        }

        col_def = (ColumnDef *)node;
        typenameTypeIdAndMod(NULL, col_def->typname, &type_id, &typmod);
        mappings = lappend(mappings,
            icebergBuildColumnMappingValues(attnum++, col_def->colname, type_id, typmod, get_typcollation(type_id),
                !col_def->is_not_null, field_id++));
    }

    if (mappings == NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                errmsg("iceberg_fdw managed Iceberg table must contain at least one user column")));
    }

    return mappings;
}
