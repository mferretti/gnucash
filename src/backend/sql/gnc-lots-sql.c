/********************************************************************
 * gnc-lots-sql.c: load and save data to SQL                        *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/
/** @file gnc-lots-sql.c
 *  @brief load and save data to SQL 
 *  @author Copyright (c) 2006-2008 Phil Longstaff <plongstaff@rogers.com>
 *
 * This file implements the top-level QofBackend API for saving/
 * restoring data to/from an SQL db
 */

#include "config.h"

#include <glib.h>

#include "qof.h"
#include "gnc-lot.h"

#include "gnc-backend-sql.h"
#include "gnc-slots-sql.h"

#include "gnc-lots-sql.h"

static QofLogModule log_module = G_LOG_DOMAIN;

#define TABLE_NAME "lots"
#define TABLE_VERSION 2

static gpointer get_lot_account( gpointer pObject, const QofParam* param );
static void set_lot_account( gpointer pObject, gpointer pValue );
static void set_lot_is_closed( gpointer pObject, gboolean value );

static const GncSqlColumnTableEntry col_table[] =
{
    { "guid",         CT_GUID,    0, COL_NNUL|COL_PKEY, "guid" },
    { "account_guid", CT_GUID,    0, 0,                 NULL, NULL, get_lot_account,   set_lot_account },
    { "is_closed",    CT_BOOLEAN, 0, COL_NNUL,          NULL, NULL,
		(QofAccessFunc)gnc_lot_is_closed, (QofSetterFunc)set_lot_is_closed },
    { NULL }
};

/* ================================================================= */
static gpointer
get_lot_account( gpointer pObject, const QofParam* param )
{
    const GNCLot* lot = GNC_LOT(pObject);
    const Account* pAccount;

	g_return_val_if_fail( pObject != NULL, NULL );
	g_return_val_if_fail( GNC_IS_LOT(pObject), NULL );

    pAccount = gnc_lot_get_account( lot );
    return (gpointer)qof_instance_get_guid( QOF_INSTANCE(pAccount) );
}

static void 
set_lot_account( gpointer pObject, gpointer pValue )
{
    GNCLot* lot = GNC_LOT(pObject);
    QofBook* pBook;
    GUID* guid = (GUID*)pValue;
    Account* pAccount;

	g_return_if_fail( pObject != NULL );
	g_return_if_fail( GNC_IS_LOT(pObject) );
	g_return_if_fail( pValue != NULL );

    pBook = qof_instance_get_book( QOF_INSTANCE(lot) );
    pAccount = xaccAccountLookup( guid, pBook );
    xaccAccountInsertLot( pAccount, lot );
}

static void
set_lot_is_closed( gpointer pObject, gboolean closed )
{
    GNCLot* lot = GNC_LOT(pObject);

	g_return_if_fail( pObject != NULL );
	g_return_if_fail( GNC_IS_LOT(pObject) );

    lot->is_closed = closed;
}

static GNCLot*
load_single_lot( GncSqlBackend* be, GncSqlRow* row )
{
	GNCLot* lot;

	g_return_val_if_fail( be != NULL, NULL );
	g_return_val_if_fail( row != NULL, NULL );

    lot = gnc_lot_new( be->primary_book );

	gnc_lot_begin_edit( lot );
    gnc_sql_load_object( be, row, GNC_ID_LOT, lot, col_table );
	gnc_lot_commit_edit( lot );

	return lot;
}

static void
load_all_lots( GncSqlBackend* be )
{
    GncSqlStatement* stmt;
    GncSqlResult* result;

	g_return_if_fail( be != NULL );

    stmt = gnc_sql_create_select_statement( be, TABLE_NAME );
    result = gnc_sql_execute_select_statement( be, stmt );
	gnc_sql_statement_dispose( stmt );
    if( result != NULL ) {
        int r;
		GList* list = NULL;
        GncSqlRow* row = gnc_sql_result_get_first_row( result );
		GNCLot* lot;

        while( row != NULL ) {
            lot = load_single_lot( be, row );
			if( lot != NULL ) {
				list = g_list_append( list, lot );
			}
			row = gnc_sql_result_get_next_row( result );
        }
		gnc_sql_result_dispose( result );

		if( list != NULL ) {
			gnc_sql_slots_load_for_list( be, list );
		}
    }
}

