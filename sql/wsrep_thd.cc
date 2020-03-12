/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#include "wsrep_thd.h"

#include "transaction.h"
#include "rpl_rli.h"
#include "log_event.h"
#include "sql_parse.h"
//#include "global_threads.h" // LOCK_thread_count, etc.
#include "sql_base.h" // close_thread_tables()
#include "mysqld.h"   // start_wsrep_THD();
#include "sql_show.h" // find_thread_by_id

#include "slave.h"    // opt_log_slave_updates
#include "rpl_filter.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "debug_sync.h"
#include <list>

extern std::list< wsrep_kill_t > wsrep_kill_list;

#if (__LP64__)
static volatile int64 wsrep_bf_aborts_counter(0);
#define WSREP_ATOMIC_LOAD_LONG my_atomic_load64
#define WSREP_ATOMIC_ADD_LONG  my_atomic_add64
#else
static volatile int32 wsrep_bf_aborts_counter(0);
#define WSREP_ATOMIC_LOAD_LONG my_atomic_load32
#define WSREP_ATOMIC_ADD_LONG  my_atomic_add32
#endif

int wsrep_show_bf_aborts (THD *thd, SHOW_VAR *var, char *buff,
                          enum enum_var_type scope)
{
  wsrep_local_bf_aborts = WSREP_ATOMIC_LOAD_LONG(&wsrep_bf_aborts_counter);
  var->type = SHOW_LONGLONG;
  var->value = (char*)&wsrep_local_bf_aborts;
  return 0;
}

/* must have (&thd->LOCK_thd_data) */
void wsrep_client_rollback(THD *thd)
{
  WSREP_DEBUG("client rollback due to BF abort for (%ld), query: %s",
              thd->thread_id, thd->query());

  WSREP_ATOMIC_ADD_LONG(&wsrep_bf_aborts_counter, 1);

  thd->wsrep_conflict_state= ABORTING;
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  trans_rollback(thd);

  if (thd->locked_tables_mode && thd->lock)
  {
    WSREP_DEBUG("unlocking tables for BF abort (%ld)", thd->thread_id);
    thd->locked_tables_list.unlock_locked_tables(thd);
    thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
  }

  if (thd->global_read_lock.is_acquired())
  {
    WSREP_DEBUG("unlocking GRL for BF abort (%ld)", thd->thread_id);
    thd->global_read_lock.unlock_global_read_lock(thd);
  }

  /* Release transactional metadata locks. */
  thd->mdl_context.release_transactional_locks();

  /* release explicit MDL locks */
  thd->mdl_context.release_explicit_locks();

  if (thd->get_binlog_table_maps())
  {
    WSREP_DEBUG("clearing binlog table map for BF abort (%ld)", thd->thread_id);
    thd->clear_binlog_table_maps();
  }
  mysql_mutex_lock(&thd->LOCK_thd_data);
  thd->wsrep_conflict_state= ABORTED;
}

#define NUMBER_OF_FIELDS_TO_IDENTIFY_COORDINATOR 1
#define NUMBER_OF_FIELDS_TO_IDENTIFY_WORKER 2

static rpl_group_info* wsrep_relay_group_init(const char* log_fname)
{
  Relay_log_info* rli= new Relay_log_info(false);

  rli->no_storage= true;
  if (!rli->relay_log.description_event_for_exec)
  {
    rli->relay_log.description_event_for_exec=
      new Format_description_log_event(4);
  }

  static LEX_STRING connection_name= { C_STRING_WITH_LEN("wsrep") };

  /*
    Master_info's constructor initializes rpl_filter by either an already
    constructed Rpl_filter object from global 'rpl_filters' list if the
    specified connection name is same, or it constructs a new Rpl_filter
    object and adds it to rpl_filters. This object is later destructed by
    Mater_info's destructor by looking it up based on connection name in
    rpl_filters list.

    However, since all Master_info objects created here would share same
    connection name ("wsrep"), destruction of any of the existing Master_info
    objects (in wsrep_return_from_bf_mode()) would free rpl_filter referenced
    by any/all existing Master_info objects.

    In order to avoid that, we have added a check in Master_info's destructor
    to not free the "wsrep" rpl_filter. It will eventually be freed by
    free_all_rpl_filters() when server terminates.
  */
  rli->mi = new Master_info(&connection_name, false);

  struct rpl_group_info *rgi= new rpl_group_info(rli);
  rgi->thd= rli->sql_driver_thd= current_thd;

  if ((rgi->deferred_events_collecting= rli->mi->rpl_filter->is_on()))
  {
    rgi->deferred_events= new Deferred_log_events(rli);
  }

  return rgi;
}

