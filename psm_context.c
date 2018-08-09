/*

  This file is provided under a dual BSD/GPLv2 license.  When using or
  redistributing this file, you may do so under either license.

  GPL LICENSE SUMMARY

  Copyright(c) 2015 Intel Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  Contact Information:
  Intel Corporation, www.intel.com

  BSD LICENSE

  Copyright(c) 2015 Intel Corporation.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

/* Copyright (c) 2003-2015 Intel Corporation. All rights reserved. */

#include <sys/types.h>
#include <sys/stat.h>

#include "psm_user.h"

#define HFI_USERINIT_RETRY_MAX 3
#define PSMI_SHARED_CONTEXTS_ENABLED_BY_DEFAULT   1
ustatic int MOCKABLE(psmi_sharedcontext_params)(int *nranks, int *rankid);
MOCK_DCL_EPILOGUE(psmi_sharedcontext_params);
static int psmi_get_hfi_selection_algorithm(void);
ustatic psm2_error_t psmi_init_userinfo_params(psm2_ep_t ep,
					     int unit_id,
					     psm2_uuid_t const unique_job_key,
					     struct hfi1_user_info_dep *user_info);

psm2_error_t psmi_context_interrupt_set(psmi_context_t *context, int enable)
{
	int poll_type;
	int ret;

	if ((enable && (context->runtime_flags & PSMI_RUNTIME_INTR_ENABLED)) ||
	    (!enable && !(context->runtime_flags & PSMI_RUNTIME_INTR_ENABLED)))
		return PSM2_OK;

	if (enable)
		poll_type = HFI1_POLL_TYPE_URGENT;
	else
		poll_type = 0;

	ret = hfi_poll_type(context->ctrl, poll_type);

	if (ret != 0)
		return PSM2_EP_NO_RESOURCES;
	else {
		if (enable)
			context->runtime_flags |= PSMI_RUNTIME_INTR_ENABLED;
		else
			context->runtime_flags &= ~PSMI_RUNTIME_INTR_ENABLED;

		return PSM2_OK;
	}
}

int psmi_context_interrupt_isenabled(psmi_context_t *context)
{
	return context->runtime_flags & PSMI_RUNTIME_INTR_ENABLED;
}

/* Returns 1 when all of the active units have their free contexts
 * equal the number of contexts.  This is an indication that no
 * jobs are currently running.
 *
 * Note that this code is clearly racy (this code may happen concurrently
 * by two or more processes, and this point of observation,
 * occurs earlier in time to when the decision is made for deciding which
 * context to assign, which will also occurs earlier in time to when the
 * context is actually assigned.  And, when the context is finally
 * assigned, this will change the "nfreectxts" observed below.)
 */
static int psmi_all_active_units_have_max_freecontexts(int nunits)
{
	int u;

	for (u=0;u < nunits;u++)
	{
		if (hfi_get_unit_active(u) > 0)
		{
			int64_t nfreectxts=0,nctxts=0;

			if (!hfi_sysfs_unit_read_s64(u, "nctxts", &nctxts, 0) &&
			    !hfi_sysfs_unit_read_s64(u, "nfreectxts", &nfreectxts, 0))
			{
				if (nfreectxts != nctxts)
					return 0;
			}
		}
	}
	return 1;
}

/* returns the integer value of an environment variable, or 0 if the environment
 * variable is not set. */
static int psmi_get_envvar(const char *env)
{
	const char *env_val = getenv(env);

	if (env_val && *env_val)
	{
		int r = atoi(env_val);
		return (r >= 0) ? r : 0;
	}
	return 0;
}

/* returns the 8-bit hash value of an uuid. */
static inline
uint8_t
psmi_get_uuid_hash(psm2_uuid_t const uuid)
{
	int i;
	uint8_t hashed_uuid = 0;

	for (i=0; i < sizeof(psm2_uuid_t); ++i)
		hashed_uuid ^= *((uint8_t const *)uuid + i);

	return hashed_uuid;
}

int psmi_get_current_proc_location()
{
        int core_id, node_id;

	core_id = sched_getcpu();
	if (core_id < 0)
		return -EINVAL;

	node_id = numa_node_of_cpu(core_id);
	if (node_id < 0)
		return -EINVAL;

	return node_id;
}

static void
psmi_spread_hfi_selection(psm2_uuid_t const job_key, long *unit_start,
			     long *unit_end, int nunits)
{
	/* if the number of ranks on the host is 1 and ... */
	if ((psmi_get_envvar("MPI_LOCALNRANKS") == 1) &&
		/*
		 * All of the active units have free contexts equal the
		 * number of contexts.
		 */
	    psmi_all_active_units_have_max_freecontexts(nunits)) {
		/* we start looking at unit 0, and end at nunits-1: */
		*unit_start = 0;
		*unit_end = nunits - 1;
	} else {
		/* else, we are going to look at:
		   (a hash of the job key plus the local rank id) mod nunits. */

		*unit_start = (psmi_get_envvar("MPI_LOCALRANKID") +
			psmi_get_uuid_hash(job_key)) % nunits;
		if (*unit_start > 0)
			*unit_end = *unit_start - 1;
		else
			*unit_end = nunits-1;
	}
}

