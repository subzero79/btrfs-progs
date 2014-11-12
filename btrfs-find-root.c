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
#include "find-root.h"

static void usage(void)
{
	fprintf(stderr, "Usage: find-roots [-o search_objectid] "
		"[ -g search_generation ] [ -l search_level ] [ -a ] "
		"[ -s [+-]{objectid|generation} ] <device>\n");
}

static inline void print_message(struct find_root_eb_entry *ebe,
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
	struct find_root_eb_entry *ebe;
	struct find_root_gen_entry *gene;

	printf("Super think's the tree root is at %llu, chunk root %llu\n",
	       btrfs_super_root(super), btrfs_super_chunk_root(super));
	list_for_each_entry(gene, result_list, gen_list)
		list_for_each_entry(ebe, &gene->eb_list, list)
			print_message(ebe, super);
}


int main(int argc, char **argv)
{
	struct btrfs_root *chunk_root;
	struct list_head result_list;
	struct find_root_search_filter search =
				{BTRFS_ROOT_TREE_OBJECTID, 0 ,0, 0};
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
	ret = find_root_start(chunk_root, &result_list, &search);
	print_result(chunk_root, &result_list);
	find_root_free(&result_list);
	close_ctree(chunk_root);
	return ret;
}
