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


bool error_to_queue( THD *thd, pwt_queued_event **event, uint error,
                     Sql_condition::enum_warning_level level, const char *msg )
{
  *event= (pwt_queued_event*) my_malloc(PSI_INSTRUMENT_ME,
                                           sizeof(pwt_queued_event),
                                           MYF(0));
  if (!*event)
    return false;
  (*event)->error= (pwt_error_message*) my_malloc(PSI_INSTRUMENT_ME,
                                               sizeof(pwt_error_message),
                                               MYF(0));
  if (!(*event)->error)
  {
    my_free(*event);
    return false;
  }
  (*event)->data= nullptr;
  (*event)->error->level= level;
  if (level == Sql_condition::enum_warning_level::WARN_LEVEL_ERROR)
    (*event)->error->worker_errno= thd->killed_errno();
  (*event)->error->code= error;
  (*event)->error->message= (char *) my_malloc(PSI_INSTRUMENT_ME, strlen(msg)+1,
                                               MYF(0));
  if (!(*event)->error->message)
  {
    my_free((*event)->error);
    my_free(*event);
    return false;
  }
  strmake((*event)->error->message, msg, strlen(msg));
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
      pwt_queued_event *event;
      if (!error_to_queue(thd, &event, sql_errno, *level, msg))
          return false;
      mysql_mutex_lock(&worker->manager->LOCK_pwt_thread);
      worker->manager->parallel_messages.push_back(event);
      mysql_mutex_unlock(&worker->manager->LOCK_pwt_thread);
    }
    return true;                // no further processing in worker thread
  }

};

static void *parallel_worker_thread_func(void *arg)
{
  struct pwt_worker *worker= (struct pwt_worker*) arg;
  struct timespec abs_timeout;
  PSI_stage_info old_stage;

  PWT_error_handler error_handler;
  abs_timeout.tv_nsec= 0;
  /*
    Set current_thd and thread local storage (my_thread_var) for our new THD
    to ensure they have their own local objects/errors/warnings etc
  */
  void *save= thd_attach_thd(worker->thd);
  my_thread_set_name(worker->thd->connection_name.str);
  THD_STAGE_INFO(worker->thd, stage_sending_data);
  worker->thd->push_internal_handler(&error_handler);

  /*
    START: in lieu of work, wait 10 seconds, push out an error or a warning,
    wait another 10 seconds then exit
  */
  abs_timeout.tv_sec= time(0)+10;
  mysql_mutex_lock(&worker->LOCK_worker);
  worker->thd->ENTER_COND(&worker->COND_worker, &worker->LOCK_worker,
                          &stage_sending_data, &old_stage);
  mysql_cond_timedwait(&worker->COND_worker, &worker->LOCK_worker,
                       &abs_timeout);
  worker->thd->EXIT_COND(&old_stage);

  if (worker->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    goto worker_thread_exit;
  }

  if ((uint)worker->thd->thread_id%2)
    push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This is an example warning to show we can push a "
                 "warning from a worker thread to it's manager ");
  else
    my_error(ER_ARGUMENT_OUT_OF_RANGE, MYF(0), "worker_busted_function()" );

  // signal manager there is something in the queue,
  mysql_cond_signal(&worker->manager->COND_pwt_new_message);

  abs_timeout.tv_sec= time(0)+10;
  mysql_mutex_lock(&worker->LOCK_worker);
  worker->thd->ENTER_COND(&worker->COND_worker, &worker->LOCK_worker,
                          &stage_sending_data, &old_stage);
  mysql_cond_timedwait(&worker->COND_worker, &worker->LOCK_worker,
                       &abs_timeout);
  worker->thd->EXIT_COND(&old_stage);

  if (worker->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
  }

  // END: in lieu of work

worker_thread_exit:

  worker->finished= true;
  worker->killed= worker->thd->killed;

  // signal manager again to wake up and end this thread
  mysql_cond_signal(&worker->manager->COND_pwt_new_message);

  worker->thd->pop_internal_handler();

  // restore saved state
  thd_detach_thd(save);
  server_threads.erase(worker->thd);
  destroy_background_thd(worker->thd);
  worker->thd= nullptr;
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