static int
psmi_create_and_open_affinity_shm(psm2_uuid_t const job_key)
{
	int shm_fd, ret;
	int first_to_create = 0;
	size_t shm_name_len = 256;
	shared_affinity_ptr = NULL;
	affinity_shm_name = NULL;
	affinity_shm_name = (char *) psmi_malloc(PSMI_EP_NONE, UNDEFINED, shm_name_len);

	snprintf(affinity_shm_name, shm_name_len,
		 AFFINITY_SHM_BASENAME".%d",
		 psmi_get_uuid_hash(job_key));
	shm_fd = shm_open(affinity_shm_name, O_RDWR | O_CREAT | O_EXCL,
			  S_IRUSR | S_IWUSR);
	if ((shm_fd < 0) && (errno == EEXIST)) {
		shm_fd = shm_open(affinity_shm_name, O_RDWR, S_IRUSR | S_IWUSR);
		if (shm_fd < 0) {
			_HFI_VDBG("Cannot open affinity shared mem fd:%s, errno=%d\n",
				  affinity_shm_name, errno);
			return shm_fd;
		}
	} else if (shm_fd > 0) {
		first_to_create = 1;
	} else {
		_HFI_VDBG("Cannot create affinity shared mem fd:%s, errno=%d\n",
			  affinity_shm_name, errno);
	}

	ret = ftruncate(shm_fd, AFFINITY_SHMEMSIZE);
	if ( ret < 0 )
		return ret;

	shared_affinity_ptr = (uint64_t *) mmap(NULL, AFFINITY_SHMEMSIZE, PROT_READ | PROT_WRITE,
					MAP_SHARED, shm_fd, 0);
	if (shared_affinity_ptr == MAP_FAILED) {
		_HFI_VDBG("Cannot mmap affinity shared memory. errno=%d\n",
			  errno);
		close(shm_fd);
		return -1;
	}
	close(shm_fd);

	psmi_affinity_shared_file_opened = 1;

	if (first_to_create) {
		_HFI_VDBG("Creating shm to store HFI affinity per socket\n");

		memset(shared_affinity_ptr, 0, AFFINITY_SHMEMSIZE);

		/*
		 * Once shm object is initialized, unlock others to be able to
		 * use it.
		 */
		psmi_sem_post(sem_affinity_shm_rw, sem_affinity_shm_rw_name);
	} else {
		_HFI_VDBG("Opening shm object to read/write HFI affinity per socket\n");
	}

	/*
	 * Start critical section to increment reference count when creating
	 * or opening shm object. Decrement of ref count will be done before
	 * closing the shm.
	 */
	if (psmi_sem_timedwait(sem_affinity_shm_rw, sem_affinity_shm_rw_name)) {
		_HFI_VDBG("Could not enter critical section to update shm refcount\n");
		return -1;
	}

	shared_affinity_ptr[AFFINITY_SHM_REF_COUNT_LOCATION] += 1;

	/* End critical section */
	psmi_sem_post(sem_affinity_shm_rw, sem_affinity_shm_rw_name);

	return 0;
}

/*
 * Spread HFI selection between units if we find more than one within a socket.
 */
static void
psmi_spread_hfi_within_socket(long *unit_start, long *unit_end, int node_id,
			      int *saved_hfis, int found, psm2_uuid_t const job_key)
{
	int ret, shm_location;

	/*
	 * Take affinity lock and open shared memory region to be able to
	 * accurately determine which HFI to pick for this process. If any
	 * issues, bail by picking first known HFI.
	 */
	if (!psmi_affinity_semaphore_open)
		goto spread_hfi_fallback;

	ret = psmi_create_and_open_affinity_shm(job_key);
	if (ret < 0)
		goto spread_hfi_fallback;

	shm_location = AFFINITY_SHM_HFI_INDEX_LOCATION + node_id;
	if (shm_location > AFFINITY_SHMEMSIZE)
		goto spread_hfi_fallback;

	/* Start critical section to read/write shm object */
	if (psmi_sem_timedwait(sem_affinity_shm_rw, sem_affinity_shm_rw_name)) {
		_HFI_VDBG("Could not enter critical section to update HFI index\n");
		goto spread_hfi_fallback;
	}

	*unit_start = *unit_end = shared_affinity_ptr[shm_location];
	shared_affinity_ptr[shm_location] =
		(shared_affinity_ptr[shm_location] + 1) % found;
	_HFI_VDBG("Selected HFI index= %ld, Next HFI=%ld, node = %d, local rank=%d, found=%d.\n",
		  *unit_start, shared_affinity_ptr[shm_location], node_id,
		  psmi_get_envvar("MPI_LOCALRANKID"), found);

	/* End Critical Section */
	psmi_sem_post(sem_affinity_shm_rw, sem_affinity_shm_rw_name);

spread_hfi_fallback:
	*unit_start = *unit_end = saved_hfis[0];
}

