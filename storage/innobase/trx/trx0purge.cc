/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0purge.cc
Purge old versions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0purge.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "que0que.h"
#include "row0purge.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "dict0load.h"
#include <mysql/service_thd_mdl.h>
#include <mysql/service_wsrep.h>

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong		srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong		srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
purge_sys_t	purge_sys;

#ifdef UNIV_DEBUG
my_bool		srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

/** Sentinel value */
static const TrxUndoRsegs NullElement;

/** Default constructor */
TrxUndoRsegsIterator::TrxUndoRsegsIterator()
	: m_rsegs(NullElement), m_iter(m_rsegs.begin())
{
}

/** Sets the next rseg to purge in purge_sys.
Executed in the purge coordinator thread.
@retval false when nothing is to be purged
@retval true  when purge_sys.rseg->latch was locked */
inline bool TrxUndoRsegsIterator::set_next()
{
	ut_ad(!purge_sys.next_stored);
	mysql_mutex_lock(&purge_sys.pq_mutex);

	/* Only purge consumes events from the priority queue, user
	threads only produce the events. */

	/* Check if there are more rsegs to process in the
	current element. */
	if (m_iter != m_rsegs.end()) {
		/* We are still processing rollback segment from
		the same transaction and so expected transaction
		number shouldn't increase. Undo the increment of
		expected commit done by caller assuming rollback
		segments from given transaction are done. */
		purge_sys.tail.trx_no = (*m_iter)->last_trx_no();
	} else if (!purge_sys.purge_queue.empty()) {
		m_rsegs = purge_sys.purge_queue.top();
		purge_sys.purge_queue.pop();
		ut_ad(purge_sys.purge_queue.empty()
		      || purge_sys.purge_queue.top() != m_rsegs);
		m_iter = m_rsegs.begin();
	} else {
		/* Queue is empty, reset iterator. */
		purge_sys.rseg = NULL;
		mysql_mutex_unlock(&purge_sys.pq_mutex);
		m_rsegs = NullElement;
		m_iter = m_rsegs.begin();
		return false;
	}

	purge_sys.rseg = *m_iter++;
	mysql_mutex_unlock(&purge_sys.pq_mutex);

	/* We assume in purge of externally stored fields that space
	id is in the range of UNDO tablespace space ids */
	ut_ad(purge_sys.rseg->space->id == TRX_SYS_SPACE
	      || srv_is_undo_tablespace(purge_sys.rseg->space->id));

	purge_sys.rseg->latch.wr_lock(SRW_LOCK_CALL);
	trx_id_t last_trx_no = purge_sys.rseg->last_trx_no();
	purge_sys.hdr_offset = purge_sys.rseg->last_offset();
	purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

	/* Only the purge_coordinator_task will access this object
	purge_sys.rseg_iter, or any of purge_sys.hdr_page_no,
	purge_sys.tail.
	The field purge_sys.head and purge_sys.view are modified by
	purge_sys_t::clone_end_view()
	in the purge_coordinator_task
	while holding exclusive purge_sys.latch.
	The purge_sys.view may also be modified by
	purge_sys_t::wake_if_not_active() while holding exclusive
	purge_sys.latch.
	The purge_sys.head may be read by
	purge_truncation_callback(). */
	ut_ad(last_trx_no == m_rsegs.trx_no);
	ut_a(purge_sys.hdr_page_no != FIL_NULL);
	ut_a(purge_sys.tail.trx_no <= last_trx_no);
	purge_sys.tail.trx_no = last_trx_no;

	return(true);
}

/** Build a purge 'query' graph. The actual purge is performed by executing
this query graph.
@return own: the query graph */
static
que_t*
purge_graph_build()
{
	ut_a(srv_n_purge_threads > 0);

	trx_t* trx = trx_create();
	ut_ad(!trx->id);
	trx->start_time = time(NULL);
	trx->start_time_micro = microsecond_interval_timer();
	trx->state = TRX_STATE_ACTIVE;
	trx->op_info = "purge trx";

	mem_heap_t*	heap = mem_heap_create(512);
	que_fork_t*	fork = que_fork_create(heap);
	fork->trx = trx;

	for (auto i = innodb_purge_threads_MAX; i; i--) {
		que_thr_t*	thr = que_thr_create(fork, heap, NULL);
		thr->child = new(mem_heap_alloc(heap, sizeof(purge_node_t)))
			purge_node_t(thr);
	}

	return(fork);
}

/** Initialise the purge system. */
void purge_sys_t::create()
{
  ut_ad(this == &purge_sys);
  ut_ad(!m_initialized);
  ut_ad(!enabled());
  m_paused= 0;
  query= purge_graph_build();
  next_stored= false;
  rseg= NULL;
  page_no= 0;
  offset= 0;
  hdr_page_no= 0;
  hdr_offset= 0;
  latch.SRW_LOCK_INIT(trx_purge_latch_key);
  end_latch.init();
  mysql_mutex_init(purge_sys_pq_mutex_key, &pq_mutex, nullptr);
  truncate.current= NULL;
  truncate.last= NULL;
  m_initialized= true;
}

/** Close the purge subsystem on shutdown. */
void purge_sys_t::close()
{
  ut_ad(this == &purge_sys);
  if (!m_initialized)
    return;

  ut_ad(!enabled());
  trx_t *trx= query->trx;
  que_graph_free(query);
  ut_ad(!trx->id);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  trx->state= TRX_STATE_NOT_STARTED;
  trx->free();
  latch.destroy();
  end_latch.destroy();
  mysql_mutex_destroy(&pq_mutex);
  m_initialized= false;
}

/** Determine if the history of a transaction is purgeable.
@param trx_id  transaction identifier
@return whether the history is purgeable */
TRANSACTIONAL_TARGET bool purge_sys_t::is_purgeable(trx_id_t trx_id) const
{
  bool purgeable;
#if !defined SUX_LOCK_GENERIC && !defined NO_ELISION
  purgeable= false;
  if (xbegin())
  {
    if (!latch.is_write_locked())
    {
      purgeable= view.changes_visible(trx_id);
      xend();
    }
    else
      xabort();
  }
  else
#endif
  {
    latch.rd_lock(SRW_LOCK_CALL);
    purgeable= view.changes_visible(trx_id);
    latch.rd_unlock();
  }
  return purgeable;
}

