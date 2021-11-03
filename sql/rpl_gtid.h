/* Copyright (c) 2013, Kristian Nielsen and MariaDB Services Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef RPL_GTID_H
#define RPL_GTID_H

#include "hash.h"
#include "queues.h"
#include <atomic>

/* Definitions for MariaDB global transaction ID (GTID). */


extern const LEX_CSTRING rpl_gtid_slave_state_table_name;

class String;

#define GTID_MAX_STR_LENGTH (10+1+10+1+20)
#define PARAM_GTID(G) G.domain_id, G.server_id, G.seq_no

struct rpl_gtid
{
  uint32 domain_id;
  uint32 server_id;
  uint64 seq_no;
};

inline bool operator==(const rpl_gtid& lhs, const rpl_gtid& rhs)
{
  return
    lhs.domain_id == rhs.domain_id &&
    lhs.server_id == rhs.server_id &&
    lhs.seq_no    == rhs.seq_no;
};

enum enum_gtid_skip_type {
  GTID_SKIP_NOT, GTID_SKIP_STANDALONE, GTID_SKIP_TRANSACTION
};


/*
  Structure to keep track of threads waiting in MASTER_GTID_WAIT().

  Since replication is (mostly) single-threaded, we want to minimise the
  performance impact on that from MASTER_GTID_WAIT(). To achieve this, we
  are careful to keep the common lock between replication threads and
  MASTER_GTID_WAIT threads held for as short as possible. We keep only
  a single thread waiting to be notified by the replication threads; this
  thread then handles all the (potentially heavy) lifting of dealing with
  all current waiting threads.
*/
struct gtid_waiting {
  /* Elements in the hash, basically a priority queue for each domain. */
  struct hash_element {
    QUEUE queue;
    uint32 domain_id;
  };
  /* A priority queue to handle waiters in one domain in seq_no order. */
  struct queue_element {
    uint64 wait_seq_no;
    THD *thd;
    int queue_idx;
    /*
      do_small_wait is true if we have responsibility for ensuring that there
      is a small waiter.
    */
    bool do_small_wait;
    /*
      The flag `done' is set when the wait is completed (either due to reaching
      the position waited for, or due to timeout or kill). The queue_element
      is in the queue if and only if `done' is true.
    */
    bool done;
  };

  mysql_mutex_t LOCK_gtid_waiting;
  HASH hash;

  void init();
  void destroy();
  hash_element *get_entry(uint32 domain_id);
  int wait_for_pos(THD *thd, String *gtid_str, longlong timeout_us);
  void promote_new_waiter(gtid_waiting::hash_element *he);
  int wait_for_gtid(THD *thd, rpl_gtid *wait_gtid, struct timespec *wait_until);
  void process_wait_hash(uint64 wakeup_seq_no, gtid_waiting::hash_element *he);
  int register_in_wait_queue(THD *thd, rpl_gtid *wait_gtid, hash_element *he,
                             queue_element *elem);
  void remove_from_wait_queue(hash_element *he, queue_element *elem);
};


class Relay_log_info;
struct rpl_group_info;
class Gtid_list_log_event;

/*
  Replication slave state.

  For every independent replication stream (identified by domain_id), this
  remembers the last gtid applied on the slave within this domain.

  Since events are always committed in-order within a single domain, this is
  sufficient to maintain the state of the replication slave.
*/
struct rpl_slave_state
{
  /* Elements in the list of GTIDs kept for each domain_id. */
  struct list_element
  {
    struct list_element *next;
    uint64 sub_id;
    uint32 domain_id;
    uint32 server_id;
    uint64 seq_no;
    /*
      hton of mysql.gtid_slave_pos* table used to record this GTID.
      Can be NULL if the gtid table failed to load (eg. missing
      mysql.gtid_slave_pos table following an upgrade).
    */
    void *hton;
  };

