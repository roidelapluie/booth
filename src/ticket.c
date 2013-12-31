/* 
 * Copyright (C) 2011 Jiaju Zhang <jjzhang@suse.de>
 * Copyright (C) 2013 Philipp Marek <philipp.marek@linbit.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include "ticket.h"
#include "config.h"
#include "pacemaker.h"
#include "inline-fn.h"
#include "log.h"
#include "booth.h"
#include "paxos.h"

#define TK_LINE			256


/* Untrusted input, must fit (incl. \0) in a buffer of max chars. */
int check_max_len_valid(const char *s, int max)
{
	int i;
	for(i=0; i<max; i++)
		if (s[i] == 0)
			return 1;
	return 0;
}

int find_ticket_by_name(const char *ticket, struct ticket_config **found)
{
	int i;

	if (found)
		*found = NULL;

	for (i = 0; i < booth_conf->ticket_count; i++) {
		if (!strcmp(booth_conf->ticket[i].name, ticket)) {
			if (found)
				*found = booth_conf->ticket + i;
			return 1;
		}
	}

	return 0;
}


int check_ticket(char *ticket, struct ticket_config **found)
{
	if (found)
		*found = NULL;
	if (!booth_conf)
		return 0;

	if (!check_max_len_valid(ticket, sizeof(booth_conf->ticket[0].name)))
		return 0;
	return find_ticket_by_name(ticket, found);
}

int check_site(char *site, int *is_local)
{
	struct booth_site *node;

	if (!check_max_len_valid(site, sizeof(node->addr_string)))
		return 0;

	if (find_site_by_name(site, &node)) {
		*is_local = node->local;
		return 1;
	}

	return 0;
}


/** Find out what others think about this ticket.
 *
 * If we're a SITE, we can ask (and have to tell) Pacemaker.
 * An ARBITRATOR can only ask others. */
static int ticket_send_catchup(struct ticket_config *tk)
{
	int i, rv = 0;
	struct booth_site *site;
	struct boothc_ticket_msg msg;

	foreach_node(i, site) {
		if (!site->local) {
			init_ticket_msg(&msg, CMD_CATCHUP, RLT_SUCCESS, tk);

			log_debug("attempting catchup from %s", site->addr_string);

			rv = booth_udp_send(site, &msg, sizeof(msg));
		}
	}

	ticket_activate_timeout(tk);

	return rv;
}


int ticket_write(struct ticket_config *tk)
{
	if (local->type != SITE)
		return -EINVAL;

	disown_if_expired(tk);

	pcmk_handler.store_ticket(tk);

	if (tk->owner == local) {
		pcmk_handler.grant_ticket(tk);
	} else {
		pcmk_handler.revoke_ticket(tk);
	}

	return 0;
}


/** Try to get the ticket for the local site.
 * */
int do_grant_ticket(struct ticket_config *tk)
{
	int rv;

	if (tk->owner == local)
		return RLT_SUCCESS;
	if (tk->owner)
		return RLT_OVERGRANT;

	rv = paxos_start_round(tk, local);
	return rv;
}


/** Start a PAXOS round for revoking.
 * That can be started from any site. */
int do_revoke_ticket(struct ticket_config *tk)
{
	int rv;

	if (!tk->owner)
		return RLT_SUCCESS;

	rv = paxos_start_round(tk, NULL);

	return rv;
}


int list_ticket(char **pdata, unsigned int *len)
{
	struct ticket_config *tk;
	char timeout_str[64];
	char *data, *cp;
	int i, alloc;

	*pdata = NULL;
	*len = 0;

	alloc = 256 +
		booth_conf->ticket_count * (BOOTH_NAME_LEN * 2 + 128);
	data = malloc(alloc);
	if (!data)
		return -ENOMEM;

	cp = data;
	foreach_ticket(i, tk) {
		if (tk->expires != 0)
			strftime(timeout_str, sizeof(timeout_str), "%F %T",
					localtime(&tk->expires));
		else
			strcpy(timeout_str, "INF");


		cp += sprintf(cp,
				"ticket: %s, owner: %s, expires: %s\n",
				tk->name,
				tk->owner ? tk->owner->addr_string : "None",
				timeout_str);

		*len = cp - data;
		assert(*len < alloc);
	}

	*pdata = data;

	return 0;
}


int setup_ticket(void)
{
	struct ticket_config *tk;
	int i;

	 /* TODO */
	foreach_ticket(i, tk) {
		tk->owner = NULL;
		tk->expires = 0;

		abort_proposal(tk);

		if (local->role & PROPOSER) {
			pcmk_handler.load_ticket(tk);
		}
	}


	return 0;
}


int ticket_answer_list(int fd, struct boothc_ticket_msg *msg)
{
	char *data;
	int olen, rv;
	struct boothc_header hdr;

	rv = list_ticket(&data, &olen);
	if (rv < 0)
		return rv;

	init_header(&hdr, CMR_LIST, RLT_SUCCESS, sizeof(hdr) + olen);

	return send_header_plus(fd, &hdr, data, olen);
}


