/**
 * The MapReduce coordinator.
 */

#include "coordinator.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#ifndef SIG_PF
#define SIG_PF void (*)(int)
#endif

typedef struct {
  bool failed; // set to false by default, set this to true if one of the tasks returns an error

  path* files;

  path output_dir;

  char *app;

  struct {
		u_int args_len;
		char *args_val;
	} args;

  int numTotalMaps; // total number of maps that will be used for reduce's calculation
  GList* waitingMaps; // maps that have not started yet
  GList* runningMaps; // maps that are currently running or have failed(and need to be handled); ordered by start time
  GHashTable* runningMapStartTimes; // maps from map operation's task number to the operation's start time
  // int numRunningMaps; // number of maps that are currently running

  int numTotalReduces; // total number of reduces that we want to divide the map's key set into
  GList* waitingReduces; // reduces that have not started yet
  GList* runningReduces; // reduces that are currently running or have failed(and need to be handled); ordered by start time
  GHashTable* runningReduceStartTimes; // maps from reduce operation's task number to the operation's start time
  // int numRunningReduces; // number of reduces that are currently running
} jobInfo;

/* Global coordinator state. */
coordinator* state;

extern void coordinator_1(struct svc_req*, struct SVCXPRT*);

/* Set up and run RPC server. */
int main(int argc, char** argv) {
  register SVCXPRT* transp;

  pmap_unset(COORDINATOR, COORDINATOR_V1);

  transp = svcudp_create(RPC_ANYSOCK);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create udp service.");
    exit(1);
  }
  if (!svc_register(transp, COORDINATOR, COORDINATOR_V1, coordinator_1, IPPROTO_UDP)) {
    fprintf(stderr, "%s", "unable to register (COORDINATOR, COORDINATOR_V1, udp).");
    exit(1);
  }

  transp = svctcp_create(RPC_ANYSOCK, 0, 0);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create tcp service.");
    exit(1);
  }
  if (!svc_register(transp, COORDINATOR, COORDINATOR_V1, coordinator_1, IPPROTO_TCP)) {
    fprintf(stderr, "%s", "unable to register (COORDINATOR, COORDINATOR_V1, tcp).");
    exit(1);
  }

  coordinator_init(&state);

  svc_run();
  fprintf(stderr, "%s", "svc_run returned");
  exit(1);
  /* NOTREACHED */
}

/* EXAMPLE RPC implementation. */
int* example_1_svc(int* argp, struct svc_req* rqstp) {
  static int result;

  result = *argp + 1;

  return &result;
}

/* SUBMIT_JOB RPC implementation. */
int* submit_job_1_svc(submit_job_request* argp, struct svc_req* rqstp) {
  static int result;

  printf("Received submit job request\n");

  /* TODO */

  /* Do not modify the following code. */
  /* BEGIN */
  struct stat st;
  if (stat(argp->output_dir, &st) == -1) {
    mkdirp(argp->output_dir);
  }

  if (get_app(argp->app).name == NULL) { // app is invalid
    result = -1;
    return &result;
  }

  int jobID = state->nextJobID++;

  // INITIALIZING NEW jobInfo FOR HASH MAP
  jobInfo* job = malloc(sizeof(jobInfo));
  if (job == NULL) {
    result = -1;
    return &result;
  }

  job->failed = false;

  job->files = malloc(argp->files.files_len * sizeof(path));
  for (int i = 0; i < argp->files.files_len; i++) {
    job->files[i] = strdup(argp->files.files_val[i]);
  }

  job->output_dir = strdup(argp->output_dir);

  job->app = strdup(argp->app);

  job->args.args_len = argp->args.args_len;
  job->args.args_val = malloc(job->args.args_len * sizeof(char));
  strncpy(job->args.args_val, argp->args.args_val, job->args.args_len);

  job->numTotalMaps = argp->files.files_len;
  job->waitingMaps = NULL;
  for (int i = 0; i < job->numTotalMaps; i++) {
    job->waitingMaps = g_list_append(job->waitingMaps, GINT_TO_POINTER(i));
  }

  // job->numRunningMaps = 0;
  job->runningMaps = NULL;

  job->runningMapStartTimes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

  job->numTotalReduces = argp->n_reduce;
  job->waitingReduces = NULL;
  for (int i = 0; i < job->numTotalReduces; i++) {
    job->waitingReduces = g_list_append(job->waitingReduces, GINT_TO_POINTER(i));
  }

  // job->numRunningReduces = 0;
  job->runningReduces = NULL;

  job->runningReduceStartTimes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

  // ADDING JOB INFO TO HASH MAP
  g_hash_table_insert(state->jobIDMap, GINT_TO_POINTER(jobID), job);

  // do this at the end because we want to initialize everything before putting it onto the queue, which marks it as ready
  state->jobQueue = g_list_append(state->jobQueue, GINT_TO_POINTER(jobID));

  result = jobID;
  return &result;
  /* END */
}

