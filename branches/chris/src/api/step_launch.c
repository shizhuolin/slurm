/*****************************************************************************\
 *  launch.c - launch a parallel job step
 *
 *  $Id: spawn.c 7973 2006-05-08 23:52:35Z morrone $
 *****************************************************************************
 *  Copyright (C) 2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Christopher J. Morrone <morrone2@llnl.gov>
 *  UCRL-CODE-217948.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <slurm/slurm.h>

#include "src/common/hostlist.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/eio.h"
#include "src/common/net.h"
#include "src/common/fd.h"
#include "src/common/slurm_auth.h"
#include "src/common/forward.h"
#include "src/common/plugstack.h"
#include "src/common/slurm_cred.h"

#include "src/api/step_ctx.h"
#include "src/api/step_pmi.h"

#define STEP_LAUNCH_TIMEOUT 10 /* FIXME - should be defined elsewhere */

/**********************************************************************
 * General declarations for step launch code
 **********************************************************************/
static int _launch_tasks(slurm_step_ctx ctx,
			 launch_tasks_request_msg_t *launch_msg);
static client_io_t *_setup_step_client_io(slurm_step_ctx ctx,
					  slurm_step_io_fds_t fds,
					  bool labelio);

/**********************************************************************
 * Message handler declarations
 **********************************************************************/
static uid_t  slurm_uid;
static int _msg_thr_create(struct step_launch_state *sls);
static void _handle_msg(struct step_launch_state *sls, slurm_msg_t *msg);
static bool _message_socket_readable(eio_obj_t *obj);
static int _message_socket_accept(eio_obj_t *obj, List objs);

static struct io_operations message_socket_ops = {
	readable:	&_message_socket_readable,
	handle_read:	&_message_socket_accept
};


/**********************************************************************
 * API functions
 **********************************************************************/

/* 
 * slurm_job_step_launch_t_init - initialize a user-allocated
 *      slurm_job_step_launch_t structure with default values.
 *	default values.  This function will NOT allocate any new memory.
 * IN ptr - pointer to a structure allocated by the use.  The structure will
 *      be intialized.
 */
void slurm_job_step_launch_t_init (slurm_job_step_launch_t *ptr)
{
	static slurm_step_io_fds_t fds = SLURM_STEP_IO_FDS_INITIALIZER;

	ptr->argc = 0;
	ptr->argv = NULL;
	ptr->envc = 0;
	ptr->env = NULL;
	ptr->cwd = NULL;
	ptr->buffered_stdio = true;
	ptr->labelio = false;
	ptr->remote_output_filename = NULL;
	ptr->remote_error_filename = NULL;
	ptr->remote_input_filename = NULL;
	memcpy(&ptr->local_fds, &fds, sizeof(fds));
	ptr->gid = getgid();
	ptr->multi_prog = false;
	ptr->slurmd_debug = 0;
	ptr->parallel_debug = false;
	ptr->task_start_callback = NULL;
	ptr->task_finish_callback = NULL;
}

/*
 * slurm_step_launch - launch a parallel job step
 * IN ctx - job step context generated by slurm_step_ctx_create
 * RET SLURM_SUCCESS or SLURM_ERROR (with errno set)
 */