void abort_worker(struct pwt_worker *worker)
{
  worker->thd->killed= ABORT_QUERY;
  mysql_mutex_lock(&worker->LOCK_worker);
  mysql_cond_signal(&worker->COND_worker);
  mysql_mutex_unlock(&worker->LOCK_worker);
  pthread_join(worker->pthread, nullptr);
  mysql_mutex_destroy(&worker->LOCK_worker);
  mysql_cond_destroy(&worker->COND_worker);
}


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
    mysql_cond_init(key_COND_pwt_new_message, &COND_pwt_new_message, NULL);
    for (uint i= 0; i < n; i++)
    {
      workers[i].thd= create_background_thd();
      if (!workers[i].thd)
      {
        for (uint j= 0; j < i; j++)
          abort_worker(workers+j);
        my_free(workers);
        workers= nullptr;
        nworkers= 0;
        mysql_cond_destroy(&COND_pwt_new_message);
        mysql_mutex_destroy(&LOCK_pwt_manager);
        mysql_mutex_destroy(&LOCK_pwt_thread);
        return false;
      }

      workers[i].manager= this;
      mysql_mutex_init(PSI_INSTRUMENT_ME, &workers[i].LOCK_worker,
                       MY_MUTEX_INIT_FAST);
      mysql_cond_init(PSI_INSTRUMENT_ME, &workers[i].COND_worker, nullptr);
      workers[i].thd->system_thread= SYSTEM_THREAD_GENERIC;
      size_t len= my_snprintf(workers[i].conn_name, MAX_THREAD_NAME,
                              WORKER_NAME);
      workers[i].thd->connection_name.str= workers[i].conn_name;
      workers[i].thd->connection_name.length= len;
      my_snprintf(workers[i].info, sizeof(workers[i].info), WORKER_NAME" %llu",
                  thd->thread_id);
      workers[i].thd->security_ctx= thd->security_ctx;
      workers[i].thd->set_command(thd->get_command());
      if (thd->db.str)
      {
        // explicit call to my_free in THD::free_connection(), so we do this
        workers[i].thd->db.str= (char*)my_malloc(PSI_INSTRUMENT_ME,
                                                 thd->db.length+1,
                                                 MYF(0));
        if (!workers[i].thd->db.str)
        {
          destroy_background_thd(workers[i].thd);
          for (uint j= 0; j < i; j++)
            abort_worker(workers+j);
          mysql_mutex_destroy(&workers[i].LOCK_worker);
          mysql_cond_destroy(&workers[i].COND_worker);
          my_free(workers);
          workers= nullptr;
          nworkers= 0;
          mysql_cond_destroy(&COND_pwt_new_message);
          mysql_mutex_destroy(&LOCK_pwt_manager);
          mysql_mutex_destroy(&LOCK_pwt_thread);
          return false;
        }

        strmake(const_cast<char*>(workers[i].thd->db.str), thd->db.str,
                thd->db.length);
        workers[i].thd->db.length= thd->db.length;
      }
      else
      {
        workers[i].thd->db.str= nullptr;
        workers[i].thd->db.length= 0;
      }
      workers[i].thd->proc_info= thd->proc_info;
      workers[i].thd->start_utime= thd->start_utime;
      workers[i].thd->thread_id= next_thread_id();
      // workers[i].thd->query_string= thd->query_string;
      workers[i].thd->query_string= CSET_STRING(workers[i].info,
                                                strlen(workers[i].info),
                                                workers[i].thd->query_charset());
      workers[i].thd->pwt_worker_info= workers+i;
      workers[i].finished= workers[i].joined= false;
      workers[i].killed= NOT_KILLED;
      server_threads.insert(workers[i].thd);               // show processlist

      if (mysql_thread_create(key_thread_pwt, &workers[i].pthread, nullptr,
                              parallel_worker_thread_func, &workers[i]))
      {
        my_free(const_cast<char*>(workers[i].thd->db.str));
        server_threads.erase(workers[i].thd);
        destroy_background_thd(workers[i].thd);
        for (uint j= 0; j < i; j++)
          abort_worker(workers+j);
        mysql_mutex_destroy(&workers[i].LOCK_worker);
        mysql_cond_destroy(&workers[i].COND_worker);
        my_free(workers);
        workers= nullptr;
        nworkers= 0;
        mysql_cond_destroy(&COND_pwt_new_message);
        mysql_mutex_destroy(&LOCK_pwt_manager);
        mysql_mutex_destroy(&LOCK_pwt_thread);
        return false;
      }
    }
  }
  return true;
}