/* POLL_JOB RPC implementation. */
poll_job_reply* poll_job_1_svc(int* argp, struct svc_req* rqstp) {
  static poll_job_reply result;

  printf("Received poll job request\n");

  /* TODO */

  result.done = false;
  result.failed = false;
  result.invalid_job_id = false;

  if (*argp < 0 || *argp >= state->nextJobID) {
    result.invalid_job_id = true;
    return &result;
  }

  jobInfo* job = g_hash_table_lookup(state->jobIDMap, GINT_TO_POINTER(*argp));
  if (job == NULL) {
    result.done = true;
    return &result;
  }

  if (job->failed) {
    result.failed = true;
    result.done = true;
    return &result;
  }

  if (job->waitingReduces == NULL && job->runningReduces == NULL) {
    result.done = true;
    return &result;
  }

  return &result;
}

// Fills the field with values equivalent to void
// Need to set values to be not NULL so that they can serialize without segfaulting
get_task_reply* fillFields(get_task_reply* result) {
  result->job_id = -1;
  result->task = -1;
  result->file = "";
  result->output_dir = "";
  result->app = "";
  result->n_reduce = -1;
  result->n_map = -1;
  result->reduce = false;
  result->wait = true; // WANT THIS TO BE TRUE TO INDICATE THAT THE CURRENT WORKER SHOULD WAIT
  result->args.args_len = 0;
  result->args.args_val = "";
  
  return result;
}

/* GET_TASK RPC implementation. */
get_task_reply* get_task_1_svc(void* argp, struct svc_req* rqstp) {
  static get_task_reply result;

  printf("Received get task request\n");
  // result.file = "";
  // result.output_dir = "";
  // result.app = "";
  // result.wait = true;
  // result.args.args_len = 0;

  /* TODO */
  if (state->jobQueue == NULL) {
    return fillFields(&result);
  }

  GList* cur = g_list_first(state->jobQueue);
  while (cur != NULL) {
    int jobID = GPOINTER_TO_INT(cur->data);
    jobInfo* job = g_hash_table_lookup(state->jobIDMap, cur->data);

    if (job->waitingMaps != NULL) {
      GList* mapTask = g_list_first(job->waitingMaps);
      result.job_id = jobID;
      result.task = GPOINTER_TO_INT(mapTask->data);

      job->waitingMaps = g_list_delete_link(mapTask, mapTask);
      job->runningMaps = g_list_append(job->runningMaps, GINT_TO_POINTER(result.task));

      time_t* timePtr = malloc(sizeof(time_t));
      *timePtr = time(NULL);
      time_t checker = time(NULL);
      g_hash_table_insert(job->runningMapStartTimes, GINT_TO_POINTER(result.task), timePtr);

      result.file = job->files[result.task];

      result.output_dir = job->output_dir;

      result.app = job->app;

      result.n_reduce = job->numTotalReduces;

      result.n_map = job->numTotalMaps;

      result.reduce = false;

      result.wait = false;

      result.args.args_len = job->args.args_len;
      result.args.args_val = job->args.args_val;

      return &result;
    } else if (job->runningMaps != NULL) {
      GList* earliestMap = g_list_first(job->runningMaps);
      int taskNum = GPOINTER_TO_INT(earliestMap->data);

      time_t* mapStart = g_hash_table_lookup(job->runningMapStartTimes, earliestMap->data);
      // sleep(TASK_TIMEOUT_SECS);
      // time_t diff = time(NULL) - *mapStart;
      if (time(NULL) - *mapStart >= TASK_TIMEOUT_SECS) {
      // if (diff >= TASK_TIMEOUT_SECS) {
        g_hash_table_remove(job->runningMapStartTimes, earliestMap->data);
        job->runningMaps = g_list_delete_link(earliestMap, earliestMap);
        job->waitingMaps = g_list_append(job->waitingMaps, GINT_TO_POINTER(taskNum));
      } else {
        cur = cur->next;
      }
      continue;
    } else if (job->waitingReduces != NULL) {
      GList* reduceTask = g_list_first(job->waitingReduces);
      result.job_id = jobID;
      result.task = GPOINTER_TO_INT(reduceTask->data);

      job->waitingReduces = g_list_delete_link(reduceTask, reduceTask);
      job->runningReduces = g_list_append(job->runningReduces, GINT_TO_POINTER(result.task));

      time_t* timePtr = malloc(sizeof(time_t));
      *timePtr = time(NULL);
      g_hash_table_insert(job->runningReduceStartTimes, GINT_TO_POINTER(result.task), timePtr);

      result.file = "";

      result.output_dir = job->output_dir;

      result.app = job->app;

      result.n_reduce = job->numTotalReduces;

      result.n_map = job->numTotalMaps;

      result.reduce = true;

      result.wait = false;

      result.args.args_len = job->args.args_len;
      result.args.args_val = job->args.args_val;

      return &result;
    } else if (job->runningReduces != NULL) {
      GList* earliestReduce = g_list_first(job->runningReduces);
      int taskNum = GPOINTER_TO_INT(earliestReduce->data);

      time_t* reduceStart = g_hash_table_lookup(job->runningReduceStartTimes, earliestReduce->data);
      if (time(NULL) - *reduceStart >= TASK_TIMEOUT_SECS) {
        g_hash_table_remove(job->runningReduceStartTimes, earliestReduce->data);
        job->runningReduces = g_list_delete_link(earliestReduce, earliestReduce);
        job->waitingReduces = g_list_append(job->waitingReduces, GINT_TO_POINTER(taskNum));
      } else {
        cur = cur->next;
      }
      continue;
    } else {
      // WE ONLY GET HERE IF THERE ARE NO WAITING OR RUNNING MAPS OR WAITING OR RUNNING REDUCES
      // IN ORDER WORDS THE JOB IS FINISHED
      // SHOULDNT EVER REACH HERE BECAUSE FINISHED JOBS SHOULD BE REMOVED IN finish_task_1_svc
      state->jobQueue = g_list_delete_link(state->jobQueue, cur);
      cur = cur->next;
    }
  }

  // WE ONLY GET HERE IF NO JOB IS SCHEDULED, SO TELL THE WORKER TO WAIT
  return fillFields(&result);
}

