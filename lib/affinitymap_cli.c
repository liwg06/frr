// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Affinity map northbound CLI implementation.
 *
 * Copyright 2022 Hiroki Shirokura, LINE Corporation
 * Copyright 2022 Masakazu Asama
 * Copyright 2022 6WIND S.A.
 */

#include <zebra.h>

#include "lib/command.h"
#include "lib/northbound_cli.h"
#include "lib/affinitymap.h"
#include "lib/affinitymap_cli_clippy.c"

/* max value is EXT_ADMIN_GROUP_MAX_POSITIONS - 1 */
DEFPY_YANG_NOSH(affinity_map, affinity_map_cmd,
		"affinity-map NAME$name bit-position (0-1023)$position",
		"Affinity map configuration\n"
		"Affinity attribute name\n"
		"Bit position for affinity attribute value\n"
		"Bit position\n")
{
	char xpathr[XPATH_MAXLEN];

	snprintf(
		xpathr, sizeof(xpathr),
		"/frr-affinity-map:lib/affinity-maps/affinity-map[name='%s']/value",
		name);
	nb_cli_enqueue_change(vty, xpathr, NB_OP_MODIFY, position_str);
	return nb_cli_apply_changes(vty, NULL);
}

/* max value is EXT_ADMIN_GROUP_MAX_POSITIONS - 1 */
DEFPY_YANG_NOSH(no_affinity_map, no_affinity_map_cmd,
		"no affinity-map NAME$name [bit-position (0-1023)$position]",
		NO_STR
		"Affinity map configuration\n"
		"Affinity attribute name\n"
		"Bit position for affinity attribute value\n"
		"Bit position\n")
{
	char xpathr[XPATH_MAXLEN];

	snprintf(xpathr, sizeof(xpathr),
		 "/frr-affinity-map:lib/affinity-maps/affinity-map[name='%s']",
		 name);
	nb_cli_enqueue_change(vty, xpathr, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

static void cli_show_affinity_map(struct vty *vty, const struct lyd_node *dnode,
			   bool show_defaults __attribute__((__unused__)))
{
	vty_out(vty, "affinity-map %s bit-position %u\n",
		yang_dnode_get_string(dnode, "name"),
		yang_dnode_get_uint16(dnode, "value"));
}

const struct frr_yang_module_info frr_affinity_map_cli_info = {
	.name = "frr-affinity-map",
	.ignore_cfg_cbs = true,
	.nodes = {
		{
			.xpath = "/frr-affinity-map:lib/affinity-maps/affinity-map",
			.cbs.cli_show = cli_show_affinity_map,
		},
		{
			.xpath = NULL,
		},
	}
};

/* Initialization of affinity map vector. */
void affinity_map_init(void)
{
	/* CLI commands. */
	install_element(CONFIG_NODE, &affinity_map_cmd);
	install_element(CONFIG_NODE, &no_affinity_map_cmd);
}