static void
psmi_create_affinity_semaphores(psm2_uuid_t const job_key)
{
	int ret;
	sem_affinity_shm_rw_name = NULL;
	size_t sem_len = 256;

	/*
	 * If already opened, no need to do anything else.
	 * This could be true for Multi-EP cases where a different thread has
	 * already created the semaphores. We don't need separate locks here as
	 * we are protected by the overall "psmi_creation_lock" which each
	 * thread will take in psm2_ep_open()
	 */
	if (psmi_affinity_semaphore_open)
		return;

	sem_affinity_shm_rw_name = (char *) psmi_malloc(PSMI_EP_NONE, UNDEFINED, sem_len);
	snprintf(sem_affinity_shm_rw_name, sem_len,
		 SEM_AFFINITY_SHM_RW_BASENAME".%d",
		 psmi_get_uuid_hash(job_key));

	ret = psmi_init_semaphore(&sem_affinity_shm_rw, sem_affinity_shm_rw_name,
				  S_IRUSR | S_IWUSR, 0);
	if (ret) {
		_HFI_VDBG("Cannot initialize semaphore: %s for read-write access to shm object.\n",
			  sem_affinity_shm_rw_name);
		sem_close(sem_affinity_shm_rw);
		psmi_free(sem_affinity_shm_rw_name);
		sem_affinity_shm_rw_name = NULL;
		return;
	}

	_HFI_VDBG("Semaphore: %s created for read-write access to shm object.\n",
		  sem_affinity_shm_rw_name);

	psmi_affinity_semaphore_open = 1;

	return;
}

static
psm2_error_t
psmi_compute_start_and_end_unit(psmi_context_t *context,long unit_param,
				int nunitsactive,int nunits,psm2_uuid_t const job_key,
				long *unit_start,long *unit_end)
{
	int node_id, unit_id, found = 0;
	int saved_hfis[nunits];

	context->user_info.hfi1_alg = HFI1_ALG_ACROSS;
	/* if the user did not set HFI_UNIT then ... */
	if (unit_param == HFI_UNIT_ID_ANY)
	{
		/* Get the actual selection algorithm from the environment: */
		context->user_info.hfi1_alg = psmi_get_hfi_selection_algorithm();
		/* If round-robin is selection algorithm and ... */
		if ((context->user_info.hfi1_alg == HFI1_ALG_ACROSS) &&
		    /* there are more than 1 active units then ... */
		    (nunitsactive > 1))
		{
			/*
			 * Pick first HFI we find on same root complex
			 * as current task. If none found, fall back to
			 * load-balancing algorithm.
			 */
			node_id = psmi_get_current_proc_location();
			if (node_id >= 0) {
				for (unit_id = 0; unit_id < nunits; unit_id++) {
					if (hfi_get_unit_active(unit_id) <= 0)
						continue;

					if (hfi_sysfs_unit_read_node_s64(unit_id) == node_id) {
						saved_hfis[found] = unit_id;
						found++;
					}
				}

				if (found > 1) {
					psmi_create_affinity_semaphores(job_key);
					psmi_spread_hfi_within_socket(unit_start, unit_end,
								      node_id, saved_hfis,
								      found, job_key);
				} else if (found == 1) {
					*unit_start = *unit_end = saved_hfis[0];
				}
			}

			if (node_id < 0 || !found) {
				psmi_spread_hfi_selection(job_key, unit_start,
							  unit_end, nunits);
			}
		} else if ((context->user_info.hfi1_alg == HFI1_ALG_ACROSS_ALL) &&
			 (nunitsactive > 1)) {
				psmi_spread_hfi_selection(job_key, unit_start,
							  unit_end, nunits);
		}
		else {
			*unit_start = 0;
			*unit_end = nunits - 1;
		}
	} else if (unit_param >= 0) {
		/* the user specified HFI_UNIT, we use it. */
		*unit_start = *unit_end = unit_param;
	} else {
		psmi_handle_error(NULL, PSM2_EP_DEVICE_FAILURE,
				 "PSM2 can't open unit: %ld for reading and writing",
				 unit_param);
		return PSM2_EP_DEVICE_FAILURE;
	}

	return PSM2_OK;
}