// gint compareQueueFunc(gconstpointer a, gconstpointer b) {
//   return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
// }

/* FINISH_TASK RPC implementation. */
void* finish_task_1_svc(finish_task_request* argp, struct svc_req* rqstp) {
  static char* result;

  printf("Received finish task request\n");

  /* TODO */

  if (argp->job_id < 0 || argp->job_id >= state->nextJobID) {
    return (void*) &result;
  }

  jobInfo* job = g_hash_table_lookup(state->jobIDMap, GINT_TO_POINTER(argp->job_id));
  if (job == NULL || job->failed) {
    return (void*) &result;
  }

  if (!argp->success) {
    job->failed = true;

    GList* cur = g_list_first(state->jobQueue);
    while (cur != NULL) {
      int jobID = GPOINTER_TO_INT(cur);
      if (jobID == argp->job_id) {
        state->jobQueue = g_list_delete_link(state->jobQueue, cur);
        break;
      }
      cur = cur->next;
    }

    return (void*) &result;
  }

  if (!argp->reduce) { // remove running map and handle as expected
    GList* runningMap = g_list_first(job->runningMaps);
    while (runningMap != NULL) {
      if (GPOINTER_TO_INT(runningMap->data) == argp->task) {
        job->runningMaps = g_list_delete_link(job->runningMaps, runningMap);
        break;
      }
      runningMap = runningMap->next;
    }
  } else {
    GList* runningReduce = g_list_first(job->runningReduces);
    while (runningReduce != NULL) {
      if (GPOINTER_TO_INT(runningReduce->data) == argp->task) {
        job->runningReduces = g_list_delete_link(job->runningReduces, runningReduce);
        break;
      }
      runningReduce = runningReduce->next;
    }

    if (job->runningReduces == NULL && job->waitingReduces == NULL) {
      GList* curJob = g_list_first(state->jobQueue);
      while (curJob != NULL) {
        if (argp->job_id == GPOINTER_TO_INT(curJob->data)) {
          state->jobQueue = g_list_delete_link(state->jobQueue, curJob);
          break;
        }
        curJob = curJob->next;
      }
    }
  }

  return (void*)&result;
}

/* Initialize coordinator state. */
void coordinator_init(coordinator** coord_ptr) {
  *coord_ptr = malloc(sizeof(coordinator));

  coordinator* coord = *coord_ptr;

  /* TODO */
  coord->nextJobID = 0;
  coord->jobQueue = NULL;
  coord->jobIDMap = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
}
