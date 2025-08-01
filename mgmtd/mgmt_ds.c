// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MGMTD Datastores
 *
 * Copyright (C) 2021  Vmware, Inc.
 *		       Pushpasis Sarkar <spushpasis@vmware.com>
 */

#include <zebra.h>
#include "md5.h"
#include "mgmtd/mgmt.h"
#include "mgmtd/mgmt_memory.h"
#include "mgmtd/mgmt_ds.h"
#include "mgmtd/mgmt_history.h"
#include "mgmtd/mgmt_txn.h"
#include "libyang/libyang.h"

#define _dbg(fmt, ...)	   DEBUGD(&mgmt_debug_ds, "DS: %s: " fmt, __func__, ##__VA_ARGS__)
#define _log_err(fmt, ...) zlog_err("%s: ERROR: " fmt, __func__, ##__VA_ARGS__)

struct mgmt_ds_ctx {
	enum mgmt_ds_id ds_id;

	bool locked;
	uint64_t vty_session_id; /* Owner of the lock or 0 */

	bool config_ds;

	union {
		struct nb_config *cfg_root;
		struct lyd_node *dnode_root;
	} root;
};

const char *mgmt_ds_names[MGMTD_DS_MAX_ID + 1] = {
	MGMTD_DS_NAME_NONE,	/* MGMTD_DS_NONE */
	MGMTD_DS_NAME_RUNNING,     /* MGMTD_DS_RUNNING */
	MGMTD_DS_NAME_CANDIDATE,   /* MGMTD_DS_CANDIDATE */
	MGMTD_DS_NAME_OPERATIONAL, /* MGMTD_DS_OPERATIONAL */
	"Unknown/Invalid",	 /* MGMTD_DS_ID_MAX */
};

/* Make sure that the datastore IDs match with the ones in mgmt_msg_native.h */
_Static_assert(MGMTD_DS_NONE == MGMT_MSG_DATASTORE_NONE, "Datastore ID mismatch");
_Static_assert(MGMTD_DS_RUNNING == MGMT_MSG_DATASTORE_RUNNING, "Datastore ID mismatch");
_Static_assert(MGMTD_DS_CANDIDATE == MGMT_MSG_DATASTORE_CANDIDATE, "Datastore ID mismatch");
_Static_assert(MGMTD_DS_OPERATIONAL == MGMT_MSG_DATASTORE_OPERATIONAL, "Datastore ID mismatch");


static struct mgmt_master *mgmt_ds_mm;
static struct mgmt_ds_ctx running, candidate, oper;

/* Dump the data tree of the specified format in the file pointed by the path */
static int mgmt_ds_dump_in_memory(struct mgmt_ds_ctx *ds_ctx,
				  const char *base_xpath, LYD_FORMAT format,
				  struct ly_out *out)
{
	struct lyd_node *root;
	uint32_t options = 0;

	if (base_xpath[0] == '\0')
		root = ds_ctx->config_ds ? ds_ctx->root.cfg_root->dnode
					  : ds_ctx->root.dnode_root;
	else
		root = yang_dnode_get(ds_ctx->config_ds
					      ? ds_ctx->root.cfg_root->dnode
					      : ds_ctx->root.dnode_root,
				      base_xpath);
	if (!root)
		return -1;

	options = ds_ctx->config_ds ? LYD_PRINT_WD_TRIM :
		LYD_PRINT_WD_EXPLICIT;

	if (base_xpath[0] == '\0')
		lyd_print_all(out, root, format, options);
	else
		lyd_print_tree(out, root, format, options);

	return 0;
}

static int ds_copy(struct mgmt_ds_ctx *dst, struct mgmt_ds_ctx *src)
{
	if (!src || !dst)
		return -1;

	_dbg("Replacing %s with %s", mgmt_ds_id2name(dst->ds_id), mgmt_ds_id2name(src->ds_id));

	if (src->config_ds && dst->config_ds)
		nb_config_replace(dst->root.cfg_root, src->root.cfg_root, true);
	else {
		assert(!src->config_ds && !dst->config_ds);
		if (dst->root.dnode_root)
			yang_dnode_free(dst->root.dnode_root);
		dst->root.dnode_root = yang_dnode_dup(src->root.dnode_root);
	}

	return 0;
}

static int ds_merge(struct mgmt_ds_ctx *dst, struct mgmt_ds_ctx *src)
{
	int ret;

	if (!src || !dst)
		return -1;

	_dbg("Merging DS %d with %d", dst->ds_id, src->ds_id);
	if (src->config_ds && dst->config_ds)
		ret = nb_config_merge(dst->root.cfg_root, src->root.cfg_root,
				      true);
	else {
		assert(!src->config_ds && !dst->config_ds);
		ret = lyd_merge_siblings(&dst->root.dnode_root,
					 src->root.dnode_root, 0);
	}
	if (ret != 0) {
		_log_err("merge failed with err: %d", ret);
		return ret;
	}

	return 0;
}