static void wsrep_prepare_bf_thd(THD *thd, struct wsrep_thd_shadow* shadow)
{
  shadow->options       = thd->variables.option_bits;
  shadow->server_status = thd->server_status;
  shadow->wsrep_exec_mode = thd->wsrep_exec_mode;
  shadow->vio           = thd->net.vio;

  // Disable general logging on applier threads
  thd->variables.option_bits |= OPTION_LOG_OFF;
  // Enable binlogging if opt_log_slave_updates is set
  if (opt_log_slave_updates)
    thd->variables.option_bits|= OPTION_BIN_LOG;
  else
    thd->variables.option_bits&= ~(OPTION_BIN_LOG);

  if (!thd->wsrep_rgi) thd->wsrep_rgi= wsrep_relay_group_init("wsrep_relay");

  /* thd->system_thread_info.rpl_sql_info isn't initialized. */
  thd->system_thread_info.rpl_sql_info=
    new rpl_sql_thread_info(thd->wsrep_rgi->rli->mi->rpl_filter);

  thd->wsrep_exec_mode= REPL_RECV;
  thd->net.vio= 0;
  thd->clear_error();

  shadow->tx_isolation        = thd->variables.tx_isolation;
  thd->variables.tx_isolation = ISO_READ_COMMITTED;
  thd->tx_isolation           = ISO_READ_COMMITTED;

  shadow->db            = thd->db;
  shadow->db_length     = thd->db_length;
  shadow->user_time     = thd->user_time;
  shadow->row_count_func= thd->get_row_count_func();
  thd->reset_db(NULL, 0);
}

static void wsrep_return_from_bf_mode(THD *thd, struct wsrep_thd_shadow* shadow)
{
  thd->variables.option_bits  = shadow->options;
  thd->server_status          = shadow->server_status;
  thd->wsrep_exec_mode        = shadow->wsrep_exec_mode;
  thd->net.vio                = shadow->vio;
  thd->variables.tx_isolation = shadow->tx_isolation;
  thd->user_time              = shadow->user_time;
  thd->reset_db(shadow->db, shadow->db_length);

  delete thd->system_thread_info.rpl_sql_info;
  delete thd->wsrep_rgi->rli->mi;
  delete thd->wsrep_rgi->rli;

  thd->wsrep_rgi->cleanup_after_session();
  delete thd->wsrep_rgi;
  thd->wsrep_rgi = NULL;
  thd->set_row_count_func(shadow->row_count_func);
}