  /* Elements in the HASH that hold the state for one domain_id. */
  struct element
  {
    struct list_element *list;
    uint32 domain_id;
    /* Highest seq_no seen so far in this domain. */
    uint64 highest_seq_no;
    /*
      If this is non-NULL, then it is the waiter responsible for the small
      wait in MASTER_GTID_WAIT().
    */
    gtid_waiting::queue_element *gtid_waiter;
    /*
      If gtid_waiter is non-NULL, then this is the seq_no that its
      MASTER_GTID_WAIT() is waiting on. When we reach this seq_no, we need to
      signal the waiter on COND_wait_gtid.
    */
    uint64 min_wait_seq_no;
    mysql_cond_t COND_wait_gtid;

    /*
      For --gtid-ignore-duplicates. The Relay_log_info that currently owns
      this domain, and the number of worker threads that are active in it.

      The idea is that only one of multiple master connections is allowed to
      actively apply events for a given domain. Other connections must either
      discard the events (if the seq_no in GTID shows they have already been
      applied), or wait to see if the current owner will apply it.
    */
    const Relay_log_info *owner_rli;
    uint32 owner_count;
    mysql_cond_t COND_gtid_ignore_duplicates;

    list_element *grab_list() { list_element *l= list; list= NULL; return l; }
    void add(list_element *l)
    {
      l->next= list;
      list= l;
    }
  };

  /* Descriptor for mysql.gtid_slave_posXXX table in specific engine. */
  enum gtid_pos_table_state {
    GTID_POS_AUTO_CREATE,
    GTID_POS_CREATE_REQUESTED,
    GTID_POS_CREATE_IN_PROGRESS,
    GTID_POS_AVAILABLE
  };
  struct gtid_pos_table {
    struct gtid_pos_table *next;
    /*
      Use a void * here, rather than handlerton *, to make explicit that we
      are not using the value to access any functionality in the engine. It
      is just used as an opaque value to identify which engine we are using
      for each GTID row.
    */
    void *table_hton;
    LEX_CSTRING table_name;
    uint8 state;
  };

  /* Mapping from domain_id to its element. */
  HASH hash;
  /* GTIDs added since last purge of old mysql.gtid_slave_pos rows. */
  uint32 pending_gtid_count;
  /* Mutex protecting access to the state. */
  mysql_mutex_t LOCK_slave_state;
  /* Auxiliary buffer to sort gtid list. */
  DYNAMIC_ARRAY gtid_sort_array;

  uint64 last_sub_id;
  /*
    List of tables available for durably storing the slave GTID position.

    Accesses to this table is protected by LOCK_slave_state. However for
    efficiency, there is also a provision for read access to it from a running
    slave without lock.

    An element can be added at the head of a list by storing the new
    gtid_pos_tables pointer atomically with release semantics, to ensure that
    the next pointer of the new element is visible to readers of the new list.
    Other changes (like deleting or replacing elements) must happen only while
    all SQL driver threads are stopped. LOCK_slave_state must be held in any
    case.

    The list can be read without lock by an SQL driver thread or worker thread
    by reading the gtid_pos_tables pointer atomically with acquire semantics,
    to ensure that it will see the correct next pointer of a new head element.
  */
  std::atomic<gtid_pos_table*> gtid_pos_tables;
  /* The default entry in gtid_pos_tables, mysql.gtid_slave_pos. */
  std::atomic<gtid_pos_table*> default_gtid_pos_table;
  bool loaded;

  rpl_slave_state();
  ~rpl_slave_state();

  void truncate_hash();
  ulong count() const { return hash.records; }
  int update(uint32 domain_id, uint32 server_id, uint64 sub_id,
             uint64 seq_no, void *hton, rpl_group_info *rgi);
  int truncate_state_table(THD *thd);
  void select_gtid_pos_table(THD *thd, LEX_CSTRING *out_tablename);
  int record_gtid(THD *thd, const rpl_gtid *gtid, uint64 sub_id,
                  bool in_transaction, bool in_statement, void **out_hton);
  list_element *gtid_grab_pending_delete_list();
  LEX_CSTRING *select_gtid_pos_table(void *hton);
  void gtid_delete_pending(THD *thd, rpl_slave_state::list_element **list_ptr);
  uint64 next_sub_id(uint32 domain_id);
  int iterate(int (*cb)(rpl_gtid *, void *), void *data,
              rpl_gtid *extra_gtids, uint32 num_extra,
              bool sort);
  int tostring(String *dest, rpl_gtid *extra_gtids, uint32 num_extra);
  bool domain_to_gtid(uint32 domain_id, rpl_gtid *out_gtid);
  int load(THD *thd, const char *state_from_master, size_t len, bool reset,
           bool in_statement);
  bool is_empty();