static int mgmt_ds_load_cfg_from_file(const char *filepath,
				      struct lyd_node **dnode)
{
	LY_ERR ret;

	*dnode = NULL;
	ret = lyd_parse_data_path(ly_native_ctx, filepath, LYD_JSON,
				  LYD_PARSE_NO_STATE | LYD_PARSE_STRICT,
				  LYD_VALIDATE_NO_STATE, dnode);

	if (ret != LY_SUCCESS) {
		if (*dnode)
			yang_dnode_free(*dnode);
		return -1;
	}

	return 0;
}

void mgmt_ds_reset_candidate(void)
{
	struct lyd_node *dnode = mm->candidate_ds->root.cfg_root->dnode;

	if (dnode)
		yang_dnode_free(dnode);

	dnode = yang_dnode_new(ly_native_ctx, true);
	mm->candidate_ds->root.cfg_root->dnode = dnode;
}


int mgmt_ds_init(struct mgmt_master *m)
{
	if (mgmt_ds_mm || m->running_ds || m->candidate_ds || m->oper_ds)
		assert(!"MGMTD: Call ds_init only once!");

	/* Use Running DS from NB module??? */
	if (!running_config)
		assert(!"MGMTD: Call ds_init after frr_init only!");

	running.root.cfg_root = running_config;
	running.config_ds = true;
	running.ds_id = MGMTD_DS_RUNNING;

	candidate.root.cfg_root = nb_config_dup(running.root.cfg_root);
	candidate.config_ds = true;
	candidate.ds_id = MGMTD_DS_CANDIDATE;

	/*
	 * Redirect lib/vty candidate-config datastore to the global candidate
	 * config Ds on the MGMTD process.
	 */
	vty_mgmt_candidate_config = candidate.root.cfg_root;

	oper.root.dnode_root = yang_dnode_new(ly_native_ctx, true);
	oper.config_ds = false;
	oper.ds_id = MGMTD_DS_OPERATIONAL;

	m->running_ds = &running;
	m->candidate_ds = &candidate;
	m->oper_ds = &oper;
	mgmt_ds_mm = m;

	return 0;
}

void mgmt_ds_destroy(void)
{
	nb_config_free(candidate.root.cfg_root);
	candidate.root.cfg_root = NULL;

	yang_dnode_free(oper.root.dnode_root);
	oper.root.dnode_root = NULL;
}

struct mgmt_ds_ctx *mgmt_ds_get_ctx_by_id(struct mgmt_master *m, enum mgmt_ds_id ds_id)
{
	switch (ds_id) {
	case MGMTD_DS_CANDIDATE:
		return (m->candidate_ds);
	case MGMTD_DS_RUNNING:
		return (m->running_ds);
	case MGMTD_DS_OPERATIONAL:
		return (m->oper_ds);
	case MGMTD_DS_NONE:
		return 0;
	}

	return 0;
}

bool mgmt_ds_is_config(struct mgmt_ds_ctx *ds_ctx)
{
	if (!ds_ctx)
		return false;

	return ds_ctx->config_ds;
}

bool mgmt_ds_is_locked(struct mgmt_ds_ctx *ds_ctx, uint64_t *session_id)
{
	if (!ds_ctx || !ds_ctx->locked)
		return false;

	if (session_id)
		*session_id = ds_ctx->vty_session_id;
	return true;
}

int mgmt_ds_lock(struct mgmt_ds_ctx *ds_ctx, uint64_t session_id)
{
	assert(ds_ctx);

	if (ds_ctx->locked) {
		_log_err("lock already taken on DS:%s by session-id %Lu",
			 mgmt_ds_id2name(ds_ctx->ds_id), ds_ctx->vty_session_id);
		return EBUSY;
	}

	_dbg("obtaining lock on DS:%s for session-id %Lu", mgmt_ds_id2name(ds_ctx->ds_id),
	     session_id);

	ds_ctx->locked = true;
	ds_ctx->vty_session_id = session_id;
	return 0;
}

void mgmt_ds_unlock(struct mgmt_ds_ctx *ds_ctx)
{
	assert(ds_ctx);
	if (!ds_ctx->locked)
		_log_err("unlock on unlocked in DS:%s last session-id %Lu",
			 mgmt_ds_id2name(ds_ctx->ds_id), ds_ctx->vty_session_id);
	else
		_dbg("releasing lock on DS:%s for session-id %Lu", mgmt_ds_id2name(ds_ctx->ds_id),
		     ds_ctx->vty_session_id);

	ds_ctx->locked = 0;
	ds_ctx->vty_session_id = MGMTD_SESSION_ID_NONE;
}