psm2_error_t
psmi_context_open(const psm2_ep_t ep, long unit_param, long port,
		  psm2_uuid_t const job_key, int64_t timeout_ns,
		  psmi_context_t *context)
{
	long open_timeout = 0, unit_start, unit_end, unit_id, unit_id_prev;
	int lid, sc, vl;
	uint64_t gid_hi, gid_lo;
	char dev_name[MAXPATHLEN];
	psm2_error_t err = PSM2_OK;
	uint32_t hfi_type;
	int nunits = hfi_get_num_units(), nunitsactive=0;

	/*
	 * If shared contexts are enabled, try our best to schedule processes
	 * across one or many devices
	 */

	/* if no units, then no joy. */
	if (nunits <= 0)
	{
		err = psmi_handle_error(NULL, PSM2_EP_DEVICE_FAILURE,
					"PSM2 no hfi units are available");
		goto ret;
	}

	/* Calculate the number of active units: */
	for (unit_id=0;unit_id < nunits;unit_id++)
	{
		if (hfi_get_unit_active(unit_id) > 0)
			nunitsactive++;
	}
	/* if no active units, then no joy. */
	if (nunitsactive == 0)
	{
		err = psmi_handle_error(NULL, PSM2_EP_DEVICE_FAILURE,
					"PSM2 no hfi units are active");
		goto ret;
	}
	if (timeout_ns > 0)
		open_timeout = (long)(timeout_ns / MSEC_ULL);


	unit_start = 0; unit_end = nunits - 1;
	err = psmi_compute_start_and_end_unit(context, unit_param,
					      nunitsactive, nunits,
					      job_key,
					      &unit_start, &unit_end);
	if (err != PSM2_OK)
		return err;

	/* this is the start of a loop that starts at unit_start and goes to unit_end.
	   but note that the way the loop computes the loop control variable is by
	   an expression involving the mod operator. */
	context->fd = -1;
	context->ctrl = NULL;
	unit_id_prev = unit_id = unit_start;
	do
	{
		/* close previous opened unit fd before attempting open of current unit. */
		if (context->fd > 0)
		{
			hfi_context_close(context->fd);
			context->fd = -1;
		}

		/* if the unit_id is not active, go to next one. */
		if (hfi_get_unit_active(unit_id) <= 0) {
			unit_id_prev = unit_id;
			unit_id = (unit_id + 1) % nunits;
			continue;
		}

		/* open this unit. */
		context->fd = hfi_context_open_ex(unit_id, port, open_timeout,
				       dev_name, sizeof(dev_name));

		/* go to next unit if failed to open. */
		if (context->fd == -1) {
			unit_id_prev = unit_id;
			unit_id = (unit_id + 1) % nunits;
			continue;
		}

		/* collect the userinfo params. */
		if ((err = psmi_init_userinfo_params(ep,
						     (int)unit_id, job_key,
						     &context->user_info)))
			goto bail;

		/* attempt to assign the context via hfi_userinit() */
		int retry = 0;
		do {
			if (retry > 0)
				_HFI_INFO("hfi_userinit: failed, trying again (%d/%d)\n",
					  retry, HFI_USERINIT_RETRY_MAX);
			context->ctrl = hfi_userinit(context->fd, &context->user_info);
		} while (context->ctrl == NULL && ++retry <= HFI_USERINIT_RETRY_MAX);
		unit_id_prev = unit_id;
		unit_id = (unit_id + 1) % nunits;
	} while (unit_id_prev != unit_end && context->ctrl == NULL);

	if (context->ctrl == NULL)
	{
		err = psmi_handle_error(NULL, PSM2_EP_DEVICE_FAILURE,
					"PSM2 can't open hfi unit: %ld",unit_param);
		goto ret;
	}
	_HFI_VDBG("hfi_userinit() passed.\n");

	if ((lid = hfi_get_port_lid(context->ctrl->__hfi_unit,
				    context->ctrl->__hfi_port)) <= 0) {
		err = psmi_handle_error(NULL,
					PSM2_EP_DEVICE_FAILURE,
					"Can't get HFI LID in psm2_ep_open: is SMA running?");
		goto bail;
	}
	if (hfi_get_port_gid(context->ctrl->__hfi_unit,
			     context->ctrl->__hfi_port, &gid_hi,
			     &gid_lo) == -1) {
		err =
		    psmi_handle_error(NULL, PSM2_EP_DEVICE_FAILURE,
				      "Can't get HFI GID in psm2_ep_open: is SMA running?");
		goto bail;
	}
	ep->unit_id = context->ctrl->__hfi_unit;
	ep->portnum = context->ctrl->__hfi_port;
	ep->gid_hi = gid_hi;
	ep->gid_lo = gid_lo;

	context->ep = (psm2_ep_t) ep;
	context->runtime_flags = context->ctrl->ctxt_info.runtime_flags;

#ifdef PSM_CUDA
	/* Check backward compatibility bits here and save the info */
	if (context->ctrl->ctxt_info.runtime_flags & HFI1_CAP_GPUDIRECT_OT)
		is_driver_gpudirect_enabled = 1;
#endif

	/* Get type of hfi assigned to context */
	hfi_type = psmi_get_hfi_type(context);

	/* Endpoint out_sl contains the default SL to use for this endpoint. */
	/* Get the MTU for this SL. */
	if ((sc = hfi_get_port_sl2sc(ep->unit_id,
				     context->ctrl->__hfi_port,
				     ep->out_sl)) < 0) {
		sc = PSMI_SC_DEFAULT;
	}
	if ((vl = hfi_get_port_sc2vl(ep->unit_id,
				     context->ctrl->__hfi_port, sc)) < 0) {
		vl = PSMI_VL_DEFAULT;
	}
	if (sc == PSMI_SC_ADMIN || vl == PSMI_VL_ADMIN) {
		err = psmi_handle_error(NULL, PSM2_INTERNAL_ERR,
			"Invalid sl: %d, please specify correct sl via HFI_SL",
			ep->out_sl);
		goto bail;
	}

	if ((ep->mtu = hfi_get_port_vl2mtu(ep->unit_id,
					   context->ctrl->__hfi_port,
					   vl)) < 0) {
		err =
		    psmi_handle_error(NULL, PSM2_EP_DEVICE_FAILURE,
				      "Can't get MTU for VL %d", vl);
		goto bail;
	}

	/* Construct epid for this Endpoint */
	switch (PSMI_EPID_VERSION) {
		case PSMI_EPID_V1:
			context->epid = PSMI_EPID_PACK_V1(lid, context->ctrl->ctxt_info.ctxt,
								context->ctrl->ctxt_info.subctxt,
								context->ctrl->__hfi_unit,
								PSMI_EPID_VERSION, 0x3ffffff);
			break;
		case PSMI_EPID_V2:
			context->epid = PSMI_EPID_PACK_V2(lid, context->ctrl->ctxt_info.ctxt,
								context->ctrl->ctxt_info.subctxt,
								PSMI_EPID_IPS_SHM, /*Not a only-shm epid */
								PSMI_EPID_VERSION, ep->gid_hi);
			break;
		default:
			/* Epid version is greater than max supportd version. */
			psmi_assert_always(PSMI_EPID_VERSION <= PSMI_EPID_V2);
			break;
	}


	_HFI_VDBG
	    ("construct epid: lid %d ctxt %d subctxt %d hcatype %d mtu %d\n",
	     lid, context->ctrl->ctxt_info.ctxt,
	     context->ctrl->ctxt_info.subctxt, hfi_type, ep->mtu);

	goto ret;

bail:
	_HFI_PRDBG("%s open failed: %d (%s)\n", dev_name, err, strerror(errno));
	if (context->fd != -1) {
		hfi_context_close(context->fd);
		context->fd = -1;
	}
ret:

	_HFI_VDBG("psmi_context_open() return %d\n", err);
	return err;
}