/*================ UNDO LOG HISTORY LIST =============================*/

/** Prepend the history list with an undo log.
Remove the undo log segment from the rseg slot if it is too big for reuse.
@param[in]	trx		transaction
@param[in,out]	undo		undo log
@param[in,out]	mtr		mini-transaction */
void
trx_purge_add_undo_to_history(const trx_t* trx, trx_undo_t*& undo, mtr_t* mtr)
{
  DBUG_PRINT("trx", ("commit(" TRX_ID_FMT "," TRX_ID_FMT ")",
                     trx->id, trx_id_t{trx->rw_trx_hash_element->no}));
  ut_ad(undo->id < TRX_RSEG_N_SLOTS);
  ut_ad(undo == trx->rsegs.m_redo.undo);
  trx_rseg_t *rseg= trx->rsegs.m_redo.rseg;
  ut_ad(undo->rseg == rseg);
  buf_block_t *rseg_header= rseg->get(mtr, nullptr);
  /* We are in transaction commit; we cannot return an error. If the
  database is corrupted, it is better to crash it than to
  intentionally violate ACID by committing something that is known to
  be corrupted. */
  ut_ad(rseg_header);
  buf_block_t *undo_page=
    buf_page_get(page_id_t(rseg->space->id, undo->hdr_page_no), 0,
                 RW_X_LATCH, mtr);
  /* This function is invoked during transaction commit, which is not
  allowed to fail. If we get a corrupted undo header, we will crash here. */
  ut_a(undo_page);
  trx_ulogf_t *undo_header= undo_page->page.frame + undo->hdr_offset;

  ut_ad(mach_read_from_2(undo_header + TRX_UNDO_NEEDS_PURGE) <= 1);
  ut_ad(rseg->needs_purge > trx->id);
  ut_ad(rseg->last_page_no != FIL_NULL);

  rseg->history_size++;

  if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                     rseg_header->page.frame)))
    /* This database must have been upgraded from before MariaDB 10.3.5. */
    trx_rseg_format_upgrade(rseg_header, mtr);

  uint16_t undo_state;

  if (undo->size == 1 &&
      TRX_UNDO_PAGE_REUSE_LIMIT >
      mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE +
                       undo_page->page.frame))
  {
    undo->state= undo_state= TRX_UNDO_CACHED;
    UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
  }
  else
  {
    ut_ad(undo->size == flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST +
                                     undo_page->page.frame));
    /* The undo log segment will not be reused */
    static_assert(FIL_NULL == 0xffffffff, "");
    mtr->memset(rseg_header, TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
                undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);
    uint32_t hist_size= mach_read_from_4(TRX_RSEG_HISTORY_SIZE + TRX_RSEG +
                                         rseg_header->page.frame);
    mtr->write<4>(*rseg_header, TRX_RSEG + TRX_RSEG_HISTORY_SIZE +
                  rseg_header->page.frame, hist_size + undo->size);
    mtr->write<8>(*rseg_header, TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                  rseg_header->page.frame, trx_sys.get_max_trx_id());
    ut_free(undo);
    undo_state= TRX_UNDO_TO_PURGE;
  }

  undo= nullptr;

  /*
  Before any transaction-generating background threads or the purge
  have been started, we can start transactions in
  row_merge_drop_temp_indexes(), and roll back recovered transactions.

  Arbitrary user transactions may be executed when all the undo log
  related background processes (including purge) are disabled due to
  innodb_force_recovery=2 or innodb_force_recovery=3.  DROP TABLE may
  be executed at any innodb_force_recovery level.

  During fast shutdown, we may also continue to execute user
  transactions. */
  ut_ad(srv_undo_sources || srv_fast_shutdown ||
        (!purge_sys.enabled() &&
         (srv_is_being_started ||
          srv_force_recovery >= SRV_FORCE_NO_BACKGROUND)));

#ifdef WITH_WSREP
  if (wsrep_is_wsrep_xid(&trx->xid))
    trx_rseg_update_wsrep_checkpoint(rseg_header, &trx->xid, mtr);
#endif

  if (trx->mysql_log_file_name && *trx->mysql_log_file_name)
    /* Update the latest binlog name and offset if log_bin=ON or this
    is a replica. */
    trx_rseg_update_binlog_offset(rseg_header, trx->mysql_log_file_name,
                                  trx->mysql_log_offset, mtr);

  /* Add the log as the first in the history list */

  /* We are in transaction commit; we cannot return an error
  when detecting corruption. It is better to crash the server
  than to intentionally violate ACID by committing something
  that is known to be corrupted. */
  ut_a(flst_add_first(rseg_header, TRX_RSEG + TRX_RSEG_HISTORY, undo_page,
                      uint16_t(page_offset(undo_header) +
                               TRX_UNDO_HISTORY_NODE), mtr) == DB_SUCCESS);

  mtr->write<2>(*undo_page, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE +
                undo_page->page.frame, undo_state);
  mtr->write<8,mtr_t::MAYBE_NOP>(*undo_page, undo_header + TRX_UNDO_TRX_NO,
                                 trx->rw_trx_hash_element->no);
  mtr->write<2,mtr_t::MAYBE_NOP>(*undo_page, undo_header +
                                 TRX_UNDO_NEEDS_PURGE, 1U);
}

/** Free an undo log segment.
@param rseg_hdr  rollback segment header page
@param block     undo segment header page
@param mtr       mini-transaction */
static void trx_purge_free_segment(buf_block_t *rseg_hdr, buf_block_t *block,
                                   mtr_t &mtr)
{
  ut_ad(mtr.memo_contains_flagged(rseg_hdr, MTR_MEMO_PAGE_X_FIX));
  ut_ad(mtr.memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));

  while (!fseg_free_step_not_header(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER +
                                    block->page.frame, &mtr))
  {
    rseg_hdr->fix();
    block->fix();
    ut_d(const page_id_t rseg_hdr_id{rseg_hdr->page.id()});
    ut_d(const page_id_t id{block->page.id()});
    mtr.commit();
    /* NOTE: If the server is killed after the log that was produced
    up to this point was written, and before the log from the mtr.commit()
    in our caller is written, then the pages belonging to the
    undo log will become unaccessible garbage.

    This does not matter when using multiple innodb_undo_tablespaces;
    innodb_undo_log_truncate=ON will be able to reclaim the space. */
    mtr.start();
    rseg_hdr->page.lock.x_lock();
    ut_ad(rseg_hdr->page.id() == rseg_hdr_id);
    block->page.lock.x_lock();
    ut_ad(block->page.id() == id);
    mtr.memo_push(rseg_hdr, MTR_MEMO_PAGE_X_MODIFY);
    mtr.memo_push(block, MTR_MEMO_PAGE_X_MODIFY);
  }

  while (!fseg_free_step(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER +
                         block->page.frame, &mtr));
}