  element *get_element(uint32 domain_id);
  int put_back_list(list_element *list);

  void update_state_hash(uint64 sub_id, rpl_gtid *gtid, void *hton,
                         rpl_group_info *rgi);
  int record_and_update_gtid(THD *thd, struct rpl_group_info *rgi);
  int check_duplicate_gtid(rpl_gtid *gtid, rpl_group_info *rgi);
  void release_domain_owner(rpl_group_info *rgi);
  void set_gtid_pos_tables_list(gtid_pos_table *new_list,
                                gtid_pos_table *default_entry);
  void add_gtid_pos_table(gtid_pos_table *entry);
  struct gtid_pos_table *alloc_gtid_pos_table(LEX_CSTRING *table_name,
      void *hton, rpl_slave_state::gtid_pos_table_state state);
  void free_gtid_pos_tables(struct gtid_pos_table *list);
};


/*
  Binlog state.
  This keeps the last GTID written to the binlog for every distinct
  (domain_id, server_id) pair.
  This will be logged at the start of the next binlog file as a
  Gtid_list_log_event; this way, it is easy to find the binlog file
  containing a given GTID, by simply scanning backwards from the newest
  one until a lower seq_no is found in the Gtid_list_log_event at the
  start of a binlog for the given domain_id and server_id.

  We also remember the last logged GTID for every domain_id. This is used
  to know where to start when a master is changed to a slave. As a side
  effect, it also allows to skip a hash lookup in the very common case of
  logging a new GTID with same server id as last GTID.
*/
struct rpl_binlog_state
{
  struct element {
    uint32 domain_id;
    HASH hash;                /* Containing all server_id for one domain_id */
    /* The most recent entry in the hash. */
    rpl_gtid *last_gtid;
    /* Counter to allocate next seq_no for this domain. */
    uint64 seq_no_counter;

    int update_element(const rpl_gtid *gtid);
  };
  /* Mapping from domain_id to collection of elements. */
  HASH hash;
  /* Mutex protecting access to the state. */
  mysql_mutex_t LOCK_binlog_state;
  my_bool initialized;

  /* Auxiliary buffer to sort gtid list. */
  DYNAMIC_ARRAY gtid_sort_array;

   rpl_binlog_state() :initialized(0) {}
  ~rpl_binlog_state();

  void init();
  void reset_nolock();
  void reset();
  void free();
  bool load(struct rpl_gtid *list, uint32 count);
  bool load(rpl_slave_state *slave_pos);
  int update_nolock(const struct rpl_gtid *gtid, bool strict);
  int update(const struct rpl_gtid *gtid, bool strict);
  int update_with_next_gtid(uint32 domain_id, uint32 server_id,
                             rpl_gtid *gtid);
  int alloc_element_nolock(const rpl_gtid *gtid);
  bool check_strict_sequence(uint32 domain_id, uint32 server_id, uint64 seq_no);
  int bump_seq_no_if_needed(uint32 domain_id, uint64 seq_no);
  int write_to_iocache(IO_CACHE *dest);
  int read_from_iocache(IO_CACHE *src);
  uint32 count();
  int get_gtid_list(rpl_gtid *gtid_list, uint32 list_size);
  int get_most_recent_gtid_list(rpl_gtid **list, uint32 *size);
  bool append_pos(String *str);
  bool append_state(String *str);
  rpl_gtid *find_nolock(uint32 domain_id, uint32 server_id);
  rpl_gtid *find(uint32 domain_id, uint32 server_id);
  rpl_gtid *find_most_recent(uint32 domain_id);
  const char* drop_domain(DYNAMIC_ARRAY *ids, Gtid_list_log_event *glev, char*);
};