psm2_error_t psmi_context_close(psmi_context_t *context)
{
	if (context->fd >= 0) {
		struct hfi1_base_info *binfo;
		struct hfi1_ctxt_info *cinfo;
		int __hfi_pg_sz = sysconf(_SC_PAGESIZE);
		binfo = &context->ctrl->base_info;
		cinfo = &context->ctrl->ctxt_info;

		munmap((void*)PSMI_ALIGNDOWN(binfo->sc_credits_addr, __hfi_pg_sz),
		       __hfi_pg_sz);
		munmap((void*)PSMI_ALIGNDOWN(binfo->pio_bufbase_sop, __hfi_pg_sz),
		       cinfo->credits * 64);
		munmap((void*)PSMI_ALIGNDOWN(binfo->pio_bufbase, __hfi_pg_sz),
		       cinfo->credits * 64);
		munmap((void*)PSMI_ALIGNDOWN(binfo->rcvhdr_bufbase, __hfi_pg_sz),
		       cinfo->rcvhdrq_cnt * cinfo->rcvhdrq_entsize);
		munmap((void*)PSMI_ALIGNDOWN(binfo->rcvegr_bufbase, __hfi_pg_sz),
		       cinfo->egrtids * cinfo->rcvegr_size);
		munmap((void*)PSMI_ALIGNDOWN(binfo->sdma_comp_bufbase, __hfi_pg_sz),
		       cinfo->sdma_ring_size * sizeof(struct hfi1_sdma_comp_entry));
		/* only unmap the RTAIL if it was enabled in the first place */
		if (cinfo->runtime_flags & HFI1_CAP_DMA_RTAIL) {
			munmap((void*)PSMI_ALIGNDOWN(binfo->rcvhdrtail_base, __hfi_pg_sz),
				__hfi_pg_sz);
		}
		munmap((void*)PSMI_ALIGNDOWN(binfo->user_regbase, __hfi_pg_sz),
			__hfi_pg_sz);
		munmap((void*)PSMI_ALIGNDOWN(binfo->events_bufbase, __hfi_pg_sz),
		       __hfi_pg_sz);
		munmap((void*)PSMI_ALIGNDOWN(binfo->status_bufbase, __hfi_pg_sz),
		       __hfi_pg_sz);

		/* only unmap subcontext-related stuff it subcontexts are enabled */
		if (context->user_info.subctxt_cnt > 0) {
			munmap((void*)PSMI_ALIGNDOWN(binfo->subctxt_uregbase, __hfi_pg_sz),
			       __hfi_pg_sz);
			munmap((void*)PSMI_ALIGNDOWN(binfo->subctxt_rcvhdrbuf, __hfi_pg_sz),
			       __hfi_pg_sz);
			munmap((void*)PSMI_ALIGNDOWN(binfo->subctxt_rcvegrbuf, __hfi_pg_sz),
			       __hfi_pg_sz);
		}

		hfi_context_close(context->fd);
		context->fd = -1;
	}
	return PSM2_OK;
}

