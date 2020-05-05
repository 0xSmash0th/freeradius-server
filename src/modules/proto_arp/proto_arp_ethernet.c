/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file proto_arp_udp.c
 * @brief RADIUS handler for UDP.
 *
 * @copyright 2016 The FreeRADIUS server project.
 * @copyright 2016 Alan DeKok (aland@deployingradius.com)
 */
#include <netdb.h>
#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/protocol.h>
#include <freeradius-devel/util/udp.h>
#include <freeradius-devel/util/trie.h>
#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/io/base.h>
#include <freeradius-devel/io/application.h>
#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/io/schedule.h>
#include <freeradius-devel/util/debug.h>

#include "proto_arp.h"

extern fr_app_io_t proto_arp_ethernet;

typedef struct {
	char const			*name;			//!< socket name
	fr_pcap_t			*pcap;			//!< PCAP handler
} proto_arp_ethernet_thread_t;

typedef struct {
	CONF_SECTION			*cs;			//!< our configuration
	char const			*interface;		//!< Interface to bind to.
} proto_arp_ethernet_t;


/** How to parse an ARP listen section
 *
 */
static CONF_PARSER const arp_listen_config[] = {
	{ FR_CONF_OFFSET("interface", FR_TYPE_STRING | FR_TYPE_NOT_EMPTY, proto_arp_ethernet_t,
			  interface), .dflt = "eth0" },

	/*
	 *	@todo - allow for pcap filter
	 */

	CONF_PARSER_TERMINATOR
};

static ssize_t mod_read(fr_listen_t *li, UNUSED void **packet_ctx, fr_time_t *recv_time_p, uint8_t *buffer, size_t buffer_len, size_t *leftover, UNUSED uint32_t *priority, UNUSED bool *is_dup)
{
	proto_arp_ethernet_thread_t	*thread = talloc_get_type_abort(li->thread_instance, proto_arp_ethernet_thread_t);
	int				ret;
	uint8_t const			*data;
	struct pcap_pkthdr		*header;
	uint8_t const			*p, *end;
	ssize_t				len;

	*leftover = 0;		/* always for message oriented protocols */

	ret = pcap_next_ex(thread->pcap->handle, &header, &data);
	if (ret == 0) return 0;
	if (ret < 0) {
		DEBUG("Failed getting next PCAP packet");
		return 0;
	}

	p = data;
	end = data + header->caplen;

	len = fr_pcap_link_layer_offset(data, header->caplen, thread->pcap->link_layer);
	if (len < 0) {
		DEBUG("Failed determining link layer header offset");
		return 0;
	}
	p += len;

	if ((end - p) < FR_ARP_PACKET_SIZE) {
		DEBUG("Packet is too small (%d) to be ARP", (int) (end - p));
		return 0;
	}

	fr_assert(buffer_len >= FR_ARP_PACKET_SIZE);

	memcpy(buffer, p, FR_ARP_PACKET_SIZE);

	// @todo - talloc packet_ctx which is the ethernet header, so we know what kind of VLAN, etc. to encode?

	*recv_time_p = fr_time();
	return FR_ARP_PACKET_SIZE;
}


static ssize_t mod_write(fr_listen_t *li, UNUSED void *packet_ctx, UNUSED fr_time_t request_time,
			 uint8_t *buffer, size_t buffer_len, UNUSED size_t written)
{
	proto_arp_ethernet_thread_t	*thread = talloc_get_type_abort(li->thread_instance, proto_arp_ethernet_thread_t);

	DEBUG("Fake write ARP");

	/*
	 *	@todo - mirror src/protocols/dhcpv4/pcap.c for ARP send / receive.
	 *	We will need that functionality for rlm_arp, too.
	 */

	return FR_ARP_PACKET_SIZE;
}

/** Open a pcap file for ARP
 *
 */
static int mod_open(fr_listen_t *li)
{
	proto_arp_ethernet_t const      *inst = talloc_get_type_abort_const(li->app_io_instance, proto_arp_ethernet_t);
	proto_arp_ethernet_thread_t	*thread = talloc_get_type_abort(li->thread_instance, proto_arp_ethernet_thread_t);

	CONF_SECTION			*server_cs;
	CONF_ITEM			*ci;

	thread->pcap = fr_pcap_init(thread, inst->interface, PCAP_INTERFACE_IN);
	if (!thread->pcap) {
		PERROR("Failed initializing pcap handle.");
		return -1;
	}

	if (fr_pcap_open(thread->pcap) < 0) {
		PERROR("Failed opening interface %s", inst->interface);
		return -1;
	}

	/*
	 *	Ensure that we only get ARP.
	 *
	 *	@todo - only get ARP requests?
	 */
	if (fr_pcap_apply_filter(thread->pcap, "arp") < 0) {
		PERROR("Failed applying pcap filter");
		return -1;
	}

	li->fd = thread->pcap->fd;

	ci = cf_parent(inst->cs); /* listen { ... } */
	fr_assert(ci != NULL);
	server_cs = cf_item_to_section(ci);

	thread->name = talloc_asprintf(thread, "proto arp on interface %s", inst->interface);

	DEBUG("Listening on %s bound to virtual server %s",
	      thread->name, cf_section_name2(server_cs));

	return 0;
}

static char const *mod_name(fr_listen_t *li)
{
	proto_arp_ethernet_thread_t	*thread = talloc_get_type_abort(li->thread_instance, proto_arp_ethernet_thread_t);

	return thread->name;
}


static int mod_bootstrap(void *instance, CONF_SECTION *cs)
{
	proto_arp_ethernet_t	*inst = talloc_get_type_abort(instance, proto_arp_ethernet_t);

	inst->cs = cs;

	return 0;
}


fr_app_io_t proto_arp_ethernet = {
	.magic			= RLM_MODULE_INIT,
	.name			= "arp_ethernet",
	.config			= arp_listen_config,
	.inst_size		= sizeof(proto_arp_ethernet_t),
	.thread_inst_size	= sizeof(proto_arp_ethernet_thread_t),
	.bootstrap		= mod_bootstrap,

	.default_message_size	= FR_ARP_PACKET_SIZE,

	.open			= mod_open,
	.read			= mod_read,
	.write			= mod_write,
	.get_name      		= mod_name,
};