/*
  Represent the GTID state that a slave connection to a master requests
  the master to start sending binlog events from.
*/
struct slave_connection_state
{
  struct entry {
    rpl_gtid gtid;
    uint32 flags;
  };
  /* Bits for 'flags' */
  enum start_flags
  {
    START_OWN_SLAVE_POS= 0x1,
    START_ON_EMPTY_DOMAIN= 0x2
  };

  /* Mapping from domain_id to the entry with GTID requested for that domain. */
  HASH hash;

  /* Auxiliary buffer to sort gtid list. */
  DYNAMIC_ARRAY gtid_sort_array;

  slave_connection_state();
  ~slave_connection_state();

  void reset() { my_hash_reset(&hash); }
  int load(const char *slave_request, size_t len);
  int load(const rpl_gtid *gtid_list, uint32 count);
  int load(rpl_slave_state *state, rpl_gtid *extra_gtids, uint32 num_extra);
  rpl_gtid *find(uint32 domain_id);
  entry *find_entry(uint32 domain_id);
  int update(const rpl_gtid *in_gtid);
  void remove(const rpl_gtid *gtid);
  void remove_if_present(const rpl_gtid *in_gtid);
  ulong count() const { return hash.records; }
  int to_string(String *out_str);
  int append_to_string(String *out_str);
  int get_gtid_list(rpl_gtid *gtid_list, uint32 list_size);
  bool is_pos_reached();
};


extern bool rpl_slave_state_tostring_helper(String *dest, const rpl_gtid *gtid,
                                            bool *first);
extern int gtid_check_rpl_slave_state_table(TABLE *table);
extern rpl_gtid *gtid_parse_string_to_list(const char *p, size_t len,
                                           uint32 *out_len);
extern rpl_gtid *gtid_unpack_string_to_list(const char *p, size_t len,
                                           uint32 *out_len);
extern void set_rpl_gtid(rpl_gtid *out, uint32 domain_id, uint32 server_id,
                         uint64 seq_no);

/*
  Interface to support different methods of filtering log events by GTID
*/
class Gtid_event_filter
{
public:
  Gtid_event_filter() {};
  virtual ~Gtid_event_filter() {};

  enum gtid_event_filter_type
  {
    DELEGATING_GTID_FILTER_TYPE = 1,
    WINDOW_GTID_FILTER_TYPE = 2,
    ACCEPT_ALL_GTID_FILTER_TYPE = 3,
    REJECT_ALL_GTID_FILTER_TYPE = 4,
    INTERSECTING_GTID_FILTER_TYPE = 5
  };

  /*
    Run the filter on an input gtid to test if the corresponding log events
    should be excluded from a result

    Returns TRUE when the event group corresponding to the input GTID should be
    excluded.
    Returns FALSE when the event group should be included.
  */
  virtual my_bool exclude(rpl_gtid *) = 0;

  /*
    The gtid_event_filter_type that corresponds to the underlying filter
    implementation
  */
  virtual uint32 get_filter_type() = 0;

  /*
    For filters that can maintain their own state, this tests if the filter
    implementation has completed.

    Returns TRUE when completed, and FALSE when the filter has not finished.
  */
  virtual my_bool has_finished() = 0;

  /*
    If any non-fatal issues occurred during filtering, to not pollute the
    output with warnings, we wait until after processing to write them.
  */
  virtual void write_warnings(FILE *out) = 0;
};

