/*
 * Copyright (C) 2014 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "list.h"
#include "volumes.h"
#include "utils.h"
#include "crc32c.h"
#include "find-root.h"

static int add_eb_to_gen(struct extent_buffer *eb,
			 struct find_root_gen_entry *gene)
{
	struct list_head *pos;
	struct find_root_eb_entry *ebe;
	struct find_root_eb_entry *n;
	struct find_root_eb_entry *new;
	u8 level = btrfs_header_level(eb);
	u64 bytenr = btrfs_header_bytenr(eb);

	if (level < gene->level)
		goto free_out;

	new = malloc(sizeof(*new));
	if (!new)
		return -ENOMEM;
	new->eb = eb;

	if (level > gene->level) {
		gene->level = level;
		list_for_each_entry_safe(ebe, n, &gene->eb_list, list) {
			list_del(&ebe->list);
			free_extent_buffer(ebe->eb);
			free(ebe);
		}
		list_add(&new->list, &gene->eb_list);
		return 0;
	}
	list_for_each(pos, &gene->eb_list) {
		ebe = list_entry(pos, struct find_root_eb_entry, list);
		if (btrfs_header_bytenr(ebe->eb) > bytenr) {
			pos = pos->prev;
			break;
		}
	}
	list_add_tail(&new->list, pos);
	return 0;
free_out:
	free_extent_buffer(eb);
	return 0;
}

static int add_eb_to_result(struct extent_buffer *eb,
			    struct list_head *result_list,
			    struct find_root_search_filter *search)
{
	struct list_head *pos;
	struct find_root_gen_entry *gene;
	struct find_root_gen_entry *new;
	u64 generation = btrfs_header_generation(eb);
	u64 level = btrfs_header_level(eb);
	u64 owner = btrfs_header_owner(eb);
	int found = 0;
	int ret = 0;

	if (owner != search->objectid || level < search->level ||
	    generation < search->generation)
		goto free_out;

	list_for_each(pos, result_list) {
		gene = list_entry(pos, struct find_root_gen_entry, gen_list);
		if (gene->generation == generation) {
			found = 1;
			break;
		}
		if (gene->generation > generation) {
			pos = pos->prev;
			break;
		}
	}
	if (found) {
		ret = add_eb_to_gen(eb, gene);
	} else {
		new = malloc(sizeof(*new));
		if (!new) {
			ret = -ENOMEM;
			goto free_out;
		}
		new->generation = generation;
		new->level = 0;
		INIT_LIST_HEAD(&new->gen_list);
		INIT_LIST_HEAD(&new->eb_list);
		list_add_tail(&new->gen_list, pos);
		ret = add_eb_to_gen(eb, new);
	}
	if (ret)
		goto free_out;
	if (generation == search->super_gen && !search->search_all)
		ret = 1;
	return ret;
free_out:
	free_extent_buffer(eb);
	return ret;
}

int find_root_start(struct btrfs_root *chunk_root,
		    struct list_head *result_list,
		    struct find_root_search_filter *search)
{
	struct extent_buffer *eb;
	struct btrfs_fs_info *fs_info = chunk_root->fs_info;
	u64 metadata_offset = 0;
	u64 metadata_size = 0;
	u64 offset;
	u64 leafsize = btrfs_super_leafsize(fs_info->super_copy);
	int ret = 0;
	int suppress_error = fs_info->suppress_error;

	fs_info->suppress_error = 1;
	while (1) {
		ret = btrfs_next_metadata(&chunk_root->fs_info->mapping_tree,
					  &metadata_offset, &metadata_size);
		if (ret) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}
		for (offset = metadata_offset;
		     offset < metadata_offset + metadata_size;
		     offset += leafsize) {
			eb = read_tree_block(chunk_root, offset, leafsize, 0);
			if (!eb || IS_ERR(eb))
				continue;
			ret = add_eb_to_result(eb, result_list, search);
			if (ret) {
				ret = ret > 0 ? 0 : ret;
				goto out;
			}
		}
	}
out:
	fs_info->suppress_error = suppress_error;
	return ret;
}

void find_root_free(struct list_head *result_list)
{
	struct find_root_eb_entry *ebe;
	struct find_root_eb_entry *ebtmp;
	struct find_root_gen_entry *gene;
	struct find_root_gen_entry *gentmp;

	list_for_each_entry_safe(gene, gentmp, result_list, gen_list) {
		list_for_each_entry_safe(ebe, ebtmp,&gene->eb_list, list) {
			list_del(&ebe->list);
			free_extent_buffer(ebe->eb);
			free(ebe);
		}
		list_del(&gene->gen_list);
		free(gene);
	}
}
