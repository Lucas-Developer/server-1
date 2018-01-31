/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/read0types.h
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#ifndef read0types_h
#define read0types_h

#include <algorithm>
#include "dict0mem.h"

#include "trx0types.h"


/** View is not in MVCC and not visible to purge thread. */
#define READ_VIEW_STATE_CLOSED 0

/** View is in MVCC, but not visible to purge thread. */
#define READ_VIEW_STATE_REGISTERED 1

/** View is in MVCC, purge thread must wait for READ_VIEW_STATE_OPEN. */
#define READ_VIEW_STATE_SNAPSHOT 2

/** View is in MVCC and is visible to purge thread. */
#define READ_VIEW_STATE_OPEN 3


/**
  Read view lists the trx ids of those transactions for which a consistent read
  should not see the modifications to the database.
*/
class ReadView
{
  /**
    View state.

    It is not defined as enum as it has to be updated using atomic operations.
    Possible values are READ_VIEW_STATE_CLOSED, READ_VIEW_STATE_REGISTERED,
    READ_VIEW_STATE_SNAPSHOT and READ_VIEW_STATE_OPEN.

    Possible state transfers...

    Opening view for the first time:
    READ_VIEW_STATE_CLOSED -> READ_VIEW_STATE_SNAPSHOT (non-atomic)

    Complete first time open or reopen:
    READ_VIEW_STATE_SNAPSHOT -> READ_VIEW_STATE_OPEN (atomic)

    Close view but keep it in list:
    READ_VIEW_STATE_OPEN -> READ_VIEW_STATE_REGISTERED (atomic)

    Close view and remove it from list:
    READ_VIEW_STATE_OPEN -> READ_VIEW_STATE_CLOSED (non-atomic)

    Reusing view:
    READ_VIEW_STATE_REGISTERED -> READ_VIEW_STATE_SNAPSHOT (atomic)

    Removing closed view from list:
    READ_VIEW_STATE_REGISTERED -> READ_VIEW_STATE_CLOSED (non-atomic)
  */
  int32_t m_state;


public:
  ReadView(): m_state(READ_VIEW_STATE_CLOSED) {}


  /**
    Copy state from another view.

    @param other    view to copy from
  */
  void copy(const ReadView &other)
  {
    ut_ad(&other != this);
    m_ids= other.m_ids;
    m_up_limit_id= other.m_up_limit_id;
    m_low_limit_no= other.m_low_limit_no;
    m_low_limit_id= other.m_low_limit_id;
  }


  /**
    Opens a read view where exactly the transactions serialized before this
    point in time are seen in the view.

    View becomes visible to purge thread via trx_sys.m_views.

    @param[in,out] trx transaction
  */
  void open(trx_t *trx);


  /**
    Closes the view.

    View becomes not visible to purge thread via trx_sys.m_views.
  */
  void close();


  /**
    Marks view unused.

    View is still in trx_sys.m_views list, but is not visible to purge threads.
  */
  void unuse()
  {
    ut_ad(m_state != READ_VIEW_STATE_SNAPSHOT);
    if (m_state == READ_VIEW_STATE_OPEN)
      my_atomic_store32_explicit(&m_state, READ_VIEW_STATE_REGISTERED,
                                 MY_MEMORY_ORDER_RELAXED);
  }


  /** m_state getter for trx_sys::clone_oldest_view() trx_sys::size(). */
  int32_t get_state() const
  {
    return my_atomic_load32_explicit(const_cast<int32*>(&m_state),
                                     MY_MEMORY_ORDER_ACQUIRE);
  }


  /**
    Returns true if view is open.

    Only used by view owner thread, thus we can omit atomic operations.
  */
  bool is_open() const
  {
    ut_ad(m_state != READ_VIEW_STATE_SNAPSHOT);
    return m_state == READ_VIEW_STATE_OPEN;
  }


  /**
    Creates a snapshot where exactly the transactions serialized before this
    point in time are seen in the view.

    @param[in,out] trx transaction
  */
  void snapshot(trx_t *trx);


  /**
    Sets the creator transaction id.

    This should be set only for views created by RW transactions.
  */
  void set_creator_trx_id(trx_id_t id)
  {
    ut_ad(id > 0);
    ut_ad(m_creator_trx_id == 0);
    m_creator_trx_id= id;
  }


	/** Check whether transaction id is valid.
	@param[in]	id		transaction id to check
	@param[in]	name		table name */
	static void check_trx_id_sanity(
		trx_id_t		id,
		const table_name_t&	name);

	/** Check whether the changes by id are visible.
	@param[in]	id	transaction id to check against the view
	@param[in]	name	table name
	@return whether the view sees the modifications of id. */
	bool changes_visible(
		trx_id_t		id,
		const table_name_t&	name) const
		MY_ATTRIBUTE((warn_unused_result))
	{
		if (id < m_up_limit_id || id == m_creator_trx_id) {

			return(true);
		}

		check_trx_id_sanity(id, name);

		if (id >= m_low_limit_id) {

			return(false);

		} else if (m_ids.empty()) {

			return(true);
		}

		return(!std::binary_search(m_ids.begin(), m_ids.end(), id));
	}

	/**
	@param id		transaction to check
	@return true if view sees transaction id */
	bool sees(trx_id_t id) const
	{
		return(id < m_up_limit_id);
	}

	/**
	Write the limits to the file.
	@param file		file to write to */
	void print_limits(FILE* file) const
	{
		fprintf(file,
			"Trx read view will not see trx with"
			" id >= " TRX_ID_FMT ", sees < " TRX_ID_FMT "\n",
			m_low_limit_id, m_up_limit_id);
	}

	/**
	@return the low limit no */
	trx_id_t low_limit_no() const
	{
		return(m_low_limit_no);
	}

	/**
	@return the low limit id */
	trx_id_t low_limit_id() const
	{
		return(m_low_limit_id);
	}


private:
	/** The read should not see any transaction with trx id >= this
	value. In other words, this is the "high water mark". */
	trx_id_t	m_low_limit_id;

	/** The read should see all trx ids which are strictly
	smaller (<) than this value.  In other words, this is the
	low water mark". */
	trx_id_t	m_up_limit_id;

	/** trx id of creating transaction, set to TRX_ID_MAX for free
	views. */
	trx_id_t	m_creator_trx_id;

	/** Set of RW transactions that was active when this snapshot
	was taken */
	trx_ids_t	m_ids;

	/** The view does not need to see the undo logs for transactions
	whose transaction number is strictly smaller (<) than this value:
	they can be removed in purge if not needed by other views */
	trx_id_t	m_low_limit_no;

	byte		pad1[CACHE_LINE_SIZE];
public:
	UT_LIST_NODE_T(ReadView)	m_view_list;
};

#endif