/*
  Filter implementation which will include any and all input GTIDs. This is
  used to set default behavior for GTIDs that do not have explicit filters
  set on their domain_id, e.g. when a Window_gtid_event_filter is used for
  a specific domain, then all other domain_ids will be accepted using this
  filter implementation.
*/
class Accept_all_gtid_filter : public Gtid_event_filter
{
public:
  Accept_all_gtid_filter() {}
  ~Accept_all_gtid_filter() {}
  my_bool exclude(rpl_gtid *gtid) { return FALSE; }
  uint32 get_filter_type() { return ACCEPT_ALL_GTID_FILTER_TYPE; }
  my_bool has_finished() { return FALSE; }
  void write_warnings(FILE *out) {}
};

/*
  TODO
*/
class Reject_all_gtid_filter : public Gtid_event_filter
{
public:
  Reject_all_gtid_filter() {}
  ~Reject_all_gtid_filter() {}
  my_bool exclude(rpl_gtid *gtid) { return TRUE; }
  uint32 get_filter_type() { return REJECT_ALL_GTID_FILTER_TYPE; }
  my_bool has_finished() { return FALSE; }
  void write_warnings(FILE *out) {}
};

/*
  A filter implementation that passes through events between two GTIDs, m_start
  (exclusive) and m_stop (inclusive).

  This filter is stateful, such that it expects GTIDs to be a sequential
  stream, and internally, the window will activate/deactivate when the start
  and stop positions of the event stream have passed through, respectively.

  Window activation is used to permit events from the same domain id which fall
  in-between m_start and m_stop, but are not from the same server id. For
  example, consider the following event stream with GTIDs 0-1-1,0-2-1,0-1-2.
  With m_start as 0-1-0 and m_stop as 0-1-2, we want 0-2-1 to be included in
  this filter. Therefore, the window activates upon seeing 0-1-1, and allows
  any GTIDs within this domain to pass through until 0-1-2 has been
  encountered.
*/
class Window_gtid_event_filter : public Gtid_event_filter
{
public:
  Window_gtid_event_filter(my_bool *is_gtid_strict_mode);
  ~Window_gtid_event_filter() {}

  my_bool exclude(rpl_gtid*);
  my_bool has_finished();
  void write_warnings(FILE *out);

  /*
    Set the GTID that begins this window (exclusive)

    Returns 0 on ok, non-zero on error
  */
  int set_start_gtid(rpl_gtid *start);

  /*
    Set the GTID that ends this window (inclusive)

    Returns 0 on ok, non-zero on error
  */
  int set_stop_gtid(rpl_gtid *stop);

  uint32 get_filter_type() { return WINDOW_GTID_FILTER_TYPE; }


  /*
    Getter/setter methods
  */
  my_bool has_start() { return m_has_start; }
  my_bool has_stop() { return m_has_stop; }
  rpl_gtid get_start_gtid() { return m_start; }
  rpl_gtid get_stop_gtid() { return m_stop; }

  void clear_start_pos()
  {
    m_has_start= FALSE;
    set_rpl_gtid(&m_start, 0, 0, 0);
  }

  void clear_stop_pos()
  {
    m_has_stop= FALSE;
    set_rpl_gtid(&m_stop, 0, 0, 0);
  }

protected:

  /*
    When processing GTID streams, the order in which they are processed should
    be sequential with no gaps between events. If a gap is found within a
    window, warn the user.
  */
  void verify_gtid_is_expected(rpl_gtid *gtid);

private:

  enum warning_flags
  {
    WARN_GTID_SEQUENCE_NUMBER_OUT_OF_ORDER= 0x1
  };

  /*
    m_has_start : Indicates if a start to this window has been explicitly
                  provided. A window starts immediately if not provided.
  */
  my_bool m_has_start;

  /*
    m_has_stop : Indicates if a stop to this window has been explicitly
                 provided. A window continues indefinitely if not provided.
  */
  my_bool m_has_stop;

  /*
    m_is_active : Indicates whether or not the program is currently reading
                  events from within this window. When TRUE, events with
                  different server ids than those specified by m_start or
                  m_stop will be passed through.
  */
  my_bool m_is_active;

  /*
    m_has_passed : Indicates whether or not the program is currently reading
                   events from within this window.
   */
  my_bool m_has_passed;

  /* m_start : marks the GTID that begins the window (exclusive). */
  rpl_gtid m_start;