#ifdef HAVE_PSI_INTERFACE
void pwt_init_psi_keys(void)
{
  PSI_server->register_thread("sql", all_pwt_threads,
                              array_elements(all_pwt_threads));
}
#endif


void pwt_management::join_parallel_workers(THD *thd)
{
  bool all_done= false, workers_killed= false;
  PSI_stage_info old_stage;
  struct timespec wait_max;
  wait_max.tv_nsec= 0;
  int killed_from= -1;

  while (!all_done)
  {
    wait_max.tv_sec= time(0)+1;                                      // wait 1s
    mysql_mutex_lock(&LOCK_pwt_manager);
    thd->ENTER_COND(&COND_pwt_new_message, &LOCK_pwt_manager,
                     &stage_waiting_for_work_from_sql_thread, &old_stage);
    mysql_cond_timedwait(&COND_pwt_new_message, &LOCK_pwt_manager, &wait_max);
    thd->EXIT_COND(&old_stage);

    all_done= true;

    // delete worker threads that are finished
    for (uint i= 0; i < nworkers; i++)
    {
      if (workers[i].finished)
      {
        if (workers[i].killed)
        {
          killed_from= i;
          thd->killed= workers[i].killed;
        }
        if (!workers[i].joined)
        {
          pthread_join(workers[i].pthread, nullptr);
          mysql_mutex_destroy(&workers[i].LOCK_worker);
          mysql_cond_destroy(&workers[i].COND_worker);
          workers[i].joined= true;
        }
      }
      else
        all_done= false;
    }

    if (thd->killed && !workers_killed)
    {
      // inform our workers the they are killed
      for (uint i= 0; i < nworkers; i++)
      {
        if (workers[i].finished)
          continue;
        if ((int)i != killed_from)
        {
          workers[i].thd->killed= thd->killed;
          mysql_mutex_lock(&workers[i].LOCK_worker);
          mysql_cond_signal(&workers[i].COND_worker);
          mysql_mutex_unlock(&workers[i].LOCK_worker);
        }
      }
      workers_killed= true;
    }
    else
    {
      // process queue
      mysql_mutex_lock(&LOCK_pwt_thread);
      pwt_queued_event *event;
      while ((event= parallel_messages.get()))
      {
        if (event->error)
        {
          thd->get_stmt_da()->set_overwrite_status(true);
          if (event->error->level ==
                            Sql_condition::enum_warning_level::WARN_LEVEL_ERROR)
          {
            my_message_sql(event->error->code, event->error->message, MYF(0));
          }
          else
          {
            push_warning(thd, event->error->level,
                         event->error->code, event->error->message);
          }
          my_free(event->error->message);
          my_free(event->error);
        }
        else
        {
          if (event->data)
          {
            // process data from our worker thread
          }
        }
        my_free(event);
      }
      mysql_mutex_unlock(&LOCK_pwt_thread);
    }
  }

  if (nworkers)
  {
    mysql_cond_destroy(&COND_pwt_new_message);
    mysql_mutex_destroy(&LOCK_pwt_manager);
    mysql_mutex_destroy(&LOCK_pwt_thread);
    my_free(workers);
  }
}