int mgmt_ds_copy_dss(struct mgmt_ds_ctx *dst, struct mgmt_ds_ctx *src, bool updt_cmt_rec)
{
	if (ds_copy(dst, src) != 0)
		return -1;

	if (updt_cmt_rec && dst->ds_id == MGMTD_DS_RUNNING)
		mgmt_history_new_record(dst);

	return 0;
}

int mgmt_ds_dump_ds_to_file(char *file_name, struct mgmt_ds_ctx *ds_ctx)
{
	struct ly_out *out;
	int ret = 0;

	if (ly_out_new_filepath(file_name, &out) == LY_SUCCESS) {
		ret = mgmt_ds_dump_in_memory(ds_ctx, "", LYD_JSON, out);
		ly_out_free(out, NULL, 0);
	}

	return ret;
}

struct nb_config *mgmt_ds_get_nb_config(struct mgmt_ds_ctx *ds_ctx)
{
	if (!ds_ctx)
		return NULL;

	return ds_ctx->config_ds ? ds_ctx->root.cfg_root : NULL;
}

static int mgmt_walk_ds_nodes(
	struct nb_config *root, const char *base_xpath,
	struct lyd_node *base_dnode,
	void (*mgmt_ds_node_iter_fn)(const char *xpath, struct lyd_node *node,
				     struct nb_node *nb_node, void *ctx),
	void *ctx)
{
	/* this is 1k per recursion... */
	char xpath[MGMTD_MAX_XPATH_LEN];
	struct lyd_node *dnode;
	struct nb_node *nbnode;
	int ret = 0;

	assert(mgmt_ds_node_iter_fn);

	_dbg(" -- START: base xpath: '%s'", base_xpath);

	if (!base_dnode)
		/*
		 * This function only returns the first node of a possible set
		 * of matches issuing a warning if more than 1 matches
		 */
		base_dnode = yang_dnode_get(root->dnode, base_xpath);
	if (!base_dnode)
		return -1;

	_dbg("           search base schema: '%s'",
	     lysc_path(base_dnode->schema, LYSC_PATH_LOG, xpath, sizeof(xpath)));

	nbnode = (struct nb_node *)base_dnode->schema->priv;
	(*mgmt_ds_node_iter_fn)(base_xpath, base_dnode, nbnode, ctx);

	/*
	 * If the base_xpath points to a leaf node we can skip the tree walk.
	 */
	if (base_dnode->schema->nodetype & LYD_NODE_TERM)
		return 0;

	/*
	 * at this point the xpath matched this container node (or some parent
	 * and we're wildcard descending now) so by walking it's children we
	 * continue to change the meaning of an xpath regex to rather be a
	 * prefix matching path
	 */

	LY_LIST_FOR (lyd_child(base_dnode), dnode) {
		assert(dnode->schema && dnode->schema->priv);

		(void)lyd_path(dnode, LYD_PATH_STD, xpath, sizeof(xpath));

		_dbg(" -- Child xpath: %s", xpath);

		ret = mgmt_walk_ds_nodes(root, xpath, dnode,
					 mgmt_ds_node_iter_fn, ctx);
		if (ret != 0)
			break;
	}

	_dbg(" -- END: base xpath: '%s'", base_xpath);

	return ret;
}

struct lyd_node *mgmt_ds_find_data_node_by_xpath(struct mgmt_ds_ctx *ds_ctx,
						 const char *xpath)
{
	if (!ds_ctx)
		return NULL;

	return yang_dnode_get(ds_ctx->config_ds ? ds_ctx->root.cfg_root->dnode
						 : ds_ctx->root.dnode_root,
			      xpath);
}

int mgmt_ds_delete_data_nodes(struct mgmt_ds_ctx *ds_ctx, const char *xpath)
{
	struct nb_node *nb_node;
	struct lyd_node *dnode, *dep_dnode;
	char dep_xpath[XPATH_MAXLEN];

	if (!ds_ctx)
		return -1;

	nb_node = nb_node_find(xpath);

	dnode = yang_dnode_get(ds_ctx->config_ds
				       ? ds_ctx->root.cfg_root->dnode
				       : ds_ctx->root.dnode_root,
			       xpath);

	if (!dnode)
		/*
		 * Return a special error code so the caller can choose
		 * whether to ignore it or not.
		 */
		return NB_ERR_NOT_FOUND;
	/* destroy dependant */
	if (nb_node && nb_node->dep_cbs.get_dependant_xpath) {
		nb_node->dep_cbs.get_dependant_xpath(dnode, dep_xpath);

		dep_dnode = yang_dnode_get(
			ds_ctx->config_ds ? ds_ctx->root.cfg_root->dnode
					  : ds_ctx->root.dnode_root,
			dep_xpath);
		if (dep_dnode)
			lyd_free_tree(dep_dnode);
	}
	lyd_free_tree(dnode);

	return 0;
}

