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
class  pwt_warning_message
{
public:
  uint code;
  Sql_condition::enum_warning_level level;
  const char *message;
};

class  pwt_error_message
{
public:
  uint worker_errno;
  uint nr;
  myf  flags;
  const char *message;
};

class  pwt_data_message
{
public:
  TABLE *tmp_table;
};


/*
  Event type, simple linked list
*/

typedef struct pwt_queued_event_t
{
    pwt_queued_event_t *next;
    enum queued_event_t
    {
      QUEUED_WARNING,
      QUEUED_ERROR,
      QUEDED_DATA
    } type;
    union
    {
      pwt_warning_message *warning;
      pwt_error_message   *error;
      pwt_data_message    *data;
    };
} pwt_queued_event;


/*
  Encapsulation of message queue
*/

struct pwt_messages
{
  pwt_queued_event *event_queue, *last_in_queue;
};

class pwt_management;
/*
  Parallel Worker Thread specific attributes
*/

struct pwt_worker
{
  THD             *thd;
  pwt_management  *manager;
  pthread_t       pthread;
  char            conn_name[MAX_THREAD_NAME+1];
  pwt_messages    *messages;
  bool            joined;
  bool            finished;
};


/*
  Class to create, manage and eventually destroy a "team" of worker threads.
*/
class pwt_management
{
public:
  struct pwt_worker *workers;
  uint              nworkers;
  pwt_messages      parallel_messages;
  mysql_cond_t      COND_pwt_new_message;
  mysql_mutex_t     LOCK_pwt_manager;
  mysql_mutex_t     LOCK_pwt_thread;
  pwt_management()
  {
    workers= nullptr;
    nworkers= 0;
    parallel_messages.event_queue= parallel_messages.last_in_queue = nullptr;
  }
  bool init_parallel_workers(THD *thd);
  void join_parallel_workers(THD *thd);
};

#endif