void wsrep_replay_transaction(THD *thd)
{
  DBUG_ENTER("wsrep_replay_transaction");
  /* checking if BF trx must be replayed */
  if (thd->wsrep_conflict_state== MUST_REPLAY) {
    DBUG_ASSERT(wsrep_thd_trx_seqno(thd));
    if (thd->wsrep_exec_mode!= REPL_RECV) {
      if (thd->get_stmt_da()->is_sent())
      {
        WSREP_ERROR("replay issue, thd has reported status already");
      }


      /*
        PS reprepare observer should have been removed already.
        open_table() will fail if we have dangling observer here.
      */
      DBUG_ASSERT(thd->m_reprepare_observer == NULL);

      struct da_shadow
      {
          enum Diagnostics_area::enum_diagnostics_status status;
          ulonglong affected_rows;
          ulonglong last_insert_id;
          char message[MYSQL_ERRMSG_SIZE];
      };
      struct da_shadow da_status;
      da_status.status= thd->get_stmt_da()->status();
      if (da_status.status == Diagnostics_area::DA_OK)
      {
        da_status.affected_rows= thd->get_stmt_da()->affected_rows();
        da_status.last_insert_id= thd->get_stmt_da()->last_insert_id();
        strmake(da_status.message,
                thd->get_stmt_da()->message(),
                sizeof(da_status.message)-1);
      }

      thd->get_stmt_da()->reset_diagnostics_area();

      thd->wsrep_conflict_state= REPLAYING;
      mysql_mutex_unlock(&thd->LOCK_thd_data);

      thd->reset_for_next_command();
      thd->reset_killed();
      close_thread_tables(thd);
      if (thd->locked_tables_mode && thd->lock)
      {
        WSREP_DEBUG("releasing table lock for replaying (%ld)",
                    thd->thread_id);
        thd->locked_tables_list.unlock_locked_tables(thd);
        thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
      }
      thd->mdl_context.release_transactional_locks();
      /*
        Replaying will call MYSQL_START_STATEMENT when handling
        BEGIN Query_log_event so end statement must be called before
        replaying.
      */
      MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
      thd->m_statement_psi= NULL;
      thd->m_digest= NULL;
      thd_proc_info(thd, "wsrep replaying trx");
      WSREP_DEBUG("replay trx: %s %lld",
                  thd->query() ? thd->query() : "void",
                  (long long)wsrep_thd_trx_seqno(thd));
      struct wsrep_thd_shadow shadow;
      wsrep_prepare_bf_thd(thd, &shadow);

      /* From trans_begin() */
      thd->variables.option_bits|= OPTION_BEGIN;
      thd->server_status|= SERVER_STATUS_IN_TRANS;

      int rcode = wsrep->replay_trx(wsrep,
                                    &thd->wsrep_ws_handle,
                                    (void *)thd);

      wsrep_return_from_bf_mode(thd, &shadow);
      if (thd->wsrep_conflict_state!= REPLAYING)
        WSREP_WARN("lost replaying mode: %d", thd->wsrep_conflict_state );

      mysql_mutex_lock(&thd->LOCK_thd_data);

      switch (rcode)
      {
      case WSREP_OK:
        thd->wsrep_conflict_state= NO_CONFLICT;
        wsrep->post_commit(wsrep, &thd->wsrep_ws_handle);
        WSREP_DEBUG("trx_replay successful for: %ld %llu",
                    thd->thread_id, (long long)thd->real_id);
        if (thd->get_stmt_da()->is_sent())
        {
          WSREP_WARN("replay ok, thd has reported status");
        }
        else if (thd->get_stmt_da()->is_set())
        {
          if (thd->get_stmt_da()->status() != Diagnostics_area::DA_OK)
          {
            WSREP_WARN("replay ok, thd has error status %d",
                       thd->get_stmt_da()->status());
          }
        }
        else
        {
          if (da_status.status == Diagnostics_area::DA_OK)
          {
            my_ok(thd,
                  da_status.affected_rows,
                  da_status.last_insert_id,
                  da_status.message);
          }
          else
          {
            my_ok(thd);
          }
        }
        break;
      case WSREP_TRX_FAIL:
        if (thd->get_stmt_da()->is_sent())
        {
          WSREP_ERROR("replay failed, thd has reported status");
        }
        else
        {
          WSREP_DEBUG("replay failed, rolling back");
          //my_error(ER_LOCK_DEADLOCK, MYF(0), "wsrep aborted transaction");
        }
        thd->wsrep_conflict_state= ABORTED;
        wsrep->post_rollback(wsrep, &thd->wsrep_ws_handle);
        break;
      default:
        WSREP_ERROR("trx_replay failed for: %d, schema: %s, query: %s",
                    rcode,
                    (thd->db ? thd->db : "(null)"),
                    thd->query() ? thd->query() : "void");
        /* we're now in inconsistent state, must abort */

        /* http://bazaar.launchpad.net/~codership/codership-mysql/5.6/revision/3962#sql/wsrep_thd.cc */
        mysql_mutex_unlock(&thd->LOCK_thd_data);

        unireg_abort(1);
        break;
      }

      wsrep_cleanup_transaction(thd);

      mysql_mutex_lock(&LOCK_wsrep_replaying);
      wsrep_replaying--;
      WSREP_DEBUG("replaying decreased: %d, thd: %lu",
                  wsrep_replaying, thd->thread_id);
      mysql_cond_broadcast(&COND_wsrep_replaying);
      mysql_mutex_unlock(&LOCK_wsrep_replaying);
    }
  }
  DBUG_VOID_RETURN;
}