  /* m_stop : marks the GTID that ends the range (inclusive). */
  rpl_gtid m_stop;

  /* last_gtid_seen: saves the last  */
  rpl_gtid last_gtid_seen;

  /*
    warning_flags: holds flags for any non-fatal issues encountered during
                   filtering
  */
  uint32 m_warning_flags;

  /*
    is_gtid_strict_mode: presents additional warnings in strict mode. This
                         points to some controller boolean which determines
                         whether or not gtid_strict_mode is enabled or not.
  */
  my_bool *m_is_gtid_strict_mode;
};

/*
  Data structure to help with quick lookup for filters. More specifically,
  if two filters have identifiers that lead to the same hash, they will be
  put into a linked list.
*/
typedef uint32 gtid_filter_identifier;
typedef struct _gtid_filter_element
{
  Gtid_event_filter *filter;
  gtid_filter_identifier identifier; /* Used for HASH lookup */
  struct _gtid_filter_element *next;
} gtid_filter_element;

/*
  Gtid_event_filter subclass which has no specific implementation, but rather
  delegates the filtering to specific identifiable/mapped implementations.

  A default filter is used for GTIDs that are passed through which no explicit
  filter can be identified.

  This class should be subclassed, where the get_id_from_gtid function
  specifies how to extract the filter identifier from a GTID.
*/
class Id_delegating_gtid_event_filter : public Gtid_event_filter
{
public:
  Id_delegating_gtid_event_filter();
  ~Id_delegating_gtid_event_filter();

  my_bool exclude(rpl_gtid *gtid);
  my_bool has_finished();
  void write_warnings(FILE *out);
  void set_default_filter(Gtid_event_filter *default_filter);

  uint32 get_filter_type() { return DELEGATING_GTID_FILTER_TYPE; }

  virtual gtid_filter_identifier get_id_from_gtid(rpl_gtid *) = 0;

  /*
    Set the default behavior to include all ids except for the ones that are
    provided in the input list or overridden with another filter.
    Returns 0 on ok, non-zero on error
  */
  int set_blacklist(gtid_filter_identifier *id_list, size_t n_ids);

  /*
    Set the default behavior to exclude all ids except for the ones that are
    provided in the input list or overridden with another filter.
    Returns 0 on ok, non-zero on error
  */
  int set_whitelist(gtid_filter_identifier *id_list, size_t n_ids);

protected:

  uint32 m_num_explicit_filters;
  uint32 m_num_completed_filters;
  Gtid_event_filter *m_default_filter;

  HASH m_filters_by_id_hash;

  my_bool m_whitelist_set, m_blacklist_set;

  gtid_filter_element *find_or_create_filter_element_for_id(gtid_filter_identifier);
};

/*
  A subclass of Id_delegating_gtid_event_filter which identifies filters using the
  domain id of a GTID.

  Additional helper functions include:
    add_start_gtid(GTID)   : adds a start GTID position to this filter, to be
                             identified by its domain id
    add_stop_gtid(GTID)    : adds a stop GTID position to this filter, to be
                             identified by its domain id
    clear_start_gtids()    : removes existing GTID start positions
    clear_stop_gtids()     : removes existing GTID stop positions
    get_start_gtids()      : gets all added GTID start positions
    get_stop_gtids()       : gets all added GTID stop positions
    get_num_start_gtids()  : gets the count of added GTID start positions
    get_num_stop_gtids()   : gets the count of added GTID stop positions
*/
class Domain_gtid_event_filter : public Id_delegating_gtid_event_filter
{
public:
  Domain_gtid_event_filter()
      : m_is_gtid_strict_mode(0)
  {
    my_init_dynamic_array(PSI_INSTRUMENT_ME, &m_start_filters,
                          sizeof(gtid_filter_element), 8, 8, MYF(0));
    my_init_dynamic_array(PSI_INSTRUMENT_ME, &m_stop_filters,
                          sizeof(gtid_filter_element), 8, 8, MYF(0));
  }
  ~Domain_gtid_event_filter()
  {
    delete_dynamic(&m_start_filters);
    delete_dynamic(&m_stop_filters);
  }

