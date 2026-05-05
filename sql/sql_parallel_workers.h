#ifndef SQL_PARALLEL_WORKERS_H
#define SQL_PARALLEL_WORKERS_H

#include "mariadb.h"
#include "sql_select.h"
#include "sql_lex.h"
#include "sql_class.h"
#include "sql_error.h"

extern MYSQL_THD create_background_thd();
extern void destroy_background_thd(MYSQL_THD thd);
extern void *thd_attach_thd(MYSQL_THD thd);
extern void thd_detach_thd(void *save);

// PWT Parallel Worker Thread


/*
  Message Types
*/
class pwt_error_message
{
public:
  uint worker_errno;
  uint code;
  Sql_condition::enum_warning_level level;
  char *message;
};

class pwt_data_message
{
public:
  TABLE *tmp_table;
};


/*
  Event type. Inherits ilink so it can live in an I_List<pwt_queued_event>.
*/
class pwt_queued_event : public ilink
{
public:
    pwt_error_message   *error;
    pwt_data_message    *data;
};

class pwt_management;


/*
  Parallel Worker Thread specific attributes
*/
class pwt_worker
{
public:
  THD             *thd;
  pwt_management  *manager;
  pthread_t       pthread;
  mysql_mutex_t   LOCK_worker;
  mysql_cond_t    COND_worker;
  char            conn_name[MAX_THREAD_NAME+1];
  char            info[MAX_THREAD_NAME+22];  // allow for thread id ull20,[ ]\0
  bool            joined;
  bool            finished;
  killed_state    killed;
  void            *parallel_scan_job;
};


/*
  Class to create, manage and eventually destroy a "team" of worker threads.
*/
class pwt_management
{
public:
  pwt_worker        *workers;
  uint              nworkers;
  I_List<pwt_queued_event> parallel_messages;
  mysql_cond_t      COND_pwt_new_message;
  mysql_mutex_t     LOCK_pwt_manager;
  mysql_mutex_t     LOCK_pwt_thread;
  pwt_management():
    workers(nullptr),
    nworkers(0)
    {}
  ~pwt_management()
  {
    if (workers)
      join_parallel_workers(current_thd);
  }
  bool init_parallel_workers(THD *thd);
  void join_parallel_workers(THD *thd);
  void free_queue();
};

#endif