static void wsrep_replication_process(THD *thd)
{
  int rcode;
  DBUG_ENTER("wsrep_replication_process");

  struct wsrep_thd_shadow shadow;
  wsrep_prepare_bf_thd(thd, &shadow);

  /* From trans_begin() */
  thd->variables.option_bits|= OPTION_BEGIN;
  thd->server_status|= SERVER_STATUS_IN_TRANS;

  rcode = wsrep->recv(wsrep, (void *)thd);
  DBUG_PRINT("wsrep",("wsrep_repl returned: %d", rcode));

  WSREP_INFO("applier thread exiting (code:%d)", rcode);

  switch (rcode) {
  case WSREP_OK:
  case WSREP_NOT_IMPLEMENTED:
  case WSREP_CONN_FAIL:
    /* provider does not support slave operations / disconnected from group,
     * just close applier thread */
    break;
  case WSREP_NODE_FAIL:
    /* data inconsistency => SST is needed */
    /* Note: we cannot just blindly restart replication here,
     * SST might require server restart if storage engines must be
     * initialized after SST */
    WSREP_ERROR("node consistency compromised, aborting");
    wsrep_kill_mysql(thd);
    break;
  case WSREP_WARNING:
  case WSREP_TRX_FAIL:
  case WSREP_TRX_MISSING:
    /* these suggests a bug in provider code */
    WSREP_WARN("bad return from recv() call: %d", rcode);
    /* Shut down this node. */
    /* fall through */
  case WSREP_FATAL:
    /* Cluster connectivity is lost.
     *
     * If applier was killed on purpose (KILL_CONNECTION), we
     * avoid mysql shutdown. This is because the killer will then handle
     * shutdown processing (or replication restarting)
     */
    if (thd->killed != KILL_CONNECTION)
    {
      wsrep_kill_mysql(thd);
    }
    break;
  }

  mysql_mutex_lock(&LOCK_thread_count);
  wsrep_close_applier(thd);
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  TABLE *tmp;
  while ((tmp = thd->temporary_tables))
  {
    WSREP_WARN("Applier %lu, has temporary tables at exit: %s.%s",
                  thd->thread_id, 
                  (tmp->s) ? tmp->s->db.str : "void",
                  (tmp->s) ? tmp->s->table_name.str : "void");
  }
  wsrep_return_from_bf_mode(thd, &shadow);
  DBUG_VOID_RETURN;
}

static bool create_wsrep_THD(wsrep_thd_processor_fun processor)
{
  ulong old_wsrep_running_threads= wsrep_running_threads;
  pthread_t unused;
  mysql_mutex_lock(&LOCK_thread_count);
  bool res= pthread_create(&unused, &connection_attrib, start_wsrep_THD,
                           (void*)processor);
  /*
    if starting a thread on server startup, wait until the this thread's THD
    is fully initialized (otherwise a THD initialization code might
    try to access a partially initialized server data structure - MDEV-8208).
  */
  if (!mysqld_server_initialized)
    while (old_wsrep_running_threads == wsrep_running_threads)
      mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  return res;
}

void wsrep_create_appliers(long threads)
{
  if (!wsrep_connected)
  {
    /* see wsrep_replication_start() for the logic */
    if (wsrep_cluster_address && strlen(wsrep_cluster_address) &&
        wsrep_provider && strcasecmp(wsrep_provider, "none"))
    {
      WSREP_ERROR("Trying to launch slave threads before creating "
                  "connection at '%s'", wsrep_cluster_address);
      assert(0);
    }
    return;
  }

  long wsrep_threads=0;
  while (wsrep_threads++ < threads) {
    if (create_wsrep_THD(wsrep_replication_process))
      WSREP_WARN("Can't create thread to manage wsrep replication");
  }
}