/*
 * This function works whether a context is initialized or not in a psm2_ep.
 *
 * Returns one of
 *
 * PSM2_OK: Port status is ok (or context not initialized yet but still "ok")
 * PSM2_OK_NO_PROGRESS: Cable pulled
 * PSM2_EP_NO_NETWORK: No network, no lid, ...
 * PSM2_EP_DEVICE_FAILURE: Chip failures, rxe/txe parity, etc.
 * The message follows the per-port status
 * As of 7322-ready driver, need to check port-specific qword for IB
 * as well as older unit-only.  For now, we don't have the port interface
 * defined, so just check port 0 qword for spi_status
 */
psm2_error_t psmi_context_check_status(const psmi_context_t *contexti)
{
	psm2_error_t err = PSM2_OK;
	psmi_context_t *context = (psmi_context_t *) contexti;
	struct hfi1_status *status =
	    (struct hfi1_status *)context->ctrl->base_info.status_bufbase;
	char *errmsg = NULL;

	/* Fatal chip-related errors */
	if (!(status->dev & HFI1_STATUS_CHIP_PRESENT) ||
	    !(status->dev & HFI1_STATUS_INITTED) ||
	    (status->dev & HFI1_STATUS_HWERROR)) {

		err = PSM2_EP_DEVICE_FAILURE;
		if (err != context->status_lasterr) {	/* report once */
			volatile char *errmsg_sp =
			    (volatile char *)status->freezemsg;
			if (*errmsg_sp)
				psmi_handle_error(context->ep, err,
						  "Hardware problem: %s",
						  errmsg_sp);
			else {
				if (status->dev & HFI1_STATUS_HWERROR)
					errmsg = "Hardware error";
				else
					errmsg = "Hardware not found";

				psmi_handle_error(context->ep, err,
						  "%s", errmsg);
			}
		}
	}
	/* Fatal network-related errors with timeout: */
	else if (!(status->port & HFI1_STATUS_IB_CONF) ||
		 !(status->port & HFI1_STATUS_IB_READY)) {
		err = PSM2_EP_NO_NETWORK;
		if (err != context->status_lasterr) {	/* report once */
			context->networkLostTime = time(NULL);
		}
		else
		{
			time_t now = time(NULL);
			static const double seventySeconds = 70.0;

			/* The linkup time duration for a system should allow the time needed
			   to complete 3 LNI passes which is:
			   50 seconds for a passive copper channel
			   65 seconds for optical channel.
			   (we add 5 seconds of margin.) */
			if (difftime(now,context->networkLostTime) > seventySeconds)
			{
				volatile char *errmsg_sp =
					(volatile char *)status->freezemsg;

				psmi_handle_error(context->ep, err, "%s",
						  *errmsg_sp ? errmsg_sp :
						  "Network down");
			}
		}
	}

	if (err == PSM2_OK && context->status_lasterr != PSM2_OK)
		context->status_lasterr = PSM2_OK;	/* clear error */
	else if (err != PSM2_OK)
		context->status_lasterr = err;	/* record error */

	return err;
}

/*
 * Prepare user_info params for driver open, used only in psmi_context_open
 */
ustatic
psm2_error_t
psmi_init_userinfo_params(psm2_ep_t ep, int unit_id,
			  psm2_uuid_t const unique_job_key,
			  struct hfi1_user_info_dep *user_info)
{
	/* static variables, shared among rails */
	static int shcontexts_enabled = -1, rankid, nranks;

	int avail_contexts = 0, max_contexts, ask_contexts;
	int ranks_per_context = 0;
	psm2_error_t err = PSM2_OK;
	union psmi_envvar_val env_maxctxt, env_ranks_per_context;
	static int subcontext_id_start;