/** Remove unnecessary history data from a rollback segment.
@param rseg   rollback segment
@param limit  truncate anything before this
@param all    whether everything can be truncated
@return error code */
static dberr_t
trx_purge_truncate_rseg_history(trx_rseg_t &rseg,
                                const purge_sys_t::iterator &limit, bool all)
{
  fil_addr_t hdr_addr;
  mtr_t mtr;

  mtr.start();

  dberr_t err;
  buf_block_t *rseg_hdr= rseg.get(&mtr, &err);
  if (!rseg_hdr)
  {
func_exit:
    mtr.commit();
    return err;
  }

  hdr_addr= flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + rseg_hdr->page.frame);
  hdr_addr.boffset= static_cast<uint16_t>(hdr_addr.boffset -
                                          TRX_UNDO_HISTORY_NODE);

loop:
  if (hdr_addr.page == FIL_NULL)
    goto func_exit;

  buf_block_t *b=
    buf_page_get_gen(page_id_t(rseg.space->id, hdr_addr.page),
                     0, RW_X_LATCH, nullptr, BUF_GET_POSSIBLY_FREED,
                     &mtr, &err);
  if (!b)
    goto func_exit;

  const trx_id_t undo_trx_no=
    mach_read_from_8(b->page.frame + hdr_addr.boffset + TRX_UNDO_TRX_NO);

  if (undo_trx_no >= limit.trx_no)
  {
    if (undo_trx_no == limit.trx_no)
      err = trx_undo_truncate_start(&rseg, hdr_addr.page,
                                    hdr_addr.boffset, limit.undo_no);
    goto func_exit;
  }

  if (!all)
    goto func_exit;

  fil_addr_t prev_hdr_addr=
    flst_get_prev_addr(b->page.frame + hdr_addr.boffset +
                       TRX_UNDO_HISTORY_NODE);
  prev_hdr_addr.boffset= static_cast<uint16_t>(prev_hdr_addr.boffset -
                                               TRX_UNDO_HISTORY_NODE);

  err= flst_remove(rseg_hdr, TRX_RSEG + TRX_RSEG_HISTORY, b,
                   uint16_t(hdr_addr.boffset + TRX_UNDO_HISTORY_NODE), &mtr);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
    goto func_exit;

  rseg_hdr->fix();

  if (mach_read_from_2(b->page.frame + hdr_addr.boffset + TRX_UNDO_NEXT_LOG))
    /* We cannot free the entire undo log segment. */;
  else
  {
    const uint32_t seg_size=
      flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + b->page.frame);
    switch (mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE +
                             b->page.frame)) {
    case TRX_UNDO_TO_PURGE:
      {
        byte *hist= TRX_RSEG + TRX_RSEG_HISTORY_SIZE + rseg_hdr->page.frame;
        ut_ad(mach_read_from_4(hist) >= seg_size);
        mtr.write<4>(*rseg_hdr, hist, mach_read_from_4(hist) - seg_size);
      }
    free_segment:
      ut_ad(rseg.curr_size >= seg_size);
      rseg.curr_size-= seg_size;
      trx_purge_free_segment(rseg_hdr, b, mtr);
      break;
    case TRX_UNDO_CACHED:
      /* rseg.undo_cached must point to this page */
      trx_undo_t *undo= UT_LIST_GET_FIRST(rseg.undo_cached);
      for (; undo; undo= UT_LIST_GET_NEXT(undo_list, undo))
        if (undo->hdr_page_no == hdr_addr.page)
          goto found_cached;
      ut_ad("inconsistent undo logs" == 0);
      if (false)
      found_cached:
        UT_LIST_REMOVE(rseg.undo_cached, undo);
      static_assert(FIL_NULL == 0xffffffff, "");
      if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                         rseg_hdr->page.frame)))
        trx_rseg_format_upgrade(rseg_hdr, &mtr);
      mtr.memset(rseg_hdr, TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
                 undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);
      ut_free(undo);
      mtr.write<8,mtr_t::MAYBE_NOP>(*rseg_hdr, TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                                    rseg_hdr->page.frame,
                                    trx_sys.get_max_trx_id() - 1);
      goto free_segment;
    }
  }

  hdr_addr= prev_hdr_addr;

  mtr.commit();
  ut_ad(rseg.history_size > 0);
  rseg.history_size--;
  mtr.start();
  rseg_hdr->page.lock.x_lock();
  ut_ad(rseg_hdr->page.id() == rseg.page_id());
  mtr.memo_push(rseg_hdr, MTR_MEMO_PAGE_X_MODIFY);

  goto loop;
}

/** Cleanse purge queue to remove the rseg that reside in undo-tablespace
marked for truncate.
@param[in]	space	undo tablespace being truncated */
static void trx_purge_cleanse_purge_queue(const fil_space_t& space)
{
	typedef	std::vector<TrxUndoRsegs>	purge_elem_list_t;
	purge_elem_list_t			purge_elem_list;

	mysql_mutex_lock(&purge_sys.pq_mutex);

	/* Remove rseg instances that are in the purge queue before we start
	truncate of corresponding UNDO truncate. */
	while (!purge_sys.purge_queue.empty()) {
		purge_elem_list.push_back(purge_sys.purge_queue.top());
		purge_sys.purge_queue.pop();
	}

	for (purge_elem_list_t::iterator it = purge_elem_list.begin();
	     it != purge_elem_list.end();
	     ++it) {

		for (TrxUndoRsegs::iterator it2 = it->begin();
		     it2 != it->end();
		     ++it2) {
			if ((*it2)->space == &space) {
				it->erase(it2);
				break;
			}
		}

		if (!it->empty()) {
			purge_sys.purge_queue.push(*it);
		}
	}

	mysql_mutex_unlock(&purge_sys.pq_mutex);
}