static void wsrep_rollback_process(THD *thd)
{
  DBUG_ENTER("wsrep_rollback_process");

  mysql_mutex_lock(&LOCK_wsrep_rollback);
  wsrep_aborting_thd= NULL;

  while (thd->killed == NOT_KILLED) {
    thd_proc_info(thd, "wsrep aborter idle");
    thd->mysys_var->current_mutex= &LOCK_wsrep_rollback;
    thd->mysys_var->current_cond=  &COND_wsrep_rollback;

    mysql_cond_wait(&COND_wsrep_rollback,&LOCK_wsrep_rollback);

    WSREP_DEBUG("WSREP rollback thread wakes for signal");

    mysql_mutex_lock(&thd->mysys_var->mutex);
    thd_proc_info(thd, "wsrep aborter active");
    thd->mysys_var->current_mutex= 0;
    thd->mysys_var->current_cond=  0;
    mysql_mutex_unlock(&thd->mysys_var->mutex);

    /* check for false alarms */
    if (!wsrep_aborting_thd)
    {
      WSREP_DEBUG("WSREP rollback thread has empty abort queue");
    }
    /* process all entries in the queue */
    while (wsrep_aborting_thd) {
      THD *aborting;
      wsrep_aborting_thd_t next = wsrep_aborting_thd->next;
      aborting = wsrep_aborting_thd->aborting_thd;
      my_free(wsrep_aborting_thd);
      wsrep_aborting_thd= next;
      /*
       * must release mutex, appliers my want to add more
       * aborting thds in our work queue, while we rollback
       */
      mysql_mutex_unlock(&LOCK_wsrep_rollback);

      mysql_mutex_lock(&aborting->LOCK_thd_data);
      if (aborting->wsrep_conflict_state== ABORTED)
      {
        WSREP_DEBUG("WSREP, thd already aborted: %llu state: %d",
                    (long long)aborting->real_id,
                    aborting->wsrep_conflict_state);

        mysql_mutex_unlock(&aborting->LOCK_thd_data);
        mysql_mutex_lock(&LOCK_wsrep_rollback);
        continue;
      }
      aborting->wsrep_conflict_state= ABORTING;

      mysql_mutex_unlock(&aborting->LOCK_thd_data);

      set_current_thd(aborting);
      aborting->store_globals();

      mysql_mutex_lock(&aborting->LOCK_thd_data);
      wsrep_client_rollback(aborting);
      WSREP_DEBUG("WSREP rollbacker aborted thd: (%lu %llu)",
                  aborting->thread_id, (long long)aborting->real_id);
      mysql_mutex_unlock(&aborting->LOCK_thd_data);

      set_current_thd(thd);
      thd->store_globals();

      mysql_mutex_lock(&LOCK_wsrep_rollback);
    }
  }

  mysql_mutex_unlock(&LOCK_wsrep_rollback);
  sql_print_information("WSREP: rollbacker thread exiting");

  DBUG_PRINT("wsrep",("wsrep rollbacker thread exiting"));
  DBUG_VOID_RETURN;
}

void wsrep_create_rollbacker()
{
  if (wsrep_provider && strcasecmp(wsrep_provider, "none"))
  {
    /* create rollbacker */
    if (create_wsrep_THD(wsrep_rollback_process))
      WSREP_WARN("Can't create thread to manage wsrep rollback");
  }
}

void wsrep_thd_set_PA_safe(void *thd_ptr, my_bool safe)
{ 
  if (thd_ptr) 
  {
    THD* thd = (THD*)thd_ptr;
    thd->wsrep_PA_safe = safe;
  }
}