	memset(user_info, 0, sizeof(*user_info));
	user_info->userversion = HFI1_USER_SWMINOR|(hfi_get_user_major_version()<<HFI1_SWMAJOR_SHIFT);

	user_info->subctxt_id = 0;
	user_info->subctxt_cnt = 0;
	memcpy(user_info->uuid, unique_job_key, sizeof(user_info->uuid));

	if (shcontexts_enabled == -1) {
		shcontexts_enabled =
		    psmi_sharedcontext_params(&nranks, &rankid);
	}
	if (!shcontexts_enabled)
		return err;

	avail_contexts = hfi_get_num_contexts(unit_id);

	if (avail_contexts == 0) {
		err = psmi_handle_error(NULL, PSM2_EP_NO_DEVICE,
					"PSM2 found 0 available contexts on opa device(s).");
		goto fail;
	}

	/* See if the user wants finer control over context assignments */
	if (!psmi_getenv("PSM2_MAX_CONTEXTS_PER_JOB",
			 "Maximum number of contexts for this PSM2 job",
			 PSMI_ENVVAR_LEVEL_USER, PSMI_ENVVAR_TYPE_INT,
			 (union psmi_envvar_val)avail_contexts, &env_maxctxt)) {
		max_contexts = max(env_maxctxt.e_int, 1);		/* needs to be non-negative */
		ask_contexts = min(max_contexts, avail_contexts);	/* needs to be available */
	} else if (!psmi_getenv("PSM2_SHAREDCONTEXTS_MAX",
				"",  /* deprecated */
				PSMI_ENVVAR_LEVEL_HIDDEN | PSMI_ENVVAR_LEVEL_NEVER_PRINT,
				PSMI_ENVVAR_TYPE_INT,
				(union psmi_envvar_val)avail_contexts, &env_maxctxt)) {

		_HFI_INFO
		    ("The PSM2_SHAREDCONTEXTS_MAX env variable is deprecated. Please use PSM2_MAX_CONTEXTS_PER_JOB in future.\n");

		max_contexts = max(env_maxctxt.e_int, 1);		/* needs to be non-negative */
		ask_contexts = min(max_contexts, avail_contexts);	/* needs to be available */
	} else
		ask_contexts = max_contexts = avail_contexts;

	if (!psmi_getenv("PSM2_RANKS_PER_CONTEXT",
			 "Number of ranks per context",
			 PSMI_ENVVAR_LEVEL_USER, PSMI_ENVVAR_TYPE_INT,
			 (union psmi_envvar_val)1, &env_ranks_per_context)) {
		ranks_per_context = max(env_ranks_per_context.e_int, 1);
		ranks_per_context = min(ranks_per_context, HFI1_MAX_SHARED_CTXTS);
	}

	/*
	 * See if we could get a valid ppn.  If not, approximate it to be the
	 * number of cores.
	 */
	if (nranks == -1) {
		long nproc = sysconf(_SC_NPROCESSORS_ONLN);
		if (nproc < 1)
			nranks = 1;
		else
			nranks = nproc;
	}

	/*
	 * Make sure that our guesses are good educated guesses
	 */
	if (rankid >= nranks) {
		_HFI_PRDBG
		    ("PSM2_SHAREDCONTEXTS disabled because lrank=%d,ppn=%d\n",
		     rankid, nranks);
		goto fail;
	}

	if (ranks_per_context) {
		int contexts =
		    (nranks + ranks_per_context - 1) / ranks_per_context;
		if (contexts > ask_contexts) {
			err = psmi_handle_error(NULL, PSM2_EP_NO_DEVICE,
						"Incompatible settings for "
						"PSM2_MAX_CONTEXTS_PER_JOB and PSM2_RANKS_PER_CONTEXT");
			goto fail;
		}
		ask_contexts = contexts;
	}

	/* group id based on total groups and local rank id */
	user_info->subctxt_id = subcontext_id_start + rankid % ask_contexts;
	/* this is for multi-rail, when we setup a new rail,
	 * we can not use the same subcontext ID as the previous
	 * rail, otherwise, the driver will match previous rail
	 * and fail.
	 */
	subcontext_id_start += ask_contexts;

	/* Need to compute with how many *other* peers we will be sharing the
	 * context */
	if (nranks > ask_contexts) {
		user_info->subctxt_cnt = nranks / ask_contexts;
		/* If ppn != multiple of contexts, some contexts get an uneven
		 * number of subcontexts */
		if (nranks % ask_contexts > rankid % ask_contexts)
			user_info->subctxt_cnt++;
		/* The case of 1 process "sharing" a context (giving 1 subcontext)
		 * is supcontexted by the driver and PSM. However, there is no
		 * need to share in this case so disable context sharing. */
		if (user_info->subctxt_cnt == 1)
			user_info->subctxt_cnt = 0;
		if (user_info->subctxt_cnt > HFI1_MAX_SHARED_CTXTS) {
			err = psmi_handle_error(NULL, PSM2_INTERNAL_ERR,
						"Calculation of subcontext count exceeded maximum supported");
			goto fail;
		}
	}
	/* else subcontext_cnt remains 0 and context sharing is disabled. */

