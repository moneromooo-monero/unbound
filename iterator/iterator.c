/*
 * iterator/iterator.c - iterative resolver DNS query response module
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains a module that performs recusive iterative DNS query
 * processing.
 */

#include "config.h"
#include "iterator/iterator.h"
#include "util/module.h"
#include "util/netevent.h"
#include "util/config_file.h"
#include "util/net_help.h"
#include "util/storage/slabhash.h"
#include "services/cache/rrset.h"

/** 
 * Set forwarder address 
 * @param ie: iterator global state.
 * @param ip: the server name.
 * @param port: port on server or NULL for default 53.
 * @return: false on error.
 */
static int
iter_set_fwd(struct iter_env* ie, const char* ip, int port)
{
        uint16_t p;
        log_assert(ie && ip);
        p = (uint16_t) port;
        if(str_is_ip6(ip)) {
                struct sockaddr_in6* sa =
                        (struct sockaddr_in6*)&ie->fwd_addr;
                ie->fwd_addrlen = (socklen_t)sizeof(struct sockaddr_in6);
                memset(sa, 0, ie->fwd_addrlen);
                sa->sin6_family = AF_INET6;
                sa->sin6_port = (in_port_t)htons(p);
                if(inet_pton((int)sa->sin6_family, ip, &sa->sin6_addr) <= 0) {
                        log_err("Bad ip6 address %s", ip);
                        return 0;
                }
        } else { /* ip4 */
                struct sockaddr_in* sa =
                        (struct sockaddr_in*)&ie->fwd_addr;
                ie->fwd_addrlen = (socklen_t)sizeof(struct sockaddr_in);
                memset(sa, 0, ie->fwd_addrlen);
                sa->sin_family = AF_INET;
                sa->sin_port = (in_port_t)htons(p);
                if(inet_pton((int)sa->sin_family, ip, &sa->sin_addr) <= 0) {
                        log_err("Bad ip4 address %s", ip);
                        return 0;
                }
        }
        verbose(VERB_ALGO, "iterator: fwd queries to: %s %d", ip, p);
        return 1;
}

/** iterator init */
static int 
iter_init(struct module_env* env, int id)
{
	struct iter_env* iter_env = (struct iter_env*)calloc(1,
		sizeof(struct iter_env));
	if(!iter_env) {
		log_err("malloc failure");
		return 0;
	}
	env->modinfo[id] = (void*)iter_env;
	/* set forwarder address */
	if(env->cfg->fwd_address && env->cfg->fwd_address[0]) {
		if(!iter_set_fwd(iter_env, env->cfg->fwd_address, 
			env->cfg->fwd_port)) {
			log_err("iterator: could not set forwarder address");
			return 0;
		}
	}
	return 1;
}

/** iterator deinit */
static void 
iter_deinit(struct module_env* env, int id)
{
	struct iter_env* iter_env;
	if(!env || !env->modinfo)
		return;
	iter_env = (struct iter_env*)env->modinfo[id];
	if(iter_env)
		free(iter_env);
}

/** store rrsets in the rrset cache. */
static void
store_rrsets(struct module_env* env, struct reply_info* rep, uint32_t now)
{
        size_t i;
        /* see if rrset already exists in cache, if not insert it. */
        for(i=0; i<rep->rrset_count; i++) {
                rep->ref[i].key = rep->rrsets[i];
                rep->ref[i].id = rep->rrsets[i]->id;
		if(rrset_cache_update(env->rrset_cache, &rep->ref[i], 
			env->alloc, now)) /* it was in the cache */
			rep->rrsets[i] = rep->ref[i].key;
        }
}


/** store message in the cache */
static void
store_msg(struct module_qstate* qstate, struct query_info* qinfo,
	struct reply_info* rep)
{
	struct msgreply_entry* e;
	uint32_t now = time(NULL);
	reply_info_set_ttls(rep, now);
	store_rrsets(qstate->env, rep, now);
	if(rep->ttl == 0) {
		log_info("TTL 0: dropped msg from cache");
		return;
	}
	reply_info_sortref(rep);
	/* store msg in the cache */
	if(!(e = query_info_entrysetup(qinfo, rep, qstate->query_hash))) {
		log_err("store_msg: malloc failed");
		return;
	}
	slabhash_insert(qstate->env->msg_cache, qstate->query_hash,
		&e->entry, rep, &qstate->env->alloc);
}