/* ================================================================= */
static void
create_lots_tables( GncSqlBackend* be )
{
	gint version;

	g_return_if_fail( be != NULL );

	version = gnc_sql_get_table_version( be, TABLE_NAME );
    if( version == 0 ) {
		/* The table doesn't exist, so create it */
        gnc_sql_create_table( be, TABLE_NAME, TABLE_VERSION, col_table );
	} else if( version == 1 ) {
		/* Version 1 -> 2 removes the 'NOT NULL' constraint on the account_guid
		field. 

		Create a temporary table, copy the data from the old table, delete the
		old table, then rename the new one. */
		gchar* sql;
#define TEMP_TABLE_NAME "lots_new"
		GncSqlStatement* stmt;

        gnc_sql_create_temp_table( be, TEMP_TABLE_NAME, col_table );
		sql = g_strdup_printf( "INSERT INTO %s SELECT * FROM %s",
								TEMP_TABLE_NAME, TABLE_NAME );
		(void)gnc_sql_execute_nonselect_sql( be, sql );

		sql = g_strdup_printf( "DROP TABLE %s", TABLE_NAME );
		(void)gnc_sql_execute_nonselect_sql( be, sql );

		sql = g_strdup_printf( "ALTER TABLE %s RENAME TO %s",
								TEMP_TABLE_NAME, TABLE_NAME );
		(void)gnc_sql_execute_nonselect_sql( be, sql );

		gnc_sql_set_table_version( be, TABLE_NAME, TABLE_VERSION );
    }
}

/* ================================================================= */

static gboolean
commit_lot( GncSqlBackend* be, QofInstance* inst )
{
	g_return_val_if_fail( be != NULL, FALSE );
	g_return_val_if_fail( inst != NULL, FALSE );
	g_return_val_if_fail( GNC_IS_LOT(inst), FALSE );

    return gnc_sql_commit_standard_item( be, inst, TABLE_NAME, GNC_ID_LOT, col_table );
}

typedef struct {
	GncSqlBackend* be;
	gboolean is_ok;
} write_objects_t;

static void
do_save_lot( QofInstance* inst, gpointer data )
{
	write_objects_t* s = (write_objects_t*)data;

	if( s->is_ok ) {
		s->is_ok = commit_lot( s->be, inst );
	}
}

static gboolean
write_lots( GncSqlBackend* be )
{
	write_objects_t data;

	g_return_val_if_fail( be != NULL, FALSE );

	data.be = be;
	data.is_ok = TRUE;
    qof_collection_foreach( qof_book_get_collection( be->primary_book, GNC_ID_LOT ),
                            (QofInstanceForeachCB)do_save_lot, &data );
	return data.is_ok;
}

/* ================================================================= */
static void
load_lot_guid( const GncSqlBackend* be, GncSqlRow* row,
            QofSetterFunc setter, gpointer pObject,
            const GncSqlColumnTableEntry* table_row )
{
    const GValue* val;
    GUID guid;
    const GUID* pGuid;
	GNCLot* lot = NULL;

	g_return_if_fail( be != NULL );
	g_return_if_fail( row != NULL );
	g_return_if_fail( pObject != NULL );
	g_return_if_fail( table_row != NULL );

    val = gnc_sql_row_get_value_at_col_name( row, table_row->col_name );
    if( val == NULL ) {
        pGuid = NULL;
    } else {
        string_to_guid( g_value_get_string( val ), &guid );
        pGuid = &guid;
    }
	if( pGuid != NULL ) {
		lot = gnc_lot_lookup( pGuid, be->primary_book );
	}
    if( table_row->gobj_param_name != NULL ) {
		g_object_set( pObject, table_row->gobj_param_name, lot, NULL );
    } else {
		(*setter)( pObject, (const gpointer)lot );
    }
}

static GncSqlColumnTypeHandler lot_guid_handler
	= { load_lot_guid,
		gnc_sql_add_objectref_guid_col_info_to_list,
		gnc_sql_add_colname_to_list,
        gnc_sql_add_gvalue_objectref_guid_to_slist };
/* ================================================================= */
void
gnc_sql_init_lot_handler( void )
{
    static GncSqlObjectBackend be_data =
    {
        GNC_SQL_BACKEND_VERSION,
        GNC_ID_LOT,
        commit_lot,            /* commit */
        load_all_lots,         /* initial_load */
        create_lots_tables,    /* create tables */
		NULL, NULL, NULL,
		write_lots             /* save all */
    };

    qof_object_register_backend( GNC_ID_LOT, GNC_SQL_BACKEND, &be_data );

	gnc_sql_register_col_type_handler( CT_LOTREF, &lot_guid_handler );
}

/* ========================== END OF FILE ===================== */