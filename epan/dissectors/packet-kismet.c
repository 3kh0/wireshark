/* packet-kismet.c
 * Routines for kismet packet dissection
 * Copyright 2006, Krzysztof Burghardt <krzysztof@burghardt.pl>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * Copied from packet-pop.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdlib.h>
#include <epan/packet.h>
#include <epan/to_str.h>
#include <epan/strutil.h>
#include <epan/expert.h>
#include <wsutil/strtoi.h>

static int proto_kismet = -1;
static int hf_kismet_response = -1;
static int hf_kismet_request = -1;
static int hf_kismet_version = -1;
static int hf_kismet_start_time = -1;
static int hf_kismet_server_name = -1;
static int hf_kismet_build_revision = -1;
static int hf_kismet_unknown_field = -1;
static int hf_kismet_extended_version_string = -1;
static int hf_kismet_time = -1;

static gint ett_kismet = -1;
static gint ett_kismet_reqresp = -1;

static expert_field ei_time_invalid = EI_INIT;

#define TCP_PORT_KISMET	2501 /* Not IANA registered */

static gboolean response_is_continuation(const guchar * data);
void proto_reg_handoff_kismet(void);
void proto_register_kismet(void);

static int
dissect_kismet(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, void * data _U_)
{
	gboolean is_request;
	gboolean is_continuation;
	proto_tree *kismet_tree=NULL, *reqresp_tree=NULL;
	proto_item *ti;
	proto_item *tmp_item;
	gint offset = 0;
	const guchar *line;
	gint next_offset;
	int linelen;
	int tokenlen;
	int i;
	const guchar *next_token;

	/*
	 * Find the end of the first line.
	 *
	 * Note that "tvb_find_line_end()" will return a value that is
	 * not longer than what's in the buffer, so the "tvb_get_ptr()"
	 * call won't throw an exception.
	 */
	linelen = tvb_find_line_end(tvb, offset, -1, &next_offset, FALSE);
	line = tvb_get_ptr(tvb, offset, linelen);

	/*
	 * Check if it is an ASCII based protocol with reasonable length
	 * packets, if not return, and try another dissector.
	 */
	if (linelen < 8) {
		/*
		 * Packet is too short
		 */
		return 0;
	} else {
		for (i = 0; i < 8; ++i) {
			/*
			 * Packet contains non-ASCII data
			 */
			if (line[i] < 32 || line[i] > 128)
				return 0;
		}
	}

	/*
	 * If it is Kismet traffic set COL_PROTOCOL.
	 */
	col_set_str(pinfo->cinfo, COL_PROTOCOL, "kismet");

	/*
	 * Check if it is request, reply or continuation.
	 */
	if (pinfo->match_uint == pinfo->destport) {
		is_request = TRUE;
		is_continuation = FALSE;
	} else {
		is_request = FALSE;
		is_continuation = response_is_continuation (line);
	}

	/*
	 * Put the first line from the buffer into the summary
	 * if it's a kismet request or reply (but leave out the
	 * line terminator).
	 * Otherwise, just call it a continuation.
	 */
	if (is_continuation)
		col_set_str(pinfo->cinfo, COL_INFO, "Continuation");
	else
		col_add_fstr(pinfo->cinfo, COL_INFO, "%s: %s",
				is_request ? "Request" : "Response",
				format_text(pinfo->pool, line, linelen));

	if (tree) {
		ti = proto_tree_add_item(tree, proto_kismet, tvb, offset, -1, ENC_NA);
		kismet_tree = proto_item_add_subtree(ti, ett_kismet);
	}

	if (is_continuation) {
		/*
		 * Put the whole packet into the tree as data.
		 */
		call_data_dissector(tvb, pinfo, kismet_tree);
		return tvb_captured_length(tvb);
	}

	if (is_request) {
		tmp_item = proto_tree_add_boolean(kismet_tree,
				hf_kismet_request, tvb, 0, 0, TRUE);
	} else {
		tmp_item = proto_tree_add_boolean(kismet_tree,
				hf_kismet_response, tvb, 0, 0, TRUE);
	}
	proto_item_set_generated (tmp_item);

	while (tvb_offset_exists(tvb, offset)) {
		/*
		 * Find the end of the line.
		 */
		linelen = tvb_find_line_end(tvb, offset, -1, &next_offset, FALSE);

		if (linelen) {
			/*
			 * Put this line.
			 */
			reqresp_tree = proto_tree_add_subtree(kismet_tree, tvb, offset,
					next_offset - offset, ett_kismet_reqresp, NULL,
					tvb_format_text(tvb, offset,
					next_offset - offset - 1));
			tokenlen = get_token_len(line, line + linelen, &next_token);
			if (tokenlen != 0) {
				guint8 *reqresp;
				reqresp = tvb_get_string_enc(pinfo->pool, tvb, offset, tokenlen, ENC_ASCII);
				if (is_request) {
					/*
					 * No request dissection
					 */
				} else {
					/*
					 * *KISMET: {Version} {Start time} \001{Server name}\001 {Build Revision}
					 * two fields left undocumented: {???} {?ExtendedVersion?}
					 */
					if (!strncmp(reqresp, "*KISMET", 7)) {
						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);
						proto_tree_add_string(reqresp_tree, hf_kismet_version, tvb, offset,
							tokenlen, format_text(pinfo->pool, line, tokenlen));

						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);
						proto_tree_add_string(reqresp_tree, hf_kismet_start_time, tvb, offset,
							tokenlen, format_text(pinfo->pool, line, tokenlen));

						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);
						proto_tree_add_string(reqresp_tree, hf_kismet_server_name, tvb, offset,
							tokenlen, format_text(pinfo->pool, line + 1, tokenlen - 2));

						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);
						proto_tree_add_string(reqresp_tree, hf_kismet_build_revision, tvb, offset,
							tokenlen, format_text(pinfo->pool, line, tokenlen));

						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);
						proto_tree_add_string(reqresp_tree, hf_kismet_unknown_field, tvb, offset,
							tokenlen, format_text(pinfo->pool, line, tokenlen));

						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);
						proto_tree_add_string(reqresp_tree, hf_kismet_extended_version_string, tvb, offset,
							tokenlen, format_text(pinfo->pool, line, tokenlen));
					}
					/*
					 * *TIME: {Time}
					 */
					if (!strncmp(reqresp, "*TIME", 5)) {
						nstime_t t;
						char *ptr = NULL;
						proto_tree* time_item;

						t.nsecs = 0;

						offset += (gint) (next_token - line);
						linelen -= (int) (next_token - line);
						line = next_token;
						tokenlen = get_token_len(line, line + linelen, &next_token);

						/* Convert form ascii to nstime */
						if (ws_strtou64(format_text(pinfo->pool, line, tokenlen), NULL, (guint64*)&t.secs)) {

							/*
							 * Format ascii representation of time
							 */
							ptr = abs_time_secs_to_str(pinfo->pool, t.secs, ABSOLUTE_TIME_LOCAL, TRUE);
						}
						time_item = proto_tree_add_time_format_value(reqresp_tree, hf_kismet_time, tvb, offset,
							tokenlen, &t, "%s", ptr ? ptr : "");
						if (!ptr)
							expert_add_info(pinfo, time_item, &ei_time_invalid);
					}
				}

				/*offset += (gint) (next_token - line);
				linelen -= (int) (next_token - line);*/
				line = next_token;
			}
		}
		offset = next_offset;
	}

	return tvb_captured_length(tvb);
}