int slurm_step_launch (slurm_step_ctx ctx,
		       const slurm_job_step_launch_t *params)
{
	launch_tasks_request_msg_t launch;
	int i;

	debug("Entering slurm_step_launch");
	if (ctx == NULL || ctx->magic != STEP_CTX_MAGIC) {
		error("Not a valid slurm_step_ctx!");

		slurm_seterrno(EINVAL);
		return SLURM_ERROR;
	}

	/* Initialize launch state structure */
	ctx->launch_state = xmalloc(sizeof(struct step_launch_state));
	if (ctx->launch_state == NULL) {
		error("Failed to allocate memory for step launch state: %m");
		return SLURM_ERROR;
	}
	pthread_mutex_init(&ctx->launch_state->lock, NULL);
	pthread_cond_init(&ctx->launch_state->cond, NULL);
	ctx->launch_state->tasks_requested = ctx->step_req->num_tasks;
	ctx->launch_state->tasks_start_success = 0;
	ctx->launch_state->tasks_start_failure = 0;
	ctx->launch_state->tasks_exited = 0;
	ctx->launch_state->task_start_callback = params->task_start_callback;
	ctx->launch_state->task_finish_callback = params->task_finish_callback;

	/* Create message receiving socket and handler thread */
	_msg_thr_create(ctx->launch_state);

	/* Start tasks on compute nodes */
	launch.job_id = ctx->alloc_resp->job_id;
	launch.uid = ctx->step_req->user_id;
	launch.gid = params->gid;
	launch.argc = params->argc;
	launch.argv = params->argv;
	launch.cred = ctx->step_resp->cred;
	launch.job_step_id = ctx->step_resp->job_step_id;
	launch.envc = params->envc;
	launch.env = params->env;
	launch.cwd = params->cwd;
	launch.nnodes = ctx->step_req->node_count;
	launch.nprocs = ctx->step_req->num_tasks;
	launch.slurmd_debug = params->slurmd_debug;
	launch.switch_job = ctx->step_resp->switch_job;
	launch.task_prolog = NULL; /* FIXME - opt.task_prolog */
	launch.task_epilog = NULL; /* FIXME - opt.task_epilog */
	launch.cpu_bind_type = 0; /* FIXME opt.cpu_bind_type; */
	launch.cpu_bind = NULL; /* FIXME opt.cpu_bind; */
	launch.mem_bind_type = 0; /* FIXME opt.mem_bind_type; */
	launch.mem_bind = NULL; /* FIXME opt.mem_bind; */
	launch.multi_prog = params->multi_prog ? 1 : 0;

	launch.options = job_options_create();
	spank_set_remote_options (launch.options);

	launch.ofname = params->remote_output_filename;
	launch.efname = params->remote_error_filename;
	launch.ifname = params->remote_input_filename;
	launch.buffered_stdio = params->buffered_stdio ? 1 : 0;

	if (params->parallel_debug)
		launch.task_flags |= TASK_PARALLEL_DEBUG;

	/* Node specific message contents */
/* 	if (slurm_mpi_single_task_per_node ()) { */
/* 		for (i = 0; i < job->step_layout->num_hosts; i++) */
/* 			job->step_layout->tasks[i] = 1; */
/* 	}  */

	launch.tasks_to_launch = ctx->step_layout->tasks;
	launch.cpus_allocated  = ctx->step_layout->cpus;
	launch.global_task_ids = ctx->step_layout->tids;
	
	ctx->launch_state->client_io = _setup_step_client_io(
		ctx, params->local_fds, params->labelio);
	if (ctx->launch_state->client_io == NULL)
		return SLURM_ERROR;
	if (client_io_handler_start(ctx->launch_state->client_io) != SLURM_SUCCESS)
		return SLURM_ERROR;

	launch.io_port = xmalloc(sizeof(uint16_t) * launch.nnodes);
	launch.resp_port = xmalloc(sizeof(uint16_t) * launch.nnodes);
	
	for (i = 0; i < launch.nnodes; i++) {
		client_io_t *client_io = ctx->launch_state->client_io;
		int port_idx = i % client_io->num_listen;

		launch.io_port[i] = ntohs(client_io->listenport[port_idx]);
		launch.resp_port[i] = ntohs(ctx->launch_state->msg_port);
	}

	_launch_tasks(ctx, &launch);
	return SLURM_SUCCESS;
}

/*
 * Block until all tasks have started.
 */
int slurm_step_launch_wait_start(slurm_step_ctx ctx)
{
	struct step_launch_state *sls = ctx->launch_state;

	/* First wait for all tasks to complete */
	pthread_mutex_lock(&sls->lock);
	while ((sls->tasks_start_success + sls->tasks_start_failure)
	       < sls->tasks_requested) {
		pthread_cond_wait(&sls->cond, &sls->lock);
	}
	pthread_mutex_unlock(&sls->lock);
}

/*
 * Block until all tasks have finished (or failed to start altogether).
 */
void slurm_step_launch_wait_finish(slurm_step_ctx ctx)
{
	struct step_launch_state *sls = ctx->launch_state;

	/* First wait for all tasks to complete */
	pthread_mutex_lock(&sls->lock);
	while (((sls->tasks_start_success + sls->tasks_start_failure)
		< sls->tasks_requested)
	       || (sls->tasks_exited < sls->tasks_start_success)) {
		pthread_cond_wait(&sls->cond, &sls->lock);
	}

	/* Then shutdown the message handler thread */
	eio_signal_shutdown(sls->msg_handle);
	pthread_join(sls->msg_thread, NULL);
	eio_handle_destroy(sls->msg_handle);

	/* Then wait for the IO thread to finish */
	client_io_handler_finish(sls->client_io);
	client_io_handler_destroy(sls->client_io);

	pthread_mutex_unlock(&sls->lock);

	/* FIXME - put these in an sls-specific desctructor */
	pthread_mutex_destroy(&sls->lock);
	pthread_cond_destroy(&sls->cond);
}

/**********************************************************************
 * Message handler functions
 **********************************************************************/
static void *_msg_thr_internal(void *arg)
{
	struct step_launch_state *sls = (struct step_launch_state *)arg;

	eio_handle_mainloop(sls->msg_handle);

	return NULL;
}

