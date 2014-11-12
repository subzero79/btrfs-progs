/*
 * Copyright (C) 2011 Red Hat.  All rights reserved.
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
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "volumes.h"
#include "utils.h"
#include "crc32c.h"

/*
 * find root result is a list like the following:
 *
 * result_list
 *    |
 *    gen_list	<->	gen_list	<->	gen_list ...
 *    gen:4		gen:5			gen:6
 *    level:0		level:1			level:2
 *    level_list	level_list		level_list
 *    |-l 0's eb	|-l 1'eb(possible root)	|-l 2'eb(possible root)
 *    |-l 0's eb
 *    ...
 *    level_list only contains the highest level's eb.
 *    if level_list only contains 1 eb, that may be root.
 *    if multiple, the root is already overwritten.
 */
struct eb_entry{
	struct list_head list;
	struct extent_buffer *eb;
};

struct generation_entry{
	struct list_head gen_list;
	struct list_head eb_list;
	u64 generation;
	u8 level;
};

struct search_filter {
	u64 objectid;
	u64 generation;
	u64 level;
	u64 super_gen;
	int search_all;
};

static void usage(void)
{
	fprintf(stderr, "Usage: find-roots [-o search_objectid] "
		"[ -g search_generation ] [ -l search_level ] [ -a ] "
		"[ -s [+-]{objectid|generation} ] <device>\n");
}

static inline void print_message(struct eb_entry *ebe,
				 struct btrfs_super_block *super)
{
	u64 generation = btrfs_header_generation(ebe->eb);

	if (generation != btrfs_super_generation(super))
		printf("Well block %llu seems great, but generation doesn't match, have=%llu, want=%llu level %u\n",
			btrfs_header_bytenr(ebe->eb),
			btrfs_header_generation(ebe->eb),
			btrfs_super_generation(super),
			btrfs_header_level(ebe->eb));
	else
		printf("Found tree root at %llu gen %llu level %u\n",
			btrfs_header_bytenr(ebe->eb),
			btrfs_header_generation(ebe->eb),
			btrfs_header_level(ebe->eb));
}

static void print_result(struct btrfs_root *chunk_root,
			 struct list_head *result_list)
{
	struct btrfs_super_block *super = chunk_root->fs_info->super_copy;
	struct eb_entry *ebe;
	struct generation_entry *gene;

	printf("Super think's the tree root is at %llu, chunk root %llu\n",
	       btrfs_super_root(super), btrfs_super_chunk_root(super));
	list_for_each_entry(gene, result_list, gen_list)
		list_for_each_entry(ebe, &gene->eb_list, list)
			print_message(ebe, super);
}

static int add_eb_to_gen(struct extent_buffer *eb,
			 struct generation_entry *gene)
{
	struct list_head *pos;
	struct eb_entry *ebe;
	struct eb_entry *n;
	struct eb_entry *new;
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
		ebe = list_entry(pos, struct eb_entry, list);
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
			    struct search_filter *search)
{
	struct list_head *pos;
	struct generation_entry *gene;
	struct generation_entry *new;
	u64 generation = btrfs_header_generation(eb);
	u64 level = btrfs_header_level(eb);
	u64 owner = btrfs_header_owner(eb);
	int found = 0;
	int ret = 0;

	if (owner != search->objectid || level < search->level ||
	    generation < search->generation)
		goto free_out;

	list_for_each(pos, result_list) {
		gene = list_entry(pos, struct generation_entry, gen_list);
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
static int find_root(struct btrfs_root *chunk_root,
		     struct list_head *result_list,
		     struct search_filter *search)
{
	struct extent_buffer *eb;
	struct btrfs_fs_info *fs_info = chunk_root->fs_info;
	u64 metadata_offset = 0;
	u64 metadata_size = 0;
	u64 offset;
	u64 leafsize = btrfs_super_leafsize(fs_info->super_copy);
	int ret = 0;

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
				return ret;
			}
		}
	}
	return ret;
}

static void free_result_list(struct list_head *result_list)
{
	struct eb_entry *ebe;
	struct eb_entry *ebtmp;
	struct generation_entry *gene;
	struct generation_entry *gentmp;

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

int main(int argc, char **argv)
{
	struct btrfs_root *chunk_root;
	struct list_head result_list;
	struct search_filter search = {BTRFS_ROOT_TREE_OBJECTID, 0 ,0, 0};
	int opt;
	int ret;

	while ((opt = getopt(argc, argv, "l:o:g:a")) != -1) {
		switch(opt) {
			case 'o':
				search.objectid = arg_strtou64(optarg);
				break;
			case 'g':
				search.generation = arg_strtou64(optarg);
				break;
			case 'l':
				search.level = arg_strtou64(optarg);
				break;
			case 'a':
				search.search_all = 1;
				break;
			default:
				usage();
				exit(1);
		}
	}

	set_argv0(argv);
	argc = argc - optind;
	if (check_argc_exact(argc, 1)) {
		usage();
		exit(1);
	}

	chunk_root = open_ctree(argv[optind], 0, OPEN_CTREE_CHUNK_ONLY);
	if (!chunk_root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}

	search.super_gen =
		btrfs_super_generation(chunk_root->fs_info->super_copy);
	INIT_LIST_HEAD(&result_list);
	ret = find_root(chunk_root, &result_list, &search);
	print_result(chunk_root, &result_list);
	free_result_list(&result_list);
	close_ctree(chunk_root);
	return ret;
}
