/*
 * Copyright (C) 2006-2014 Michael Roitzsch <mroi@os.inf.tu-dresden.de>
 * economic rights: Technische Universitaet Dresden (Germany)
 */

#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <assert.h>

#ifdef __linux__
#include <sched.h>
#include <signal.h>
#endif

#define BUFFER_TYPE double
#include "estimator.h"
#include "scheduler.h"
#include "llsp.h"

#pragma clang diagnostic ignored "-Wvla"

struct estimator_s {
	struct estimator_s *next;
	pthread_mutex_t lock;
	void *code;
	
	double time;
	size_t metrics_count;
	llsp_t *llsp;
	double mse;
	
	struct buffer metrics;
};

static pthread_rwlock_t estimator_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct estimator_s *estimator_list = NULL;

static inline struct estimator_s *find_estimator(void *code)
{
	struct estimator_s *estimator;
	pthread_rwlock_rdlock(&estimator_lock);
	for (estimator = estimator_list; estimator; estimator = estimator->next)
		if (estimator->code == code) break;
	pthread_rwlock_unlock(&estimator_lock);
	return estimator;
}

#pragma mark -


#pragma mark Job Management

void atlas_job_submit(void *code, pid_t tid, atlas_job_t job)
{
	struct estimator_s *estimator = find_estimator(code);
	
	if (!estimator) {
		pthread_rwlock_wrlock(&estimator_lock);
		for (estimator = estimator_list; estimator; estimator = estimator->next)
			if (estimator->code == code) break;
		
		if (!estimator) {
			estimator = malloc(sizeof(struct estimator_s));
			if (!estimator) abort();
			pthread_mutex_init(&estimator->lock, NULL);
			
			estimator->code = code;
			estimator->time = 0.0;
			estimator->metrics_count = 0;
			estimator->llsp = NULL;
			estimator->mse = 0.0;
			
			buffer_init(&estimator->metrics);
			
			estimator->next = estimator_list;
			estimator_list = estimator;
		}
		pthread_rwlock_unlock(&estimator_lock);
	}
	
	pthread_mutex_lock(&estimator->lock);
	
	if (!estimator->llsp) {
		estimator->metrics_count = job.metrics_count;
		estimator->llsp = llsp_new(estimator->metrics_count + 1);  // add an extra 1-column
	}
	assert(estimator->metrics_count == job.metrics_count);
	
	double prediction = 0.0;
#if LLSP_PREDICT
	double llsp_metrics[estimator->metrics_count + 1];
	for (size_t i = 0; i < estimator->metrics_count + 1; i++) {
		if (i < estimator->metrics_count)
			llsp_metrics[i] = job.metrics[i];
		else
			llsp_metrics[i] = 1.0;  // add an extra 1-column
	}
	prediction = llsp_predict(estimator->llsp, llsp_metrics);
#endif
	
	for (size_t i = 0; i < estimator->metrics_count; i++)
		buffer_put(&estimator->metrics, job.metrics[i]);
	buffer_put(&estimator->metrics, job.deadline);
	buffer_put(&estimator->metrics, prediction);
	
	double reservation = JOB_OVERALLOCATION(prediction);
#if JOB_SCHEDULING
	struct timeval tv_deadline = {
		.tv_sec = (time_t)job.deadline,
		.tv_usec = (suseconds_t)(1000000 * (job.deadline - (time_t)job.deadline))
	};
	struct timeval tv_exectime = {
		.tv_sec = (time_t)reservation,
		.tv_usec = (suseconds_t)(1000000 * (reservation - (time_t)reservation))
	};
	sched_submit(tid, &tv_exectime, &tv_deadline, sched_deadline_absolute);
#else
	(void)tid;
#endif
	if (hook_job_submit)
		hook_job_submit(code, prediction, reservation, job.deadline);
	
	pthread_mutex_unlock(&estimator->lock);
}

void atlas_job_next(void *code)
{
	double time = atlas_progress();
	struct estimator_s *estimator = find_estimator(code);
	assert(estimator);
	
	estimator->time = time;
	
#if JOB_SCHEDULING
	sched_next();
#endif
	if (hook_job_release)
		hook_job_release(code);
}

void atlas_job_train(void *code)
{
	double time = atlas_progress();
	struct estimator_s *estimator = find_estimator(code);
	assert(estimator);
	assert(estimator->time > 0.0);
	
	pthread_mutex_lock(&estimator->lock);
	
	if (estimator->metrics.read != estimator->metrics.write) {
		double llsp_metrics[estimator->metrics_count + 1];
		for (size_t i = 0; i < estimator->metrics_count + 1; i++) {
			if (i < estimator->metrics_count)
				llsp_metrics[i] = buffer_get(&estimator->metrics);
			else
				llsp_metrics[i] = 1.0;  // add an extra 1-column
		}
		double deadline = buffer_get(&estimator->metrics);
		double prediction = buffer_get(&estimator->metrics);
		double execution_time = time - estimator->time;
		double mse = (prediction - execution_time) * (prediction - execution_time);
		
#if LLSP_PREDICT
		llsp_add(estimator->llsp, llsp_metrics, execution_time);
		const double *result = llsp_solve(estimator->llsp);
		if (hook_llsp_result)
			hook_llsp_result(result, estimator->metrics_count + 1);
#endif
		estimator->mse = (1.0 - AGING_FACTOR) * estimator->mse + AGING_FACTOR * mse;
		if (hook_job_complete)
			hook_job_complete(code, time, deadline, prediction, execution_time);
	}
	
	pthread_mutex_unlock(&estimator->lock);
}

void atlas_pin_cpu(int cpu)
{
#ifdef __linux__
	cpu_set_t cpu_set;
	CPU_ZERO(&cpu_set);
	CPU_SET(cpu, &cpu_set);
	if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) abort();
        
	const struct sigaction action = { .sa_handler = SIG_IGN };
	if (sigaction(SIGXCPU, &action, NULL) != 0) abort();
#else
#pragma message "CPU affinity not supported"
	(void)cpu;
#endif
}

#pragma mark -


#pragma mark ATLAS Timebase

double atlas_now(void)
{
#ifdef __linux__
	/* ATLAS uses CLOCK_MONOTONIC as reference, because seconds are always a wallclock second there */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#else
	struct timeval tv;
	gettimeofday(&tv,NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}

double atlas_progress(void)
{
#ifdef __linux__
	/* a per-thread clock, which only counts the time the thread is running */
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#else
#pragma message "falling back to gettimeofday() which includes blocking and waiting time, results will be wrong"
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}
