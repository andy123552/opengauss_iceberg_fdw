#include "postgres.h"

#include "access/tupdesc.h"
#include "executor/spi.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"

#include "iceberg_fdw.h"

#define ICEBERG_FDW_VECTOR_MAX_DIM 16000

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

    switch (pg_type) {
        case INT2OID:
            mapping->iceberg_type = pstrdup("int");
            break;
        case INT4OID:
            mapping->iceberg_type = pstrdup("int");
            break;
        case INT8OID:
            mapping->iceberg_type = pstrdup("long");
            break;
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
            mapping->iceberg_type = pstrdup("string");
            break;
        case VECTOROID:
            (void)icebergVectorDimFromTypmod(pg_typmod, attname);
            mapping->iceberg_type = pstrdup("list<float>");
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                    errmsg("iceberg_fdw column \"%s\" type %s is not supported for managed Iceberg tables",
                        attname, format_type_be(pg_type)),
                    errdetail("Supported DDL types are int2, int4, int8, text, varchar, bpchar, and vector(n).")));
    }

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

List *
iceberg_type_build_column_mappings_from_relid(Oid relid)
{
    char *sql;
    int rc;
    List *mappings = NIL;
    int i;

    rc = SPI_connect();
    if (rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_ERROR),
                errmsg("iceberg_fdw could not connect to SPI to read relation attributes")));
    }

    PG_TRY();
    {
        sql = psprintf(
            "SELECT attnum, attname, atttypid, atttypmod, attcollation, attnotnull "
            "FROM pg_attribute "
            "WHERE attrelid = %u AND attnum > 0 AND NOT attisdropped "
            "ORDER BY attnum",
            relid);
        if (SPI_execute(sql, true, 0) < 0) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_ERROR),
                    errmsg("iceberg_fdw catalog SQL failed while reading relation attributes"),
                    errdetail_internal("%s", sql)));
        }

        for (i = 0; i < (int)SPI_processed; i++) {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc desc = SPI_tuptable->tupdesc;
            AttrNumber attnum;
            char *attname;
            Oid atttypid;
            int32 atttypmod;
            Oid attcollation;
            bool attnotnull;
            bool isnull = false;

            attnum = DatumGetInt16(SPI_getbinval(tuple, desc, SPI_fnumber(desc, "attnum"), &isnull));
            attname = SPI_getvalue(tuple, desc, SPI_fnumber(desc, "attname"));
            if (attname == NULL) {
                ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                        errmsg("iceberg_fdw catalog column \"attname\" is unexpectedly null")));
            }
            atttypid = DatumGetObjectId(SPI_getbinval(tuple, desc, SPI_fnumber(desc, "atttypid"), &isnull));
            atttypmod = DatumGetInt32(SPI_getbinval(tuple, desc, SPI_fnumber(desc, "atttypmod"), &isnull));
            attcollation = DatumGetObjectId(SPI_getbinval(tuple, desc, SPI_fnumber(desc, "attcollation"), &isnull));
            attnotnull = DatumGetBool(SPI_getbinval(tuple, desc, SPI_fnumber(desc, "attnotnull"), &isnull));

            mappings = lappend(mappings, icebergBuildColumnMappingValues(attnum, attname, atttypid, atttypmod,
                attcollation, !attnotnull, (int)attnum));
            pfree(attname);
        }

        SPI_finish();
    }
    PG_CATCH();
    {
        SPI_finish();
        PG_RE_THROW();
    }
    PG_END_TRY();

    return mappings;
}

static const char *
icebergTypeMockLiteral(Oid pg_type, int32 pg_typmod)
{
    switch (pg_type) {
        case INT2OID:
            return "7";
        case INT4OID:
            return "42";
        case INT8OID:
            return "922337203685477580";
        case TEXTOID:
            return "iceberg text";
        case VARCHAROID:
            return "iceberg varchar";
        case BPCHAROID:
            return "ABCD";
        case VECTOROID:
            (void)icebergVectorDimFromTypmod(pg_typmod, "vector");
            return "[1,2,3]";
        default:
            break;
    }

    return NULL;
}

const char *
iceberg_type_mock_literal_for_mapping(const IcebergFdwColumnMapping *mapping)
{
    const char *literal;

    if (mapping == NULL) {
        return NULL;
    }

    literal = icebergTypeMockLiteral(mapping->pg_type, mapping->pg_typmod);
    if (literal == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg("iceberg_fdw cannot build mock literal for column \"%s\"", mapping->field_name)));
    }

    return literal;
}

Datum
iceberg_type_literal_to_datum(const char *literal, Oid pg_type, int32 pg_typmod)
{
    Oid input_oid;
    Oid typio_param;

    if (literal == NULL) {
        return (Datum)0;
    }

    if (pg_type == TEXTOID) {
        return CStringGetTextDatum(literal);
    }

    getTypeInputInfo(pg_type, &input_oid, &typio_param);
    return OidInputFunctionCall(input_oid, (char *)literal, typio_param, pg_typmod);
}
