/*
 * hash-tree.c
 *
 * Copyright (C) 2013 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Authors: Mark Fasheh <mfasheh@suse.de>
 */

#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kernel.h"
#include "rbtree.h"
#include "list.h"

#include "csum.h"	/* for digest_len variable and DIGEST_LEN_MAX */
#include "filerec.h"

#include "hash-tree.h"
#include "debug.h"

declare_alloc_tracking(file_block);
declare_alloc_tracking(dupe_blocks_list);
declare_alloc_tracking(filerec_token);

struct filerec_token *find_filerec_token_rb(struct dupe_blocks_list *dups,
					    struct filerec *val)
{
	struct rb_node *n = dups->dl_files_root.rb_node;
	struct filerec_token *t;

	while (n) {
		t = rb_entry(n, struct filerec_token, t_node);

		if (t->t_file > val)
			n = n->rb_left;
		else if (t->t_file < val)
			n = n->rb_right;
		else
			return t;
	}
	return NULL;
}

static void insert_filerec_token_rb(struct dupe_blocks_list *dups,
				    struct filerec_token *token)
{
	struct rb_node **p = &dups->dl_files_root.rb_node;
	struct rb_node *parent = NULL;
	struct filerec_token *tmp;

	while (*p) {
		parent = *p;

		tmp = rb_entry(parent, struct filerec_token, t_node);

		if (tmp->t_file > token->t_file)
			p = &(*p)->rb_left;
		else if (tmp->t_file < token->t_file)
			p = &(*p)->rb_right;
		else
			abort_lineno(); /* We should never find a duplicate */
	}

	rb_link_node(&token->t_node, parent, p);
	rb_insert_color(&token->t_node, &dups->dl_files_root);
}

static int add_one_filerec_token(struct dupe_blocks_list *dups,
				 struct filerec *file)
{
	struct filerec_token *t = NULL;

	if (find_filerec_token_rb(dups, file))
		return 0;

	t = malloc_filerec_token();
	if (!t)
		return ENOMEM;

	rb_init_node(&t->t_node);
	t->t_file = file;

	insert_filerec_token_rb(dups, t);
	dups->dl_num_files++;
	return 0;
}

static int add_filerec_tokens(struct dupe_blocks_list *dups)
{
	struct file_block *block;

	list_for_each_entry(block, &dups->dl_list, b_list) {
		if (add_one_filerec_token(dups, block->b_file))
			return ENOMEM;
	}
	return 0;
}

static void free_filerec_tokens(struct dupe_blocks_list *dups)
{
	struct rb_node *node = rb_first(&dups->dl_files_root);
	struct filerec_token *t;

	while (node) {
		t = rb_entry(node, struct filerec_token, t_node);

		node = rb_next(node);

		dups->dl_num_files--;
		rb_erase(&t->t_node, &dups->dl_files_root);
		free_filerec_token(t);
	}
}

static void insert_block_list(struct hash_tree *tree,
			      struct dupe_blocks_list *list)
{
	struct rb_node **p = &tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct dupe_blocks_list *tmp;
	int cmp;

	while (*p) {
		parent = *p;

		tmp = rb_entry(parent, struct dupe_blocks_list, dl_node);

		cmp = memcmp(list->dl_hash, tmp->dl_hash, digest_len);
		if (cmp < 0)
			p = &(*p)->rb_left;
		else if (cmp > 0)
			p = &(*p)->rb_right;
		else abort_lineno(); /* We should never find a duplicate */
	}

	rb_link_node(&list->dl_node, parent, p);
	rb_insert_color(&list->dl_node, &tree->root);

	tree->num_hashes++;
	return;
}

static struct dupe_blocks_list *find_block_list(struct hash_tree *tree,
					       unsigned char *digest)
{
	struct rb_node *n = tree->root.rb_node;
	struct dupe_blocks_list *list;
	int cmp;

	while (n) {
		list = rb_entry(n, struct dupe_blocks_list, dl_node);

		cmp = memcmp(digest, list->dl_hash, digest_len);
		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else return list;
	}
	return NULL;
}

int insert_hashed_block(struct hash_tree *tree,	unsigned char *digest,
			struct filerec *file, uint64_t loff, unsigned int flags)
{
	struct file_block *e = malloc_file_block();
	struct dupe_blocks_list *d;

	if (!e)
		return ENOMEM;

	d = find_block_list(tree, digest);
	if (d == NULL) {
		d = calloc_dupe_blocks_list(1);
		if (!d) {
			free_file_block(e);
			return ENOMEM;
		}

		memcpy(d->dl_hash, digest, digest_len);
		rb_init_node(&d->dl_node);
		rb_init_node(&d->dl_by_size);
		INIT_LIST_HEAD(&d->dl_list);
		INIT_LIST_HEAD(&d->dl_large_list);
		d->dl_files_root = RB_ROOT;

		insert_block_list(tree, d);
	}

	if (d->dl_num_elem >= DUPLIST_CONVERT_LIMIT && d->dl_num_files == 0) {
		if (add_filerec_tokens(d))
			return ENOMEM;
	}

	e->b_file = file;
	e->b_seen = 0;
	e->b_loff = loff;
	e->b_flags = flags;
	list_add_tail(&e->b_file_next, &file->block_list);
	file->num_blocks++;
	e->b_parent = d;

	if (d->dl_num_files) {
		if (add_one_filerec_token(d, file))
			return ENOMEM;
	}

	d->dl_num_elem++;
	list_add_tail(&e->b_list, &d->dl_list);

	tree->num_blocks++;
	return 0;
}

static void remove_hashed_block(struct hash_tree *tree,
				struct file_block *block, struct filerec *file)
{
	struct dupe_blocks_list *blocklist = block->b_parent;

	abort_on(blocklist->dl_num_elem == 0);

	if (!list_empty(&block->b_file_next)) {
		abort_on(file->num_blocks == 0);
		file->num_blocks--;
	}

	list_del(&block->b_file_next);
	list_del(&block->b_list);

	blocklist->dl_num_elem--;
	if (blocklist->dl_num_elem == 0) {
		rb_erase(&blocklist->dl_node, &tree->root);
		tree->num_hashes--;

		free_filerec_tokens(blocklist);
		free_dupe_blocks_list(blocklist);
	}

	free_file_block(block);
	tree->num_blocks--;
}

void remove_hashed_blocks(struct hash_tree *tree, struct filerec *file)
{
	struct file_block *block, *tmp;

	list_for_each_entry_safe(block, tmp, &file->block_list, b_file_next)
		remove_hashed_block(tree, block, file);
}

void for_each_dupe(struct file_block *block, struct filerec *file,
		   for_each_dupe_t func, void *priv)
{
	struct dupe_blocks_list *parent = block->b_parent;
	struct file_block *cur;

	list_for_each_entry(cur, &parent->dl_list, b_list) {
		/* Ignore self and any blocks from another file */
		if (cur == block)
			continue;

		if (cur->b_file != file)
			continue;

		if (func(cur, priv))
			break;
	}
}

static unsigned int seen_counter = 1;

int block_seen(struct file_block *block)
{
	return !!(block->b_seen == seen_counter);
}

int block_ever_seen(struct file_block *block)
{
	return !(block->b_seen == 0);
}

void mark_block_seen(struct file_block *block)
{
	block->b_seen = seen_counter;
}

void clear_all_seen_blocks(void)
{
	seen_counter++;
}

void init_hash_tree(struct hash_tree *tree)
{
	tree->root = RB_ROOT;
	tree->num_blocks = tree->num_hashes = 0;
}
