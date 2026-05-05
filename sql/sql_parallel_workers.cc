/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

/**
  @file

    Contains
*/


#include "sql_parallel_workers.h"


bool Sql_condition_to_queue( THD *thd, pwt_queued_event **event,
                             const Sql_condition_identity *value,
                             const char *msg )
{
  *event= (pwt_queued_event*) my_malloc(PSI_INSTRUMENT_ME,
                                           sizeof(pwt_queued_event),
                                           MYF(0));
  if (!*event)
    return false;
  (*event)->next= nullptr;
  (*event)->type= pwt_queued_event::queued_event_t::QUEUED_WARNING;
  (*event)->warning= (pwt_warning_message*) my_malloc(PSI_INSTRUMENT_ME,
                                               sizeof(pwt_warning_message),
                                               MYF(0));
  if (!(*event)->warning)
    return false;
  (*event)->warning->code= value->get_sql_errno();
  (*event)->warning->level= value->get_level();
  (*event)->warning->message= msg;
  return true;
}


bool error_to_queue( THD *thd, pwt_queued_event **event, uint error,
                     const char *msg )
{
  *event= (pwt_queued_event*) my_malloc(PSI_INSTRUMENT_ME,
                                           sizeof(pwt_queued_event),
                                           MYF(0));
  if (!*event)
    return false;
  (*event)->next= nullptr;
  (*event)->type= pwt_queued_event::queued_event_t::QUEUED_ERROR;
  (*event)->error= (pwt_error_message*) my_malloc(PSI_INSTRUMENT_ME,
                                               sizeof(pwt_error_message),
                                               MYF(0));
  if (!(*event)->error)
    return false;
  (*event)->warning->code= error;
  (*event)->warning->message= msg;
  return true;
}


class PWT_error_handler : public Internal_error_handler
{
public:
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sql_state,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl) override
  {
    if (struct pwt_worker *worker= thd->pwt_worker_info)
    {
      // add an event to our queue
      mysql_mutex_lock(&worker->manager->LOCK_pwt_thread);
      if (worker->messages->event_queue)
      {
        pwt_queued_event *last= worker->messages->event_queue;

        while( last->next )
          last= last->next;

        if (!error_to_queue(thd, &last, sql_errno, msg))
          return false;
        worker->messages->last_in_queue= last->next;
      }
      else
      {
        if (!error_to_queue(thd, &worker->messages->event_queue, sql_errno,
                            msg))
          return false;
        worker->messages->last_in_queue= worker->messages->event_queue;
      }
      mysql_mutex_unlock(&worker->manager->LOCK_pwt_thread);
    }
    return true;
  }

};

static void *parallel_worker_thread_func(void *arg)
{
  struct pwt_worker *worker= (struct pwt_worker*) arg;
  struct timespec abs_timeout;
  PSI_stage_info old_stage;

  PWT_error_handler error_handler;
  bzero((void*)&abs_timeout, sizeof(abs_timeout));
  abs_timeout.tv_sec= time(0)+10;                               // wait 10s
  /*
    Set current_thd and thread local storage (my_thread_var) for our new THD
    to ensure they have their own local objects/errors/warnings etc
  */
  void *save= thd_attach_thd(worker->thd);
  my_thread_set_name(worker->thd->connection_name.str);
  THD_STAGE_INFO(worker->thd, stage_sending_data);
  worker->thd->push_internal_handler(&error_handler);

  // work happens here

  // this is a wait, can be interrupted by the manager
  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  worker->thd->ENTER_COND(&worker->thd->COND_wakeup_ready,
                          &worker->thd->LOCK_thd_kill,
                          &stage_sending_data, &old_stage);
  mysql_cond_timedwait(&worker->thd->COND_wakeup_ready,
                       &worker->thd->LOCK_thd_kill, &abs_timeout);
  worker->thd->EXIT_COND(&old_stage);

  if (worker->thd->killed)
  {
    worker->finished= true;
    worker->thd->pop_internal_handler();
    thd_detach_thd(save);
    server_threads.erase(worker->thd);
    destroy_background_thd(worker->thd);
    return nullptr;
  }

#if 0
  push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This is an example warning to show we can push a "
                 "warning from a worker thread to it's manager ");
#else

  my_error(ER_ARGUMENT_OUT_OF_RANGE, MYF(0), "This is an example error" );

#endif

  // signal manager there is something in the queue, with actually doing
  // anything it will be only a warning or an error
  mysql_cond_signal(&worker->manager->COND_pwt_new_message);
  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  mysql_cond_timedwait(&worker->thd->COND_wakeup_ready,
                       &worker->thd->LOCK_thd_kill, &abs_timeout);
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);

  worker->finished= true;

  // signal manager again to wake up and end this thread
  mysql_cond_signal(&worker->manager->COND_pwt_new_message);

  worker->thd->pop_internal_handler();
  // restore saved state
  thd_detach_thd(save);
  server_threads.erase(worker->thd);
  destroy_background_thd(worker->thd);
  return nullptr;
}

#define WORKER_NAME "parallel worker"

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_pwt;

static PSI_thread_info all_pwt_threads[]=
{
  { &key_thread_pwt, WORKER_NAME, PSI_FLAG_GLOBAL},
};
#endif /* HAVE_PSI_INTERFACE */