dberr_t purge_sys_t::iterator::free_history() const
{
  for (auto &rseg : trx_sys.rseg_array)
    if (rseg.space)
    {
      ut_ad(rseg.is_persistent());
      log_free_check();
      rseg.latch.wr_lock(SRW_LOCK_CALL);
      dberr_t err=
        trx_purge_truncate_rseg_history(rseg, *this, !rseg.is_referenced() &&
                                        purge_sys.sees(rseg.needs_purge));
      rseg.latch.wr_unlock();
      if (err)
        return err;
    }
  return DB_SUCCESS;
}

#if defined __GNUC__ && __GNUC__ == 4 && !defined __clang__
# if defined __arm__ || defined __aarch64__
/* Work around an internal compiler error in GCC 4.8.5 */
__attribute__((optimize(0)))
# endif
#endif
/**
Remove unnecessary history data from rollback segments. NOTE that when this
function is called, the caller
(purge_coordinator_callback or purge_truncation_callback)
must not have any latches on undo log pages!
*/
TRANSACTIONAL_TARGET void trx_purge_truncate_history()
{
  ut_ad(purge_sys.head <= purge_sys.tail);
  purge_sys_t::iterator &head= purge_sys.head.trx_no
    ? purge_sys.head : purge_sys.tail;

  if (head.trx_no >= purge_sys.low_limit_no())
  {
    /* This is sometimes necessary. TODO: find out why. */
    head.trx_no= purge_sys.low_limit_no();
    head.undo_no= 0;
  }

  if (head.free_history() != DB_SUCCESS || srv_undo_tablespaces_active < 2)
    return;

  while (srv_undo_log_truncate)
  {
    if (!purge_sys.truncate.current)
    {
      const ulint threshold=
        ulint(srv_max_undo_log_size >> srv_page_size_shift);
      for (uint32_t i= purge_sys.truncate.last
           ? purge_sys.truncate.last->id - srv_undo_space_id_start : 0,
           j= i;; )
      {
        const uint32_t space_id= srv_undo_space_id_start + i;
        ut_ad(srv_is_undo_tablespace(space_id));
        fil_space_t *space= fil_space_get(space_id);
        ut_a(UT_LIST_GET_LEN(space->chain) == 1);

        if (space && space->get_size() > threshold)
        {
          purge_sys.truncate.current= space;
          break;
        }

        ++i;
        i %= srv_undo_tablespaces_active;
        if (i == j)
          return;
      }
    }

    fil_space_t &space= *purge_sys.truncate.current;
    /* Undo tablespace always are a single file. */
    fil_node_t *file= UT_LIST_GET_FIRST(space.chain);
    /* The undo tablespace files are never closed. */
    ut_ad(file->is_open());

    DBUG_LOG("undo", "marking for truncate: " << file->name);

    for (auto &rseg : trx_sys.rseg_array)
      if (rseg.space == &space)
        /* Once set, this rseg will not be allocated to subsequent
        transactions, but we will wait for existing active
        transactions to finish. */
        rseg.set_skip_allocation();

    for (auto &rseg : trx_sys.rseg_array)
    {
      if (rseg.space != &space)
        continue;

      rseg.latch.rd_lock(SRW_LOCK_CALL);
      ut_ad(rseg.skip_allocation());
      if (rseg.is_referenced() || !purge_sys.sees(rseg.needs_purge))
      {
not_free:
        rseg.latch.rd_unlock();
        return;
      }

      ut_ad(UT_LIST_GET_LEN(rseg.undo_list) == 0);
      /* Check if all segments are cached and safe to remove. */
      ulint cached= 0;

      for (const trx_undo_t *undo= UT_LIST_GET_FIRST(rseg.undo_cached); undo;
           undo= UT_LIST_GET_NEXT(undo_list, undo))
      {
        if (head.trx_no && head.trx_no < undo->trx_id)
          goto not_free;
        else
          cached+= undo->size;
      }

      ut_ad(rseg.curr_size > cached);
      if (rseg.curr_size > cached + 1 &&
          (rseg.history_size || srv_fast_shutdown || srv_undo_sources))
        goto not_free;

      rseg.latch.rd_unlock();
    }

    ib::info() << "Truncating " << file->name;
    trx_purge_cleanse_purge_queue(space);

    log_free_check();

    mtr_t mtr;
    mtr.start();
    mtr.x_lock_space(&space);
    const auto space_id= space.id;

    /* Lock all modified pages of the tablespace.

    During truncation, we do not want any writes to the file.

    If a log checkpoint was completed at LSN earlier than our
    mini-transaction commit and the server was killed, then
    discarding the to-be-trimmed pages without flushing would
    break crash recovery. */

  rescan:
    if (UNIV_UNLIKELY(srv_shutdown_state != SRV_SHUTDOWN_NONE) &&
        srv_fast_shutdown)
    {
    fast_shutdown:
      mtr.commit();
      return;
    }

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
    {
      ut_ad(bpage->oldest_modification());
      ut_ad(bpage->in_file());

      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      if (bpage->oldest_modification() > 2 && bpage->id().space() == space_id)
      {
        ut_ad(bpage->frame);
        bpage->fix();
        {
          /* Try to acquire an exclusive latch while the cache line is
          fresh after fix(). */
          const bool got_lock{bpage->lock.x_lock_try()};
          buf_pool.flush_hp.set(prev);
          mysql_mutex_unlock(&buf_pool.flush_list_mutex);
          if (!got_lock)
            bpage->lock.x_lock();
        }

#ifdef BTR_CUR_HASH_ADAPT
        /* There is no AHI on undo tablespaces. */
        ut_ad(!reinterpret_cast<buf_block_t*>(bpage)->index);
#endif
        ut_ad(!bpage->is_io_fixed());
        ut_ad(bpage->id().space() == space_id);

        if (bpage->oldest_modification() > 2 &&
            !mtr.have_x_latch(*reinterpret_cast<buf_block_t*>(bpage)))
          mtr.memo_push(reinterpret_cast<buf_block_t*>(bpage),
                        MTR_MEMO_PAGE_X_FIX);
        else
        {
          bpage->unfix();
          bpage->lock.x_unlock();
        }

        mysql_mutex_lock(&buf_pool.flush_list_mutex);

        if (prev != buf_pool.flush_hp.get())
        {
          /* The functions buf_pool_t::release_freed_page() or
          buf_do_flush_list_batch() may be right now holding
          buf_pool.mutex and waiting to acquire
          buf_pool.flush_list_mutex. Ensure that they can proceed,
          to avoid extreme waits. */
          mysql_mutex_unlock(&buf_pool.flush_list_mutex);
          mysql_mutex_lock(&buf_pool.mutex);
          mysql_mutex_unlock(&buf_pool.mutex);
          goto rescan;
        }
      }

      bpage= prev;
    }

    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (UNIV_UNLIKELY(srv_shutdown_state != SRV_SHUTDOWN_NONE) &&
        srv_fast_shutdown)
      goto fast_shutdown;

    /* Re-initialize tablespace, in a single mini-transaction. */
    const ulint size= SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;

    /* Adjust the tablespace metadata. */
    mysql_mutex_lock(&fil_system.mutex);
    space.set_stopping();
    space.is_being_truncated= true;
    if (space.crypt_data)
    {
      space.reacquire();
      mysql_mutex_unlock(&fil_system.mutex);
      fil_space_crypt_close_tablespace(&space);
      space.release();
    }
    else
      mysql_mutex_unlock(&fil_system.mutex);

    for (auto i= 6000; space.referenced();
         std::this_thread::sleep_for(std::chrono::milliseconds(10)))
    {
      if (!--i)
      {
        mtr.commit();
        ib::error() << "Failed to freeze UNDO tablespace " << file->name;
        return;
      }
    }

    /* Associate the undo tablespace with mtr.
    During mtr::commit_shrink(), InnoDB can use the undo
    tablespace object to clear all freed ranges */
    mtr.set_named_space(&space);
    mtr.trim_pages(page_id_t(space.id, size));
    ut_a(fsp_header_init(&space, size, &mtr) == DB_SUCCESS);
    mysql_mutex_lock(&fil_system.mutex);
    space.size= file->size= size;
    mysql_mutex_unlock(&fil_system.mutex);

    for (auto &rseg : trx_sys.rseg_array)
    {
      if (rseg.space != &space)
        continue;

      ut_ad(!rseg.is_referenced());
      /* We may actually have rseg.needs_purge > head.trx_no here
      if trx_t::commit_empty() had been executed in the past,
      possibly before this server had been started up. */

      dberr_t err;
      buf_block_t *rblock= trx_rseg_header_create(&space,
                                                  &rseg - trx_sys.rseg_array,
                                                  trx_sys.get_max_trx_id(),
                                                  &mtr, &err);
      ut_a(rblock);
      /* These were written by trx_rseg_header_create(). */
      ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                              rblock->page.frame));
      ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE +
                              rblock->page.frame));
      rseg.reinit(rblock->page.id().page_no());
    }

    mtr.commit_shrink(space);

    /* No mutex; this is only updated by the purge coordinator. */
    export_vars.innodb_undo_truncations++;

    if (purge_sys.rseg && purge_sys.rseg->last_page_no == FIL_NULL)
    {
      /* If purge_sys.rseg is pointing to rseg that was recently
      truncated then move to next rseg element.

      Note: Ideally purge_sys.rseg should be NULL because purge should
      complete processing of all the records but srv_purge_batch_size
      can force the purge loop to exit before all the records are purged. */
      purge_sys.rseg= nullptr;
      purge_sys.next_stored= false;
    }

    DBUG_EXECUTE_IF("ib_undo_trunc", ib::info() << "ib_undo_trunc";
                    log_buffer_flush_to_disk();
                    DBUG_SUICIDE(););

    ib::info() << "Truncated " << file->name;
    purge_sys.truncate.last= purge_sys.truncate.current;
    ut_ad(&space == purge_sys.truncate.current);
    purge_sys.truncate.current= nullptr;
  }
}