static gboolean
response_is_continuation(const guchar * data)
{
	if (!strncmp(data, "*", 1))
		return FALSE;

	if (!strncmp(data, "!", 1))
		return FALSE;

	return TRUE;
}

void
proto_register_kismet(void)
{
	static hf_register_info hf[] = {
		{&hf_kismet_response,
		{"Response", "kismet.response", FT_BOOLEAN, BASE_NONE,
		NULL, 0x0, "TRUE if kismet response", HFILL}},

		{&hf_kismet_request,
		{"Request", "kismet.request", FT_BOOLEAN, BASE_NONE,
		NULL, 0x0, "TRUE if kismet request", HFILL}},

		{&hf_kismet_version,
		{"Version", "kismet.version", FT_STRING, BASE_NONE,
		NULL, 0x0, NULL, HFILL}},

		{&hf_kismet_start_time,
		{"Start time", "kismet.start_time", FT_STRING, BASE_NONE,
		NULL, 0x0, NULL, HFILL}},

		{&hf_kismet_server_name,
		{"Server name", "kismet.server_name", FT_STRING, BASE_NONE,
		NULL, 0x0, NULL, HFILL}},

		{&hf_kismet_build_revision,
		{"Build revision", "kismet.build_revision", FT_STRING, BASE_NONE,
		NULL, 0x0, NULL, HFILL}},

		{&hf_kismet_unknown_field,
		{"Unknown field", "kismet.unknown_field", FT_STRING, BASE_NONE,
		NULL, 0x0, NULL, HFILL}},

		{&hf_kismet_extended_version_string,
		{"Extended version string", "kismet.extended_version_string", FT_STRING, BASE_NONE,
		NULL, 0x0, NULL, HFILL}},

		{&hf_kismet_time,
		{"Time", "kismet.time", FT_ABSOLUTE_TIME, ABSOLUTE_TIME_LOCAL,
		NULL, 0x0, NULL, HFILL}},
	};

	static ei_register_info ei[] = {
		{ &ei_time_invalid, { "kismet.time.invalid", PI_PROTOCOL, PI_WARN, "Invalid time", EXPFILL }}
	};

	static gint *ett[] = {
		&ett_kismet,
		&ett_kismet_reqresp,
	};
	expert_module_t* expert_kismet;

	proto_kismet = proto_register_protocol("Kismet Client/Server Protocol", "Kismet", "kismet");
	proto_register_field_array(proto_kismet, hf, array_length (hf));
	proto_register_subtree_array(ett, array_length (ett));
	expert_kismet = expert_register_protocol(proto_kismet);
	expert_register_field_array(expert_kismet, ei, array_length(ei));
}

void
proto_reg_handoff_kismet(void)
{
	dissector_handle_t kismet_handle;

	kismet_handle = create_dissector_handle(dissect_kismet, proto_kismet);

	dissector_add_uint_with_preference("tcp.port", TCP_PORT_KISMET, kismet_handle);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