static int _msg_thr_create(struct step_launch_state *sls)
{
	int sock = -1;
	int port = -1;
	eio_obj_t *obj;

	debug("Entering _msg_thr_create()");
	slurm_uid = (uid_t) slurm_get_slurm_user_id();

	if (net_stream_listen(&sock, &port) < 0) {
		error("unable to intialize step launch listening socket: %m");
		return SLURM_ERROR;
	}
	sls->msg_port = port;

	obj = eio_obj_create(sock, &message_socket_ops, (void *)sls);

	sls->msg_handle = eio_handle_create();
	eio_new_initial_obj(sls->msg_handle, obj);
	/* FIXME check return code */
	pthread_create(&sls->msg_thread, NULL, _msg_thr_internal, (void *)sls);

}

static bool _message_socket_readable(eio_obj_t *obj)
{
	debug3("Called _message_socket_readable");
	if (obj->shutdown == true) {
		if (obj->fd != -1) {
			debug2("  false, shutdown");
			close(obj->fd);
			obj->fd = -1;
			/*_wait_for_connections();*/
		} else {
			debug2("  false");
		}
		return false;
	}
	return true;
}

static int _message_socket_accept(eio_obj_t *obj, List objs)
{
	struct step_launch_state *sls = (struct step_launch_state *)obj->arg;

	int fd;
	unsigned char *uc;
	short        port;
	struct sockaddr_un addr;
	slurm_msg_t *msg = NULL;
	int len = sizeof(addr);
	List ret_list = NULL;

	debug3("Called _msg_socket_accept");

	while ((fd = accept(obj->fd, (struct sockaddr *)&addr,
			    (socklen_t *)&len)) < 0) {
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN
		    || errno == ECONNABORTED
		    || errno == EWOULDBLOCK) {
			return SLURM_SUCCESS;
		}
		error("Error on msg accept socket: %m");
		obj->shutdown = true;
		return SLURM_SUCCESS;
	}

	fd_set_close_on_exec(fd);
	fd_set_blocking(fd);

	/* Should not call slurm_get_addr() because the IP may not be
	   in /etc/hosts. */
	uc = (unsigned char *)&((struct sockaddr_in *)&addr)->sin_addr.s_addr;
	port = ((struct sockaddr_in *)&addr)->sin_port;
	debug2("got message connection from %u.%u.%u.%u:%d",
	       uc[0], uc[1], uc[2], uc[3], ntohs(port));
	fflush(stdout);

	msg = xmalloc(sizeof(slurm_msg_t));
	forward_init(&msg->forward, NULL);
	msg->ret_list = NULL;
	msg->conn_fd = fd;
	msg->forward_struct_init = 0;

again:
	ret_list = slurm_receive_msg(fd, msg, STEP_LAUNCH_TIMEOUT);
	if(!ret_list || errno != SLURM_SUCCESS) {
		printf("error on slurm_recieve_msg\n");
		fflush(stdout);
		if (errno == EINTR) {
			list_destroy(ret_list);
			goto again;
		}
		error("slurm_receive_msg[%u.%u.%u.%u]: %m",
		      uc[0],uc[1],uc[2],uc[3]);
		goto cleanup;
	}
	if(list_count(ret_list)>0) {
		printf("_message_socket_accept connection: "
		      "got %d from receive, expecting 0\n",
		       list_count(ret_list));
		fflush(stdout);
		error("_message_socket_accept connection: "
		      "got %d from receive, expecting 0",
		      list_count(ret_list));
	}
	msg->ret_list = ret_list;

	_handle_msg(sls, msg); /* handle_msg frees msg */
cleanup:
	if ((msg->conn_fd >= 0) && slurm_close_accepted_conn(msg->conn_fd) < 0)
		error ("close(%d): %m", msg->conn_fd);
	slurm_free_msg(msg);

	return SLURM_SUCCESS;
}

static void
_launch_handler(struct step_launch_state *sls, slurm_msg_t *resp)
{
	launch_tasks_response_msg_t *msg = resp->data;

	pthread_mutex_lock(&sls->lock);

	if (msg->return_code == SLURM_SUCCESS)
		sls->tasks_start_success += msg->count_of_pids;
	else
		sls->tasks_start_failure += msg->count_of_pids;

	if (sls->task_start_callback != NULL)
		(sls->task_start_callback)(msg);

	pthread_cond_signal(&sls->cond);
	pthread_mutex_unlock(&sls->lock);

}