buf_block_t *purge_sys_t::get_page(page_id_t id)
{
  buf_block_t*& undo_page= pages[id];

  if (undo_page)
    return undo_page;

  mtr_t mtr;
  mtr.start();
  undo_page=
    buf_page_get_gen(id, 0, RW_S_LATCH, nullptr, BUF_GET_POSSIBLY_FREED, &mtr);

  if (UNIV_LIKELY(undo_page != nullptr))
  {
    undo_page->fix();
    mtr.commit();
    return undo_page;
  }

  mtr.commit();
  pages.erase(id);
  return nullptr;
}

void purge_sys_t::rseg_get_next_history_log()
{
  fil_addr_t prev_log_addr;

#ifndef SUX_LOCK_GENERIC
  ut_ad(rseg->latch.is_write_locked());
#endif
  ut_a(rseg->last_page_no != FIL_NULL);

  tail.trx_no= rseg->last_trx_no() + 1;
  tail.undo_no= 0;
  next_stored= false;

  if (buf_block_t *undo_page=
      get_page(page_id_t(rseg->space->id, rseg->last_page_no)))
  {
    const byte *log_hdr= undo_page->page.frame + rseg->last_offset();
    prev_log_addr= flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE);
    prev_log_addr.boffset = static_cast<uint16_t>(prev_log_addr.boffset -
                                                  TRX_UNDO_HISTORY_NODE);
  }
  else
    prev_log_addr.page= FIL_NULL;

  if (prev_log_addr.page == FIL_NULL)
    rseg->last_page_no= FIL_NULL;
  else
  {
    /* Read the previous log header. */
    trx_id_t trx_no= 0;
    if (const buf_block_t* undo_page=
        get_page(page_id_t(rseg->space->id,
                                     prev_log_addr.page)))
    {
      const byte *log_hdr= undo_page->page.frame + prev_log_addr.boffset;
      trx_no= mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);
      ut_ad(mach_read_from_2(log_hdr + TRX_UNDO_NEEDS_PURGE) <= 1);
    }

    if (UNIV_LIKELY(trx_no != 0))
    {
      rseg->last_page_no= prev_log_addr.page;
      rseg->set_last_commit(prev_log_addr.boffset, trx_no);

      /* Purge can also produce events, however these are already
      ordered in the rollback segment and any user generated event
      will be greater than the events that Purge produces. ie. Purge
      can never produce events from an empty rollback segment. */

      mysql_mutex_lock(&pq_mutex);
      purge_queue.push(*rseg);
      mysql_mutex_unlock(&pq_mutex);
    }
  }

  rseg->latch.wr_unlock();
}