	_HFI_PRDBG("PSM2_SHAREDCONTEXTS lrank=%d,ppn=%d,avail_contexts=%d,"
		   "max_contexts=%d,ask_contexts=%d,"
		   "ranks_per_context=%d,id=%u,cnt=%u\n",
		   rankid, nranks, avail_contexts, max_contexts,
		   ask_contexts, ranks_per_context,
		   user_info->subctxt_id, user_info->subctxt_cnt);
fail:
	return err;
}

ustatic
int MOCKABLE(psmi_sharedcontext_params)(int *nranks, int *rankid)
{
	union psmi_envvar_val enable_shcontexts;
	char *ppn_env = NULL, *lrank_env = NULL, *c;

	*rankid = -1;
	*nranks = -1;

#if 0
	/* DEBUG: Used to selectively test possible shared context and shm-only
	 * settings */
	unsetenv("PSC_MPI_NODE_RANK");
	unsetenv("PSC_MPI_PPN");
	unsetenv("MPI_LOCALRANKID");
	unsetenv("MPI_LOCALRANKS");
#endif

	/* We do not support context sharing for multiple endpoints */
	if (psmi_multi_ep_enabled) {
		return 0;
	}

	/* New name in 2.0.1, keep observing old name */
	psmi_getenv("PSM2_SHAREDCONTEXTS", "Enable shared contexts",
		    PSMI_ENVVAR_LEVEL_USER, PSMI_ENVVAR_TYPE_YESNO,
		    (union psmi_envvar_val)
		    PSMI_SHARED_CONTEXTS_ENABLED_BY_DEFAULT,
		    &enable_shcontexts);
	if (!enable_shcontexts.e_int)
		return 0;

	/* We support two types of syntaxes to let users give us a hint what
	 * our local rankid is.  Moving towards MPI_, but still support PSC_ */
	if ((c = getenv("MPI_LOCALRANKID")) && *c != '\0') {
		lrank_env = "MPI_LOCALRANKID";
		ppn_env = "MPI_LOCALNRANKS";
	} else if ((c = getenv("PSC_MPI_PPN")) && *c != '\0') {
		ppn_env = "PSC_MPI_PPN";
		lrank_env = "PSC_MPI_NODE_RANK";
	}

	if (ppn_env != NULL && lrank_env != NULL) {
		union psmi_envvar_val env_rankid, env_nranks;

		psmi_getenv(lrank_env, "Shared context rankid",
			    PSMI_ENVVAR_LEVEL_HIDDEN, PSMI_ENVVAR_TYPE_INT,
			    (union psmi_envvar_val)-1, &env_rankid);

		psmi_getenv(ppn_env, "Shared context numranks",
			    PSMI_ENVVAR_LEVEL_HIDDEN, PSMI_ENVVAR_TYPE_INT,
			    (union psmi_envvar_val)-1, &env_nranks);

		*rankid = env_rankid.e_int;
		*nranks = env_nranks.e_int;

		return 1;
	} else
		return 0;
}
MOCK_DEF_EPILOGUE(psmi_sharedcontext_params);

static
int psmi_get_hfi_selection_algorithm(void)
{
	union psmi_envvar_val env_hfi1_alg;
	int hfi1_alg = HFI1_ALG_ACROSS;

	/* If a specific unit is set in the environment, use that one. */
	psmi_getenv("HFI_SELECTION_ALG",
		    "HFI Device Selection Algorithm to use. Round Robin (Default) "
		    ", Packed or Round Robin All.",
		    PSMI_ENVVAR_LEVEL_USER, PSMI_ENVVAR_TYPE_STR,
		    (union psmi_envvar_val)"Round Robin", &env_hfi1_alg);

	if (!strcasecmp(env_hfi1_alg.e_str, "Round Robin"))
		hfi1_alg = HFI1_ALG_ACROSS;
	else if (!strcasecmp(env_hfi1_alg.e_str, "Packed"))
		hfi1_alg = HFI1_ALG_WITHIN;
	else if (!strcasecmp(env_hfi1_alg.e_str, "Round Robin All"))
		hfi1_alg = HFI1_ALG_ACROSS_ALL;
	else {
		_HFI_ERROR
		    ("Unknown HFI selection algorithm %s. Defaulting to Round Robin "
		     "allocation of HFIs.\n", env_hfi1_alg.e_str);
		hfi1_alg = HFI1_ALG_ACROSS;
	}

	return hfi1_alg;
}