int ticket_answer_grant(int fd, struct boothc_ticket_msg *msg)
{
	int rv;
	struct ticket_config *tk;


	if (!check_ticket(msg->ticket.id, &tk)) {
		log_error("Client asked to grant unknown ticket");
		rv = RLT_INVALID_ARG;
		goto reply;
	}

	if (tk->owner) {
		log_error("client wants to get an granted ticket %s",
				msg->ticket.id);
		rv = RLT_OVERGRANT;
		goto reply;
	}

	rv = do_grant_ticket(tk);

reply:
	init_header(&msg->header, CMR_GRANT, rv ?: RLT_ASYNC, sizeof(*msg));
	return send_ticket_msg(fd, msg);
}


int ticket_answer_revoke(int fd, struct boothc_ticket_msg *msg)
{
	int rv;
	struct ticket_config *tk;

	if (!check_ticket(msg->ticket.id, &tk)) {
		log_error("Client asked to grant unknown ticket");
		rv = RLT_INVALID_ARG;
		goto reply;
	}

	if (!tk->owner) {
		log_info("client wants to revoke a free ticket \"%s\"",
				msg->ticket.id);
		rv = RLT_SUCCESS;
		goto reply;
	}

	rv = do_revoke_ticket(tk);

reply:
	init_ticket_msg(msg, CMR_REVOKE, rv ?: RLT_ASYNC, tk);
	return send_ticket_msg(fd, msg);
}


/** Got a CMD_CATCHUP query.
 * In this file because it's mostly used during startup. */
static int ticket_answer_catchup(
		struct ticket_config *tk,
		struct booth_site *from,
		struct boothc_ticket_msg *msg,
		uint32_t ballot,
		struct booth_site *new_owner)
{
	int rv;


	log_debug("got CATCHUP query for \"%s\" from %s",
			msg->ticket.id, from->addr_string);

	/* We do _always_ answer.
	 * In case all booth daemons are restarted at the same time, nobody
	 * would answer any questions, leading to timeouts and delays.
	 * Just admit we don't know. */

	rv = (tk->state == ST_INIT) ?
		RLT_PROBABLY_SUCCESS : RLT_SUCCESS;

	init_ticket_msg(msg, CMR_CATCHUP, rv, tk);
	return booth_udp_send(from, msg, sizeof(*msg));
}


/** Got a CMR_CATCHUP message.
 * Gets handled here because it's not PAXOS per se,
 * but only needed during startup. */
static int ticket_process_catchup(
		struct ticket_config *tk,
		struct booth_site *from,
		struct boothc_ticket_msg *msg,
		uint32_t ballot,
		struct booth_site *new_owner)
{
	int rv;


	log_info("got CATCHUP answer for \"%s\" from %s; says owner %s with ballot %d",
			tk->name, from->addr_string,
			ticket_owner_string(new_owner), ballot);

	rv = ntohl(msg->header.result);
	if (ballot == tk->new_ballot &&
			ballot == tk->last_ack_ballot &&
			new_owner == tk->owner &&
			(rv == RLT_SUCCESS ||
			 rv == RLT_PROBABLY_SUCCESS))  {
		/* Peer says the same thing we're believing. */
		tk->proposal_acknowledges |= from->bitmask;
		tk->expires                = ntohl(msg->ticket.expiry) + time(NULL);

		if (promote_ticket_state(tk)) {
			if (tk->state == ST_INIT)
				tk->state = ST_STABLE;
		}

		disown_if_expired(tk);
		goto ex;
	}


	if (ballot >= tk->new_ballot &&
			ballot >= tk->last_ack_ballot &&
			rv == RLT_SUCCESS) {
		/* Peers seems to know better, but as yet we only have _her_
		 * word for that. */
accept:
		tk->expires               = ntohl(msg->ticket.expiry) + time(NULL);
		tk->new_ballot            = ballot;
		tk->last_ack_ballot       = ballot;
		tk->owner                 = new_owner;
		tk->proposal_acknowledges = from->bitmask;

		/* We stay in ST_INIT and wait for confirmation. */
		goto ex;
	}


	if (ballot >= tk->last_ack_ballot &&
			rv == RLT_PROBABLY_SUCCESS &&
			tk->state == ST_INIT &&
			tk->retry_number > 3) {
		/* Peer seems to know better than us, and there's no
		 * convincing other report. Just take it. */
		tk->state = ST_STABLE;
		goto accept;
	}


	if (ballot < tk->new_ballot ||
			ballot < tk->last_ack_ballot) {
		/* Peer seems outdated ... tell it to reload? */
#if 0
		init_ticket_msg(&msg, CMD_DO_CATCHUP, RLT_SUCCESS, tk, &tk->current_state);
#endif
	}


ex:
	ticket_write(tk);

	return 0;
}


/** Send new state request to all sites.
 * Perhaps this should take a flag for ACCEPTOR etc.?
 * No need currently, as all nodes are more or less identical. */