  /*
    Returns the domain id of from the input GTID
  */
  gtid_filter_identifier get_id_from_gtid(rpl_gtid *gtid)
  {
    return gtid->domain_id;
  }

  /*
    Helper function to start a GTID window filter at the given GTID

    Returns 0 on ok, non-zero on error
  */
  int add_start_gtid(rpl_gtid *gtid);

  /*
    Helper function to end a GTID window filter at the given GTID

    Returns 0 on ok, non-zero on error
  */
  int add_stop_gtid(rpl_gtid *gtid);

  /*
    If start or stop position is respecified, we remove all existing values
    and start over with the new specification.
  */
  void clear_start_gtids();
  void clear_stop_gtids();

  /*
    Return list of all GTIDs used as start position.

    Note that this list is allocated and it is up to the user to free it
  */
  rpl_gtid *get_start_gtids();

  /*
    Return list of all GTIDs used as stop position.

    Note that this list is allocated and it is up to the user to free it
  */
  rpl_gtid *get_stop_gtids();

  size_t get_num_start_gtids() { return m_start_filters.elements; }
  size_t get_num_stop_gtids() { return m_stop_filters.elements; }

  /*
    Enable or disable gtid_strict_mode for GTID sequence number processing.
  */
  void set_gtid_strict_mode(my_bool gtid_strict_mode_arg)
  {
    m_is_gtid_strict_mode= gtid_strict_mode_arg;
  }

private:
  DYNAMIC_ARRAY m_start_filters;
  DYNAMIC_ARRAY m_stop_filters;

  /*
    This controls whether gtid_strict_mode is enabled or disabled for all
    child filters, e.g. of type Window_gtid_event_filter. More specifically,
    they point to this variable, so when it changes, the behavior of all
    children using this value changes.
  */
  my_bool m_is_gtid_strict_mode;

  Window_gtid_event_filter *
      find_or_create_window_filter_for_id(gtid_filter_identifier);
};

/*
  A subclass of Id_delegating_gtid_event_filter which identifies filters using the
  server id of a GTID.
*/
class Server_gtid_event_filter : public Id_delegating_gtid_event_filter
{
public:
  /*
    Returns the server id of from the input GTID
  */
  gtid_filter_identifier get_id_from_gtid(rpl_gtid *gtid)
  {
    return gtid->server_id;
  }
};

/*
  A Gtid_event_filter implementation that delegates the filtering to two
  other filters, where the result is the intersection between the two.
*/
class Intersecting_gtid_event_filter : public Gtid_event_filter
{
public:
  Intersecting_gtid_event_filter(Gtid_event_filter *filter1,
                                 Gtid_event_filter *filter2)
      : m_filter1(filter1), m_filter2(filter2) {}
  ~Intersecting_gtid_event_filter()
  {
    delete m_filter1;
    delete m_filter2;
  }

  /*
    Returns TRUE if either m_filter1 or m_filter1 exclude the gtid, returns
    FALSE otherwise, i.e. both m_filter1 and m_filter2 allow the gtid
  */
  my_bool exclude(rpl_gtid *gtid);
  uint32 get_filter_type() { return INTERSECTING_GTID_FILTER_TYPE; }

  Gtid_event_filter *get_filter_1() { return m_filter1; }
  Gtid_event_filter *get_filter_2() { return m_filter2; }

  my_bool has_finished()
  {
    DBUG_ASSERT(m_filter1 && m_filter2);
    return m_filter1->has_finished() && m_filter2->has_finished();
  }

  void write_warnings(FILE *out)
  {
    DBUG_ASSERT(m_filter1 && m_filter2);
    m_filter1->write_warnings(out);
    m_filter2->write_warnings(out);
  }

  protected:
    Gtid_event_filter *m_filter1;
    Gtid_event_filter *m_filter2;
};

#endif  /* RPL_GTID_H */