/** Position the purge sys "iterator" on the undo record to use for purging.
@retval false when nothing is to be purged
@retval true  when purge_sys.rseg->latch was locked */
bool purge_sys_t::choose_next_log()
{
  if (!rseg_iter.set_next())
    return false;

  hdr_offset= rseg->last_offset();
  hdr_page_no= rseg->last_page_no;

  if (!rseg->needs_purge)
  {
  purge_nothing:
    page_no= hdr_page_no;
    offset= 0;
    tail.undo_no= 0;
  }
  else
  {
    page_id_t id{rseg->space->id, hdr_page_no};
    buf_block_t *b= get_page(id);
    if (!b)
      goto purge_nothing;
    const trx_undo_rec_t *undo_rec=
      trx_undo_page_get_first_rec(b, hdr_page_no, hdr_offset);
    if (!undo_rec)
    {
      if (mach_read_from_2(b->page.frame + hdr_offset + TRX_UNDO_NEXT_LOG))
        goto purge_nothing;
      const uint32_t next=
        mach_read_from_4(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                         FLST_NEXT + FIL_ADDR_PAGE + b->page.frame);
      if (next == FIL_NULL)
        goto purge_nothing;
      id.set_page_no(next);
      b= get_page(id);
      if (!b)
        goto purge_nothing;
      undo_rec=
        trx_undo_page_get_first_rec(b, page_no, hdr_offset);
      if (!undo_rec)
        goto purge_nothing;
    }

    offset= page_offset(undo_rec);
    tail.undo_no= trx_undo_rec_get_undo_no(undo_rec);
    page_no= id.page_no();
  }

  next_stored= true;
  return true;
}

/**
Get the next record to purge and update the info in the purge system.
@param roll_ptr           undo log pointer to the record
@return buffer-fixed reference to undo log record
@retval {nullptr,1} if the whole undo log can skipped in purge
@retval {nullptr,0} if nothing is left, or on corruption */
inline trx_purge_rec_t purge_sys_t::get_next_rec(roll_ptr_t roll_ptr)
{
  ut_ad(next_stored);
  ut_ad(tail.trx_no < low_limit_no());
#ifndef SUX_LOCK_GENERIC
  ut_ad(rseg->latch.is_write_locked());
#endif

  if (!offset)
  {
    /* It is the dummy undo log record, which means that there is no
    need to purge this undo log */
    rseg_get_next_history_log();

    /* Look for the next undo log and record to purge */
    if (choose_next_log())
      rseg->latch.wr_unlock();
    return {nullptr, 1};
  }

  ut_ad(offset == uint16_t(roll_ptr));

  page_id_t page_id{rseg->space->id, page_no};
  bool locked= true;
  buf_block_t *b= get_page(page_id);
  if (UNIV_UNLIKELY(!b))
  {
    if (locked)
      rseg->latch.wr_unlock();
    return {nullptr, 0};
  }

  if (const trx_undo_rec_t *rec2=
      trx_undo_page_get_next_rec(b, offset, hdr_page_no, hdr_offset))
  {
  got_rec:
    ut_ad(page_no == page_id.page_no());
    offset= page_offset(rec2);
    tail.undo_no= trx_undo_rec_get_undo_no(rec2);
  }
  else if (hdr_page_no != page_no ||
           !mach_read_from_2(b->page.frame + hdr_offset + TRX_UNDO_NEXT_LOG))
  {
    uint32_t next= mach_read_from_4(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE +
                                    FLST_NEXT + FIL_ADDR_PAGE + b->page.frame);
    if (next != FIL_NULL)
    {
      page_id.set_page_no(next);
      if (buf_block_t *next_page= get_page(page_id))
      {
        rec2= trx_undo_page_get_first_rec(next_page, hdr_page_no, hdr_offset);
        if (rec2)
        {
          page_no= next;
          goto got_rec;
        }
      }
    }
    goto got_no_rec;
  }
  else
  {
  got_no_rec:
    rseg_get_next_history_log();
    /* Look for the next undo log and record to purge */
    locked= choose_next_log();
  }

  if (locked)
    rseg->latch.wr_unlock();

  return {b->page.frame + uint16_t(roll_ptr), roll_ptr};
}

inline trx_purge_rec_t purge_sys_t::fetch_next_rec()
{
  roll_ptr_t roll_ptr;

  if (!next_stored)
  {
    bool locked= choose_next_log();
    ut_ad(locked == next_stored);
    if (!locked)
      goto got_nothing;
    if (tail.trx_no >= low_limit_no())
    {
      rseg->latch.wr_unlock();
      goto got_nothing;
    }
    /* row_purge_record_func() will later set ROLL_PTR_INSERT_FLAG for
    TRX_UNDO_INSERT_REC */
    roll_ptr= trx_undo_build_roll_ptr(false, trx_sys.rseg_id(rseg, true),
                                      page_no, offset);
  }
  else if (tail.trx_no >= low_limit_no())
  got_nothing:
    return {nullptr, 0};
  else
  {
    roll_ptr= trx_undo_build_roll_ptr(false, trx_sys.rseg_id(rseg, true),
                                      page_no, offset);
    rseg->latch.wr_lock(SRW_LOCK_CALL);
  }

  /* The following will advance the purge iterator. */
  return get_next_rec(roll_ptr);
}