static void 
_exit_handler(struct step_launch_state *sls, slurm_msg_t *exit_msg)
{
	task_exit_msg_t *msg = (task_exit_msg_t *) exit_msg->data;
	pthread_mutex_lock(&sls->lock);

	sls->tasks_exited += msg->num_tasks;

	if (sls->task_finish_callback != NULL)
		(sls->task_finish_callback)(msg);

	pthread_cond_signal(&sls->cond);
	pthread_mutex_unlock(&sls->lock);
}

static void
_node_fail_handler(struct step_launch_state *sls, slurm_msg_t *fail_msg)
{
	/*srun_node_fail_msg_t *nf = fail_msg->data;*/

	pthread_mutex_lock(&sls->lock);

	/* does nothing yet */

	pthread_cond_signal(&sls->cond);
	pthread_mutex_unlock(&sls->lock);
	slurm_send_rc_msg(fail_msg, SLURM_SUCCESS);
}

static void
_handle_msg(struct step_launch_state *sls, slurm_msg_t *msg)
{
	uid_t req_uid = g_slurm_auth_get_uid(msg->auth_cred);
	uid_t uid = getuid();
	int rc;
	
	if ((req_uid != slurm_uid) && (req_uid != 0) && (req_uid != uid)) {
		error ("Security violation, slurm message from uid %u", 
		       (unsigned int) req_uid);
		return;
	}

	switch (msg->msg_type) {
	case RESPONSE_LAUNCH_TASKS:
		debug2("received task launch\n");
		_launch_handler(sls, msg);
		slurm_free_launch_tasks_response_msg(msg->data);
		break;
	case MESSAGE_TASK_EXIT:
		debug2("received task exit\n");
		_exit_handler(sls, msg);
		slurm_free_task_exit_msg(msg->data);
		break;
	case SRUN_NODE_FAIL:
		debug2("received srun node fail\n");
		_node_fail_handler(sls, msg);
		slurm_free_srun_node_fail_msg(msg->data);
		break;
	case PMI_KVS_PUT_REQ:
		debug2("PMI_KVS_PUT_REQ received\n");
		rc = pmi_kvs_put((struct kvs_comm_set *) msg->data);
		slurm_send_rc_msg(msg, rc);
		break;
	case PMI_KVS_GET_REQ:
		debug2("PMI_KVS_GET_REQ received\n");
		rc = pmi_kvs_get((kvs_get_msg_t *) msg->data);
		slurm_send_rc_msg(msg, rc);
		slurm_free_get_kvs_msg((kvs_get_msg_t *) msg->data);
		break;
	default:
		error("received spurious message type: %d\n",
		      msg->msg_type);
		break;
	}
	return;
}

/**********************************************************************
 * Task launch functions
 **********************************************************************/
static int _launch_tasks(slurm_step_ctx ctx,
			 launch_tasks_request_msg_t *launch_msg)
{
	slurm_msg_t msg;
	Buf buffer = NULL;
	hostlist_t hostlist = NULL;
	hostlist_iterator_t itr = NULL;
	int zero = 0;
	List ret_list = NULL;
	ListIterator ret_itr;
	ListIterator data_itr;

	debug("Entering _launch_tasks");

	msg.msg_type = REQUEST_LAUNCH_TASKS;
	msg.data = launch_msg;
	buffer = slurm_pack_msg_no_header(&msg);
	hostlist = hostlist_create(ctx->step_resp->node_list);
	itr = hostlist_iterator_create(hostlist);
	msg.srun_node_id = 0;
	msg.ret_list = NULL;
	msg.orig_addr.sin_addr.s_addr = 0;
	msg.buffer = buffer;
	memcpy(&msg.address, &ctx->alloc_resp->node_addr[0],
	       sizeof(slurm_addr));
	forward_set_launch(&msg.forward,
			   ctx->step_req->node_count,
			   &zero,
			   ctx->step_layout,
			   ctx->alloc_resp->node_addr,
			   itr,
			   STEP_LAUNCH_TIMEOUT);
	hostlist_iterator_destroy(itr);
	hostlist_destroy(hostlist);

	ret_list = slurm_send_recv_rc_packed_msg(&msg, STEP_LAUNCH_TIMEOUT);
	/* FIXME - look through list of return codes */
}

static client_io_t *_setup_step_client_io(slurm_step_ctx ctx,
					  slurm_step_io_fds_t fds,
					  bool labelio)
{
	int siglen;
	char *sig;
	client_io_t *client_io;

	if (slurm_cred_get_signature(ctx->step_resp->cred, &sig, &siglen)
	    < 0) {
		debug("_setup_step_client_io slurm_cred_get_signature failed");
		return NULL;
	}
		
	client_io = client_io_handler_create(fds,
					     ctx->step_req->num_tasks,
					     ctx->step_req->node_count,
					     sig,
					     labelio);

	/* no need to free sig, it is just a pointer into the credential */
	return client_io;
}