/** iterator operate on a query */
static void 
iter_operate(struct module_qstate* qstate, enum module_ev event, int id)
{
	struct module_env* env = qstate->env;
	struct iter_env* ie = (struct iter_env*)env->modinfo[id];
	verbose(VERB_ALGO, "iterator[module %d] operate: extstate:%s event:%s", 
		id, strextstate(qstate->ext_state[id]), strmodulevent(event));
	if(event == module_event_error) {
		qstate->ext_state[id] = module_error;
		return;
	}
	if(event == module_event_new) {
		/* send UDP query to forwarder address */
		(*env->send_query)(qstate->buf, &ie->fwd_addr, 
			ie->fwd_addrlen, UDP_QUERY_TIMEOUT, qstate, 0);
		qstate->ext_state[id] = module_wait_reply;
		qstate->minfo[id] = NULL;
		return;
	}
	if(event == module_event_timeout) {
		/* try TCP if UDP fails */
		/* TODO: disabled now, make better retry with EDNS.
		if(qstate->reply->c->type == comm_udp) {
			qinfo_query_encode(qstate->buf, &qstate->qinfo);
			(*env->send_query)(qstate->buf, &ie->fwd_addr, 
				ie->fwd_addrlen, TCP_QUERY_TIMEOUT, qstate, 1);
			return;
		}
		*/
		qstate->ext_state[id] = module_error;
		return;
	}
	if(event == module_event_reply) {
		uint16_t us = qstate->edns.udp_size;
		struct query_info reply_qinfo;
		struct reply_info* reply_msg;
		struct edns_data reply_edns;
		int r;
		/* see if it is truncated */
		if(LDNS_TC_WIRE(ldns_buffer_begin(qstate->reply->c->buffer)) 
			&& qstate->reply->c->type == comm_udp) {
			log_info("TC: truncated. retry in TCP mode.");
			qinfo_query_encode(qstate->buf, &qstate->qinfo);
			(*env->send_query)(qstate->buf, &ie->fwd_addr, 
				ie->fwd_addrlen, TCP_QUERY_TIMEOUT, qstate, 1);
			/* stay in wait_reply state */
			return;
		}
		if((r=reply_info_parse(qstate->reply->c->buffer, env->alloc, 
			&reply_qinfo, &reply_msg, qstate->scratch, 
			&reply_edns))!=0) {
			qstate->ext_state[id] = module_error;
			return;
		}

		qstate->edns.edns_version = EDNS_ADVERTISED_VERSION;
		qstate->edns.udp_size = EDNS_ADVERTISED_SIZE;
		qstate->edns.ext_rcode = 0;
		qstate->edns.bits &= EDNS_DO;
		if(!reply_info_answer_encode(&reply_qinfo, reply_msg, 0, 
			qstate->query_flags, qstate->buf, 0, 0, 
			qstate->scratch, us, &qstate->edns)) {
			qstate->ext_state[id] = module_error;
			return;
		}
		store_msg(qstate, &reply_qinfo, reply_msg);
		qstate->ext_state[id] = module_finished;
		return;
	}
	log_err("bad event for iterator");
	qstate->ext_state[id] = module_error;
}

/** iterator cleanup query state */
static void 
iter_clear(struct module_qstate* qstate, int id)
{
	if(!qstate)
		return;
	/* allocated in region, so nothing to do */
	qstate->minfo[id] = NULL;
}

/**
 * The iterator function block 
 */
static struct module_func_block iter_block = {
	"iterator",
	&iter_init, &iter_deinit, &iter_operate, &iter_clear
};

struct module_func_block* 
iter_get_funcblock()
{
	return &iter_block;
}