/** Close all tables that were opened in a purge batch for a worker.
@param node   purge task context
@param thd    purge coordinator thread handle */
static void trx_purge_close_tables(purge_node_t *node, THD *thd)
{
  for (auto &t : node->tables)
  {
    if (!t.second.first);
    else if (t.second.first == reinterpret_cast<dict_table_t*>(-1));
    else
    {
      dict_table_close(t.second.first, false, thd, t.second.second);
      t.second.first= reinterpret_cast<dict_table_t*>(-1);
    }
  }
}

void purge_sys_t::wait_FTS(bool also_sys)
{
  bool paused;
  do
  {
    latch.wr_lock(SRW_LOCK_CALL);
    paused= m_FTS_paused || (also_sys && m_SYS_paused);
    latch.wr_unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  while (paused);
}

__attribute__((nonnull))
/** Aqcuire a metadata lock on a table.
@param table        table handle
@param mdl_context  metadata lock acquisition context
@param mdl          metadata lcok
@return table handle
@retval nullptr if the table is not found or accessible
@retval -1      if the purge of history must be suspended due to DDL */
static dict_table_t *trx_purge_table_acquire(dict_table_t *table,
                                             MDL_context *mdl_context,
                                             MDL_ticket **mdl)
{
  ut_ad(dict_sys.frozen_not_locked());
  *mdl= nullptr;

  if (!table->is_readable() || table->corrupted)
  {
    table->release();
    return nullptr;
  }

  size_t db_len= dict_get_db_name_len(table->name.m_name);
  if (db_len == 0)
    return table; /* InnoDB system tables are not covered by MDL */

  if (purge_sys.must_wait_FTS())
  {
  must_wait:
    table->release();
    return reinterpret_cast<dict_table_t*>(-1);
  }

  char db_buf[NAME_LEN + 1];
  char tbl_buf[NAME_LEN + 1];
  size_t tbl_len;

  if (!table->parse_name<true>(db_buf, tbl_buf, &db_len, &tbl_len))
    /* The name of an intermediate table starts with #sql */
    return table;

  {
    MDL_request request;
    MDL_REQUEST_INIT(&request,MDL_key::TABLE, db_buf, tbl_buf, MDL_SHARED,
                     MDL_EXPLICIT);
    if (mdl_context->try_acquire_lock(&request))
      goto must_wait;
    *mdl= request.ticket;
    if (!*mdl)
      goto must_wait;
  }

  return table;
}

/** Open a table handle for the purge of committed transaction history
@param table_id     InnoDB table identifier
@param mdl_context  metadata lock acquisition context
@param mdl          metadata lcok
@return table handle
@retval nullptr if the table is not found or accessible
@retval -1      if the purge of history must be suspended due to DDL */
static dict_table_t *trx_purge_table_open(table_id_t table_id,
                                          MDL_context *mdl_context,
                                          MDL_ticket **mdl)
{
  dict_sys.freeze(SRW_LOCK_CALL);

  dict_table_t *table= dict_sys.find_table(table_id);

  if (table)
    table->acquire();
  else
  {
    dict_sys.unfreeze();
    dict_sys.lock(SRW_LOCK_CALL);
    table= dict_load_table_on_id(table_id, DICT_ERR_IGNORE_FK_NOKEY);
    if (table)
      table->acquire();
    dict_sys.unlock();
    if (!table)
      return nullptr;
    dict_sys.freeze(SRW_LOCK_CALL);
  }

  table= trx_purge_table_acquire(table, mdl_context, mdl);
  dict_sys.unfreeze();
  return table;
}

ATTRIBUTE_COLD
dict_table_t *purge_sys_t::close_and_reopen(table_id_t id, THD *thd,
                                            MDL_ticket **mdl)
{
  MDL_context *mdl_context= static_cast<MDL_context*>(thd_mdl_context(thd));
  ut_ad(mdl_context);
 retry:
  ut_ad(m_active);

  for (que_thr_t *thr= UT_LIST_GET_FIRST(purge_sys.query->thrs); thr;
       thr= UT_LIST_GET_NEXT(thrs, thr))
  {
    purge_node_t *node= static_cast<purge_node_t*>(thr->child);
    trx_purge_close_tables(node, thd);
  }

  m_active= false;
  wait_FTS(false);
  m_active= true;

  dict_table_t *table= trx_purge_table_open(id, mdl_context, mdl);
  if (table == reinterpret_cast<dict_table_t*>(-1))
    goto retry;

  for (que_thr_t *thr= UT_LIST_GET_FIRST(purge_sys.query->thrs); thr;
       thr= UT_LIST_GET_NEXT(thrs, thr))
  {
    purge_node_t *node= static_cast<purge_node_t*>(thr->child);
    for (auto &t : node->tables)
    {
      if (t.second.first)
      {
        t.second.first= trx_purge_table_open(t.first, mdl_context,
                                             &t.second.second);
        if (t.second.first == reinterpret_cast<dict_table_t*>(-1))
        {
          if (table)
            dict_table_close(table, false, thd, *mdl);
          goto retry;
        }
      }
    }
  }

  return table;
}

/** Run a purge batch.
@param n_purge_threads	number of purge threads
@return new purge_sys.head */
static purge_sys_t::iterator
trx_purge_attach_undo_recs(ulint n_purge_threads, THD *thd)
{
	que_thr_t*	thr;
	ulint		i;

	ut_a(n_purge_threads > 0);
	ut_a(UT_LIST_GET_LEN(purge_sys.query->thrs) >= n_purge_threads);

	purge_sys_t::iterator head = purge_sys.tail;

#ifdef UNIV_DEBUG
	i = 0;
	/* Debug code to validate some pre-requisites and reset done flag. */
	for (thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);
	     thr != NULL && i < n_purge_threads;
	     thr = UT_LIST_GET_NEXT(thrs, thr), ++i) {

		purge_node_t*		node;

		/* Get the purge node. */
		node = (purge_node_t*) thr->child;

		ut_ad(que_node_get_type(node) == QUE_NODE_PURGE);
		ut_ad(node->undo_recs.empty());
		ut_ad(!node->in_progress);
		ut_d(node->in_progress = true);
	}

	/* There should never be fewer nodes than threads, the inverse
	however is allowed because we only use purge threads as needed. */
	ut_ad(i == n_purge_threads);
#endif

	/* Fetch and parse the UNDO records. The UNDO records are added
	to a per purge node vector. */
	thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);

	ut_ad(head <= purge_sys.tail);

	i = 0;

	std::unordered_map<table_id_t, purge_node_t*>
		table_id_map(TRX_PURGE_TABLE_BUCKETS);
	purge_sys.m_active = true;

	MDL_context* const mdl_context
		= static_cast<MDL_context*>(thd_mdl_context(thd));
	ut_ad(mdl_context);

	const size_t max_pages = std::min(buf_pool.curr_size * 3 / 4,
					  size_t{srv_purge_batch_size});

	while (UNIV_LIKELY(srv_undo_sources) || !srv_fast_shutdown) {
		/* Track the max {trx_id, undo_no} for truncating the
		UNDO logs once we have purged the records. */

		if (head <= purge_sys.tail) {
			head = purge_sys.tail;
		}

		/* Fetch the next record, and advance the purge_sys.tail. */
		trx_purge_rec_t purge_rec = purge_sys.fetch_next_rec();

		if (!purge_rec.undo_rec) {
			if (!purge_rec.roll_ptr) {
				break;
			}
			ut_ad(purge_rec.roll_ptr == 1);
			continue;
		}

		table_id_t table_id = trx_undo_rec_get_table_id(
			purge_rec.undo_rec);

		purge_node_t*& table_node = table_id_map[table_id];

		if (!table_node) {
			std::pair<dict_table_t*,MDL_ticket*> p;
			p.first = trx_purge_table_open(table_id, mdl_context,
						       &p.second);
			if (p.first == reinterpret_cast<dict_table_t*>(-1)) {
				p.first = purge_sys.close_and_reopen(
					table_id, thd, &p.second);
			}

			thr = UT_LIST_GET_NEXT(thrs, thr);

			if (!(++i % n_purge_threads)) {
				thr = UT_LIST_GET_FIRST(
					purge_sys.query->thrs);
			}

			table_node = static_cast<purge_node_t*>(thr->child);
			ut_a(que_node_get_type(table_node) == QUE_NODE_PURGE);
			ut_d(auto i=)
			table_node->tables.emplace(table_id, p);
			ut_ad(i.second);
			if (p.first) {
				goto enqueue;
			}
		} else if (table_node->tables[table_id].first) {
enqueue:
			table_node->undo_recs.push(purge_rec);
		}

		if (purge_sys.n_pages_handled() >= max_pages) {
			break;
		}
	}

	purge_sys.m_active = false;

	ut_ad(head <= purge_sys.tail);

	return head;
}