bool pwt_management::init_parallel_workers(THD *thd)
{
  if (const uint n= thd->variables.parallel_worker_threads)
  {
    workers= (struct pwt_worker *) my_malloc(PSI_INSTRUMENT_ME,
                                     n * sizeof(struct pwt_worker),
                                     MYF(0));
    if (!workers)
      return false;
    nworkers= n;

    mysql_mutex_init(PSI_INSTRUMENT_ME, &LOCK_pwt_manager, MY_MUTEX_INIT_SLOW);
    mysql_mutex_init(PSI_INSTRUMENT_ME, &LOCK_pwt_thread, MY_MUTEX_INIT_SLOW);
    mysql_cond_init(key_COND_parallel_entry, &COND_pwt_new_message, NULL);
    for (uint i= 0; i < n; i++)
    {
      workers[i].thd= create_background_thd();
      workers[i].manager= this;
      workers[i].messages= &parallel_messages;
      workers[i].thd->system_thread= SYSTEM_THREAD_GENERIC;
      size_t len= my_snprintf(workers[i].conn_name, MAX_THREAD_NAME,
                              WORKER_NAME);
      workers[i].thd->connection_name.str= workers[i].conn_name;
      workers[i].thd->connection_name.length= len;
      workers[i].thd->security_ctx= thd->security_ctx;
      workers[i].thd->set_command(thd->get_command());
      // explicit call to my_free in THD::free_connection(), so we do this
      workers[i].thd->db.str= (char*)my_malloc(PSI_INSTRUMENT_ME,
                                               thd->db.length+1,
                                               MYF(0));
      strncpy(const_cast<char*>(workers[i].thd->db.str), thd->db.str,
              thd->db.length);
      workers[i].thd->db.length= thd->db.length;
      workers[i].thd->proc_info= thd->proc_info;
      workers[i].thd->start_utime= thd->start_utime;
//      workers[i].thd->thread_id= thd->thread_id;
      workers[i].thd->query_string= thd->query_string;
      workers[i].thd->pwt_worker_info= workers+i;

      workers[i].finished= workers[i].joined= false;

#ifdef HAVE_PSI_INTERFACE
      if (PSI_server)
        PSI_server->register_thread("sql", all_pwt_threads,
                                    array_elements(all_pwt_threads));
#endif
      server_threads.insert(workers[i].thd);               // show processlist

      if (mysql_thread_create(key_thread_pwt, &workers[i].pthread, nullptr,
                              parallel_worker_thread_func, &workers[i]))
      {
        server_threads.erase(workers[i].thd);
        destroy_background_thd(workers[i].thd);
        for (uint j= 0; j < i; j++)
        {
          destroy_background_thd(workers[j].thd);
          pthread_join(workers[j].pthread, nullptr);
        }
        workers= nullptr;
        return false;
      }
    }
  }
  return true;
}


void pwt_management::join_parallel_workers(THD *thd)
{
  bool all_done= false, workers_killed= false;;
  PSI_stage_info old_stage;

  while (!all_done)
  {
    all_done= true;

    // delete worker threads that are finished
    for (uint i= 0; i < nworkers; i++)
    {
      if (workers[i].finished)
      {
        if (!workers[i].joined)
        {
          pthread_join(workers[i].pthread, nullptr);
          workers[i].joined= true;
        }
      }
      else
        all_done= false;
    }

    if (thd->killed && workers_killed)            // loop until our workers exit
      continue;

    mysql_mutex_lock(&LOCK_pwt_manager);
    thd->ENTER_COND(&COND_pwt_new_message, &LOCK_pwt_manager,
                     &stage_waiting_for_work_from_sql_thread, &old_stage);

    if (!thd->killed && !all_done)
      mysql_cond_wait(&COND_pwt_new_message, &LOCK_pwt_manager);

    thd->EXIT_COND(&old_stage);

    if (thd->killed)
    {
      // inform our workers
      for (uint i= 0; i < nworkers; i++)
      {
        workers[i].thd->killed= thd->killed;
        mysql_mutex_lock(&workers[i].thd->LOCK_thd_kill);
        mysql_cond_signal(&workers[i].thd->COND_wakeup_ready);
        mysql_mutex_unlock(&workers[i].thd->LOCK_thd_kill);
      }
      workers_killed= true;
    }
    else
    {
      // process queue
      mysql_mutex_lock(&LOCK_pwt_thread);
      while(parallel_messages.event_queue)
      {
        switch(parallel_messages.event_queue->type)
        {
          case pwt_queued_event::QUEUED_ERROR:
            thd->get_stmt_da()->set_overwrite_status(true);
            my_error(parallel_messages.event_queue->error->nr, MYF(0),
                     parallel_messages.event_queue->error->message);
            my_free(parallel_messages.event_queue->error);
            break;
          case pwt_queued_event::QUEUED_WARNING:
            push_warning(thd, parallel_messages.event_queue->warning->level,
                         parallel_messages.event_queue->warning->code,
                         parallel_messages.event_queue->warning->message);
            my_free(parallel_messages.event_queue->warning);
            break;
          case pwt_queued_event::QUEDED_DATA:
            break;
        }
        pwt_queued_event *last= parallel_messages.event_queue;
        parallel_messages.event_queue= parallel_messages.event_queue->next;
        my_free(last);
      }
      mysql_mutex_unlock(&LOCK_pwt_thread);
    }
  }

  mysql_cond_destroy(&COND_pwt_new_message);
  mysql_mutex_destroy(&LOCK_pwt_manager);
  mysql_mutex_destroy(&LOCK_pwt_thread);
  if (nworkers)
    my_free(workers);
}