int ticket_broadcast_proposed_state(struct ticket_config *tk, cmd_request_t state)
{
	struct boothc_ticket_msg msg;

	tk->proposal_acknowledges = local->bitmask;
	tk->state                 = state;

	init_ticket_msg(&msg, state, RLT_SUCCESS, tk);

	msg.ticket.owner          = htonl(get_node_id(tk->proposed_owner));

	log_debug("broadcasting %s for ticket \"%s\"",
			STATE_STRING(state), tk->name);

	return transport()->broadcast(&msg, sizeof(msg));
}


static void ticket_cron(struct ticket_config *tk)
{
	time_t now;

	now = time(NULL);

	switch(tk->state) {
	case ST_INIT:
		/* Unknown state, ask others. */
		ticket_send_catchup(tk);
		return;


	case OP_COMMITTED:
	case ST_STABLE:

		/* Has an owner, has an expiry date, and expiry date in the past? */
		if (tk->expires &&
				tk->owner &&
				now > tk->expires) {
			log_info("LOST ticket: \"%s\" no longer at %s",
					tk->name,
					ticket_owner_string(tk->owner));

			/* Couldn't renew in time - ticket lost. */
			tk->owner = NULL;
			abort_proposal(tk);
			ticket_write(tk);

			if (tk->acquire_after)
				ticket_next_cron_in(tk, tk->acquire_after);
		}

		/* Do we need to refresh? */
		if (tk->owner == local &&
				ticket_valid_for(tk) < tk->expiry/2) {
			log_info("RENEW ticket \"%s\"", tk->name);
			paxos_start_round(tk, local);

			/* TODO: remember when we started, and restart afresh after some retries */
		}

		break;

	case OP_PREPARING:
	case OP_PROPOSING:
		tk->retry_number ++;
		if (tk->retry_number > RETRIES) {
			log_info("ABORT %s for ticket \"%s\" - "
					"not enough answers after %d retries",
					tk->state == OP_PREPARING ? "prepare" : "propose",
					tk->name, tk->retry_number);
			abort_proposal(tk);
		} else {
			/* We ask others for a change; retry to get consensus. */
			ticket_broadcast_proposed_state(tk, tk->state);
			ticket_activate_timeout(tk);
		}
		break;

	case OP_PROMISING:
	case OP_ACCEPTING:
	case OP_RECOVERY:
	case OP_REJECTED:
		break;

	default:
		break;
	}
}


void process_tickets(void)
{
	struct ticket_config *tk;
	int i;
	time_t now;

	time(&now);

	foreach_ticket(i, tk) {
		if (0)
		log_debug("ticket %s next cron %" PRIx64 ", now %" PRIx64 ", in %" PRIi64,
				tk->name, (uint64_t)tk->next_cron, (uint64_t)now,
				tk->next_cron - now);
		if (tk->next_cron > now)
			continue;

		log_debug("ticket cron: doing %s", tk->name);
		/* Set next value, handler may override. */
		tk->next_cron = INT_MAX;
		ticket_cron(tk);
	}
}



void tickets_log_info(void)
{
	struct ticket_config *tk;
	int i;

	foreach_ticket(i, tk) {
		log_info("Ticket %s: state %s "
				"mask %" PRIx64 "/%" PRIx64 " "
				"ballot %d (current %d) "
				"expires %-24.24s",
				tk->name,
				STATE_STRING(tk->state),
				tk->proposal_acknowledges,
				booth_conf->site_bits,
				tk->last_ack_ballot, tk->new_ballot,
				ctime(&tk->expires));
	}
}


/* UDP message receiver. */
int message_recv(struct boothc_ticket_msg *msg, int msglen)
{
	int cmd, rv;
	uint32_t from;
	struct booth_site *dest;
	struct ticket_config *tk;
	struct booth_site *new_owner_p;
	uint32_t ballot, new_owner;


	if (check_boothc_header(&msg->header, sizeof(*msg)) < 0 ||
			msglen != sizeof(*msg)) {
		log_error("message receive error");
		return -1;
	}

	from = ntohl(msg->header.from);
	if (!find_site_by_id(from, &dest)) {
		log_error("unknown sender: %08x", from);
		return -1;
	}

	if (!check_ticket(msg->ticket.id, &tk)) {
		log_error("got invalid ticket name \"%s\" from %s",
				msg->ticket.id, dest->addr_string);
		return -EINVAL;
	}


	cmd = ntohl(msg->header.cmd);
	ballot = ntohl(msg->ticket.ballot);

	new_owner = ntohl(msg->ticket.owner);
	if (!find_site_by_id(new_owner, &new_owner_p)) {
		log_error("Message with unknown owner %x received", new_owner);
		return -EINVAL;
	}


	switch (cmd) {
	case CMD_CATCHUP:
		return ticket_answer_catchup(tk, dest, msg, ballot, new_owner_p);

	case CMR_CATCHUP:
		return ticket_process_catchup(tk, dest, msg, ballot, new_owner_p);

	default:
		/* only used in catchup, and not even really there ?? */
		assert(ntohl(msg->header.result) == 0);


		rv = paxos_answer(tk, dest, msg, ballot, new_owner_p);
		assert((tk->proposal_acknowledges & ~booth_conf->site_bits) == 0);
		return rv;
	}
	return 0;
}