extern tpool::waitable_task purge_worker_task;

/** Wait for pending purge jobs to complete. */
static void trx_purge_wait_for_workers_to_complete()
{
  const bool notify_wait{purge_worker_task.is_running()};

  if (notify_wait)
    tpool::tpool_wait_begin();

  purge_worker_task.wait();

  if (notify_wait)
    tpool::tpool_wait_end();

  /* There should be no outstanding tasks as long
  as the worker threads are active. */
  ut_ad(srv_get_task_queue_length() == 0);
}

TRANSACTIONAL_INLINE
void purge_sys_t::batch_cleanup(const purge_sys_t::iterator &head)
{
  /* Release the undo pages. */
  for (auto p : pages)
    p.second->unfix();
  pages.clear();
  pages.reserve(srv_purge_batch_size);

  /* This is only invoked only by the purge coordinator,
  which is the only thread that can modify our inputs head, tail, view.
  Therefore, we only need to protect end_view from concurrent reads. */

  /* Limit the end_view similar to what trx_purge_truncate_history() does. */
  const trx_id_t trx_no= head.trx_no ? head.trx_no : tail.trx_no;
#ifdef SUX_LOCK_GENERIC
  end_latch.wr_lock();
#else
  transactional_lock_guard<srw_spin_lock_low> g(end_latch);
#endif
  this->head= head;
  end_view= view;
  end_view.clamp_low_limit_id(trx_no);
#ifdef SUX_LOCK_GENERIC
  end_latch.wr_unlock();
#endif
}

/**
Run a purge batch.
@param n_tasks       number of purge tasks to submit to the queue
@param history_size  trx_sys.history_size()
@return number of undo log pages handled in the batch */
TRANSACTIONAL_TARGET ulint trx_purge(ulint n_tasks, ulint history_size)
{
	ut_ad(n_tasks > 0);

	purge_sys.clone_oldest_view();

#ifdef UNIV_DEBUG
	if (srv_purge_view_update_only_debug) {
		return(0);
	}
#endif /* UNIV_DEBUG */

	THD* const thd = current_thd;

	/* Fetch the UNDO recs that need to be purged. */
	const purge_sys_t::iterator head
		=  trx_purge_attach_undo_recs(n_tasks, thd);
	const size_t n_pages = purge_sys.n_pages_handled();

	{
		ulint delay = n_pages ? srv_max_purge_lag : 0;
		if (UNIV_UNLIKELY(delay)) {
			if (delay >= history_size) {
		no_throttle:
				delay = 0;
			} else if (const ulint max_delay =
				   srv_max_purge_lag_delay) {
				delay = std::min(max_delay,
						 10000 * history_size / delay
						 - 5000);
			} else {
				goto no_throttle;
			}
		}
		srv_dml_needed_delay = delay;
	}

	que_thr_t* thr = nullptr;

	/* Submit tasks to workers queue if using multi-threaded purge. */
	for (ulint i = n_tasks; --i; ) {
		thr = que_fork_scheduler_round_robin(purge_sys.query, thr);
		ut_a(thr);
		srv_que_task_enqueue_low(thr);
		srv_thread_pool->submit_task(&purge_worker_task);
	}

	thr = que_fork_scheduler_round_robin(purge_sys.query, thr);

	que_run_threads(thr);

	trx_purge_wait_for_workers_to_complete();

	for (thr = UT_LIST_GET_FIRST(purge_sys.query->thrs); thr;
	     thr = UT_LIST_GET_NEXT(thrs, thr)) {
		purge_node_t* node = static_cast<purge_node_t*>(thr->child);
		trx_purge_close_tables(node, thd);
		node->tables.clear();
	}

	purge_sys.batch_cleanup(head);

	MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
	MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages);

	return n_pages;
}
