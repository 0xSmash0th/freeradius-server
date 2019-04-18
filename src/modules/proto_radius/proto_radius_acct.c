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
 * @file proto_radius_acct.c
 * @brief RADIUS accounting processing.
 *
 * @copyright 2016 The FreeRADIUS server project.
 * @copyright 2016 Alan DeKok (aland@deployingradius.com)
 */
#include <freeradius-devel/io/application.h>
#include <freeradius-devel/server/protocol.h>
#include <freeradius-devel/server/module.h>
#include <freeradius-devel/unlang/base.h>
#include <freeradius-devel/util/dict.h>
#include <freeradius-devel/server/rad_assert.h>

static fr_dict_t *dict_radius;

extern fr_dict_autoload_t proto_radius_acct_dict[];
fr_dict_autoload_t proto_radius_acct_dict[] = {
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_packet_type;
static fr_dict_attr_t const *attr_acct_status_type;

extern fr_dict_attr_autoload_t proto_radius_acct_dict_attr[];
fr_dict_attr_autoload_t proto_radius_acct_dict_attr[] = {
	{ .out = &attr_packet_type, .name = "Packet-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_acct_status_type, .name = "Acct-Status-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ NULL }
};


static fr_io_final_t mod_process(UNUSED void const *instance, REQUEST *request, fr_io_action_t action)
{
	VALUE_PAIR 	*vp;
	rlm_rcode_t	rcode;
	CONF_SECTION	*unlang;
	fr_dict_enum_t	const *dv;

	REQUEST_VERIFY(request);

	/*
	 *	Pass this through asynchronously to the module which
	 *	is waiting for something to happen.
	 */
	if (action != FR_IO_ACTION_RUN) {
		unlang_interpret_signal(request, (fr_state_signal_t) action);
		return FR_IO_DONE;
	}

	switch (request->request_state) {
	case REQUEST_INIT:
		if (request->parent && RDEBUG_ENABLED) {
			RDEBUG("Received %s ID %i", fr_packet_codes[request->packet->code], request->packet->id);
			log_request_pair_list(L_DBG_LVL_1, request, request->packet->vps, "");
		}

		request->component = "radius";

		unlang = cf_section_find(request->server_cs, "recv", "Accounting-Request");
		if (!unlang) {
			REDEBUG("Failed to find 'recv Accounting-Request' section");
			return FR_IO_FAIL;
		}

		RDEBUG("Running 'recv Accounting-Request' from file %s", cf_filename(unlang));
		unlang_interpret_push_section(request, unlang, RLM_MODULE_NOOP, UNLANG_TOP_FRAME);

		request->request_state = REQUEST_RECV;
		/* FALL-THROUGH */

	case REQUEST_RECV:
		rcode = unlang_interpret_resume(request);

		if (request->master_state == REQUEST_STOP_PROCESSING) return FR_IO_DONE;

		if (rcode == RLM_MODULE_YIELD) return FR_IO_YIELD;

		rad_assert(request->log.unlang_indent == 0);

		switch (rcode) {
		/*
		 *	The module has a number of OK return codes.
		 */
		case RLM_MODULE_NOOP:
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
		case RLM_MODULE_HANDLED:
			request->reply->code = FR_CODE_ACCOUNTING_RESPONSE;
			break;

		/*
		 *	The module failed, or said the request is
		 *	invalid, therefore we stop here.
		 */
		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		case RLM_MODULE_NOTFOUND:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
		default:
			return FR_IO_FAIL;
		}

		/*
		 *	Run Acct-Status-Type foo { ... }
		 */
		vp = fr_pair_find_by_da(request->packet->vps, attr_acct_status_type, TAG_ANY);
		if (!vp) goto setup_send;

		dv = fr_dict_enum_by_value(vp->da, &vp->data);
		if (!dv) goto setup_send;

		unlang = cf_section_find(request->server_cs, "Acct-Status-Type", dv->alias);
		if (!unlang) {
			REDEBUG2("No 'Acct-Status-Type %s' section found: Ignoring it.", dv->alias);
			goto setup_send;
		}

		RDEBUG("Running 'Acct-Status-Type %s' from file %s", cf_section_name2(unlang), cf_filename(unlang));
		unlang_interpret_push_section(request, unlang, RLM_MODULE_NOTFOUND, UNLANG_TOP_FRAME);

		request->request_state = REQUEST_PROCESS;
		/* FALL-THROUGH */

	case REQUEST_PROCESS:
		rcode = unlang_interpret_resume(request);

		if (request->master_state == REQUEST_STOP_PROCESSING) return FR_IO_DONE;

		if (rcode == RLM_MODULE_YIELD) return FR_IO_YIELD;

		rad_assert(request->log.unlang_indent == 0);

		switch (rcode) {
		/*
		 *	The module has a number of OK return codes.
		 */
		case RLM_MODULE_NOOP:
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
		case RLM_MODULE_HANDLED:
			break;

		/*
		 *	The module failed, or said the request is
		 *	invalid, therefore we stop here.
		 */
		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		case RLM_MODULE_NOTFOUND:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
		default:
			return FR_IO_FAIL;
		}

	setup_send:
		/*
		 *	Allow for over-ride of reply code.
		 */
		vp = fr_pair_find_by_da(request->reply->vps, attr_packet_type, TAG_ANY);
		if (vp) request->reply->code = vp->vp_uint32;

		dv = fr_dict_enum_by_value(attr_packet_type, fr_box_uint32(request->reply->code));
		unlang = NULL;
		if (dv) unlang = cf_section_find(request->server_cs, "send", dv->alias);

		if (!unlang) goto send_reply;

		RDEBUG("Running 'send %s' from file %s", cf_section_name2(unlang), cf_filename(unlang));
		unlang_interpret_push_section(request, unlang, RLM_MODULE_NOOP, UNLANG_TOP_FRAME);

		request->request_state = REQUEST_SEND;
		/* FALL-THROUGH */

	case REQUEST_SEND:
		rcode = unlang_interpret_resume(request);

		if (request->master_state == REQUEST_STOP_PROCESSING) return FR_IO_DONE;

		if (rcode == RLM_MODULE_YIELD) return FR_IO_YIELD;

		rad_assert(request->log.unlang_indent == 0);

		switch (rcode) {
		case RLM_MODULE_NOOP:
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
		case RLM_MODULE_HANDLED:
			/* reply is already set */
			break;

		default:
			request->reply->code = FR_CODE_DO_NOT_RESPOND;
			break;
		}

	send_reply:
		gettimeofday(&request->reply->timestamp, NULL);

		/*
		 *	Check for "do not respond".
		 */
		if (request->reply->code == FR_CODE_DO_NOT_RESPOND) {
			RDEBUG("Not sending reply to client.");
			break;
		}

		if (request->parent && RDEBUG_ENABLED) {
			RDEBUG("Sending %s ID %i", fr_packet_codes[request->reply->code], request->reply->id);
			log_request_pair_list(L_DBG_LVL_1, request, request->reply->vps, "");
		}
		break;

	default:
		return FR_IO_FAIL;
	}

	return FR_IO_REPLY;
}


static virtual_server_compile_t compile_list[] = {
	{ "recv", "Accounting-Request",	MOD_PREACCT },
	{ "send", "Accounting-Response", MOD_ACCOUNTING },
	{ "send", "Do-Not-Respond",	MOD_POST_AUTH },
	{ "send", "Protocol-Error",    	MOD_POST_AUTH },
	{ "Acct-Status-Type", CF_IDENT_ANY, MOD_ACCOUNTING },

	COMPILE_TERMINATOR
};

static int mod_instantiate(UNUSED void *instance, CONF_SECTION *process_app_cs)
{
	CONF_SECTION		*listen_cs = cf_item_to_section(cf_parent(process_app_cs));
	CONF_SECTION		*server_cs;
	vp_tmpl_rules_t		parse_rules;

	memset(&parse_rules, 0, sizeof(parse_rules));
	parse_rules.dict_def = dict_radius;

	rad_assert(listen_cs);

	server_cs = cf_item_to_section(cf_parent(listen_cs));
	rad_assert(strcmp(cf_section_name1(server_cs), "server") == 0);

	return virtual_server_compile_sections(server_cs, compile_list, &parse_rules);
}


extern fr_app_worker_t proto_radius_acct;
fr_app_worker_t proto_radius_acct = {
	.magic		= RLM_MODULE_INIT,
	.name		= "radius_acct",
	.instantiate	= mod_instantiate,
	.entry_point	= mod_process,
};