enum wsrep_conflict_state wsrep_thd_conflict_state(THD *thd, my_bool sync)
{ 
  enum wsrep_conflict_state state = NO_CONFLICT;
  if (thd)
  {
    if (sync) mysql_mutex_lock(&thd->LOCK_thd_data);
    
    state = thd->wsrep_conflict_state;
    if (sync) mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  return state;
}

my_bool wsrep_thd_is_wsrep(THD *thd)
{
  my_bool status = FALSE;
  if (thd)
  {
    status = (WSREP(thd) && WSREP_PROVIDER_EXISTS);
  }
  return status;
}

my_bool wsrep_thd_is_BF(THD *thd, my_bool sync)
{
  my_bool status = FALSE;
  if (thd)
  {
    // THD can be BF only if provider exists
    if (wsrep_thd_is_wsrep(thd))
    {
      if (sync)
	mysql_mutex_lock(&thd->LOCK_thd_data);

      status = ((thd->wsrep_exec_mode == REPL_RECV)    ||
      	        (thd->wsrep_exec_mode == TOTAL_ORDER));
      if (sync)
        mysql_mutex_unlock(&thd->LOCK_thd_data);
    }
  }
  return status;
}

extern "C"
my_bool wsrep_thd_is_BF_or_commit(void *thd_ptr, my_bool sync)
{
  bool status = FALSE;
  if (thd_ptr) 
  {
    THD* thd = (THD*)thd_ptr;
    if (sync) mysql_mutex_lock(&thd->LOCK_thd_data);
    
    status = ((thd->wsrep_exec_mode == REPL_RECV)    ||
	      (thd->wsrep_exec_mode == TOTAL_ORDER)  ||
	      (thd->wsrep_exec_mode == LOCAL_COMMIT));
    if (sync) mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  return status;
}

extern "C"
my_bool wsrep_thd_is_local(void *thd_ptr, my_bool sync)
{
  bool status = FALSE;
  if (thd_ptr) 
  {
    THD* thd = (THD*)thd_ptr;
    if (sync) mysql_mutex_lock(&thd->LOCK_thd_data);

    status = (thd->wsrep_exec_mode == LOCAL_STATE);
    if (sync) mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  return status;
}

int wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr, my_bool signal)
{
  THD *victim_thd = (THD *) victim_thd_ptr;
  THD *bf_thd     = (THD *) bf_thd_ptr;
  DBUG_ENTER("wsrep_abort_thd");

  if ( (WSREP(bf_thd) ||
         ( (WSREP_ON || bf_thd->variables.wsrep_OSU_method == WSREP_OSU_RSU) &&
           bf_thd->wsrep_exec_mode == TOTAL_ORDER) )                         &&
       victim_thd)
  {
    if ((victim_thd->wsrep_conflict_state == MUST_ABORT) ||
        (victim_thd->wsrep_conflict_state == ABORTED) ||
        (victim_thd->wsrep_conflict_state == ABORTING))
    {
      WSREP_DEBUG("wsrep_abort_thd called by %llu with victim %llu already "
                  "aborted. Ignoring.",
                  (bf_thd) ? (long long)bf_thd->real_id : 0,
                  (long long)victim_thd->real_id);
      DBUG_RETURN(1);
    }

    WSREP_DEBUG("wsrep_abort_thd, by: %llu, victim: %llu", (bf_thd) ?
                (long long)bf_thd->real_id : 0, (long long)victim_thd->real_id);
    ha_abort_transaction(bf_thd, victim_thd, signal);
  }
  else
  {
    WSREP_DEBUG("wsrep_abort_thd not effective: %p %p", bf_thd, victim_thd);
  }

  DBUG_RETURN(1);
}

extern "C"
int wsrep_thd_in_locking_session(void *thd_ptr)
{
  if (thd_ptr && ((THD *)thd_ptr)->in_lock_tables) {
    return 1;
  }
  return 0;
}

bool wsrep_thd_has_explicit_locks(THD *thd)
{
  assert(thd);
  return thd->mdl_context.has_explicit_locks();
}

/*
  Get auto increment variables for THD. Use global settings for
  applier threads.
 */
void wsrep_thd_auto_increment_variables(THD* thd,
                                        unsigned long long* offset,
                                        unsigned long long* increment)
{
  if (thd->wsrep_exec_mode == REPL_RECV &&
      thd->wsrep_conflict_state != REPLAYING)
  {
    *offset= global_system_variables.auto_increment_offset;
    *increment= global_system_variables.auto_increment_increment;
  }
  else
  {
    *offset= thd->variables.auto_increment_offset;
    *increment= thd->variables.auto_increment_increment;
  }
}

static void wsrep_abort_slave_trx(long long bf_seqno, long long victim_seqno)
{
  WSREP_ERROR("Trx %lld tries to abort slave trx %lld. This could be "
              "caused by:\n\t"
              "1) unsupported configuration options combination, please check documentation.\n\t"
              "2) a bug in the code.\n\t"
              "3) a database corruption.\n Node consistency compromized, "
              "need to abort. Restart the node to resync with cluster.",
              bf_seqno, victim_seqno);
  abort();
}

static int wsrep_kill(wsrep_kill_t* item)
{
  bool signal= item->signal;
  unsigned long long victim_trx_id= static_cast<unsigned long long>(item->victim_trx_id);
  unsigned long long bf_trx_id= static_cast<unsigned long long>(item->bf_trx_id);

  // Note that find_thread_by_id will acquire LOCK_thd_data mutex
  // for thd if it's found
  THD* bf_thd= find_thread_by_id(item->bf_thd_id, false);

  if (!bf_thd)
  {
    WSREP_ERROR("BF thread: %lu not found", item->bf_thd_id);
    assert(0);
  }

  long long bf_seqno= wsrep_thd_trx_seqno(bf_thd);

  WSREP_DEBUG("Aborter %s trx_id: %llu thread: %ld "
              "seqno: %lld query_state: %s conflict_state: %s query: %s",
              wsrep_thd_is_BF(bf_thd, false) ? "BF" : "normal",
              bf_trx_id,
              item->bf_thd_id,
              bf_seqno,
              wsrep_thd_query_state_str(bf_thd),
              wsrep_thd_conflict_state_str(bf_thd),
              wsrep_thd_query(bf_thd));

  // Note that we need to release LOCK_thd_data mutex from BF thread
  // to obey safe mutex ordering of LOCK_thread_count -> LOCK_thd_data
  // that both are taken on find_thread_by_id
  wsrep_thd_UNLOCK(bf_thd);

  THD* thd= find_thread_by_id(item->victim_thd_id, false);

  if (!thd)
  {
    WSREP_DEBUG("Victim thread: %lu not found", item->victim_thd_id);
    return(0);
  }

  WSREP_LOG_CONFLICT(bf_thd, thd, TRUE);

  unsigned long victim_thread= item->victim_thd_id;
  long long victim_seqno= wsrep_thd_trx_seqno(thd);

  WSREP_DEBUG("Victim %s trx_id: %llu thread: %ld "
              "seqno: %lld query_state: %s  conflict_state: %s query: %s",
              wsrep_thd_is_BF(thd, false) ? "BF" : "normal",
              victim_trx_id,
              victim_thread,
              victim_seqno,
              wsrep_thd_query_state_str(thd),
              wsrep_thd_conflict_state_str(thd),
              wsrep_thd_query(thd));

  if (wsrep_thd_query_state(thd) == QUERY_EXITING)
  {
    WSREP_DEBUG("Victim query state QUERY_EXITING trx: %llu"
                " thread: %lu",
                victim_trx_id,
                victim_thread);
    wsrep_thd_UNLOCK(thd);
    return(0);
  }

  if (wsrep_thd_exec_mode(thd) != LOCAL_STATE)
  {
    WSREP_DEBUG("Victim withdraw of non local for BF trx: %llu "
                 ", thread: %lu exec_mode: %s",
                victim_trx_id,
                victim_thread,
                wsrep_thd_exec_mode_str(thd));
  }

  switch (wsrep_thd_get_conflict_state(thd))
  {
  case NO_CONFLICT:
    WSREP_DEBUG("Victim thread: %lu trx: %llu in NO_CONFLICT state",
                victim_thread,
                victim_trx_id);
    wsrep_thd_set_conflict_state(thd, MUST_ABORT);
    break;
  case MUST_ABORT:
    WSREP_DEBUG("Victim thread: %lu trx: %llu in MUST_ABORT state",
                victim_thread,
                victim_trx_id);
    wsrep_thd_awake(thd, signal);
    return(0);
    break;
  case ABORTED:
  case ABORTING: // fall through
  default:
    WSREP_DEBUG("Victim thread: %lu trx: %llu in state: %s",
                victim_thread,
                victim_trx_id,
                wsrep_thd_conflict_state_str(thd));
    wsrep_thd_UNLOCK(thd);
    return(0);
    break;
  }

  switch (wsrep_thd_query_state(thd))
  {
  case QUERY_COMMITTING:
  {
    enum wsrep_status rcode=WSREP_OK;

    WSREP_DEBUG("Victim kill trx QUERY_COMMITTING state thread: %ld trx: %llu",
                victim_thread,
                victim_trx_id);

    if (wsrep_thd_exec_mode(thd) == REPL_RECV)
    {
      WSREP_DEBUG("Victim REPL_RECV abort slave thread: %ld trx: %llu"
                  " bf_seqno: %lld victim_seqno: %lld",
                  victim_thread,
                  victim_trx_id,
                  bf_seqno,
                  victim_seqno);

      wsrep_abort_slave_trx(bf_seqno, victim_seqno);
    }
    else
    {
      wsrep_t *wsrep= get_wsrep();

      rcode= wsrep->abort_pre_commit(wsrep, bf_seqno,
              (wsrep_trx_id_t)wsrep_thd_ws_handle(thd)->trx_id);

      switch (rcode)
      {
      case WSREP_WARNING:
      {
        WSREP_DEBUG("Victim cancel commit warning thread: %lu trx: %llu",
                    victim_thread,
                    victim_trx_id);

	wsrep_thd_awake(thd, signal);
	return(1);
	break;
      }
      case WSREP_OK:
        break;
      default:
      {
        WSREP_ERROR("Victim cancel commit bad commit exit thread: "
                    "%lu trx: %llu rcode: %d ",
                    victim_thread,
                    victim_trx_id,
                    rcode);
        /* unable to interrupt, must abort */
        /* note: kill_mysql() will block, if we cannot.
         * kill the lock holder first. */
        abort();
        break;
      }
      }
    }

    wsrep_thd_awake(thd, signal);
    break;
  }
  case QUERY_EXEC:
  {
    /* it is possible that victim trx is itself waiting for some
    * other lock. We need to cancel this waiting */
    WSREP_DEBUG("Victim kill trx QUERY_EXEC state thread: %ld trx: %llu",
                victim_thread, victim_trx_id);

    bool wait_lock= item->wait_lock;

    if (wait_lock)
    {
      WSREP_DEBUG("Victim thread: %lu trx: %llu has lock wait flag",
	      victim_thread,
	      victim_trx_id);

      wsrep_thd_awake(thd, signal);
    }
    else
    {
      /* Abort currently executing query */
      WSREP_DEBUG("Kill query for victim thread: %lu trx: %llu",
                  victim_thread,
                  victim_trx_id);

      wsrep_thd_awake(thd, signal);

      /* for BF thd, we need to prevent him from committing */
      if (wsrep_thd_exec_mode(thd) == REPL_RECV)
      {
        WSREP_DEBUG("Victim REPL_RECV abort slave for thread: "
                    "%lu trx: %llu"
                    " bf_seqno: %lld victim_seqno: %lld",
                    victim_thread,
                    victim_trx_id,
                    bf_seqno,
                    victim_seqno);

        wsrep_abort_slave_trx(bf_seqno, victim_seqno);
      }
    }
    break;
  }
  case QUERY_IDLE:
  {
    WSREP_DEBUG("Victim kill trx QUERY_IDLE state thread: %ld trx: %llu",
                victim_thread,
                victim_trx_id);

    if (wsrep_thd_exec_mode(thd) == REPL_RECV)
    {
      WSREP_DEBUG("Victim REPL_RECV kill BF IDLE, thread: %ld trx: "
                  "%llu bf_seqno: %lld victim_seqno: %lld",
                  victim_thread,
                  victim_trx_id,
                  bf_seqno,
                  victim_seqno);

      wsrep_thd_UNLOCK(thd);
      wsrep_abort_slave_trx(bf_seqno, victim_seqno);
      return(0);
    }

    /* This will lock thd from proceeding after net_read() */
    wsrep_thd_set_conflict_state(thd, ABORTING);

    wsrep_lock_rollback();

    if (wsrep_aborting_thd_contains(thd))
    {
      WSREP_WARN("Victim is duplicate thd aborter thread: %ld trx: %llu",
                 victim_thread,
                 victim_trx_id);
    }
    else
    {
      wsrep_aborting_thd_enqueue(thd);
      WSREP_DEBUG("Enqueuing victim thread: %ld trx: %llu for abort",
                  victim_thread,
                  victim_trx_id);
    }

    wsrep_unlock_rollback();
    wsrep_thd_UNLOCK(thd);

    break;
  }
  default:
  {
    WSREP_WARN("Victim thread: %ld trx: %llu in bad wsrep query state: %s",
               victim_thread,
               victim_trx_id,
               wsrep_thd_query_state_str(thd));

    wsrep_thd_UNLOCK(thd);
    break;
  }
  }

  return(0);
}

static void wsrep_process_kill(THD *thd)
{
  DBUG_ENTER("wsrep_process_kill");

  mysql_mutex_lock(&LOCK_wsrep_kill);

  WSREP_DEBUG("WSREP killer thread started");

  while (thd->killed == NOT_KILLED)
  {
    thd_proc_info(thd, "wsrep killer idle");
    thd->mysys_var->current_mutex= &LOCK_wsrep_kill;
    thd->mysys_var->current_cond=  &COND_wsrep_kill;

    mysql_cond_wait(&COND_wsrep_kill,&LOCK_wsrep_kill);

    WSREP_DEBUG("WSREP killer thread wakes for signal");

    mysql_mutex_lock(&thd->mysys_var->mutex);
    thd_proc_info(thd, "wsrep killer active");
    thd->mysys_var->current_mutex= 0;
    thd->mysys_var->current_cond=  0;
    mysql_mutex_unlock(&thd->mysys_var->mutex);

    /* process all entries in the queue */
    while (!wsrep_kill_list.empty())
    {
      wsrep_kill_t to_be_killed= wsrep_kill_list.front();
      // Release list mutex while we kill one thread
      mysql_mutex_unlock(&LOCK_wsrep_kill);
      wsrep_kill(&to_be_killed);
      mysql_mutex_lock(&LOCK_wsrep_kill);
      wsrep_kill_list.pop_front();
    }
  }

  assert(wsrep_kill_list.empty());

  mysql_mutex_unlock(&LOCK_wsrep_kill);
  sql_print_information("WSREP: killer thread exiting");
  DBUG_PRINT("wsrep",("wsrep killer thread exiting"));
  DBUG_VOID_RETURN;
}


void wsrep_create_killer()
{
  if (create_wsrep_THD(wsrep_process_kill)) {
    WSREP_WARN("Can't create thread to manage wsrep background kill");
  }
}