int mgmt_ds_load_config_from_file(struct mgmt_ds_ctx *dst,
				  const char *file_path, bool merge)
{
	struct lyd_node *iter;
	struct mgmt_ds_ctx parsed;

	if (!dst)
		return -1;

	if (mgmt_ds_load_cfg_from_file(file_path, &iter) != 0) {
		_log_err("Failed to load config from the file %s", file_path);
		return -1;
	}

	parsed.root.cfg_root = nb_config_new(iter);
	parsed.config_ds = true;
	parsed.ds_id = dst->ds_id;

	if (merge)
		ds_merge(dst, &parsed);
	else
		ds_copy(dst, &parsed);

	nb_config_free(parsed.root.cfg_root);

	return 0;
}

int mgmt_ds_iter_data(enum mgmt_ds_id ds_id, struct nb_config *root, const char *base_xpath,
		      void (*mgmt_ds_node_iter_fn)(const char *xpath, struct lyd_node *node,
						   struct nb_node *nb_node, void *ctx),
		      void *ctx)
{
	int ret = 0;
	char xpath[MGMTD_MAX_XPATH_LEN];
	struct lyd_node *base_dnode = NULL;
	struct lyd_node *node;

	if (!root)
		return -1;

	strlcpy(xpath, base_xpath, sizeof(xpath));
	mgmt_remove_trailing_separator(xpath, '/');

	/*
	 * mgmt_ds_iter_data is the only user of mgmt_walk_ds_nodes other than
	 * mgmt_walk_ds_nodes itself, so we can modify the API if we would like.
	 * Oper-state should be kept in mind though for the prefix walk
	 */

	_dbg(" -- START DS walk for DSid: %d", ds_id);

	/* If the base_xpath is empty then crawl the sibblings */
	if (xpath[0] == 0) {
		base_dnode = root->dnode;

		/* get first top-level sibling */
		while (base_dnode->parent)
			base_dnode = lyd_parent(base_dnode);

		while (base_dnode->prev->next)
			base_dnode = base_dnode->prev;

		LY_LIST_FOR (base_dnode, node) {
			ret = mgmt_walk_ds_nodes(root, xpath, node,
						 mgmt_ds_node_iter_fn, ctx);
		}
	} else
		ret = mgmt_walk_ds_nodes(root, xpath, base_dnode,
					 mgmt_ds_node_iter_fn, ctx);

	return ret;
}

void mgmt_ds_dump_tree(struct vty *vty, struct mgmt_ds_ctx *ds_ctx,
		       const char *xpath, FILE *f, LYD_FORMAT format)
{
	struct ly_out *out;
	char *str;
	char base_xpath[MGMTD_MAX_XPATH_LEN] = {0};

	if (!ds_ctx) {
		vty_out(vty, "    >>>>> Datastore Not Initialized!\n");
		return;
	}

	if (xpath) {
		strlcpy(base_xpath, xpath, MGMTD_MAX_XPATH_LEN);
		mgmt_remove_trailing_separator(base_xpath, '/');
	}

	if (f)
		ly_out_new_file(f, &out);
	else
		ly_out_new_memory(&str, 0, &out);

	mgmt_ds_dump_in_memory(ds_ctx, base_xpath, format, out);

	if (!f)
		vty_out(vty, "%s\n", str);

	ly_out_free(out, NULL, 0);
}

void mgmt_ds_status_write_one(struct vty *vty, struct mgmt_ds_ctx *ds_ctx)
{
	uint64_t session_id;
	bool locked;

	if (!ds_ctx) {
		vty_out(vty, "    >>>>> Datastore Not Initialized!\n");
		return;
	}

	locked = mgmt_ds_is_locked(ds_ctx, &session_id);
	vty_out(vty, "  DS: %s\n", mgmt_ds_id2name(ds_ctx->ds_id));
	vty_out(vty, "    DS-Hndl: \t\t\t%p\n", ds_ctx);
	vty_out(vty, "    Config: \t\t\t%s\n",
		ds_ctx->config_ds ? "True" : "False");
	vty_out(vty, "    Locked: \t\t\t%s Session-ID: %Lu\n", locked ? "True" : "False",
		locked ? session_id : 0);
}

void mgmt_ds_status_write(struct vty *vty)
{
	vty_out(vty, "MGMTD Datastores\n");

	mgmt_ds_status_write_one(vty, mgmt_ds_mm->running_ds);

	mgmt_ds_status_write_one(vty, mgmt_ds_mm->candidate_ds);

	mgmt_ds_status_write_one(vty, mgmt_ds_mm->oper_ds);
}
